#include <state.hpp>
#include <boundary.hpp>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <srz80/abi.h>

// SRC32-ALMSI CPU card. Mirrors the reference implementation in
// s3w2_core/../src32 (src/cpu.rs) and its specification (doc/spec_CPU.md).
//
// Normal mode executes 32-bit instructions, Extension S adds the 16-bit short
// mode, Extension I adds the external interrupt input: a rising edge on the
// engine's "IRQ" level signal latches one request, which the CPU samples after
// the next committed instruction. Interrupt entry saves EPC, records CAUSE,
// clears the enable bit and vectors to 0xFFFF0100 + CAUSE * 4 (see
// doc/spec.md, section 6.1); IRET restores the pre-interrupt state.
//
// A guest-caused failure (illegal instruction, bus error) stops this CPU and
// records the reason instead of failing the clock callback: a card must never
// abort the host's simulation run.

namespace {
constexpr uint32_t kCpuId = 0x53524332u; // "SRC2"
constexpr uint32_t kFeatures = 0x3Fu;    // base, A, L, M, S and I extensions
constexpr uint32_t kIrqVectorBase = 0xFFFF0100u;
constexpr uint32_t kNormalSize = 4;
constexpr uint32_t kShortSize = 2;
constexpr uint32_t kCyclesPerInstruction = 3;
constexpr uint32_t kMessageSize = 128;
constexpr uint32_t kSavedStateSize = 32 * 4 + 4 + 1 + 1 + 8 + 4 + 4 + 1 + 1 + 4;

enum class Mode : uint8_t { Normal, Short };

struct Cpu {
    const ShouryoHost *host{};
    SrhHandle owner{}, space{}, irq_signal{};
    uint32_t pc{}, reset{};
    uint32_t r[32]{};
    uint32_t epc{}, cause{}, irq_number{};
    bool irq_enable{true}, irq_line{false}, irq_pending{false};
    bool running{true}, faulted{false};
    Mode mode{Mode::Normal};
    uint64_t cycles{};
    char fault[kMessageSize]{};

    uint32_t reg(uint32_t index) const { return index == 0 ? 0 : r[index & 31]; }
    void set_reg(uint32_t index, uint32_t value) {
        if (index != 0)
            r[index & 31] = value;
    }

    void halt(const char *reason) {
        std::snprintf(fault, sizeof(fault), "%s", reason);
        faulted = true;
        running = false;
        if (host->log) {
            char message[kMessageSize + 8];
            std::snprintf(message, sizeof(message), "SRC32: %s", fault);
            host->log(host->context, owner, message);
        }
    }
    void halt_illegal(uint32_t raw, uint32_t size) {
        char reason[kMessageSize];
        std::snprintf(reason, sizeof(reason), "illegal instruction 0x%0*X at PC 0x%08X",
                      size == kNormalSize ? 8 : 4, raw, pc - size);
        halt(reason);
    }
    void halt_bus(uint32_t address, bool write, SrhStatus status) {
        char reason[kMessageSize];
        std::snprintf(reason, sizeof(reason), "bus %s error at 0x%08X (status %u)",
                      write ? "write" : "read", address, uint32_t(status));
        halt(reason);
    }

    bool read8(uint32_t address, uint8_t &value) {
        const SrhStatus status = host->read(host->context, owner, space, address, &value);
        if (status == SRH_OK)
            return true;
        halt_bus(address, false, status);
        value = 0;
        return false;
    }
    bool write8(uint32_t address, uint8_t value) {
        const SrhStatus status = host->write(host->context, owner, space, address, value);
        if (status == SRH_OK)
            return true;
        halt_bus(address, true, status);
        return false;
    }
    bool read16(uint32_t address, uint16_t &value) {
        uint8_t high = 0, low = 0;
        if (!read8(address, high) || !read8(address + 1, low))
            return false;
        value = uint16_t((uint16_t(high) << 8) | low);
        return true;
    }
    bool read32(uint32_t address, uint32_t &value) {
        uint8_t bytes[kNormalSize]{};
        for (uint32_t i = 0; i < kNormalSize; ++i)
            if (!read8(address + i, bytes[i]))
                return false;
        value = (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) |
                (uint32_t(bytes[2]) << 8) | bytes[3];
        return true;
    }
    bool write32(uint32_t address, uint32_t value) {
        for (uint32_t i = 0; i < kNormalSize; ++i)
            if (!write8(address + i, uint8_t(value >> (24 - 8 * i))))
                return false;
        return true;
    }

