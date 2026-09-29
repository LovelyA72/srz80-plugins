#include <state.hpp>
#include <boundary.hpp>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

extern "C" {
#include "cpu/z80.h"
// Disassembly lives in a separate C bridge because cpu/z80.h and z80_dasm.h
// both define Z80_REG_* identifiers and cannot share one translation unit.
int srz80_z80_dasm(uint16_t pc, const uint8_t *bytes, uint32_t bytes_len,
                   uint32_t *length, uint32_t *tstates, char *text, uint32_t text_size);
}

namespace {
enum TraceKind : uint32_t {
    TRACE_KIND_OPCODE = 1,
    TRACE_KIND_OPERAND = 2,
    TRACE_KIND_DATA_READ = 3,
    TRACE_KIND_DATA_WRITE = 4,
    TRACE_KIND_IO_READ = 5,
    TRACE_KIND_IO_WRITE = 6,
    TRACE_KIND_INT_ACK = 7
};

struct Cpu {
    const ShouryoHost *host = nullptr;
    const SrhHostDebugV1 *debug = nullptr;
    SrhHandle owner = 0, memory_space = 0, io_space = 0;
    uint32_t clock = 0;
    uint64_t reset_vector = 0;
    z80_t *z80 = nullptr;
    int cycles_remaining = 0;
    SrhHandle irq_signal = 0, nmi_signal = 0;
    bool irq_high = false, nmi_high = false;
    SrhStatus bus_error = SRH_OK;
    bool trace_enabled = true;

