#include <boundary.hpp>
#include <nlohmann/json.hpp>

#include "x1_010/x1_010.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>

namespace {
using Json = nlohmann::json;

constexpr uint64_t kRegisterWindow = 0x2000;
constexpr uint64_t kSampleMemorySize = 0x100000;
constexpr uint32_t kDefaultClockHz = 16000000;
constexpr uint32_t kDefaultSampleRate = 44100;
constexpr uint64_t kChipTicksPerSample = 512;

struct Settings {
    uint32_t chip_clock_hz = kDefaultClockHz;
    uint32_t sample_rate = kDefaultSampleRate;
    uint64_t sample_base = 0x100000;
    std::string sample_space;
    std::string stream_name = "X1-010";
};

bool read_unsigned(const Json &value, uint64_t &out) {
    if (value.is_number_unsigned()) {
        out = value.get<uint64_t>();
        return true;
    }
    if (value.is_number_integer()) {
        const auto number = value.get<int64_t>();
        if (number < 0)
            return false;
        out = static_cast<uint64_t>(number);
        return true;
    }
    if (!value.is_string())
        return false;
    const auto text = value.get<std::string>();
    if (text.empty())
        return false;
    size_t position = 0;
    try {
        out = std::stoull(text, &position, 0);
    } catch (...) {
        return false;
    }
    return position == text.size();
}

bool parse_settings(const SrhConfig *config, Settings &settings) {
    if (!srz80::sdk::has_field(config, &SrhConfig::config_json) || !config->config_json ||
        config->config_json_size == 0)
        return true;
    const auto json = Json::parse(config->config_json,
                                  config->config_json + config->config_json_size, nullptr, false);
    if (json.is_discarded() || !json.is_object())
        return false;
    for (const auto &[key, value] : json.items()) {
        uint64_t number = 0;
        if (key == "chip_clock_hz") {
            if (!read_unsigned(value, number) || number < 1000000 || number > 50000000)
                return false;
            settings.chip_clock_hz = static_cast<uint32_t>(number);
        } else if (key == "sample_rate") {
            if (!read_unsigned(value, number) || number < 8000 || number > 192000)
                return false;
            settings.sample_rate = static_cast<uint32_t>(number);
        } else if (key == "sample_base") {
            if (!read_unsigned(value, settings.sample_base))
                return false;
        } else if (key == "sample_space") {
            if (!value.is_string())
                return false;
            settings.sample_space = value.get<std::string>();
            if (settings.sample_space.size() > 256)
                return false;
        } else if (key == "stream_name") {
            if (!value.is_string())
                return false;
            settings.stream_name = value.get<std::string>();
            if (settings.stream_name.empty() || settings.stream_name.size() > 256)
                return false;
        } else {
            return false;
        }
    }
    return settings.sample_base <= UINT64_MAX - (kSampleMemorySize - 1);
}

struct Card final : vgsound_emu::vgsound_emu_mem_intf {
    explicit Card(const ShouryoHost *host_value) : host(host_value), chip(*this) {}

    vgsound_emu::u8 read_byte(vgsound_emu::u32 address) override {
        if (address >= kSampleMemorySize || !value_host_readable()) {
            sample_error = SRH_INVALID;
            return 0;
        }
        uint8_t value = 0;
        sample_error = host->read(host->context, owner, sample_space,
                                  sample_base + static_cast<uint64_t>(address), &value);
        return sample_error == SRH_OK ? value : 0;
    }

    bool value_host_readable() const {
        return host && host->read && sample_space != 0;
    }

    SrhStatus read_register(uint64_t address, uint8_t *value) {
        if (!value || address < base || address - base >= kRegisterWindow)
            return SRH_INVALID;
        *value = chip.ram_r(static_cast<vgsound_emu::u16>(address - base));
        return SRH_OK;
    }

    SrhStatus write_register(uint64_t address, uint8_t value) {
        if (address < base || address - base >= kRegisterWindow)
            return SRH_INVALID;
        chip.ram_w(static_cast<vgsound_emu::u16>(address - base), value);
        return SRH_OK;
    }

