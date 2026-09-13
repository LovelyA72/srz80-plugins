#include <boundary.hpp>

#include "ymw258.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace {
constexpr uint32_t port_count = 3;
constexpr uint32_t default_clock_hz = 9'878'400;
constexpr uint32_t config_properties = 3;
constexpr uint32_t raw_registers = srz80::ymw258::Engine::voice_count * srz80::ymw258::Engine::register_count;
constexpr uint32_t interface_properties = 3;
constexpr uint32_t voice_state_fields = 14;
constexpr uint32_t interface_start = config_properties + raw_registers;
constexpr uint32_t voice_state_start = interface_start + interface_properties;
constexpr uint32_t property_count =
    voice_state_start + srz80::ymw258::Engine::voice_count * voice_state_fields;

struct Settings {
    uint32_t chip_clock_hz = default_clock_hz;
    uint32_t sample_rate = 44'100;
    std::string stream_name = "YMW258";
};

bool parse_settings(const SrhConfig *config, Settings &settings) {
    try {
        const bool has_json = srz80::sdk::has_field(config, &SrhConfig::config_json) && config->config_json;
        auto json = has_json
                        ? nlohmann::json::parse(config->config_json,
                                                config->config_json + config->config_json_size)
                        : nlohmann::json::object();
        if (!json.is_object()) return false;
        for (auto it = json.begin(); it != json.end(); ++it)
            if (it.key() != "chip_clock_hz" && it.key() != "sample_rate" && it.key() != "stream_name") return false;
        settings.chip_clock_hz = json.value("chip_clock_hz", settings.chip_clock_hz);
        settings.sample_rate = json.value("sample_rate", settings.sample_rate);
        settings.stream_name = json.value("stream_name", settings.stream_name);
        return settings.chip_clock_hz >= 1'000'000 && settings.chip_clock_hz <= 50'000'000 &&
               settings.sample_rate >= 8'000 && settings.sample_rate <= 192'000 &&
               !settings.stream_name.empty() && settings.stream_name.size() <= 256;
    } catch (...) {
        return false;
    }
}

struct Card {
    const ShouryoHost *host = nullptr;
    SrhHandle owner = 0, space = 0, mapping = 0, stream = 0;
    uint64_t base = 0;
    int32_t priority = 0;
    uint32_t chip_clock_hz = default_clock_hz;
    uint32_t sample_rate = 44'100;
    uint8_t selected_slot = 0;
    uint8_t selected_register = 0;
    uint8_t last_data = 0;
    srz80::ymw258::Engine engine;
    srz80::ymw258::LinearResampler resampler;

    Card(std::span<const uint8_t> image, uint32_t clock, uint32_t rate)
        : chip_clock_hz(clock), sample_rate(rate), engine(image, clock), resampler(engine, clock, rate) {}

    void reset() {
        selected_slot = selected_register = last_data = 0;
        engine.reset();
        resampler.reset();
    }

    SrhStatus write(uint64_t address, uint8_t value) {
        if (address < base || address - base >= port_count) return SRH_INVALID;
        switch (address - base) {
        case 0:
            last_data = value;
            engine.write(selected_slot, selected_register, value);
            break;
        case 1: selected_slot = value & 0x1f; break;
        case 2: selected_register = std::min<uint8_t>(value, 7); break;
        }
        return SRH_OK;
    }
};

SrhStatus SRH_CALL read(void *context, uint64_t address, uint8_t *value) {
    auto &card = *static_cast<Card *>(context);
    if (!value || address < card.base || address - card.base >= port_count) return SRH_INVALID;
    *value = 0;
    return SRH_OK;
}

SrhStatus SRH_CALL write(void *context, uint64_t address, uint8_t value) {
    return static_cast<Card *>(context)->write(address, value);
}

SrhStatus SRH_CALL render(void *context, uint64_t, uint32_t frames, int16_t *interleaved) {
    if (!interleaved) return SRH_INVALID;
    static_cast<Card *>(context)->resampler.render(frames, interleaved);
    return SRH_OK;
}

