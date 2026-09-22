#include <state.hpp>
#include <boundary.hpp>
#include <capstone/capstone.h>

extern "C" {
#include "common.h"
#include "riscv.h"
}

#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <type_traits>

namespace {
enum TraceKind : uint32_t {
    TRACE_KIND_OPCODE = 1,
    TRACE_KIND_DATA_READ = 3,
    TRACE_KIND_DATA_WRITE = 4,
};

enum class ExecutionEngine { cached, reference, jit };
#if RV32_HAS(JIT)
constexpr const char *execution_modes = "Cached|Reference|JIT";
#else
constexpr const char *execution_modes = "Cached|Reference";
#endif

ExecutionEngine configured_execution_engine(const ShouryoHost *host) {
    const void *extension = nullptr;
    if (!host || !host->query ||
        host->query(host->context, "host.config.v1", &extension) != SRH_OK || !extension)
        return ExecutionEngine::cached;
    const auto *config = static_cast<const SrhHostConfigV1 *>(extension);
    if (!srz80::sdk::valid(config) || !config->get_value)
        return ExecutionEngine::cached;
    char value[32]{};
    if (config->get_value(config->context, "cards.riscv.execution_engine", value,
                          static_cast<uint32_t>(sizeof(value))) != SRH_OK)
        return ExecutionEngine::cached;
#if RV32_HAS(JIT)
    if (std::strcmp(value, "JIT") == 0) return ExecutionEngine::jit;
#endif
    return std::strcmp(value, "Reference") == 0 ? ExecutionEngine::reference
                                                 : ExecutionEngine::cached;
}

struct Cpu {
    const ShouryoHost *host = nullptr;
    const SrhHostConfigV1 *host_config = nullptr;
    const SrhHostDebugV1 *debug = nullptr;
    const SrhHostMemoryV1 *memory = nullptr;
    SrhHandle owner = 0;
    SrhHandle memory_space = 0;
    SrhHandle irq_signal = 0;
    SrhHandle irq_subscription = 0;
    uint32_t clock = 0;
    uint32_t reset_vector = 0;
    riscv_t *rv = nullptr;
    csh capstone = 0;
    SrhStatus bus_error = SRH_OK;
    bool halted = false;
    bool trace_enabled = true;
    ExecutionEngine execution_engine = ExecutionEngine::cached;
    uint64_t instructions = 0;
    uint32_t ticks_until_batch = 0;
    uint32_t jit_batch_instructions = 1024;
    bool irq_asserted = false;

    ~Cpu() {
        if (capstone)
            cs_close(&capstone);
        if (rv)
            rv_delete(rv);
    }

    SrhStatus set_execution_engine(ExecutionEngine next) {
        const bool policy_supported = srz80::sdk::has_field(debug, &SrhHostDebugV1::set_disassembly_enabled) && debug->set_disassembly_enabled;
        if (next == ExecutionEngine::jit) {
            if (!policy_supported) return SRH_UNAVAILABLE;
#if RV32_HAS(JIT)
            // Prepare executable storage without advancing the CPU. Failure
            // leaves the previous mode and its disassembler usable.
            if (!rv_step_bare_jit(rv, 0)) return SRH_ERROR;
#else
            return SRH_UNAVAILABLE;
#endif
        } else if (!capstone && cs_open(CS_ARCH_RISCV, CS_MODE_RISCV32, &capstone) != CS_ERR_OK) {
            return SRH_ERROR;
        }
        if (policy_supported) {
            const auto status = debug->set_disassembly_enabled(debug->context, owner,
                next != ExecutionEngine::jit,
                next == ExecutionEngine::jit ? "Live disassembly is disabled in JIT throughput mode. Select Cached or Reference to debug instructions." : nullptr);
            if (status != SRH_OK) return status;
        }
        if (execution_engine != next) instructions = 0;
        execution_engine = next;
        if (next == ExecutionEngine::jit) {
            trace_enabled = false;
            if (capstone) cs_close(&capstone);
        } else {
#if RV32_HAS(JIT)
            rv_bare_jit_destroy(rv);
#endif
        }
        return SRH_OK;
    }