    static uint32_t add(uint32_t base, int16_t offset) { return base + uint32_t(int32_t(offset)); }
    static uint32_t branch(uint32_t next, int16_t offset) {
        return next + uint32_t(int32_t(offset));
    }

    void reset_cpu() {
        std::memset(r, 0, sizeof(r));
        pc = reset;
        epc = 0;
        cause = 0;
        irq_number = 0;
        irq_enable = true;
        irq_pending = false;
        running = true;
        faulted = false;
        fault[0] = '\0';
        mode = Mode::Normal;
        cycles = 0;
    }

    void set_irq_level(bool asserted) {
        if (asserted && !irq_line)
            irq_pending = true; // a rising edge latches exactly one request
        irq_line = asserted;
    }

    // Interrupt entry, sampled only after a committed instruction.
    void service_interrupt() {
        if (!running || !irq_enable || !irq_pending)
            return;
        epc = pc;
        cause = irq_number & 0xF;
        irq_pending = false;
        irq_enable = false;
        mode = Mode::Normal;
        pc = kIrqVectorBase + cause * 4;
    }

    void execute_normal() {
        uint32_t raw = 0;
        if (!read32(pc, raw))
            return;
        const uint32_t next = pc + kNormalSize;
        pc = next;
        const uint32_t op = raw >> 26;
        const uint32_t rd = (raw >> 21) & 31;
        const uint32_t rs1 = (raw >> 16) & 31;
        const uint32_t rs2 = (raw >> 11) & 31;
        const int16_t offset = int16_t(raw & 0xFFFF);
        const uint32_t unsigned_immediate = uint32_t(raw & 0xFFFF);
        const auto write = [&](uint32_t value) { set_reg(rd, value); };
        switch (op) {
        case 0x00: // NOP
            break;
        case 0x01: { // LD rd, [rs1 + off16]
            uint32_t value = 0;
            if (!read32(add(reg(rs1), offset), value))
                return;
            write(value);
            break;
        }
        case 0x02: // ST [rs1 + off16], rd
            if (!write32(add(reg(rs1), offset), reg(rd)))
                return;
            break;
        case 0x03: write(reg(rs1) + reg(rs2)); break;              // ADD
        case 0x04: write(reg(rs1) + uint32_t(int32_t(offset))); break; // ADDI
        case 0x05: write(reg(rs1) - reg(rs2)); break;              // SUB
        case 0x06: write(uint32_t(int32_t(reg(rs1)) < int32_t(reg(rs2)))); break; // SLT
        case 0x07: // BEQ
            if (reg(rs1) == reg(rd))
                pc = branch(next, offset);
            break;
        case 0x08: // BNE
            if (reg(rs1) != reg(rd))
                pc = branch(next, offset);
            break;
        case 0x09: pc = branch(next, offset); break;               // JMP
        case 0x0A: set_reg(31, next); pc = branch(next, offset); break; // JAL
        case 0x0B: pc = reg(rd); break;                            // JR
        case 0x0C: write(reg(rs1) & reg(rs2)); break;              // AND
        case 0x0D: write(reg(rs1) | reg(rs2)); break;              // OR
        case 0x0E: write(reg(rs1) ^ reg(rs2)); break;              // XOR
        case 0x0F: write(reg(rs1) << (reg(rs2) & 31)); break;      // SLL
        case 0x10: write(reg(rs1) >> (reg(rs2) & 31)); break;      // SRL
        case 0x11: write(reg(rs1) << (reg(rs2) & 31)); break;      // SLA
        case 0x12: write(uint32_t(int32_t(reg(rs1)) >> (reg(rs2) & 31))); break; // SRA
        case 0x13: { // LDB
            uint8_t value = 0;
            if (!read8(add(reg(rs1), offset), value))
                return;
            write(value);
            break;
        }
        case 0x14: { // LDH
            uint16_t value = 0;
            if (!read16(add(reg(rs1), offset), value))
                return;
            write(value);
            break;
        }
        case 0x15: // STB
            write8(add(reg(rs1), offset), uint8_t(reg(rd)));
            break;
        case 0x16: { // STH
            const uint32_t value = reg(rd);
            const uint32_t address = add(reg(rs1), offset);
            if (!write8(address, uint8_t(value >> 8)))
                return;
            write8(address + 1, uint8_t(value));
            break;
        }
        case 0x17: write(uint32_t(reg(rs1) < reg(rs2))); break;    // SLTU
        case 0x18: write(uint32_t(uint64_t(reg(rs1)) * reg(rs2))); break; // MUL
        case 0x19: {                                               // DIV
            const int32_t lhs = int32_t(reg(rs1)), rhs = int32_t(reg(rs2));
            write(rhs ? uint32_t((lhs == INT32_MIN && rhs == -1) ? INT32_MIN : lhs / rhs) : 0);
            break;
        }
        case 0x1A: {                                               // MOD
            const int32_t lhs = int32_t(reg(rs1)), rhs = int32_t(reg(rs2));
            write(rhs ? uint32_t((lhs == INT32_MIN && rhs == -1) ? 0 : lhs % rhs) : 0);
            break;
        }
        case 0x1B: // MULH
            write(uint32_t((int64_t(int32_t(reg(rs1))) * int64_t(int32_t(reg(rs2)))) >> 32));
            break;
        case 0x1C: write(reg(rs2) ? reg(rs1) / reg(rs2) : 0); break; // DIVU
        case 0x1D: pc = branch(next, offset); mode = Mode::Short; break; // JMPS
        case 0x1E: set_reg(31, next); pc = branch(next, offset); mode = Mode::Short; break; // JALS
        case 0x1F: pc = reg(rd); mode = Mode::Short; break;        // JRS
        case 0x20: // IRET
            pc = epc;
            mode = Mode::Normal;
            irq_enable = true;
            break;
        case 0x21: set_reg(31, next); pc = reg(rd); break;         // JALR
        case 0x22: {                                               // JALRS
            const uint32_t target = reg(rd);
            set_reg(31, next);
            pc = target;
            mode = Mode::Short;
            break;
        }
        case 0x3C: write((reg(rd) & 0xFFFF0000u) | unsigned_immediate); break; // LDIL
        case 0x3D: write((reg(rd) & 0x0000FFFFu) | (unsigned_immediate << 16)); break; // LDIH
        case 0x3E: // CPUID
            set_reg(1, kCpuId);
            set_reg(2, kFeatures);
            break;
        case 0x3F: running = false; break;                         // HALT
        default: halt_illegal(raw, kNormalSize); break;
        }
    }