SrhStatus SRH_CALL create(const ShouryoHost *host, SrhHandle owner, const SrhConfig *config, void **result) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!srz80::sdk::valid(host) || !srz80::sdk::valid(config) || !result || !config->space ||
            config->size != port_count ||
            config->base > UINT64_MAX - (port_count - 1)) return SRH_INVALID;
        std::vector<uint8_t> rom;
        const bool has_parts = srz80::sdk::has_field(config, &SrhConfig::image_count) &&
                               config->image_count != 0;
        if (has_parts) {
            if (!config->images || config->image_count > 4) return SRH_INVALID;
            bool empty_seen = false;
            for (uint32_t index = 0; index < config->image_count; ++index) {
                const auto &part = config->images[index];
                if (!srz80::sdk::valid(&part)) return SRH_INVALID;
                if (!part.size) {
                    empty_seen = true;
                    continue;
                }
                if (empty_seen || !part.data ||
                    part.size > srz80::ymw258::Engine::address_space_size - rom.size())
                    return SRH_INVALID;
                rom.insert(rom.end(), part.data, part.data + part.size);
            }
        } else {
            if (!config->image || !config->image_size ||
                config->image_size > srz80::ymw258::Engine::address_space_size)
                return SRH_INVALID;
            rom.assign(config->image, config->image + config->image_size);
        }
        if (rom.empty()) return SRH_INVALID;
        Settings settings;
        if (!parse_settings(config, settings)) return SRH_INVALID;
        const void *extension = nullptr;
        if (!host->query || host->query(host->context, "host.audio.v1", &extension) != SRH_OK || !extension)
            return SRH_UNAVAILABLE;
        const auto *audio = static_cast<const SrhHostAudioV1 *>(extension);
        if (!srz80::sdk::valid(audio) || !audio->register_source || settings.sample_rate != audio->sample_rate)
            return SRH_INVALID;
        auto card = std::make_unique<Card>(rom, settings.chip_clock_hz, settings.sample_rate);
        card->host = host;
        card->owner = owner;
        card->space = config->space;
        card->base = config->base;
        card->priority = config->priority;
        SrhMapping mapping{SRH_INIT(SrhMapping), config->space, config->base,
                           config->base + port_count - 1, config->priority, card.get(), read, write, read, nullptr};
        auto status = host->map(host->context, owner, &mapping, &card->mapping);
        if (status != SRH_OK) return status;
        status = audio->register_source(audio->context, owner, settings.sample_rate, 2,
                                        SRH_AUDIO_S16_STEREO, settings.stream_name.c_str(), render,
                                        card.get(), &card->stream);
        if (status != SRH_OK) return status;
        *result = card.release();
        return SRH_OK;
    });
}

void SRH_CALL destroy(void *context) { delete static_cast<Card *>(context); }
SrhStatus SRH_CALL reset(void *context, uint32_t) { static_cast<Card *>(context)->reset(); return SRH_OK; }
uint32_t SRH_CALL count(void *) { return property_count; }

