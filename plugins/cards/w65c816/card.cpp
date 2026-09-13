#include <boundary.hpp>

#include "g65816/g65816.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {
enum TraceKind : uint32_t { TRACE_OPCODE = 1, TRACE_DATA_READ = 3, TRACE_DATA_WRITE = 4 };
machine_config core_machine_config;

class Core final : public g65816_device {
public:
    Core() : g65816_device(core_machine_config, "w65c816", nullptr, 0) {
        device_start();
    }
    void reset() { device_reset(); }
    int step() {
        m_ICount = 1;
        execute_run();
        return 1 - m_ICount;
    }
    void set_line(int line, bool high) { execute_set_input(line, high ? 1 : 0); }
    uint32_t pc() { return (g65816_get_reg(G65816_PB) << 16) | g65816_get_reg(G65816_PC); }
    uint32_t reg(int id) { return g65816_get_reg(id); }
    void reg(int id, uint32_t value) { g65816_set_reg(id, value); }
    SrhStatus disassemble(uint64_t pc, const uint8_t *bytes, uint32_t available,
                          uint32_t *length, uint32_t *cycles, char *text, uint32_t text_size) {
        if (!bytes || !available || !length || !cycles || !text || !text_size || pc > 0xffffff)
            return SRH_UNAVAILABLE;
        std::vector<uint8_t> input(bytes, bytes + available);
        util::disasm_interface::data_buffer buffer(input, static_cast<offs_t>(pc));
        g65816_disassembler disassembler(this);
        std::ostringstream out;
        const auto result = disassembler.disassemble(out, static_cast<offs_t>(pc), buffer, buffer);
        *length = result & 0xffff;
        if (!*length || *length > available)
            return SRH_UNAVAILABLE;
        *cycles = disassembler.instruction_cycles(static_cast<offs_t>(pc), buffer);
        std::snprintf(text, text_size, "%s", out.str().c_str());
        return SRH_OK;
    }
};

struct Cpu {
    const ShouryoHost *host = nullptr;
    const SrhHostDebugV1 *debug = nullptr;
    SrhHandle owner = 0, memory_space = 0;
    uint32_t clock = 0;
    Core core;
    int cycles_remaining = 0;
    SrhStatus bus_error = SRH_OK;
    bool trace_enabled = true;
    bool irq_high = false, nmi_high = false;

    uint8_t read(uint32_t address) {
        uint8_t value = 0xff;
        const auto status = host->read(host->context, owner, memory_space, address, &value);
        if (status != SRH_OK)
            bus_error = status;
        return value;
    }
    void write(uint32_t address, uint8_t value) {
        if (trace_enabled && debug)
            debug->set_trace_kind(debug->context, owner, TRACE_DATA_WRITE);
        const auto status = host->write(host->context, owner, memory_space, address, value);
        if (status != SRH_OK)
            bus_error = status;
    }
    SrhStatus tick() {
        if (srz80::sdk::has_field(debug, &SrhHostDebugV1::trace_enabled))
            trace_enabled = debug->trace_enabled(debug->context) != 0;
        if (cycles_remaining > 0) {
            --cycles_remaining;
            return SRH_OK;
        }
        const auto pc = core.pc();
        const bool boundary_required = !srz80::sdk::has_field(debug, &SrhHostDebugV1::boundary_required) ||
                                       debug->boundary_required(debug->context) != 0;
        if (boundary_required) {
            const auto status = debug->boundary_ex(debug->context, owner, memory_space, pc, 0, 0);
            if (status == SRH_STOP)
                return SRH_OK;
            if (status != SRH_OK)
                return status;
        }
        if (trace_enabled)
            debug->set_trace_kind(debug->context, owner, TRACE_OPCODE);
        bus_error = SRH_OK;
        const int cycles = core.step();
        cycles_remaining = cycles > 1 ? cycles - 1 : 0;
        if (bus_error != SRH_OK) {
            debug->request_stop(debug->context, owner, "W65C816 bus error");
            return bus_error;
        }
        return SRH_OK;
    }
    void reset() {
        cycles_remaining = 0;
        bus_error = SRH_OK;
        irq_high = nmi_high = false;
        core.reset();
    }
};

