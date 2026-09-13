#include <boundary.hpp>
#include <cstring>
#include <memory>
namespace {
struct Cpu {
    const ShouryoHost *host;
    SrhHandle owner, space;
    uint64_t pc, reset_vector;
    uint8_t a = 0;
    bool halted = false;
    SrhStatus fetch(uint8_t &value) {
        if (pc == UINT64_MAX)
            return SRH_INVALID;
        auto status = host->read(host->context, owner, space, pc, &value);
        if (status == SRH_OK)
            ++pc;
        return status;
    }
    SrhStatus address(uint64_t &value) {
        value = 0;
        for (unsigned i = 0; i < 8; ++i) {
            uint8_t b = 0;
            auto status = fetch(b);
            if (status != SRH_OK)
                return status;
            value |= uint64_t(b) << (i * 8);
        }
        return SRH_OK;
    }
    SrhStatus tick() {
        if (halted)
            return SRH_OK;
        auto status = host->boundary(host->context, owner, space, pc);
        if (status != SRH_OK)
            return status;
        uint8_t op = 0;
        status = fetch(op);
        if (status != SRH_OK)
            return fail(status);
        uint64_t addr = 0;
        switch (op) {
        case 0:
            break;
        case 1:
            status = fetch(a);
            break;
        case 2:
            status = address(addr);
            if (status == SRH_OK)
                status = host->read(host->context, owner, space, addr, &a);
            break;
        case 3:
            status = address(addr);
            if (status == SRH_OK)
                status = host->write(host->context, owner, space, addr, a);
            break;
        case 4:
            status = address(addr);
            if (status == SRH_OK)
                pc = addr;
            break;
        case 5:
            a = static_cast<uint8_t>(a + 1);
            break;
        case 6:
            a = static_cast<uint8_t>(a - 1);
            break;
        case 7:
            halted = true;
            break;
        default:
            return fail(SRH_INVALID);
        }
        return status == SRH_OK ? SRH_OK : fail(status);
    }
    SrhStatus fail(SrhStatus status) {
        halted = true;
        host->log(host->context, owner, "Fake CPU halted: invalid opcode/address or bus error");
        return status;
    }
};
SrhStatus SRH_CALL tick(void *p) {
    return srz80::sdk::guard([&] { return static_cast<Cpu *>(p)->tick(); });
}
SrhStatus SRH_CALL create(const ShouryoHost *host, SrhHandle owner, const SrhConfig *config, void **result) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!srz80::sdk::valid(host) || !srz80::sdk::valid(config) || !result || config->clock >= 3)
            return SRH_INVALID;
        auto cpu = std::make_unique<Cpu>(
            Cpu{host, owner, config->space, config->reset_vector, config->reset_vector});
        SrhHandle subscription = 0;
        auto status =
            host->subscribe_clock(host->context, owner, config->clock, tick, cpu.get(), &subscription);
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
    auto &c = *static_cast<Cpu *>(p);
    c.pc = c.reset_vector;
    c.a = 0;
    c.halted = false;
    return SRH_OK;
}
uint32_t SRH_CALL count(void *) {
    return 3;
}
SrhStatus SRH_CALL info(void *, uint32_t index, SrhProperty *out) {
    if (!srz80::sdk::valid(out) || index >= 3)
        return SRH_INVALID;
    static const char *names[]{"PC", "A", "HALTED"};
    *out = {SRH_INIT(SrhProperty),
            names[index],
            "CPU",
            "Fake CPU state",
            static_cast<uint32_t>(index == 2 ? SRH_BOOLEAN : SRH_UNSIGNED),
            index == 0   ? 64u
            : index == 1 ? 8u
                         : 1u,
            index == 2 ? 10u : 16u,
            1,
            nullptr};
    return SRH_OK;
}
SrhStatus SRH_CALL get(void *p, uint32_t index, SrhValue *out) {
    if (!srz80::sdk::valid(out) || index >= 3)
        return SRH_INVALID;
    auto &c = *static_cast<Cpu *>(p);
    out->unsigned_value = index == 0 ? c.pc : index == 1 ? c.a : c.halted;
    return SRH_OK;
}
SrhStatus SRH_CALL set(void *p, uint32_t index, const SrhValue *in) {
    if (!srz80::sdk::valid(in) || index >= 3 || (index == 1 && in->unsigned_value > 255) ||
        (index == 2 && in->unsigned_value > 1))
        return SRH_INVALID;
    auto &c = *static_cast<Cpu *>(p);
    if (index == 0)
        c.pc = in->unsigned_value;
    else if (index == 1)
        c.a = static_cast<uint8_t>(in->unsigned_value);
    else
        c.halted = in->unsigned_value != 0;
    return SRH_OK;
}
SrhStatus SRH_CALL save_state(void *p, uint8_t *buffer, uint64_t *size) {
    if (!size)
        return SRH_INVALID;
    constexpr uint64_t required = 10;
    if (!buffer) {
        *size = required;
        return SRH_OK;
    }
    if (*size < required) {
        *size = required;
        return SRH_UNAVAILABLE;
    }
    auto &c = *static_cast<Cpu *>(p);
    std::memcpy(buffer, &c.pc, 8);
    buffer[8] = c.a;
    buffer[9] = c.halted ? 1 : 0;
    *size = required;
    return SRH_OK;
}
SrhStatus SRH_CALL load_state(void *p, const uint8_t *buffer, uint64_t size) {
    if (!buffer || size != 10)
        return SRH_INVALID;
    auto &c = *static_cast<Cpu *>(p);
    std::memcpy(&c.pc, buffer, 8);
    c.a = buffer[8];
    c.halted = buffer[9] != 0;
    return SRH_OK;
}
const SrhCardDescriptor descriptor{SRH_INIT(SrhCardDescriptor), "CPU", "Fake CPU",
                                   "Minimal verification CPU", 0, 0, 0, 0, 0,
                                   SRH_CARD_SHOW_CLOCK, "{}", nullptr, nullptr};
const SrhPlugin api{SRH_INIT(SrhPlugin), "fake_cpu", create, destroy, reset, count, info, get, set,
                    save_state, load_state, &descriptor};
} // namespace
extern "C" SRH_EXPORT const SrhPlugin *SRH_CALL srz80_plugin_init(const ShouryoHost *host) {
    return srz80::sdk::valid(host) ? &api : nullptr;
}
