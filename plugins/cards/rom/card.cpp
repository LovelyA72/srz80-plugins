#include <state.hpp>
#include <boundary.hpp>
#include <memory>
#include <vector>
namespace {
struct Rom {
    uint64_t base;
    std::vector<uint8_t> bytes;
};
SrhStatus SRH_CALL read(void *p, uint64_t address, uint8_t *value) {
    auto &rom = *static_cast<Rom *>(p);
    if (!value || address < rom.base || address - rom.base >= rom.bytes.size())
        return SRH_INVALID;
    *value = rom.bytes[static_cast<size_t>(address - rom.base)];
    return SRH_OK;
}
SrhStatus SRH_CALL read_word(void *p, uint64_t address, uint32_t *value) {
    auto &rom = *static_cast<Rom *>(p);
    if (!value || address < rom.base || rom.bytes.size() < 4 ||
        address - rom.base > rom.bytes.size() - 4)
        return SRH_INVALID;
    const auto offset = static_cast<size_t>(address - rom.base);
    *value = static_cast<uint32_t>(rom.bytes[offset]) |
             (static_cast<uint32_t>(rom.bytes[offset + 1]) << 8) |
             (static_cast<uint32_t>(rom.bytes[offset + 2]) << 16) |
             (static_cast<uint32_t>(rom.bytes[offset + 3]) << 24);
    return SRH_OK;
}
SrhStatus SRH_CALL create(const ShouryoHost *host, SrhHandle owner, const SrhConfig *config, void **result) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!srz80::sdk::valid(host) || !srz80::sdk::valid(config) || !result || !config->image ||
            !config->image_size || config->image_size > 64 * 1024 * 1024 ||
            config->base > UINT64_MAX - (config->image_size - 1))
            return SRH_INVALID;
        auto rom = std::make_unique<Rom>(
            Rom{config->base, std::vector<uint8_t>(config->image, config->image + config->image_size)});
        SrhMapping m{SRH_INIT(SrhMapping),
                     config->space,
                     config->base,
                     config->base + config->image_size - 1,
                     config->priority,
                     rom.get(),
                     read,
                     nullptr,
                     read,
                     read_word};
        SrhHandle mapping = 0;
        auto status = host->map(host->context, owner, &m, &mapping);
        if (status != SRH_OK)
            return status;
        *result = rom.release();
        return SRH_OK;
    });
}
void SRH_CALL destroy(void *p) {
    delete static_cast<Rom *>(p);
}
SrhStatus SRH_CALL reset(void *, uint32_t) {
    return SRH_OK;
}
uint32_t SRH_CALL count(void *) {
    return 0;
}
SrhStatus SRH_CALL info(void *, uint32_t, SrhProperty *) {
    return SRH_NOT_FOUND;
}
SrhStatus SRH_CALL get(void *, uint32_t, SrhValue *) {
    return SRH_NOT_FOUND;
}
SrhStatus SRH_CALL set(void *, uint32_t, const SrhValue *) {
    return SRH_NOT_FOUND;
}
SrhStatus SRH_CALL save_payload(void *, uint8_t *, uint64_t *size) {
    if (!size)
        return SRH_INVALID;
    *size = 0;
    return SRH_OK;
}
SrhStatus SRH_CALL load_payload(void *, const uint8_t *, uint64_t size) {
    return size == 0 ? SRH_OK : SRH_INVALID;
}
const SrhCardDescriptor descriptor{SRH_INIT(SrhCardDescriptor), "Memory", "ROM",
                                   "Read-only memory image", 0, 256, 0, 0, 0,
                                   SRH_CARD_REQUIRES_IMAGE, "{}", nullptr, nullptr};
using State = srz80::sdk::state::Callbacks<save_payload, load_payload, 1>;
const SrhPlugin api{SRH_INIT(SrhPlugin), "rom", create, destroy, reset, count, info, get, set,
                    State::save, State::load, &descriptor};
} // namespace
extern "C" SRH_EXPORT const SrhPlugin *SRH_CALL srz80_plugin_init(const ShouryoHost *host) {
    return srz80::sdk::valid(host) ? &api : nullptr;
}