SrhStatus SRH_CALL info(void *, uint32_t index, SrhProperty *out) {
    if (!srz80::sdk::valid(out) || index >= property_count) return SRH_INVALID;
    if (index < config_properties) {
        static const char *names[] = {"base", "chip_clock_hz", "sample_rate"};
        static const char *descriptions[] = {"First MultiPCM port", "YMW258 input clock", "Host output rate"};
        *out = {SRH_INIT(SrhProperty), names[index], "YMW258", descriptions[index], SRH_UNSIGNED,
                index == 0 ? 16u : 32u, index == 0 ? 16u : 10u, 0, nullptr,
                index ? SRH_PROPERTY_PERSISTENT : SRH_PROPERTY_RUNTIME};
        return SRH_OK;
    }
    if (index < config_properties + raw_registers) {
        const uint32_t raw = index - config_properties;
        static thread_local char name[32], group[24], description[80];
        std::snprintf(name, sizeof(name), "V%02u.R%u", raw / 8, raw % 8);
        std::snprintf(group, sizeof(group), "Voice %u", raw / 8 + 1);
        std::snprintf(description, sizeof(description), "YMW258 voice %u register %u", raw / 8,
                      raw % 8);
        *out = {SRH_INIT(SrhProperty), name, group, description, SRH_UNSIGNED, 8, 16, 0, nullptr,
                SRH_PROPERTY_RUNTIME | SRH_PROPERTY_HIDE_UI};
        return SRH_OK;
    }
    if (index < voice_state_start) {
        const uint32_t item = index - interface_start;
        static const char *names[] = {"Data", "Slot", "Register"};
        static const char *descriptions[] = {"Last value written to the data port", "Encoded hardware slot selector",
                                             "Voice register selector"};
        *out = {SRH_INIT(SrhProperty), names[item], "Interface", descriptions[item], SRH_UNSIGNED, 8, 16, 0,
                nullptr, SRH_PROPERTY_RUNTIME};
        return SRH_OK;
    }

    const uint32_t relative = index - voice_state_start;
    const uint32_t voice = relative / voice_state_fields;
    const uint32_t field = relative % voice_state_fields;
    static const char *suffixes[] = {"Start", "Loop", "Length", "Envelope", "TotalLevel", "Output",
                                     "Attack", "Decay1", "Decay2", "DecayLevel", "RateCorrection",
                                     "Release", "EnvelopeStage", "Playing"};
    static const char *descriptions[] = {"Sample start address", "Sample loop offset", "Sample length",
                                         "Current envelope level", "Current total-level attenuation",
                                         "Recent post-envelope output peak", "Attack rate", "First decay rate",
                                         "Second decay rate", "Decay level", "Key-rate scaling",
                                         "Release rate", "Current envelope stage", "Voice playback state"};
    static thread_local char name[48], group[24], description[96];
    std::snprintf(name, sizeof(name), "V%02u.%s", voice, suffixes[field]);
    std::snprintf(group, sizeof(group), "Voice %u", voice + 1);
    std::snprintf(description, sizeof(description), "Voice %u: %s", voice + 1, descriptions[field]);
    uint32_t kind = SRH_UNSIGNED;
    uint32_t bits = 8;
    uint32_t base = 10;
    const char *labels = nullptr;
    if (field <= 2) {
        bits = 22;
        base = 16;
    } else if (field == 3) {
        bits = 10;
    } else if (field == 4) {
        bits = 7;
    } else if (field == 5) {
        bits = 15;
    } else if (field == 12) {
        kind = SRH_ENUM;
        bits = 3;
        labels = "off|attack|decay|sustain|release";
    } else if (field == 13) {
        kind = SRH_BOOLEAN;
        bits = 1;
    } else {
        bits = 4;
    }
    *out = {SRH_INIT(SrhProperty), name, group, description, kind, bits, base, 0, labels,
            SRH_PROPERTY_RUNTIME | SRH_PROPERTY_HIDE_UI};
    return SRH_OK;
}

SrhStatus SRH_CALL get(void *context, uint32_t index, SrhValue *out) {
    if (!srz80::sdk::valid(out) || index >= property_count) return SRH_INVALID;
    const auto &card = *static_cast<Card *>(context);
    out->unsigned_value = 0;
    out->signed_value = 0;
    if (index == 0) out->unsigned_value = card.base;
    else if (index == 1) out->unsigned_value = card.chip_clock_hz;
    else if (index == 2) out->unsigned_value = card.sample_rate;
    else if (index < config_properties + raw_registers) {
        const uint32_t raw = index - config_properties;
        out->unsigned_value = card.engine.reg(raw / 8, raw % 8);
    } else if (index < voice_state_start) {
        const uint32_t item = index - interface_start;
        out->unsigned_value = item == 0 ? card.last_data : item == 1 ? card.selected_slot : card.selected_register;
    } else {
        const uint32_t relative = index - voice_state_start;
        const auto voice = card.engine.inspect_voice(relative / voice_state_fields);
        switch (relative % voice_state_fields) {
        case 0: out->unsigned_value = voice.start; break;
        case 1: out->unsigned_value = voice.loop; break;
        case 2: out->unsigned_value = voice.length; break;
        case 3: out->unsigned_value = voice.envelope; break;
        case 4: out->unsigned_value = voice.total_level; break;
        case 5: out->unsigned_value = voice.output_peak; break;
        case 6: out->unsigned_value = voice.attack; break;
        case 7: out->unsigned_value = voice.decay1; break;
        case 8: out->unsigned_value = voice.decay2; break;
        case 9: out->unsigned_value = voice.decay_level; break;
        case 10: out->unsigned_value = voice.rate_correction; break;
        case 11: out->unsigned_value = voice.release; break;
        case 12: out->unsigned_value = voice.envelope_stage; break;
        case 13: out->unsigned_value = voice.playing ? 1u : 0u; break;
        }
    }
    return SRH_OK;
}