    void execute_short() {
        uint16_t raw = 0;
        if (!read16(pc, raw))
            return;
        const uint32_t next = pc + kShortSize;
        pc = next;
        const uint32_t op = raw >> 12;
        const uint32_t rd = (raw >> 8) & 15;
        const uint32_t rs1 = (raw >> 4) & 15;
        const uint32_t rs2 = raw & 15;
        const auto read = [&](uint32_t index) { return reg(index < 15 ? index : 31); };
        const auto write = [&](uint32_t index, uint32_t value) {
            set_reg(index < 15 ? index : 31, value);
        };
        const int8_t offset8 = int8_t(raw & 0xFF);
        int16_t offset12 = int16_t(raw & 0x0FFF);
        if (offset12 & 0x0800)
            offset12 = int16_t(offset12 | int16_t(0xF000));
        switch (op) {
        case 0x0: write(rd, read(rs1)); break;                       // S.MOV
        case 0x1: write(rd, read(rs1) + read(rs2)); break;           // S.ADD
        case 0x2: write(rd, read(rd) + uint32_t(int32_t(offset8))); break; // S.ADDI
        case 0x3: {                                                  // S.LD
            uint32_t value = 0;
            if (!read32(read(rs1), value))
                return;
            write(rd, value);
            break;
        }
        case 0x4: // S.ST
            if (!write32(read(rd), read(rs1)))
                return;
            break;
        case 0x5: // S.BZ
            if (read(rd) == 0)
                pc = branch(next, offset8);
            break;
        case 0x6: // S.BNZ
            if (read(rd) != 0)
                pc = branch(next, offset8);
            break;
        case 0x7: pc = read(rd); break;                              // S.JR
        case 0x8: set_reg(31, next); pc = branch(next, offset12); break; // S.JAL
        case 0x9: write(rd, uint32_t(raw & 0xFF)); break;            // S.LDI
        case 0xF: mode = Mode::Normal; break;                        // S.RET
        default: halt_illegal(raw, kShortSize); break;
        }
    }