    void set_kind(uint32_t kind) {
        if (trace_enabled && debug)
            debug->set_trace_kind(debug->context, owner, kind);
    }
    SrhStatus tick() {
        if (!z80)
            return SRH_INVALID;
        if (srz80::sdk::has_field(debug, &SrhHostDebugV1::trace_enabled))
            trace_enabled = debug->trace_enabled(debug->context) != 0;
        if (cycles_remaining > 0) {
            --cycles_remaining;
            return SRH_OK;
        }
        // One logical tick starts at most one Z80 instruction.  The z80-mz800
        // core executes whole instructions, so the instruction's T-state count
        // is accounted over the following ticks: bus transactions are
        // attributed to the starting tick, subsequent ticks are idle T-states.
        // This keeps tick totals equal to instruction T-state totals.
        const bool boundary_required =
            !srz80::sdk::has_field(debug, &SrhHostDebugV1::boundary_required) ||
            debug->boundary_required(debug->context) != 0;
        if (!z80->halted && boundary_required) {
            auto status =
                debug->boundary_ex(debug->context, owner, memory_space, z80->pc, 0, 0);
            if (status == SRH_STOP)
                return SRH_OK; // debugger already paused the host
            if (status != SRH_OK)
                return status;
        }
        bus_error = SRH_OK;
        int tstates = z80_step(z80);
        if (tstates < 0)
            return SRH_ERROR;
        cycles_remaining = tstates > 1 ? tstates - 1 : 0;
        if (bus_error != SRH_OK) {
            if (debug)
                debug->request_stop(debug->context, owner, "Z80 bus error");
            return bus_error;
        }
        return SRH_OK;
    }
    SrhStatus reset(bool cold) {
        (void)cold;
        if (!z80)
            return SRH_INVALID;
        z80_reset(z80);
        z80->pc = static_cast<u16>(reset_vector & 0xFFFF);
        z80->cycles = 0;
        z80->total_cycles = 0;
        z80->wait_cycles = 0;
        cycles_remaining = 0;
        irq_high = false;
        nmi_high = false;
        return SRH_OK;
    }
};

u8 mem_read(z80_t *, u16 addr, int m1_state, void *data) {
    auto &c = *static_cast<Cpu *>(data);
    c.set_kind(m1_state ? TRACE_KIND_OPCODE : TRACE_KIND_OPERAND);
    uint8_t value = 0xFF;
    auto status = c.host->read(c.host->context, c.owner, c.memory_space, addr, &value);
    if (status != SRH_OK)
        c.bus_error = status;
    return value;
}
void mem_write(z80_t *, u16 addr, u8 value, void *data) {
    auto &c = *static_cast<Cpu *>(data);
    c.set_kind(TRACE_KIND_DATA_WRITE);
    auto status = c.host->write(c.host->context, c.owner, c.memory_space, addr, value);
    if (status != SRH_OK)
        c.bus_error = status;
}
u8 io_read(z80_t *, u16 port, void *data) {
    auto &c = *static_cast<Cpu *>(data);
    c.set_kind(TRACE_KIND_IO_READ);
    uint8_t value = 0xFF;
    auto status = c.host->read(c.host->context, c.owner, c.io_space, port & 0xFF, &value);
    if (status != SRH_OK)
        c.bus_error = status;
    return value;
}
void io_write(z80_t *, u16 port, u8 value, void *data) {
    auto &c = *static_cast<Cpu *>(data);
    c.set_kind(TRACE_KIND_IO_WRITE);
    auto status = c.host->write(c.host->context, c.owner, c.io_space, port & 0xFF, value);
    if (status != SRH_OK)
        c.bus_error = status;
}
u8 int_read(z80_t *, void *data) {
    auto &c = *static_cast<Cpu *>(data);
    c.set_kind(TRACE_KIND_INT_ACK);
    uint8_t value = 0xFF;
    auto status = c.host->read(c.host->context, c.owner, c.io_space, 0, &value);
    if (status != SRH_OK)
        c.bus_error = status;
    return value;
}

struct Json {
    const char *text;
    size_t size;
    Json(const char *t, uint64_t n) : text(t ? t : ""), size(t ? static_cast<size_t>(n) : 0) {}
    std::string get_string(const char *key, const std::string &fallback) const {
        std::string src(text, size);
        std::string needle = std::string("\"") + key + "\"";
        size_t pos = src.find(needle);
        if (pos == std::string::npos)
            return fallback;
        pos += needle.size();
        while (pos < src.size() && (src[pos] == ' ' || src[pos] == '\t' || src[pos] == '\n'))
            ++pos;
        if (pos >= src.size() || src[pos] != ':')
            return fallback;
        ++pos;
        while (pos < src.size() && (src[pos] == ' ' || src[pos] == '\t' || src[pos] == '\n'))
            ++pos;
        if (pos >= src.size() || src[pos] != '"')
            return fallback;
        ++pos;
        std::string out;
        while (pos < src.size() && src[pos] != '"') {
            if (src[pos] == '\\' && pos + 1 < src.size()) {
                ++pos;
                out += src[pos];
                ++pos;
            } else
                out += src[pos++];
        }
        return out;
    }
    uint64_t get_uint(const char *key, uint64_t fallback) const {
        std::string src(text, size);
        std::string needle = std::string("\"") + key + "\"";
        size_t pos = src.find(needle);
        if (pos == std::string::npos)
            return fallback;
        pos += needle.size();
        while (pos < src.size() && (src[pos] == ' ' || src[pos] == '\t' || src[pos] == '\n'))
            ++pos;
        if (pos >= src.size() || src[pos] != ':')
            return fallback;
        ++pos;
        while (pos < src.size() && (src[pos] == ' ' || src[pos] == '\t' || src[pos] == '\n'))
            ++pos;
        if (pos >= src.size() || !(src[pos] >= '0' && src[pos] <= '9'))
            return fallback;
        uint64_t value = 0;
        while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') {
            value = value * 10 + static_cast<uint64_t>(src[pos] - '0');
            ++pos;
        }
        return value;
    }
};

SrhStatus SRH_CALL tick_callback(void *p) {
    return srz80::sdk::guard([&] { return static_cast<Cpu *>(p)->tick(); });
}
SrhStatus SRH_CALL irq_callback(void *p, int32_t millivolts) {
    auto &c = *static_cast<Cpu *>(p);
    bool high = millivolts > 700;
    if (high && !c.irq_high)
        z80_int(c.z80);
    if (!high && c.irq_high)
        c.z80->int_pending = false;
    c.irq_high = high;
    return SRH_OK;
}
SrhStatus SRH_CALL nmi_callback(void *p, int32_t millivolts) {
    auto &c = *static_cast<Cpu *>(p);
    bool high = millivolts > 700;
    if (high && !c.nmi_high)
        z80_nmi(c.z80);
    if (!high && c.nmi_high)
        c.z80->nmi_pending = false;
    c.nmi_high = high;
    return SRH_OK;
}