    void reset_chip() {
        chip.reset();
        clock_accum = 0;
        sample_error = SRH_OK;
    }

    const ShouryoHost *host = nullptr;
    SrhHandle owner = 0;
    SrhHandle space = 0;
    SrhHandle sample_space = 0;
    SrhHandle mapping = 0;
    SrhHandle stream = 0;
    uint64_t base = 0;
    uint64_t sample_base = 0;
    uint32_t chip_clock_hz = kDefaultClockHz;
    uint32_t sample_rate = kDefaultSampleRate;
    uint64_t clock_accum = 0;
    SrhStatus sample_error = SRH_OK;
    x1_010_core chip;
};

SrhStatus SRH_CALL read_register(void *context, uint64_t address, uint8_t *value) {
    return static_cast<Card *>(context)->read_register(address, value);
}

SrhStatus SRH_CALL write_register(void *context, uint64_t address, uint8_t value) {
    return static_cast<Card *>(context)->write_register(address, value);
}

SrhStatus SRH_CALL render(void *context, uint64_t, uint32_t frames, int16_t *interleaved) {
    if (!context || !interleaved)
        return SRH_INVALID;
    auto &card = *static_cast<Card *>(context);
    card.sample_error = SRH_OK;
    const uint64_t denominator = uint64_t(card.sample_rate) * kChipTicksPerSample;
    for (uint32_t frame = 0; frame < frames; ++frame) {
        card.clock_accum += card.chip_clock_hz;
        while (card.clock_accum >= denominator) {
            card.clock_accum -= denominator;
            card.chip.tick();
        }
        const auto left = std::clamp(
            card.chip.output(0), static_cast<vgsound_emu::s32>(std::numeric_limits<int16_t>::min()),
            static_cast<vgsound_emu::s32>(std::numeric_limits<int16_t>::max()));
        const auto right = std::clamp(
            card.chip.output(1), static_cast<vgsound_emu::s32>(std::numeric_limits<int16_t>::min()),
            static_cast<vgsound_emu::s32>(std::numeric_limits<int16_t>::max()));
        interleaved[static_cast<size_t>(frame) * 2] = static_cast<int16_t>(left);
        interleaved[static_cast<size_t>(frame) * 2 + 1] = static_cast<int16_t>(right);
    }
    return card.sample_error;
}

SrhStatus SRH_CALL create(const ShouryoHost *host, SrhHandle owner, const SrhConfig *config,
                          void **result) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!srz80::sdk::valid(host) || !srz80::sdk::valid(config) || !result || !config->space ||
            !host->map || !host->read || config->size != kRegisterWindow ||
            config->base > UINT64_MAX - (kRegisterWindow - 1))
            return SRH_INVALID;

        Settings settings;
        if (!parse_settings(config, settings))
            return SRH_INVALID;
        if (settings.sample_base > UINT64_MAX - (kSampleMemorySize - 1))
            return SRH_INVALID;

        const void *extension = nullptr;
        if (!host->query || host->query(host->context, "host.audio.v1", &extension) != SRH_OK ||
            !extension)
            return SRH_UNAVAILABLE;
        const auto audio = static_cast<const SrhHostAudioV1 *>(extension);
        if (!srz80::sdk::valid(audio) || !audio->register_source || audio->channels < 2 ||
            audio->format != SRH_AUDIO_S16_STEREO)
            return SRH_UNAVAILABLE;
        if (settings.sample_rate != audio->sample_rate)
            return SRH_INVALID;

        SrhHandle sample_space = config->space;
        if (!settings.sample_space.empty()) {
            extension = nullptr;
            if (host->query(host->context, "host.resources.v1", &extension) != SRH_OK ||
                !extension)
                return SRH_UNAVAILABLE;
            const auto resources = static_cast<const SrhHostResourcesV1 *>(extension);
            if (!srz80::sdk::valid(resources) || !resources->lookup)
                return SRH_UNAVAILABLE;
            if (resources->lookup(resources->context, "space", settings.sample_space.c_str(),
                                  &sample_space) != SRH_OK ||
                !sample_space)
                return SRH_NOT_FOUND;
        }

        auto card = std::make_unique<Card>(host);
        card->owner = owner;
        card->space = config->space;
        card->sample_space = sample_space;
        card->base = config->base;
        card->sample_base = settings.sample_base;
        card->chip_clock_hz = settings.chip_clock_hz;
        card->sample_rate = settings.sample_rate;
        card->reset_chip();

        SrhMapping mapping{SRH_INIT(SrhMapping), config->space, config->base,
                           config->base + kRegisterWindow - 1, config->priority, card.get(),
                           read_register, write_register, read_register, nullptr};
        auto status = host->map(host->context, owner, &mapping, &card->mapping);
        if (status != SRH_OK)
            return status;
        status = audio->register_source(audio->context, owner, settings.sample_rate, 2,
                                        SRH_AUDIO_S16_STEREO, settings.stream_name.c_str(), render,
                                        card.get(), &card->stream);
        if (status != SRH_OK) {
            if (host->unmap)
                host->unmap(host->context, card->mapping);
            return status;
        }
        *result = card.release();
        return SRH_OK;
    });
}