    void tick() {
        if (!running)
            return;
        const SrhStatus boundary = host->boundary(host->context, owner, space, pc);
        if (boundary == SRH_STOP)
            return; // a host breakpoint holds this instruction
        if (boundary != SRH_OK) {
            halt("host boundary check failed");
            return;
        }
        if (mode == Mode::Normal)
            execute_normal();
        else
            execute_short();
        if (faulted || !running)
            return;
        cycles += kCyclesPerInstruction;
        service_interrupt();
    }
};

SrhStatus SRH_CALL tick_callback(void *context) {
    return srz80::sdk::guard([&] {
        static_cast<Cpu *>(context)->tick();
        return SRH_OK;
    });
}

SrhStatus SRH_CALL irq_callback(void *context, int32_t level) {
    static_cast<Cpu *>(context)->set_irq_level(level != 0);
    return SRH_OK;
}

void set_error_message(const SrhConfig *config, const char *message) {
    if (!config || !message || !srz80::sdk::has_field(config, &SrhConfig::error_message) ||
        !config->error_message || config->error_message_capacity == 0)
        return;
    const uint32_t limit = config->error_message_capacity - 1;
    uint32_t count = static_cast<uint32_t>(std::strlen(message));
    if (count > limit)
        count = limit;
    std::memcpy(config->error_message, message, count);
    config->error_message[count] = '\0';
}

SrhStatus SRH_CALL create(const ShouryoHost *host, SrhHandle owner, const SrhConfig *config,
                          void **out) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!out)
            return SRH_INVALID;
        *out = nullptr;
        if (!srz80::sdk::valid(host) || !srz80::sdk::valid(config))
            return SRH_INVALID;
        if (!host->read || !host->write || !host->boundary || !host->subscribe_clock) {
            set_error_message(config, "host bus and clock services are unavailable");
            return SRH_UNAVAILABLE;
        }
        if (config->clock >= 3) {
            set_error_message(config, "clock index must be 0, 1, or 2");
            return SRH_INVALID;
        }
        auto cpu = std::make_unique<Cpu>();
        cpu->host = host;
        cpu->owner = owner;
        cpu->space = config->space;
        cpu->reset = uint32_t(config->reset_vector);
        cpu->reset_cpu();
        // The interrupt input is optional: projects without an IRQ signal still
        // run the CPU, they just cannot raise interrupts.
        if (host->signal_find && host->signal_subscribe && host->signal_read) {
            SrhHandle signal = 0;
            if (host->signal_find(host->context, "IRQ", &signal) == SRH_OK && signal) {
                SrhHandle subscription = 0;
                const auto status = host->signal_subscribe(host->context, owner, signal,
                                                           irq_callback, cpu.get(), &subscription);
                if (status != SRH_OK)
                    return status;
                cpu->irq_signal = signal;
                int32_t level = 0;
                if (host->signal_read(host->context, signal, &level) == SRH_OK)
                    cpu->set_irq_level(level != 0);
            }
        }
        SrhHandle clock_subscription = 0;
        const auto status = host->subscribe_clock(host->context, owner, config->clock,
                                                  tick_callback, cpu.get(), &clock_subscription);
        if (status != SRH_OK)
            return status;
        *out = cpu.release();
        return SRH_OK;
    });
}