    void trace(uint32_t kind) const {
        if (trace_enabled && debug)
            debug->set_trace_kind(debug->context, owner, kind);
    }
    uint8_t read_byte(uint32_t address, uint32_t kind) {
        trace(kind);
        uint8_t value = 0;
        auto status = host->read(host->context, owner, memory_space, address, &value);
        if (status != SRH_OK) {
            bus_error = status;
            if (execution_engine == ExecutionEngine::jit) stop("RISC-V bus error");
        }
        return value;
    }
    void write_byte(uint32_t address, uint8_t value) {
        trace(TRACE_KIND_DATA_WRITE);
        auto status = host->write(host->context, owner, memory_space, address, value);
        if (status != SRH_OK) {
            bus_error = status;
            if (execution_engine == ExecutionEngine::jit) stop("RISC-V bus error");
        }
    }
    uint16_t read_half(uint32_t address, uint32_t kind) {
        uint16_t value = read_byte(address, kind);
        value |= static_cast<uint16_t>(read_byte(address + 1, kind)) << 8;
        return value;
    }
    uint32_t read_word(uint32_t address, uint32_t kind) {
        // The host admits this path only for a single contiguous mapping with
        // tracing and watchpoints disabled. Never cache RAM/ROM or bus routes
        // here: live card insertion and self-modifying code remain visible.
        if (!trace_enabled && memory && memory->read_word) {
            uint32_t value = 0;
            const auto status = memory->read_word(memory->context, owner, memory_space,
                                                   address, &value);
            if (status == SRH_OK)
                return value;
            if (status != SRH_UNAVAILABLE) {
                bus_error = status;
                if (execution_engine == ExecutionEngine::jit) stop("RISC-V bus error");
                return 0;
            }
        }
        uint32_t value = read_byte(address, kind);
        value |= static_cast<uint32_t>(read_byte(address + 1, kind)) << 8;
        value |= static_cast<uint32_t>(read_byte(address + 2, kind)) << 16;
        value |= static_cast<uint32_t>(read_byte(address + 3, kind)) << 24;
        return value;
    }
    uint32_t fetch_word(uint32_t address) {
        return read_word(address, TRACE_KIND_OPCODE);
    }
    void write_half(uint32_t address, uint16_t value) {
        write_byte(address, static_cast<uint8_t>(value));
        write_byte(address + 1, static_cast<uint8_t>(value >> 8));
    }
    void write_word(uint32_t address, uint32_t value) {
        write_byte(address, static_cast<uint8_t>(value));
        write_byte(address + 1, static_cast<uint8_t>(value >> 8));
        write_byte(address + 2, static_cast<uint8_t>(value >> 16));
        write_byte(address + 3, static_cast<uint8_t>(value >> 24));
    }
    void stop(const char *reason) {
        halted = true;
        rv_halt(rv);
        if (debug)
            debug->request_stop(debug->context, owner, reason);
    }
    SrhStatus reset() {
        if (!rv_reset_bare(rv, reset_vector))
            return SRH_ERROR;
        const auto irq_status = refresh_irq();
        if (irq_status != SRH_OK)
            return irq_status;
        bus_error = SRH_OK;
        halted = false;
        instructions = 0;
        ticks_until_batch = 0;
        return SRH_OK;
    }
    SrhStatus tick() {
        if (halted || rv_has_halted(rv))
            return SRH_OK;
        const auto irq_status = refresh_irq();
        if (irq_status != SRH_OK)
            return irq_status;
        // The signal is sampled at a CPU boundary, so a card cannot re-enter
        // this CPU while it is executing.
        rv_service_machine_interrupts(rv);
        if (ticks_until_batch) {
            --ticks_until_batch;
            return SRH_OK;
        }
#if RV32_HAS(JIT)
        if (execution_engine == ExecutionEngine::jit) {
            trace_enabled = false;
            bus_error = SRH_OK;
            // Charge every instruction (including block overrun) to the
            // clock. Batching must not multiply the selected CPU frequency.
            const auto before = rv_bare_jit_instructions(rv);
            if (!rv_step_bare_jit(rv, jit_batch_instructions)) {
                stop("RISC-V JIT allocation or compilation failed");
                return SRH_ERROR;
            }
            const auto executed = rv_bare_jit_instructions(rv) - before;
            ticks_until_batch = executed ? static_cast<uint32_t>(executed - 1) : 0;
            return bus_error;
        }
#endif
        if (srz80::sdk::has_field(debug, &SrhHostDebugV1::trace_enabled))
            trace_enabled = debug->trace_enabled(debug->context) != 0;
        const auto pc = rv_get_pc(rv);
        const bool boundary_required =
            !srz80::sdk::has_field(debug, &SrhHostDebugV1::boundary_required) ||
            debug->boundary_required(debug->context) != 0;
        if (boundary_required) {
            auto status = debug->boundary_ex(debug->context, owner, memory_space, pc, 4, 1);
            if (status == SRH_STOP)
                return SRH_OK;
            if (status != SRH_OK)
                return status;
        }
        bus_error = SRH_OK;
        if (execution_engine == ExecutionEngine::cached)
            rv_step_cached_debug(rv);
        else
            rv_step_debug(rv);
        ++instructions;
        if (bus_error != SRH_OK) {
            stop("RISC-V bus error");
            return bus_error;
        }
        return SRH_OK;
    }

