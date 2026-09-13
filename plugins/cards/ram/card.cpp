#include <algorithm>
#include <boundary.hpp>
#include <cstring>
#include <memory>
#include <vector>
namespace {
struct Ram {
    uint64_t base;
    std::vector<uint8_t> bytes;
};
SrhStatus SRH_CALL read(void *p, uint64_t address, uint8_t *value) {
    auto &ram = *static_cast<Ram *>(p);
    if (!value || address < ram.base || address - ram.base >= ram.bytes.size())
        return SRH_INVALID;
    *value = ram.bytes[static_cast<size_t>(address - ram.base)];
    return SRH_OK;
}
SrhStatus SRH_CALL read_word(void *p, uint64_t address, uint32_t *value) {
    auto &ram = *static_cast<Ram *>(p);
    if (!value || address < ram.base || ram.bytes.size() < 4 ||
        address - ram.base > ram.bytes.size() - 4)
        return SRH_INVALID;
    const auto offset = static_cast<size_t>(address - ram.base);
    *value = static_cast<uint32_t>(ram.bytes[offset]) |
             (static_cast<uint32_t>(ram.bytes[offset + 1]) << 8) |
             (static_cast<uint32_t>(ram.bytes[offset + 2]) << 16) |
             (static_cast<uint32_t>(ram.bytes[offset + 3]) << 24);
    return SRH_OK;
}
SrhStatus SRH_CALL write(void *p, uint64_t address, uint8_t value) {
    auto &ram = *static_cast<Ram *>(p);
    if (address < ram.base || address - ram.base >= ram.bytes.size())
        return SRH_INVALID;
    ram.bytes[static_cast<size_t>(address - ram.base)] = value;
    return SRH_OK;
}
SrhStatus SRH_CALL create(const ShouryoHost *host, SrhHandle owner, const SrhConfig *config, void **result) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!srz80::sdk::valid(host) || !srz80::sdk::valid(config) || !result || !config->size ||
            config->size > 64 * 1024 * 1024 || config->base > UINT64_MAX - (config->size - 1))
            return SRH_INVALID;
        auto ram = std::make_unique<Ram>(
            Ram{config->base, std::vector<uint8_t>(static_cast<size_t>(config->size), 0)});
        SrhMapping m{SRH_INIT(SrhMapping),
                     config->space,
                     config->base,
                     config->base + config->size - 1,
                     config->priority,
                     ram.get(),
                     read,
                     write,
                     read,
                     read_word};
        SrhHandle mapping = 0;
        auto status = host->map(host->context, owner, &m, &mapping);
        if (status != SRH_OK)
            return status;
        *result = ram.release();
        return SRH_OK;
    });
}
void SRH_CALL destroy(void *p) {
    delete static_cast<Ram *>(p);
}
SrhStatus SRH_CALL reset(void *p, uint32_t cold) {
    if (cold) {
        auto &bytes = static_cast<Ram *>(p)->bytes;
        std::fill(bytes.begin(), bytes.end(), uint8_t{0});
    }
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
SrhStatus SRH_CALL save_state(void *p, uint8_t *buffer, uint64_t *size) {
    if (!size)
        return SRH_INVALID;
    auto &bytes = static_cast<Ram *>(p)->bytes;
    if (!buffer) {
        *size = bytes.size();
        return SRH_OK;
    }
    if (*size < bytes.size()) {
        *size = bytes.size();
        return SRH_UNAVAILABLE;
    }
    if (!bytes.empty())
        std::memcpy(buffer, bytes.data(), bytes.size());
    *size = bytes.size();
    return SRH_OK;
}
SrhStatus SRH_CALL load_state(void *p, const uint8_t *buffer, uint64_t size) {
    auto &bytes = static_cast<Ram *>(p)->bytes;
    if (size != bytes.size() || (!buffer && size))
        return SRH_INVALID;
    if (size)
        std::memcpy(bytes.data(), buffer, size);
    return SRH_OK;
}
const SrhCardDescriptor descriptor{SRH_INIT(SrhCardDescriptor), "Memory", "RAM",
                                   "Read/write memory", 0x1000, 256, 0, 0, 0, 0, "{}", nullptr,
                                   nullptr};
const SrhPlugin api{SRH_INIT(SrhPlugin), "ram", create, destroy, reset, count, info, get, set,
                    save_state, load_state, &descriptor};
} // namespace
extern "C" SRH_EXPORT const SrhPlugin *SRH_CALL srz80_plugin_init(const ShouryoHost *host) {
    return srz80::sdk::valid(host) ? &api : nullptr;
}
