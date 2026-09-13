#include <boundary.hpp>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {
constexpr size_t bank_size = 32 * 1024;
constexpr size_t memory_size = 512 * 1024;
constexpr uint64_t io_first = 0x78;

struct MmuMemory {
    std::vector<uint8_t> rom;
    std::vector<uint8_t> ram = std::vector<uint8_t>(memory_size);
    uint8_t bank_latch = 0;

    static std::string io_space_name(const SrhConfig *config) {
        if (!srz80::sdk::has_field(config, &SrhConfig::config_json) || !config->config_json)
            return "cpu0.io";
        const std::string json(config->config_json, config->config_json_size);
        const std::string key = "\"io_space\"";
        auto position = json.find(key);
        if (position == std::string::npos)
            return "cpu0.io";
        position = json.find(':', position + key.size());
        if (position == std::string::npos)
            return "cpu0.io";
        position = json.find('"', position + 1);
        if (position == std::string::npos)
            return "cpu0.io";
        const auto end = json.find('"', position + 1);
        return end == std::string::npos ? "cpu0.io" : json.substr(position + 1, end - position - 1);
    }

    size_t lower_offset(uint64_t address) const {
        return (static_cast<size_t>(bank_latch >> 1) & 0x0f) * bank_size +
               static_cast<size_t>(address);
    }

    SrhStatus read_memory(uint64_t address, uint8_t *value) const {
        if (!value || address > 0xffff)
            return SRH_INVALID;
        if (address < 0x8000) {
            const auto offset = lower_offset(address);
            *value = (bank_latch & 0x20) ? ram[offset] : rom[offset];
        } else
            *value = ram[memory_size - bank_size + static_cast<size_t>(address - 0x8000)];
        return SRH_OK;
    }

    SrhStatus write_memory(uint64_t address, uint8_t value) {
        if (address > 0xffff)
            return SRH_INVALID;
        if (address < 0x8000) {
            if (bank_latch & 0x20)
                ram[lower_offset(address)] = value;
        } else
            ram[memory_size - bank_size + static_cast<size_t>(address - 0x8000)] = value;
        return SRH_OK;
    }

    SrhStatus write_port(uint64_t address, uint8_t value) {
        if (address < io_first || address > io_first + 1)
            return SRH_INVALID;
        bank_latch = value & 0x3e;
        return SRH_OK;
    }
};

SrhStatus SRH_CALL read_memory(void *context, uint64_t address, uint8_t *value) {
    return static_cast<MmuMemory *>(context)->read_memory(address, value);
}
SrhStatus SRH_CALL write_memory(void *context, uint64_t address, uint8_t value) {
    return static_cast<MmuMemory *>(context)->write_memory(address, value);
}
SrhStatus SRH_CALL write_port(void *context, uint64_t address, uint8_t value) {
    return static_cast<MmuMemory *>(context)->write_port(address, value);
}

SrhStatus SRH_CALL create(const ShouryoHost *host, SrhHandle owner, const SrhConfig *config,
                          void **result) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!srz80::sdk::valid(host) || !srz80::sdk::valid(config) || !result || !config->space ||
            !config->image || config->image_size != memory_size || !host->query)
            return SRH_INVALID;
        const void *extension = nullptr;
        if (host->query(host->context, "host.resources.v1", &extension) != SRH_OK || !extension)
            return SRH_UNAVAILABLE;
        const auto resources = static_cast<const SrhHostResourcesV1 *>(extension);
        if (!srz80::sdk::valid(resources))
            return SRH_UNAVAILABLE;
        SrhHandle io_space = 0;
        const auto io_name = MmuMemory::io_space_name(config);
        if (resources->lookup(resources->context, "space", io_name.c_str(), &io_space) != SRH_OK ||
            !io_space)
            return SRH_NOT_FOUND;

        auto memory = std::make_unique<MmuMemory>();
        memory->rom.assign(config->image, config->image + memory_size);
        SrhMapping memory_mapping{SRH_INIT(SrhMapping), config->space, 0, 0xffff, config->priority,
                                  memory.get(), read_memory, write_memory, read_memory, nullptr};
        SrhHandle memory_handle = 0;
        auto status = host->map(host->context, owner, &memory_mapping, &memory_handle);
        if (status != SRH_OK)
            return status;
        SrhMapping io_mapping{SRH_INIT(SrhMapping), io_space, io_first, io_first + 1,
                              config->priority, memory.get(), nullptr, write_port, nullptr, nullptr};
        SrhHandle io_handle = 0;
        status = host->map(host->context, owner, &io_mapping, &io_handle);
        if (status != SRH_OK) {
            host->unmap(host->context, memory_handle);
            return status;
        }
        *result = memory.release();
        return SRH_OK;
    });
}

void SRH_CALL destroy(void *context) { delete static_cast<MmuMemory *>(context); }
SrhStatus SRH_CALL reset(void *context, uint32_t) {
    static_cast<MmuMemory *>(context)->bank_latch = 0;
    return SRH_OK;
}
uint32_t SRH_CALL property_count(void *) { return 0; }
SrhStatus SRH_CALL property_info(void *, uint32_t, SrhProperty *) { return SRH_NOT_FOUND; }
SrhStatus SRH_CALL property_get(void *, uint32_t, SrhValue *) { return SRH_NOT_FOUND; }
SrhStatus SRH_CALL property_set(void *, uint32_t, const SrhValue *) { return SRH_NOT_FOUND; }

SrhStatus SRH_CALL save_state(void *context, uint8_t *buffer, uint64_t *size) {
    if (!size)
        return SRH_INVALID;
    constexpr uint64_t state_size = memory_size + 1;
    if (!buffer) {
        *size = state_size;
        return SRH_OK;
    }
    if (*size < state_size) {
        *size = state_size;
        return SRH_UNAVAILABLE;
    }
    auto &memory = *static_cast<MmuMemory *>(context);
    buffer[0] = memory.bank_latch;
    std::memcpy(buffer + 1, memory.ram.data(), memory.ram.size());
    *size = state_size;
    return SRH_OK;
}
SrhStatus SRH_CALL load_state(void *context, const uint8_t *buffer, uint64_t size) {
    if (!buffer || size != memory_size + 1)
        return SRH_INVALID;
    auto &memory = *static_cast<MmuMemory *>(context);
    memory.bank_latch = buffer[0] & 0x3e;
    std::memcpy(memory.ram.data(), buffer + 1, memory.ram.size());
    return SRH_OK;
}

const SrhCardDescriptor descriptor{
    SRH_INIT(SrhCardDescriptor), "Memory", "Banked 512K RAM + 512K ROM",
    "SC714-compatible banked memory module", 0, 0x10000, 0, 256, 0,
    SRH_CARD_REQUIRES_IMAGE | SRH_CARD_REQUIRES_IO_SPACE, R"({"io_space":"cpu0.io"})", "io_space", nullptr};
const SrhPlugin api{SRH_INIT(SrhPlugin), "mmu_memory", create, destroy, reset,
                    property_count, property_info, property_get, property_set,
                    save_state, load_state, &descriptor};
} // namespace

extern "C" SRH_EXPORT const SrhPlugin *SRH_CALL srz80_plugin_init(const ShouryoHost *host) {
    return srz80::sdk::valid(host) ? &api : nullptr;
}