SrhStatus SRH_CALL tick_callback(void *context) {
    return srz80::sdk::guard([&] { return static_cast<Cpu *>(context)->tick(); });
}
SrhStatus SRH_CALL irq_callback(void *context, int32_t millivolts) {
    auto &cpu = *static_cast<Cpu *>(context);
    cpu.irq_high = millivolts > 700;
    cpu.core.set_line(G65816_LINE_IRQ, cpu.irq_high);
    return SRH_OK;
}
SrhStatus SRH_CALL nmi_callback(void *context, int32_t millivolts) {
    auto &cpu = *static_cast<Cpu *>(context);
    const bool high = millivolts > 700;
    cpu.core.set_line(G65816_LINE_NMI, high);
    cpu.nmi_high = high;
    return SRH_OK;
}
SrhStatus SRH_CALL disasm(void *context, SrhHandle, SrhHandle, uint64_t pc, const uint8_t *bytes,
                          uint32_t available, uint32_t *length, uint32_t *cycles,
                          char *text, uint32_t text_size) {
    if (!cycles)
        return SRH_INVALID;
    return static_cast<Cpu *>(context)->core.disassemble(pc, bytes, available, length, cycles,
                                                         text, text_size);
}

SrhStatus SRH_CALL create(const ShouryoHost *host, SrhHandle owner, const SrhConfig *config, void **result) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!srz80::sdk::valid(host) || !srz80::sdk::valid(config) || !result || !config->space || config->clock >= 3)
            return SRH_INVALID;
        const void *extension = nullptr;
        if (!host->query || host->query(host->context, "host.debug.v1", &extension) != SRH_OK || !extension)
            return SRH_UNAVAILABLE;
        const auto *debug = static_cast<const SrhHostDebugV1 *>(extension);
        if (!srz80::sdk::valid(debug))
            return SRH_UNAVAILABLE;
        auto cpu = std::make_unique<Cpu>();
        cpu->host = host;
        cpu->debug = debug;
        cpu->owner = owner;
        cpu->memory_space = config->space;
        cpu->clock = config->clock;
        cpu->core.set_bus([raw = cpu.get()](uint32_t address) { return raw->read(address); },
                          [raw = cpu.get()](uint32_t address, uint8_t value) { raw->write(address, value); });
        auto status = debug->register_disasm(debug->context, owner, disasm, cpu.get());
        if (status != SRH_OK)
            return status;
        SrhHandle subscription = 0;
        status = host->subscribe_clock(host->context, owner, cpu->clock, tick_callback, cpu.get(), &subscription);
        if (status != SRH_OK)
            return status;
        const std::array<std::pair<const char *, SrhSignalCallback>, 2> signals{{
            {"IRQ", irq_callback}, {"NMI", nmi_callback}}};
        for (const auto &[name, callback] : signals) {
            SrhHandle signal = 0;
            if (host->signal_find(host->context, name, &signal) == SRH_OK && signal) {
                status = host->signal_subscribe(host->context, owner, signal, callback, cpu.get(), &subscription);
                if (status != SRH_OK)
                    return status;
            }
        }
        cpu->reset();
        *result = cpu.release();
        return SRH_OK;
    });
}
void SRH_CALL destroy(void *context) { delete static_cast<Cpu *>(context); }
SrhStatus SRH_CALL reset(void *context, uint32_t) {
    return srz80::sdk::guard([&] { static_cast<Cpu *>(context)->reset(); return SRH_OK; });
}

