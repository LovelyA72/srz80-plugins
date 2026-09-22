#include <state.hpp>
#include <boundary.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace {
using Json = nlohmann::json;

constexpr uint64_t kControlSize = 0x6000;
constexpr uint64_t kMaximumRomBanks = 512;
constexpr uint64_t kMaximumRamBanks = 16;
constexpr uint64_t kMaximumBackingSize = 64 * 1024 * 1024;
constexpr size_t kStateHeaderSize = 8;

struct Range {
    uint64_t first = 0;
    uint64_t last = 0;

    uint64_t size() const { return last - first + 1; }
    bool contains(uint64_t address) const { return address >= first && address <= last; }
};

bool overlaps(const Range &left, const Range &right) {
    return left.first <= right.last && right.first <= left.last;
}

bool add(uint64_t left, uint64_t right, uint64_t &result) {
    if (left > std::numeric_limits<uint64_t>::max() - right)
        return false;
    result = left + right;
    return true;
}

bool parse_range(const Json &json, const char *key, Range fallback, Range &result) {
    if (!json.contains(key)) {
        result = fallback;
        return true;
    }
    const auto &value = json[key];
    if (!value.is_array() || value.size() != 2 || !value[0].is_number_unsigned() ||
        !value[1].is_number_unsigned())
        return false;
    result = {value[0].get<uint64_t>(), value[1].get<uint64_t>()};
    return result.first <= result.last && result.last - result.first != UINT64_MAX;
}

void store_u32(uint8_t *out, uint32_t value) {
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8);
    out[2] = static_cast<uint8_t>(value >> 16);
    out[3] = static_cast<uint8_t>(value >> 24);
}

uint32_t load_u32(const uint8_t *in) {
    return static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8) |
           (static_cast<uint32_t>(in[2]) << 16) | (static_cast<uint32_t>(in[3]) << 24);
}

struct Mbc5 {
    uint64_t control_base = 0;
    Range rom_range;
    Range ram_range;
    uint64_t rom_bank_size = 0;
    uint64_t ram_bank_size = 0;
    std::vector<uint8_t> rom;
    std::vector<uint8_t> ram;
    uint16_t rom_bank = 1;
    uint8_t ram_bank = 0;
    bool ram_enabled = false;

    SrhStatus read_rom(uint64_t address, uint8_t *value) const {
        if (!value || !rom_range.contains(address))
            return SRH_INVALID;
        const uint64_t relative = address - rom_range.first;
        const uint64_t offset = relative < rom_bank_size
                                    ? relative
                                    : uint64_t{rom_bank} * rom_bank_size + relative - rom_bank_size;
        *value = offset < rom.size() ? rom[static_cast<size_t>(offset)] : uint8_t{0xff};
        return SRH_OK;
    }

    SrhStatus write_control(uint64_t address, uint8_t value) {
        if (address < control_base || address - control_base >= kControlSize)
            return SRH_INVALID;
        const uint64_t offset = address - control_base;
        if (offset < 0x2000)
            ram_enabled = (value & 0x0f) == 0x0a;
        else if (offset < 0x3000)
            rom_bank = static_cast<uint16_t>((rom_bank & 0x100) | value);
        else if (offset < 0x4000)
            rom_bank = static_cast<uint16_t>((rom_bank & 0x0ff) | ((value & 1u) << 8));
        else
            ram_bank = value & 0x0f;
        return SRH_OK;
    }

    SrhStatus read_ram(uint64_t address, uint8_t *value) const {
        if (!value || !ram_range.contains(address))
            return SRH_INVALID;
        if (!ram_enabled) {
            *value = 0xff;
            return SRH_OK;
        }
        const uint64_t offset = uint64_t{ram_bank} * ram_bank_size + address - ram_range.first;
        *value = offset < ram.size() ? ram[static_cast<size_t>(offset)] : uint8_t{0xff};
        return SRH_OK;
    }

    SrhStatus write_ram(uint64_t address, uint8_t value) {
        if (!ram_range.contains(address))
            return SRH_INVALID;
        if (ram_enabled) {
            const uint64_t offset = uint64_t{ram_bank} * ram_bank_size + address - ram_range.first;
            if (offset < ram.size())
                ram[static_cast<size_t>(offset)] = value;
        }
        return SRH_OK;
    }
};

SrhStatus SRH_CALL read_rom(void *context, uint64_t address, uint8_t *value) {
    return static_cast<Mbc5 *>(context)->read_rom(address, value);
}
SrhStatus SRH_CALL write_control(void *context, uint64_t address, uint8_t value) {
    return static_cast<Mbc5 *>(context)->write_control(address, value);
}
SrhStatus SRH_CALL read_ram(void *context, uint64_t address, uint8_t *value) {
    return static_cast<Mbc5 *>(context)->read_ram(address, value);
}
SrhStatus SRH_CALL write_ram(void *context, uint64_t address, uint8_t value) {
    return static_cast<Mbc5 *>(context)->write_ram(address, value);
}