void SRH_CALL destroy(void *context) {
    delete static_cast<Card *>(context);
}

SrhStatus SRH_CALL reset(void *context, uint32_t) {
    if (!context)
        return SRH_INVALID;
    static_cast<Card *>(context)->reset_chip();
    return SRH_OK;
}

uint32_t SRH_CALL property_count(void *) { return 0; }
SrhStatus SRH_CALL property_info(void *, uint32_t, SrhProperty *) { return SRH_NOT_FOUND; }
SrhStatus SRH_CALL property_get(void *, uint32_t, SrhValue *) { return SRH_NOT_FOUND; }
SrhStatus SRH_CALL property_set(void *, uint32_t, const SrhValue *) { return SRH_NOT_FOUND; }

SrhStatus SRH_CALL save_state(void *context, uint8_t *buffer, uint64_t *size) {
    if (!context || !size)
        return SRH_INVALID;
    constexpr uint64_t required = sizeof(uint64_t) + x1_010_core::serialized_size;
    if (!buffer) {
        *size = required;
        return SRH_OK;
    }
    if (*size < required) {
        *size = required;
        return SRH_UNAVAILABLE;
    }
    auto &card = *static_cast<Card *>(context);
    std::memcpy(buffer, &card.clock_accum, sizeof(card.clock_accum));
    card.chip.save_state(buffer + sizeof(card.clock_accum), x1_010_core::serialized_size);
    *size = required;
    return SRH_OK;
}

SrhStatus SRH_CALL load_state(void *context, const uint8_t *buffer, uint64_t size) {
    constexpr uint64_t required = sizeof(uint64_t) + x1_010_core::serialized_size;
    if (!context || !buffer || size != required)
        return SRH_INVALID;
    auto &card = *static_cast<Card *>(context);
    if (!card.chip.load_state(buffer + sizeof(card.clock_accum), x1_010_core::serialized_size))
        return SRH_INVALID;
    std::memcpy(&card.clock_accum, buffer, sizeof(card.clock_accum));
    card.sample_error = SRH_OK;
    return SRH_OK;
}

const SrhCardDescriptor descriptor{
    SRH_INIT(SrhCardDescriptor),
    "Audio",
    "X1-010",
    "Seta/Allumer X1-010 16-voice wavetable and PCM audio card",
    0x20000,
    kRegisterWindow,
    0,
    0,
    0,
    0,
    R"({"chip_clock_hz":16000000,"sample_rate":44100,"sample_base":1048576,"stream_name":"X1-010"})",
    nullptr,
    nullptr,
    nullptr,
    0};

const SrhPlugin api{SRH_INIT(SrhPlugin), "x1_010", create, destroy, reset,
                    property_count, property_info, property_get, property_set,
                    save_state, load_state, &descriptor, nullptr, nullptr};
} // namespace

extern "C" SRH_EXPORT const SrhPlugin *SRH_CALL srz80_plugin_init(const ShouryoHost *host) {
    return srz80::sdk::valid(host) ? &api : nullptr;
}