SrhStatus SRH_CALL disasm(void *, SrhHandle, SrhHandle, uint64_t pc, const uint8_t *bytes,
                          uint32_t bytes_available, uint32_t *instruction_bytes,
                          uint32_t *cycles, char *text, uint32_t text_size) {
    if (!instruction_bytes || !cycles || !text || text_size == 0)
        return SRH_INVALID;
    if (pc > 0xFFFF || bytes_available == 0)
        return SRH_UNAVAILABLE;
    uint32_t length = 0, tstates = 0;
    int status = srz80_z80_dasm(static_cast<uint16_t>(pc & 0xFFFF), bytes, bytes_available,
                                &length, &tstates, text, text_size);
    if (status != 0)
        return SRH_UNAVAILABLE;
    *instruction_bytes = length;
    *cycles = tstates;
    return SRH_OK;
}

SrhStatus SRH_CALL create(const ShouryoHost *host, SrhHandle owner, const SrhConfig *config,
                          void **result) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!srz80::sdk::valid(host) || !srz80::sdk::valid(config) || !result || !config->space ||
            config->clock >= 3)
            return SRH_INVALID;
        const void *extension = nullptr;
        if (!host->query || host->query(host->context, "host.resources.v1", &extension) != SRH_OK ||
            !extension)
            return SRH_UNAVAILABLE;
        auto resources = static_cast<const SrhHostResourcesV1 *>(extension);
        if (!srz80::sdk::valid(resources))
            return SRH_UNAVAILABLE;
        extension = nullptr;
        if (host->query(host->context, "host.debug.v1", &extension) != SRH_OK || !extension)
            return SRH_UNAVAILABLE;
        auto debug = static_cast<const SrhHostDebugV1 *>(extension);
        if (!srz80::sdk::valid(debug))
            return SRH_UNAVAILABLE;
        Json json(config->config_json, config->config_json_size);
        auto io_space_name = json.get_string("io_space", "cpu0.io");
        SrhHandle io_space = 0;
        auto status = resources->lookup(resources->context, "space", io_space_name.c_str(), &io_space);
        if (status != SRH_OK)
            return status;
        auto cpu = std::make_unique<Cpu>();
        cpu->host = host;
        cpu->debug = debug;
        cpu->owner = owner;
        cpu->memory_space = config->space;
        cpu->io_space = io_space;
        cpu->clock = config->clock;
        cpu->reset_vector = config->reset_vector;
        cpu->z80 = z80_create(mem_read, cpu.get(), mem_write, cpu.get(), io_read, cpu.get(),
                              io_write, cpu.get(), int_read, cpu.get());
        if (!cpu->z80)
            return SRH_ERROR;
        status = debug->register_disasm(debug->context, owner, disasm, cpu.get());
        if (status != SRH_OK)
            return status;
        SrhHandle subscription = 0;
        status = host->subscribe_clock(host->context, owner, cpu->clock, tick_callback, cpu.get(),
                                       &subscription);
        if (status != SRH_OK)
            return status;
        SrhHandle irq = 0, nmi = 0;
        if (host->signal_find(host->context, "IRQ", &irq) == SRH_OK && irq) {
            cpu->irq_signal = irq;
            host->signal_subscribe(host->context, owner, irq, irq_callback, cpu.get(), &subscription);
        }
        if (host->signal_find(host->context, "NMI", &nmi) == SRH_OK && nmi) {
            cpu->nmi_signal = nmi;
            host->signal_subscribe(host->context, owner, nmi, nmi_callback, cpu.get(), &subscription);
        }
        cpu->reset(true);
        *result = cpu.release();
        return SRH_OK;
    });
}
void SRH_CALL destroy(void *p) {
    auto cpu = static_cast<Cpu *>(p);
    if (cpu->z80)
        z80_destroy(cpu->z80);
    delete cpu;
}
SrhStatus SRH_CALL reset(void *p, uint32_t cold) {
    return srz80::sdk::guard([&] { return static_cast<Cpu *>(p)->reset(cold != 0); });
}
uint32_t SRH_CALL property_count(void *) {
    return 20;
}
SrhStatus SRH_CALL property_info(void *, uint32_t index, SrhProperty *out) {
    if (!srz80::sdk::valid(out) || index >= 20)
        return SRH_INVALID;
    struct Info {
        const char *name, *group, *description;
        uint32_t kind, bits, base, editable;
        const char *enums;
    };
    static const Info infos[20] = {
        {"model", "Control", "Z80 core model/version", SRH_TEXT, 0, 0, 0, nullptr},
        {"PC", "Main", "Program counter", SRH_UNSIGNED, 16, 16, 1, nullptr},
        {"SP", "Main", "Stack pointer", SRH_UNSIGNED, 16, 16, 1, nullptr},
        {"AF", "Main", "Accumulator and flags", SRH_UNSIGNED, 16, 16, 1, nullptr},
        {"BC", "Main", "General pair BC", SRH_UNSIGNED, 16, 16, 1, nullptr},
        {"DE", "Main", "General pair DE", SRH_UNSIGNED, 16, 16, 1, nullptr},
        {"HL", "Main", "General pair HL", SRH_UNSIGNED, 16, 16, 1, nullptr},
        {"F", "Main", "Flags", SRH_UNSIGNED, 8, 16, 1, nullptr},
        {"AF'", "Alternate", "Alternate accumulator and flags", SRH_UNSIGNED, 16, 16, 0, nullptr},
        {"BC'", "Alternate", "Alternate pair BC", SRH_UNSIGNED, 16, 16, 0, nullptr},
        {"DE'", "Alternate", "Alternate pair DE", SRH_UNSIGNED, 16, 16, 0, nullptr},
        {"HL'", "Alternate", "Alternate pair HL", SRH_UNSIGNED, 16, 16, 0, nullptr},
        {"IX", "Index", "Index register IX", SRH_UNSIGNED, 16, 16, 1, nullptr},
        {"IY", "Index", "Index register IY", SRH_UNSIGNED, 16, 16, 1, nullptr},
        {"I", "Control", "Interrupt vector", SRH_UNSIGNED, 8, 16, 1, nullptr},
        {"R", "Control", "Memory refresh", SRH_UNSIGNED, 8, 16, 0, nullptr},
        {"IM", "Control", "Interrupt mode", SRH_ENUM, 2, 10, 0, "0|1|2"},
        {"IFF1", "Control", "Interrupt flip-flop 1", SRH_BOOLEAN, 1, 10, 0, nullptr},
        {"IFF2", "Control", "Interrupt flip-flop 2", SRH_BOOLEAN, 1, 10, 0, nullptr},
        {"HALTED", "Control", "HALT state", SRH_BOOLEAN, 1, 10, 0, nullptr},
    };
    const auto &i = infos[index];
    *out = {SRH_INIT(SrhProperty), i.name, i.group, i.description, i.kind, i.bits, i.base,
            i.editable, i.enums};
    return SRH_OK;
}
SrhStatus SRH_CALL property_get(void *p, uint32_t index, SrhValue *out) {
    if (!srz80::sdk::valid(out) || index >= 20)
        return SRH_INVALID;
    auto &c = *static_cast<Cpu *>(p);
    auto &z = *c.z80;
    switch (index) {
    case 0: std::snprintf(out->text, sizeof(out->text), "z80-mz800 %s", CPU_Z80_VERSION); break;
    case 1: out->unsigned_value = z.pc; break;
    case 2: out->unsigned_value = z.sp; break;
    case 3: out->unsigned_value = z.af.w; break;
    case 4: out->unsigned_value = z.bc.w; break;
    case 5: out->unsigned_value = z.de.w; break;
    case 6: out->unsigned_value = z.hl.w; break;
    case 7: out->unsigned_value = z.af.l; break;
    case 8: out->unsigned_value = z.af2.w; break;
    case 9: out->unsigned_value = z.bc2.w; break;
    case 10: out->unsigned_value = z.de2.w; break;
    case 11: out->unsigned_value = z.hl2.w; break;
    case 12: out->unsigned_value = z.ix.w; break;
    case 13: out->unsigned_value = z.iy.w; break;
    case 14: out->unsigned_value = z.i; break;
    case 15: out->unsigned_value = z.r; break;
    case 16: out->unsigned_value = z.im; break;
    case 17: out->unsigned_value = z.iff1; break;
    case 18: out->unsigned_value = z.iff2; break;
    case 19: out->unsigned_value = z.halted; break;
    }
    return SRH_OK;
}
SrhStatus SRH_CALL property_set(void *p, uint32_t index, const SrhValue *in) {
    if (!srz80::sdk::valid(in) || index >= 20 || in->unsigned_value > 0xFFFF)
        return SRH_INVALID;
    auto &c = *static_cast<Cpu *>(p);
    auto &z = *c.z80;
    switch (index) {
    case 1: z.pc = static_cast<u16>(in->unsigned_value); break;
    case 2: z.sp = static_cast<u16>(in->unsigned_value); break;
    case 3: z.af.w = static_cast<u16>(in->unsigned_value); break;
    case 4: z.bc.w = static_cast<u16>(in->unsigned_value); break;
    case 5: z.de.w = static_cast<u16>(in->unsigned_value); break;
    case 6: z.hl.w = static_cast<u16>(in->unsigned_value); break;
    case 7:
        if (in->unsigned_value > 0xFF)
            return SRH_INVALID;
        z.af.l = static_cast<u8>(in->unsigned_value);
        break;
    case 12: z.ix.w = static_cast<u16>(in->unsigned_value); break;
    case 13: z.iy.w = static_cast<u16>(in->unsigned_value); break;
    case 14:
        if (in->unsigned_value > 0xFF)
            return SRH_INVALID;
        z.i = static_cast<u8>(in->unsigned_value);
        break;
    default: return SRH_INVALID;
    }
    c.cycles_remaining = 0;
    return SRH_OK;
}
struct PackedZ80 {
    uint16_t af, bc, de, hl, af2, bc2, de2, hl2, ix, iy, wz, sp, pc;
    uint8_t i, r, iff1, iff2, im;
    uint8_t halted, int_pending, nmi_pending, ei_delay, ld_a_ir, int_vector, q;
    uint32_t cycles, total_cycles;
    int32_t wait_cycles, op_tstate, cycles_remaining, bus_error;
    uint8_t irq_high, nmi_high;
};
template <class Archive> void archive_state(Archive &ar, PackedZ80 &s) {
    ar.fields(s.af, s.bc, s.de, s.hl, s.af2, s.bc2, s.de2, s.hl2, s.ix, s.iy,
              s.wz, s.sp, s.pc, s.i, s.r, s.iff1, s.iff2, s.im, s.halted,
              s.int_pending, s.nmi_pending, s.ei_delay, s.ld_a_ir, s.int_vector,
              s.q, s.cycles, s.total_cycles, s.wait_cycles, s.op_tstate,
              s.cycles_remaining, s.bus_error, s.irq_high, s.nmi_high);
}
SrhStatus SRH_CALL save_payload(void *p, uint8_t *buffer, uint64_t *size) {
    auto &c = *static_cast<Cpu *>(p);
    auto &z = *c.z80;
    PackedZ80 s{};
    s.af = z.af.w;
    s.bc = z.bc.w;
    s.de = z.de.w;
    s.hl = z.hl.w;
    s.af2 = z.af2.w;
    s.bc2 = z.bc2.w;
    s.de2 = z.de2.w;
    s.hl2 = z.hl2.w;
    s.ix = z.ix.w;
    s.iy = z.iy.w;
    s.wz = z.wz.w;
    s.sp = z.sp;
    s.pc = z.pc;
    s.i = z.i;
    s.r = z.r;
    s.iff1 = z.iff1;
    s.iff2 = z.iff2;
    s.im = z.im;
    s.halted = z.halted ? 1 : 0;
    s.int_pending = z.int_pending ? 1 : 0;
    s.nmi_pending = z.nmi_pending ? 1 : 0;
    s.ei_delay = z.ei_delay ? 1 : 0;
    s.ld_a_ir = z.ld_a_ir ? 1 : 0;
    s.int_vector = z.int_vector;
    s.q = z.q;
    s.cycles = z.cycles;
    s.total_cycles = z.total_cycles;
    s.wait_cycles = z.wait_cycles;
    s.op_tstate = z.op_tstate;
    s.cycles_remaining = c.cycles_remaining;
    s.bus_error = c.bus_error;
    s.irq_high = c.irq_high ? 1 : 0;
    s.nmi_high = c.nmi_high ? 1 : 0;
    srz80::sdk::state::Writer writer;
    archive_state(writer, s);
    return srz80::sdk::state::copy_payload(writer.bytes, buffer, size);
}
SrhStatus SRH_CALL load_payload(void *p, const uint8_t *buffer, uint64_t size) {
    auto &c = *static_cast<Cpu *>(p);
    auto &z = *c.z80;
    PackedZ80 s{};
    srz80::sdk::state::Reader reader({buffer, static_cast<size_t>(size)});
    archive_state(reader, s);
    if (!reader.finished() || s.iff1 > 1 || s.iff2 > 1 || s.im > 2 || s.halted > 1 ||
        s.int_pending > 1 || s.nmi_pending > 1 || s.ei_delay > 1 || s.ld_a_ir > 1 ||
        s.irq_high > 1 || s.nmi_high > 1 || s.bus_error < SRH_OK || s.bus_error > SRH_CONFLICT)
        return SRH_INVALID;
    z.af.w = s.af;
    z.bc.w = s.bc;
    z.de.w = s.de;
    z.hl.w = s.hl;
    z.af2.w = s.af2;
    z.bc2.w = s.bc2;
    z.de2.w = s.de2;
    z.hl2.w = s.hl2;
    z.ix.w = s.ix;
    z.iy.w = s.iy;
    z.wz.w = s.wz;
    z.sp = s.sp;
    z.pc = s.pc;
    z.i = s.i;
    z.r = s.r;
    z.iff1 = s.iff1;
    z.iff2 = s.iff2;
    z.im = s.im;
    z.halted = s.halted != 0;
    z.int_pending = s.int_pending != 0;
    z.nmi_pending = s.nmi_pending != 0;
    z.ei_delay = s.ei_delay != 0;
    z.ld_a_ir = s.ld_a_ir != 0;
    z.int_vector = s.int_vector;
    z.q = s.q;
    z.cycles = s.cycles;
    z.total_cycles = s.total_cycles;
    z.wait_cycles = s.wait_cycles;
    z.op_tstate = s.op_tstate;
    z._active_cache = nullptr;
    c.cycles_remaining = s.cycles_remaining;
    c.bus_error = static_cast<SrhStatus>(s.bus_error);
    c.irq_high = s.irq_high != 0;
    c.nmi_high = s.nmi_high != 0;
    return SRH_OK;
}
const SrhCardDescriptor descriptor{SRH_INIT(SrhCardDescriptor), "CPU", "Z80",
                                   "Zilog Z80 processor", 0, 0, 0, 0, 0,
                                   SRH_CARD_REQUIRES_IO_SPACE | SRH_CARD_SHOW_CLOCK,
                                   R"({"io_space":"cpu0.io","clock_hz_hint":3686400})", "io_space",
                                   nullptr};
using State = srz80::sdk::state::Callbacks<save_payload, load_payload, 1>;
const SrhPlugin api{SRH_INIT(SrhPlugin), "z80", create, destroy, reset,
                    property_count, property_info, property_get, property_set,
                    State::save, State::load, &descriptor};
} // namespace
extern "C" SRH_EXPORT const SrhPlugin *SRH_CALL srz80_plugin_init(const ShouryoHost *host) {
    return srz80::sdk::valid(host) ? &api : nullptr;
}