void SRH_CALL destroy(void *context) { delete static_cast<Cpu *>(context); }

SrhStatus SRH_CALL reset(void *context, uint32_t) {
    return srz80::sdk::guard([&] {
        static_cast<Cpu *>(context)->reset_cpu();
        return SRH_OK;
    });
}

uint32_t SRH_CALL count(void *) { return 40; }

SrhStatus SRH_CALL info(void *, uint32_t index, SrhProperty *out) {
    if (!srz80::sdk::valid(out) || index >= count(nullptr))
        return SRH_INVALID;
    if (index == 0) {
        *out = {SRH_INIT(SrhProperty), "PC", "Control", "Program counter", SRH_UNSIGNED,
                32, 16, 1, nullptr, 0};
    } else if (index <= 32) {
        static const char *names[] = {"R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7",
                                      "R8", "R9", "R10", "R11", "R12", "R13", "R14", "R15",
                                      "R16", "R17", "R18", "R19", "R20", "R21", "R22", "R23",
                                      "R24", "R25", "R26", "R27", "R28", "R29", "R30", "R31"};
        *out = {SRH_INIT(SrhProperty), names[index - 1], "Registers", "General-purpose register",
                SRH_UNSIGNED, 32, 16, index != 1, nullptr, 0};
    } else if (index == 33) {
        *out = {SRH_INIT(SrhProperty), "MODE", "Control", "Instruction encoding mode", SRH_ENUM,
                1, 10, 0, "Normal|Short", 0};
    } else if (index == 34) {
        *out = {SRH_INIT(SrhProperty), "RUNNING", "Control", "Whether the CPU is running",
                SRH_BOOLEAN, 1, 10, 0, nullptr, 0};
    } else if (index == 35) {
        *out = {SRH_INIT(SrhProperty), "CYCLES", "Control", "Cycles executed", SRH_UNSIGNED,
                64, 10, 0, nullptr, 0};
    } else if (index == 36) {
        *out = {SRH_INIT(SrhProperty), "EPC", "Interrupts",
                "PC saved when the last interrupt was accepted", SRH_UNSIGNED, 32, 16, 0,
                nullptr, 0};
    } else if (index == 37) {
        *out = {SRH_INIT(SrhProperty), "CAUSE", "Interrupts", "Interrupt source number",
                SRH_UNSIGNED, 4, 10, 0, nullptr, 0};
    } else if (index == 38) {
        *out = {SRH_INIT(SrhProperty), "IRQ_ENABLE", "Interrupts", "Whether interrupts are enabled",
                SRH_BOOLEAN, 1, 10, 0, nullptr, 0};
    } else {
        *out = {SRH_INIT(SrhProperty), "FAULT", "Control", "Why this CPU stopped, empty while running",
                SRH_TEXT, 0, 0, 0, nullptr, 0};
    }
    return SRH_OK;
}