SrhStatus SRH_CALL create(const ShouryoHost *host, SrhHandle owner, const SrhConfig *config,
                          void **result) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!srz80::sdk::valid(host) || !srz80::sdk::valid(config) || !result || !config->space ||
            !host->map || !host->unmap || !config->image || !config->image_size ||
            config->image_size > kMaximumBackingSize)
            return SRH_INVALID;

        Json json = Json::object();
        if (srz80::sdk::has_field(config, &SrhConfig::config_json) && config->config_json) {
            json = Json::parse(config->config_json, config->config_json + config->config_json_size,
                               nullptr, false);
            if (json.is_discarded() || !json.is_object())
                return SRH_INVALID;
        }
        static constexpr std::array<const char *, 3> allowed{"rom_range", "ram_range",
                                                              "ram_banks"};
        for (const auto &[key, value] : json.items()) {
            (void)value;
            if (std::find_if(allowed.begin(), allowed.end(), [&](const char *candidate) {
                    return key == candidate;
                }) == allowed.end())
                return SRH_INVALID;
        }

        uint64_t default_rom_last = 0;
        uint64_t default_ram_first = 0;
        uint64_t default_ram_last = 0;
        if (!add(config->base, 0x7fff, default_rom_last) ||
            !add(config->base, 0xa000, default_ram_first) ||
            !add(config->base, 0xbfff, default_ram_last))
            return SRH_INVALID;
        Range rom_range;
        Range ram_range;
        if (!parse_range(json, "rom_range", {config->base, default_rom_last}, rom_range) ||
            !parse_range(json, "ram_range", {default_ram_first, default_ram_last}, ram_range))
            return SRH_INVALID;
        const uint64_t rom_window_size = rom_range.size();
        const uint64_t ram_bank_size = ram_range.size();
        if (rom_window_size < 2 || (rom_window_size & 1) || overlaps(rom_range, ram_range))
            return SRH_INVALID;
        const uint64_t rom_bank_size = rom_window_size / 2;
        if (rom_bank_size > kMaximumBackingSize || config->image_size % rom_bank_size != 0 ||
            config->image_size / rom_bank_size > kMaximumRomBanks ||
            ram_bank_size > kMaximumBackingSize)
            return SRH_INVALID;
        uint64_t ram_banks = 16;
        if (json.contains("ram_banks")) {
            if (!json["ram_banks"].is_number_unsigned())
                return SRH_INVALID;
            ram_banks = json["ram_banks"].get<uint64_t>();
        }
        if (ram_banks > kMaximumRamBanks ||
            (ram_banks && ram_bank_size > kMaximumBackingSize / ram_banks))
            return SRH_INVALID;
        uint64_t control_last = 0;
        if (!add(config->base, kControlSize - 1, control_last))
            return SRH_INVALID;
        if (overlaps({config->base, control_last}, ram_range))
            return SRH_INVALID;

        auto mbc = std::make_unique<Mbc5>();
        mbc->control_base = config->base;
        mbc->rom_range = rom_range;
        mbc->ram_range = ram_range;
        mbc->rom_bank_size = rom_bank_size;
        mbc->ram_bank_size = ram_bank_size;
        mbc->rom.assign(config->image, config->image + config->image_size);
        mbc->ram.resize(static_cast<size_t>(ram_banks * ram_bank_size));

        std::vector<SrhHandle> mappings;
        auto register_mapping = [&](uint64_t first, uint64_t last, SrhRead read, SrhWrite write,
                                    SrhRead peek) {
            SrhMapping mapping{SRH_INIT(SrhMapping), config->space, first, last, config->priority,
                               mbc.get(), read, write, peek, nullptr};
            SrhHandle handle = 0;
            const auto status = host->map(host->context, owner, &mapping, &handle);
            if (status == SRH_OK)
                mappings.push_back(handle);
            return status;
        };
        auto status = register_mapping(rom_range.first, rom_range.last, read_rom, nullptr, read_rom);
        if (status == SRH_OK)
            status = register_mapping(config->base, control_last, nullptr, write_control, nullptr);
        if (status == SRH_OK)
            status = register_mapping(ram_range.first, ram_range.last, read_ram, write_ram, read_ram);
        if (status != SRH_OK) {
            for (auto handle : mappings)
                host->unmap(host->context, handle);
            return status;
        }
        *result = mbc.release();
        return SRH_OK;
    });
}

void SRH_CALL destroy(void *context) { delete static_cast<Mbc5 *>(context); }

SrhStatus SRH_CALL reset(void *context, uint32_t cold) {
    auto &mbc = *static_cast<Mbc5 *>(context);
    mbc.rom_bank = 1;
    mbc.ram_bank = 0;
    mbc.ram_enabled = false;
    if (cold)
        std::fill(mbc.ram.begin(), mbc.ram.end(), uint8_t{0});
    return SRH_OK;
}