    SrhStatus set_irq(int32_t level) {
        irq_asserted = level > 0;
        return rv_set_machine_external_interrupt(rv, irq_asserted) ? SRH_OK : SRH_ERROR;
    }
    SrhStatus refresh_irq() {
        if (!irq_signal)
            return SRH_OK;
        int32_t level = 0;
        const auto status = host->signal_read(host->context, irq_signal, &level);
        return status == SRH_OK ? set_irq(level) : status;
    }
};

SrhStatus SRH_CALL execution_engine_config_get(void *context, char *value, uint32_t capacity) {
    const auto *cpu = static_cast<const Cpu *>(context);
    if (!cpu || !value || !capacity) return SRH_INVALID;
    const char *mode = cpu->execution_engine == ExecutionEngine::jit ? "JIT" :
        cpu->execution_engine == ExecutionEngine::reference ? "Reference" : "Cached";
    if (std::strlen(mode) >= capacity) return SRH_UNAVAILABLE;
    std::snprintf(value, capacity, "%s", mode);
    return SRH_OK;
}

SrhStatus SRH_CALL execution_engine_config_set(void *context, const char *value) {
    if (!value || (std::strcmp(value, "Cached") != 0 && std::strcmp(value, "Reference") != 0
#if RV32_HAS(JIT)
                   && std::strcmp(value, "JIT") != 0
#endif
                   ))
        return SRH_INVALID;
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!context) return SRH_INVALID;
        const auto mode = std::strcmp(value, "JIT") == 0 ? ExecutionEngine::jit :
            std::strcmp(value, "Reference") == 0 ? ExecutionEngine::reference : ExecutionEngine::cached;
        return static_cast<Cpu *>(context)->set_execution_engine(mode);
    });
}

#if RV32_HAS(JIT)
SrhStatus SRH_CALL jit_throughput_get(void *context, char *value, uint32_t capacity) {
    if (!context || !value || !capacity) return SRH_INVALID;
    const int length = std::snprintf(value, capacity, "%u",
        static_cast<const Cpu *>(context)->jit_batch_instructions);
    return length >= 0 && static_cast<uint32_t>(length) < capacity ? SRH_OK : SRH_UNAVAILABLE;
}

SrhStatus SRH_CALL jit_throughput_set(void *context, const char *value) {
    if (!context || !value) return SRH_INVALID;
    uint32_t batch = 0;
    const auto end = value + std::strlen(value);
    const auto parsed = std::from_chars(value, end, batch);
    if (parsed.ec != std::errc{} || parsed.ptr != end || batch < 1 || batch > 65536)
        return SRH_INVALID;
    static_cast<Cpu *>(context)->jit_batch_instructions = batch;
    return SRH_OK;
}
#endif