SrhStatus SRH_CALL get(void *context, uint32_t index, SrhValue *out) {
    if (!srz80::sdk::valid(out) || index >= count(nullptr))
        return SRH_INVALID;
    const auto &cpu = *static_cast<Cpu *>(context);
    if (index == 0)
        out->unsigned_value = cpu.pc;
    else if (index <= 32)
        out->unsigned_value = cpu.reg(index - 1);
    else if (index == 33)
        out->unsigned_value = static_cast<uint32_t>(cpu.mode);
    else if (index == 34)
        out->unsigned_value = cpu.running;
    else if (index == 35)
        out->unsigned_value = cpu.cycles;
    else if (index == 36)
        out->unsigned_value = cpu.epc;
    else if (index == 37)
        out->unsigned_value = cpu.cause;
    else if (index == 38)
        out->unsigned_value = cpu.irq_enable;
    else
        std::snprintf(out->text, sizeof(out->text), "%s", cpu.fault);
    return SRH_OK;
}

SrhStatus SRH_CALL set(void *context, uint32_t index, const SrhValue *in) {
    if (!srz80::sdk::valid(in) || index >= count(nullptr) || index >= 33 ||
        in->unsigned_value > UINT32_MAX)
        return SRH_INVALID;
    auto &cpu = *static_cast<Cpu *>(context);
    if (index == 0)
        cpu.pc = static_cast<uint32_t>(in->unsigned_value);
    else
        cpu.set_reg(index - 1, static_cast<uint32_t>(in->unsigned_value));
    return SRH_OK;
}

SrhStatus SRH_CALL save_payload(void *context, uint8_t *buffer, uint64_t *size) {
    if (!size)
        return SRH_INVALID;
    if (!buffer) {
        *size = kSavedStateSize;
        return SRH_OK;
    }
    if (*size < kSavedStateSize) {
        *size = kSavedStateSize;
        return SRH_UNAVAILABLE;
    }
    auto &cpu = *static_cast<Cpu *>(context);
    srz80::sdk::state::Writer writer;
    writer.fields(cpu.r, cpu.pc, cpu.running, uint8_t(cpu.mode), cpu.cycles,
                  cpu.epc, cpu.cause, cpu.irq_enable, cpu.irq_pending, cpu.irq_number);
    return srz80::sdk::state::copy_payload(writer.bytes, buffer, size);
}

SrhStatus SRH_CALL load_payload(void *context, const uint8_t *buffer, uint64_t size) {
    if (!buffer || size != kSavedStateSize)
        return SRH_INVALID;
    auto &cpu = *static_cast<Cpu *>(context);
    auto staged = cpu;
    uint8_t mode = 0;
    srz80::sdk::state::Reader reader({buffer, static_cast<size_t>(size)});
    reader.fields(staged.r, staged.pc, staged.running, mode, staged.cycles,
                  staged.epc, staged.cause, staged.irq_enable, staged.irq_pending, staged.irq_number);
    if (!reader.finished() || mode > 1 || staged.r[0] != 0) return SRH_INVALID;
    staged.mode = Mode(mode);
    staged.faulted = false;
    staged.fault[0] = '\0';
    cpu = staged;
    return SRH_OK;
}

const SrhCardDescriptor descriptor{SRH_INIT(SrhCardDescriptor), "CPU", "SRC32-ALMSI", "SRC32 CPU",
                                   0, 0, 0, 0, 0, SRH_CARD_SHOW_CLOCK, "{}", nullptr, nullptr,
                                   nullptr, 0};
using State = srz80::sdk::state::Callbacks<save_payload, load_payload, 1>;
const SrhPlugin api{SRH_INIT(SrhPlugin), "src32", create, destroy, reset, count, info, get, set,
                    State::save, State::load, &descriptor, nullptr, nullptr};
} // namespace

extern "C" SRH_EXPORT const SrhPlugin *SRH_CALL srz80_plugin_init(const ShouryoHost *host) {
    return srz80::sdk::valid(host) ? &api : nullptr;
}