uint32_t SRH_CALL property_count(void *) { return 7; }

SrhStatus SRH_CALL property_info(void *, uint32_t index, SrhProperty *out) {
    if (!srz80::sdk::valid(out) || index >= 7)
        return SRH_INVALID;
    static const char *names[]{"control_base", "rom_first", "rom_last", "ram_first",
                               "ram_last", "rom_bank", "ram_bank"};
    static const char *descriptions[]{"First MBC5 control-register address",
                                      "First mapped ROM address", "Last mapped ROM address",
                                      "First mapped external-RAM address",
                                      "Last mapped external-RAM address", "Selected ROM bank",
                                      "Selected external-RAM bank"};
    static const uint32_t bits[]{64, 64, 64, 64, 64, 9, 4};
    *out = {SRH_INIT(SrhProperty), names[index], "MBC5", descriptions[index], SRH_UNSIGNED,
            bits[index], 16, 0, nullptr, 0};
    return SRH_OK;
}

SrhStatus SRH_CALL property_get(void *context, uint32_t index, SrhValue *out) {
    if (!srz80::sdk::valid(out) || index >= 7)
        return SRH_INVALID;
    const auto &mbc = *static_cast<Mbc5 *>(context);
    const uint64_t values[]{mbc.control_base, mbc.rom_range.first, mbc.rom_range.last,
                            mbc.ram_range.first, mbc.ram_range.last, mbc.rom_bank, mbc.ram_bank};
    out->unsigned_value = values[index];
    return SRH_OK;
}

SrhStatus SRH_CALL property_set(void *, uint32_t, const SrhValue *) { return SRH_INVALID; }

SrhStatus SRH_CALL save_payload(void *context, uint8_t *buffer, uint64_t *size) {
    if (!size)
        return SRH_INVALID;
    const auto &mbc = *static_cast<Mbc5 *>(context);
    const uint64_t required = kStateHeaderSize + mbc.ram.size();
    if (!buffer) {
        *size = required;
        return SRH_OK;
    }
    if (*size < required) {
        *size = required;
        return SRH_UNAVAILABLE;
    }
    buffer[0] = mbc.ram_enabled ? 1 : 0;
    buffer[1] = static_cast<uint8_t>(mbc.rom_bank);
    buffer[2] = static_cast<uint8_t>(mbc.rom_bank >> 8);
    buffer[3] = mbc.ram_bank;
    store_u32(buffer + 4, static_cast<uint32_t>(mbc.ram.size()));
    if (!mbc.ram.empty())
        std::memcpy(buffer + kStateHeaderSize, mbc.ram.data(), mbc.ram.size());
    *size = required;
    return SRH_OK;
}

SrhStatus SRH_CALL load_payload(void *context, const uint8_t *buffer, uint64_t size) {
    auto &mbc = *static_cast<Mbc5 *>(context);
    if (!buffer || size != kStateHeaderSize + mbc.ram.size() ||
        buffer[0] > 1 || buffer[2] > 1 || buffer[3] > 0x0f ||
        load_u32(buffer + 4) != mbc.ram.size())
        return SRH_INVALID;
    const uint16_t new_rom_bank = static_cast<uint16_t>(buffer[1] | (buffer[2] << 8));
    const uint8_t new_ram_bank = buffer[3];
    const bool new_ram_enabled = buffer[0] != 0;
    if (!mbc.ram.empty())
        std::memcpy(mbc.ram.data(), buffer + kStateHeaderSize, mbc.ram.size());
    mbc.rom_bank = new_rom_bank;
    mbc.ram_bank = new_ram_bank;
    mbc.ram_enabled = new_ram_enabled;
    return SRH_OK;
}

const SrhCardDescriptor descriptor{
    SRH_INIT(SrhCardDescriptor), "Memory", "MBC5 memory mapper",
    "Relocatable MBC5-style ROM and external-RAM banking", 0, 0x8000, 0, 0, 0,
    SRH_CARD_REQUIRES_IMAGE,
    R"({"rom_range":[0,32767],"ram_range":[40960,49151],"ram_banks":16})", nullptr,
    nullptr, nullptr, 0};
using State = srz80::sdk::state::Callbacks<save_payload, load_payload, 1>;
const SrhPlugin api{SRH_INIT(SrhPlugin), "mbc5", create, destroy, reset,
                    property_count, property_info, property_get, property_set,
                    State::save, State::load, &descriptor, nullptr, nullptr};
} // namespace

extern "C" SRH_EXPORT const SrhPlugin *SRH_CALL srz80_plugin_init(const ShouryoHost *host) {
    return srz80::sdk::valid(host) ? &api : nullptr;
}