constexpr uint32_t kPropertyCount = 10;
SrhStatus SRH_CALL property_info(void *, uint32_t index, SrhProperty *out) {
    if (!srz80::sdk::valid(out) || index >= kPropertyCount)
        return SRH_INVALID;
    struct Info { const char *name, *group, *description; uint32_t bits, editable; };
    static const Info infos[kPropertyCount] = {
        {"model", "Control", "Imported MAME G65816 instruction core", 0, 0},
        {"PC", "Main", "24-bit program-bank/program-counter", 24, 1},
        {"A", "Main", "Accumulator", 16, 1}, {"X", "Main", "Index X", 16, 1},
        {"Y", "Main", "Index Y", 16, 1}, {"S", "Main", "Stack pointer", 16, 1},
        {"D", "Main", "Direct-page register", 16, 1}, {"P", "Control", "Processor status", 8, 1},
        {"PB", "Control", "Program bank", 8, 1}, {"DB", "Control", "Data bank", 8, 1}};
    const auto &info = infos[index];
    *out = {SRH_INIT(SrhProperty), info.name, info.group, info.description,
            index == 0 ? SRH_TEXT : SRH_UNSIGNED, info.bits, 16, info.editable, nullptr, 0};
    return SRH_OK;
}
uint32_t SRH_CALL property_count(void *) { return kPropertyCount; }
SrhStatus SRH_CALL property_get(void *context, uint32_t index, SrhValue *out) {
    if (!srz80::sdk::valid(out) || index >= kPropertyCount)
        return SRH_INVALID;
    auto &core = static_cast<Cpu *>(context)->core;
    if (index == 0) { std::snprintf(out->text, sizeof(out->text), "MAME G65816"); return SRH_OK; }
    static const int registers[] = {g65816_device::G65816_PC, g65816_device::G65816_A, g65816_device::G65816_X,
        g65816_device::G65816_Y, g65816_device::G65816_S, g65816_device::G65816_D, g65816_device::G65816_P,
        g65816_device::G65816_PB, g65816_device::G65816_DB};
    out->unsigned_value = core.reg(registers[index - 1]);
    return SRH_OK;
}
SrhStatus SRH_CALL property_set(void *context, uint32_t index, const SrhValue *in) {
    if (!srz80::sdk::valid(in) || index == 0 || index >= kPropertyCount)
        return SRH_INVALID;
    static const int registers[] = {g65816_device::G65816_PC, g65816_device::G65816_A, g65816_device::G65816_X,
        g65816_device::G65816_Y, g65816_device::G65816_S, g65816_device::G65816_D, g65816_device::G65816_P,
        g65816_device::G65816_PB, g65816_device::G65816_DB};
    static const uint32_t masks[] = {0xffffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xff, 0xff, 0xff};
    if (in->unsigned_value > masks[index - 1])
        return SRH_INVALID;
    auto &cpu = *static_cast<Cpu *>(context);
    if (index == 1) {
        cpu.core.reg(g65816_device::G65816_PB, static_cast<uint32_t>(in->unsigned_value >> 16));
        cpu.core.reg(g65816_device::G65816_PC, static_cast<uint32_t>(in->unsigned_value));
    } else {
        cpu.core.reg(registers[index - 1], static_cast<uint32_t>(in->unsigned_value));
    }
    cpu.cycles_remaining = 0;
    return SRH_OK;
}

const SrhCardDescriptor descriptor{SRH_INIT(SrhCardDescriptor), "CPU", "W65C816",
    "WDC W65C816 processor", 0, 0, 0, 0, 0, SRH_CARD_SHOW_CLOCK,
    R"({"clock_hz_hint":1000000})", nullptr, nullptr, nullptr, 0};
const SrhPlugin api{SRH_INIT(SrhPlugin), "w65c816", create, destroy, reset, property_count,
                    property_info, property_get, property_set, nullptr, nullptr, &descriptor,
                    nullptr, nullptr};
} // namespace

extern "C" SRH_EXPORT const SrhPlugin *SRH_CALL srz80_plugin_init(const ShouryoHost *host) {
    return srz80::sdk::valid(host) ? &api : nullptr;
}