SrhStatus SRH_CALL set(void *, uint32_t, const SrhValue *) { return SRH_INVALID; }

SrhStatus SRH_CALL save_state(void *context, uint8_t *buffer, uint64_t *size) {
    if (!size) return SRH_INVALID;
    const auto &card = *static_cast<Card *>(context);
    const auto engine = card.engine.save_state();
    const auto resampler = card.resampler.save_state();
    const uint64_t required = 3 + 4 + engine.size() + resampler.size();
    if (!buffer) { *size = required; return SRH_OK; }
    if (*size < required) { *size = required; return SRH_UNAVAILABLE; }
    buffer[0] = card.selected_slot;
    buffer[1] = card.selected_register;
    buffer[2] = card.last_data;
    const uint32_t engine_size = static_cast<uint32_t>(engine.size());
    for (uint32_t byte = 0; byte < 4; ++byte) buffer[3 + byte] = static_cast<uint8_t>(engine_size >> (byte * 8));
    std::memcpy(buffer + 7, engine.data(), engine.size());
    std::memcpy(buffer + 7 + engine.size(), resampler.data(), resampler.size());
    *size = required;
    return SRH_OK;
}

SrhStatus SRH_CALL load_state(void *context, const uint8_t *buffer, uint64_t size) {
    if (!buffer || size < 7) return SRH_INVALID;
    uint32_t engine_size = 0;
    for (uint32_t byte = 0; byte < 4; ++byte) engine_size |= static_cast<uint32_t>(buffer[3 + byte]) << (byte * 8);
    if (engine_size > size - 7) return SRH_INVALID;
    auto &card = *static_cast<Card *>(context);
    const auto engine_state = std::span(buffer + 7, engine_size);
    const auto resampler_state = std::span(buffer + 7 + engine_size, static_cast<size_t>(size - 7 - engine_size));
    if (!card.engine.load_state(engine_state) || !card.resampler.load_state(resampler_state)) return SRH_INVALID;
    card.selected_slot = buffer[0];
    card.selected_register = buffer[1];
    card.last_data = buffer[2];
    return SRH_OK;
}

const SrhImageSlotDescriptor image_slots[]{
    {SRH_INIT(SrhImageSlotDescriptor), "ROM 1"},
    {SRH_INIT(SrhImageSlotDescriptor), "ROM 2"},
    {SRH_INIT(SrhImageSlotDescriptor), "ROM 3"},
    {SRH_INIT(SrhImageSlotDescriptor), "ROM 4"},
};
const SrhCardDescriptor descriptor{
    SRH_INIT(SrhCardDescriptor), "Audio", "YMW258-F",
    "28-voice Yamaha YMW258-F with private 4 MiB sample ROM", 0xB0, port_count, 0, 0, 0,
    SRH_CARD_REQUIRES_IMAGE,
    R"({"chip_clock_hz":9878400,"sample_rate":44100,"stream_name":"YMW258"})", nullptr, nullptr,
    image_slots, 4};
const SrhPlugin api{SRH_INIT(SrhPlugin), "ymw258", create, destroy, reset, count, info, get, set,
                    save_state, load_state, &descriptor, nullptr, nullptr};
} // namespace

extern "C" SRH_EXPORT const SrhPlugin *SRH_CALL srz80_plugin_init(const ShouryoHost *host) {
    return srz80::sdk::valid(host) ? &api : nullptr;
}