Cpu *cpu_from(riscv_t *rv) {
    return static_cast<Cpu *>(rv_get_user(rv));
}
riscv_word_t mem_ifetch(riscv_t *rv, riscv_word_t address) {
    return cpu_from(rv)->fetch_word(address);
}
riscv_word_t mem_read_w(riscv_t *rv, riscv_word_t address) {
    return cpu_from(rv)->read_word(address, TRACE_KIND_DATA_READ);
}
riscv_half_t mem_read_s(riscv_t *rv, riscv_word_t address) {
    return cpu_from(rv)->read_half(address, TRACE_KIND_DATA_READ);
}
riscv_byte_t mem_read_b(riscv_t *rv, riscv_word_t address) {
    return cpu_from(rv)->read_byte(address, TRACE_KIND_DATA_READ);
}
void mem_write_w(riscv_t *rv, riscv_word_t address, riscv_word_t value) {
    cpu_from(rv)->write_word(address, value);
}
void mem_write_s(riscv_t *rv, riscv_word_t address, riscv_half_t value) {
    cpu_from(rv)->write_half(address, value);
}
void mem_write_b(riscv_t *rv, riscv_word_t address, riscv_byte_t value) {
    cpu_from(rv)->write_byte(address, value);
}
void on_ecall(riscv_t *rv) {
    cpu_from(rv)->stop("RISC-V ECALL");
}
void on_ebreak(riscv_t *rv) {
    cpu_from(rv)->stop("RISC-V EBREAK");
}
void on_trap(riscv_t *rv) {
    cpu_from(rv)->stop("RISC-V trap");
}
void on_memset(riscv_t *) {}
void on_memcpy(riscv_t *) {}

SrhStatus SRH_CALL irq_callback(void *p, int32_t level) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!p) return SRH_INVALID;
        return static_cast<Cpu *>(p)->set_irq(level);
    });
}

SrhStatus SRH_CALL tick_callback(void *p) {
    return srz80::sdk::guard([&] { return static_cast<Cpu *>(p)->tick(); });
}
SrhStatus SRH_CALL disasm(void *p, SrhHandle, SrhHandle, uint64_t pc, const uint8_t *bytes,
                          uint32_t bytes_available, uint32_t *instruction_bytes,
                          uint32_t *cycles, char *text, uint32_t text_size) {
    auto *cpu = static_cast<Cpu *>(p);
    if (!instruction_bytes || !cycles || !text || text_size == 0 || !bytes || pc > UINT32_MAX ||
        bytes_available == 0)
        return SRH_INVALID;
    if (cpu->execution_engine == ExecutionEngine::jit || !cpu->capstone) return SRH_UNAVAILABLE;
    cs_insn *insn = nullptr;
    const auto count = cs_disasm(cpu->capstone, bytes, bytes_available, pc, 1, &insn);
    if (count != 1) {
        /* Data or an unallocated encoding is not an error for the memory
         * browser.  If at least one RV32I word is available, present it as a
         * raw data word instead of claiming the memory peek itself failed. */
        if (bytes_available < 4)
            return SRH_UNAVAILABLE;
        const uint32_t word = static_cast<uint32_t>(bytes[0]) |
                              (static_cast<uint32_t>(bytes[1]) << 8) |
                              (static_cast<uint32_t>(bytes[2]) << 16) |
                              (static_cast<uint32_t>(bytes[3]) << 24);
        *instruction_bytes = 4;
        *cycles = 1;
        std::snprintf(text, text_size, ".word 0x%08x", word);
        return SRH_OK;
    }
    *instruction_bytes = insn[0].size;
    *cycles = 1;
    if (insn[0].op_str[0])
        std::snprintf(text, text_size, "%s %s", insn[0].mnemonic, insn[0].op_str);
    else
        std::snprintf(text, text_size, "%s", insn[0].mnemonic);
    cs_free(insn, count);
    return SRH_OK;
}

constexpr std::array<const char *, 32> aliases = {
    "zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2", "s0/fp", "s1", "a0", "a1",
    "a2",   "a3", "a4", "a5", "a6", "a7", "s2",    "s3", "s4", "s5", "s6", "s7",
    "s8",   "s9", "s10", "s11", "t3", "t4", "t5", "t6",
};

SrhStatus SRH_CALL create(const ShouryoHost *host, SrhHandle owner, const SrhConfig *config,
                          void **result) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!srz80::sdk::valid(host) || !srz80::sdk::valid(config) || !result || !config->space ||
            config->clock >= 3 || config->reset_vector > UINT32_MAX)
            return SRH_INVALID;
        const void *extension = nullptr;
        if (!host->query || host->query(host->context, "host.debug.v1", &extension) != SRH_OK ||
            !extension)
            return SRH_UNAVAILABLE;
        auto debug = static_cast<const SrhHostDebugV1 *>(extension);
        if (!srz80::sdk::valid(debug))
            return SRH_UNAVAILABLE;

        auto cpu = std::make_unique<Cpu>();
        cpu->host = host;
        cpu->debug = debug;
        extension = nullptr;
        if (host->query(host->context, "host.memory.v1", &extension) == SRH_OK && extension) {
            auto memory = static_cast<const SrhHostMemoryV1 *>(extension);
            if (srz80::sdk::valid(memory))
                cpu->memory = memory;
        }
        cpu->owner = owner;
        cpu->memory_space = config->space;
        cpu->clock = config->clock;
        cpu->reset_vector = static_cast<uint32_t>(config->reset_vector);
        cpu->execution_engine = configured_execution_engine(host);
        extension = nullptr;
        if (host->query(host->context, "host.config.v1", &extension) == SRH_OK && extension) {
            const auto host_config = static_cast<const SrhHostConfigV1 *>(extension);
            if (srz80::sdk::valid(host_config) && host_config->register_entry &&
                host_config->get_value) {
                cpu->host_config = host_config;
                const SrhConfigEntry entry{
                    SRH_INIT(SrhConfigEntry),
                    "Cards/RISC-V",
                    "cards.riscv.execution_engine",
                    "Execution engine",
                    "Cached and Reference run one instruction per tick. JIT runs batches at the same average rate, but device timing is rougher and stepping, execution breakpoints, and live disassembly are unavailable. PC and instruction count show N/A. Registers update once per batch. Changes apply without resetting the CPU",
                    Srh_CONFIG_ENUM,
                    execution_modes,
                    "Cached",
                    cpu.get(),
                    owner,
                    execution_engine_config_get,
                    execution_engine_config_set};
                const auto config_status =
                    host_config->register_entry(host_config->context, &entry);
                if (config_status != SRH_OK)
                    return config_status;
#if RV32_HAS(JIT)
                const SrhConfigEntry throughput{
                    SRH_INIT(SrhConfigEntry), "Cards/RISC-V",
                    "cards.riscv.jit_instruction_throughput", "JIT instruction throughput",
                    "Instructions per batch, from 1 to 65536. Bigger batches run with less overhead but rougher device timing. A block may run up to 63 instructions over the target. Takes effect on the next batch. Does nothing outside JIT mode",
                    Srh_CONFIG_INT, nullptr, "1024", cpu.get(), owner,
                    jit_throughput_get, jit_throughput_set};
                const auto throughput_status = host_config->register_entry(host_config->context, &throughput);
                if (throughput_status != SRH_OK) return throughput_status;
                char configured[32]{};
                if (host_config->get_value(host_config->context, throughput.name, configured, sizeof(configured)) == SRH_OK)
                    jit_throughput_set(cpu.get(), configured);
#endif
            }
        }
        const riscv_io_t io = {mem_ifetch,  mem_read_w, mem_read_s, mem_read_b,
                               mem_write_w, mem_write_s, mem_write_b, on_ecall,
                               on_ebreak,   on_memset,   on_memcpy,   on_trap};
        cpu->rv = rv_create_bare(cpu.get(), &io, cpu->reset_vector);
        if (!cpu->rv)
            return SRH_ERROR;
        // IRQ is an engine-owned level signal shared by cards such as the
        // VDP and keyboard. The CPU is its consumer; no device-specific
        // wiring belongs in the engine or GUI.
        if (!host->signal_find || !host->signal_subscribe || !host->signal_read ||
            host->signal_find(host->context, "IRQ", &cpu->irq_signal) != SRH_OK ||
            !cpu->irq_signal)
            return SRH_UNAVAILABLE;
        auto status = host->signal_subscribe(host->context, owner, cpu->irq_signal,
                                             irq_callback, cpu.get(),
                                             &cpu->irq_subscription);
        if (status != SRH_OK)
            return status;
        int32_t irq_level = 0;
        status = host->signal_read(host->context, cpu->irq_signal, &irq_level);
        if (status != SRH_OK)
            return status;
        status = cpu->set_irq(irq_level);
        if (status != SRH_OK)
            return status;
        const auto mode_status = cpu->set_execution_engine(cpu->execution_engine);
        if (mode_status != SRH_OK) return mode_status;
        status = debug->register_disasm(debug->context, owner, disasm, cpu.get());
        if (status != SRH_OK)
            return status;
        SrhHandle subscription = 0;
        status = host->subscribe_clock(host->context, owner, cpu->clock, tick_callback, cpu.get(),
                                       &subscription);
        if (status != SRH_OK)
            return status;
        *result = cpu.release();
        return SRH_OK;
    });
}
void SRH_CALL destroy(void *p) {
    delete static_cast<Cpu *>(p);
}
SrhStatus SRH_CALL reset(void *p, uint32_t) {
    return srz80::sdk::guard([&] { return static_cast<Cpu *>(p)->reset(); });
}
uint32_t SRH_CALL property_count(void *) {
    return 36;
}
SrhStatus SRH_CALL property_info(void *p, uint32_t index, SrhProperty *out) {
    if (!srz80::sdk::valid(out) || index >= property_count(nullptr))
        return SRH_INVALID;
    if (index == 0) {
        *out = {SRH_INIT(SrhProperty), "model", "Control", "CPU core and ISA", SRH_TEXT,
                0, 0, 0, nullptr, 0};
    } else if (index == 1) {
        *out = {SRH_INIT(SrhProperty), "PC", "Control", "Program counter", SRH_UNSIGNED,
                32, 16, 1, nullptr, 0};
    } else if (index <= 33) {
        const auto reg = index - 2;
        static std::array<std::array<char, 16>, 32> names = [] {
            std::array<std::array<char, 16>, 32> value{};
            for (size_t i = 0; i < value.size(); ++i)
                std::snprintf(value[i].data(), value[i].size(), "x%zu (%s)", i, aliases[i]);
            return value;
        }();
        *out = {SRH_INIT(SrhProperty), names[reg].data(), "Integer", aliases[reg], SRH_UNSIGNED,
                32, 16, reg != 0, nullptr, 0};
    } else if (index == 34) {
        *out = {SRH_INIT(SrhProperty), "HALTED", "Control", "Stopped by trap or breakpoint",
                SRH_BOOLEAN, 1, 10, 0, nullptr, 0};
    } else {
        *out = {SRH_INIT(SrhProperty), "INSTRUCTIONS", "Control", "Instructions attempted",
                SRH_UNSIGNED, 64, 10, 0, nullptr, 0};
    }
    if (static_cast<Cpu *>(p)->execution_engine == ExecutionEngine::jit && (index == 1 || index == 35)) {
        out->ui_flags |= SRH_PROPERTY_UNAVAILABLE;
        out->editable = 0;
    }
    return SRH_OK;
}
SrhStatus SRH_CALL property_get(void *p, uint32_t index, SrhValue *out) {
    if (!srz80::sdk::valid(out) || index >= property_count(nullptr))
        return SRH_INVALID;
    auto &cpu = *static_cast<Cpu *>(p);
    if (cpu.execution_engine == ExecutionEngine::jit && (index == 1 || index == 35))
        return SRH_UNAVAILABLE;
    if (index == 0)
        std::snprintf(out->text, sizeof(out->text), "rv32emu RV32IMF (%s)",
                      cpu.execution_engine == ExecutionEngine::jit ? "JIT" : cpu.execution_engine == ExecutionEngine::cached ? "cached" : "reference");
    else if (index == 1)
        out->unsigned_value = rv_get_pc(cpu.rv);
    else if (index <= 33)
        out->unsigned_value = rv_get_reg(cpu.rv, index - 2);
    else if (index == 34)
        out->unsigned_value = cpu.halted || rv_has_halted(cpu.rv);
    else
        out->unsigned_value = cpu.instructions;
    return SRH_OK;
}
SrhStatus SRH_CALL property_set(void *p, uint32_t index, const SrhValue *in) {
    if (!srz80::sdk::valid(in) || index >= property_count(nullptr) || index <= 2 || index >= 34 ||
        in->unsigned_value > UINT32_MAX)
        return SRH_INVALID;
    auto &cpu = *static_cast<Cpu *>(p);
    if (cpu.execution_engine == ExecutionEngine::jit && index == 1) return SRH_UNAVAILABLE;
    if (index == 1)
        return rv_set_pc(cpu.rv, static_cast<uint32_t>(in->unsigned_value)) ? SRH_OK : SRH_INVALID;
    rv_set_reg(cpu.rv, index - 2, static_cast<uint32_t>(in->unsigned_value));
    return SRH_OK;
}

struct CpuState {
    uint32_t pc;
    uint32_t registers[32];
    uint64_t instructions;
    uint8_t halted;
    uint32_t ticks_until_batch;
    uint32_t floating_registers[32];
    uint32_t fcsr;
};
template <class Archive> void archive_state(Archive &ar, CpuState &s) {
    ar.fields(s.pc, s.registers, s.instructions, s.halted, s.ticks_until_batch,
              s.floating_registers, s.fcsr);
}
SrhStatus SRH_CALL save_payload(void *p, uint8_t *buffer, uint64_t *size) {
    const auto &cpu = *static_cast<Cpu *>(p);
    CpuState state{};
    state.pc = rv_get_pc(cpu.rv);
    for (uint32_t i = 0; i < 32; ++i)
        state.registers[i] = rv_get_reg(cpu.rv, i);
    state.instructions = cpu.instructions;
    state.halted = (cpu.halted || rv_has_halted(cpu.rv)) ? 1 : 0;
    state.ticks_until_batch = cpu.ticks_until_batch;
    for (uint32_t i = 0; i < 32; ++i)
        state.floating_registers[i] = rv_get_freg(cpu.rv, i);
    state.fcsr = rv_get_fcsr(cpu.rv);
    srz80::sdk::state::Writer writer;
    archive_state(writer, state);
    return srz80::sdk::state::copy_payload(writer.bytes, buffer, size);
}
SrhStatus SRH_CALL load_payload(void *p, const uint8_t *buffer, uint64_t size) {
    CpuState state{};
    srz80::sdk::state::Reader reader({buffer, static_cast<size_t>(size)});
    archive_state(reader, state);
    if (!reader.finished() || (state.pc & 3) != 0 || state.halted > 1 ||
        state.ticks_until_batch > 65598 || state.registers[0] != 0 || (state.fcsr & ~0xffu))
        return SRH_INVALID;
    auto &cpu = *static_cast<Cpu *>(p);
    if (!rv_reset_bare(cpu.rv, state.pc))
        return SRH_ERROR;
    for (uint32_t i = 1; i < 32; ++i)
        rv_set_reg(cpu.rv, i, state.registers[i]);
    cpu.instructions = state.instructions;
    cpu.ticks_until_batch = state.ticks_until_batch;
    for (uint32_t i = 0; i < 32; ++i)
        rv_set_freg(cpu.rv, i, state.floating_registers[i]);
    rv_set_fcsr(cpu.rv, state.fcsr);
    cpu.halted = state.halted != 0;
    if (cpu.halted)
        rv_halt(cpu.rv);
    return SRH_OK;
}

const SrhCardDescriptor descriptor{SRH_INIT(SrhCardDescriptor), "CPU", "RISC-V RV32IMF",
                                   "Bare-metal RV32IMF processor", 0, 0, 0, 0, 0,
                                   SRH_CARD_SHOW_CLOCK, R"({"isa":"rv32imf"})", nullptr, nullptr};
using State = srz80::sdk::state::Callbacks<save_payload, load_payload, 1>;
const SrhPlugin api{SRH_INIT(SrhPlugin), "riscv", create, destroy, reset,
                    property_count, property_info, property_get, property_set,
                    State::save, State::load, &descriptor};
} // namespace

extern "C" SRH_EXPORT const SrhPlugin *SRH_CALL srz80_plugin_init(const ShouryoHost *host) {
    return srz80::sdk::valid(host) ? &api : nullptr;
}
