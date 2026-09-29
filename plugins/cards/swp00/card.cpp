#include <state.hpp>
#include <boundary.hpp>
#include <json.hpp>

#include "swp00.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {
constexpr uint32_t port_count = 0x800;
constexpr uint32_t default_clock_hz = 33'868'800;
constexpr uint32_t config_properties = 3;
constexpr uint32_t voice_state_fields = srz80::swp00::Engine::fields.size();
constexpr uint32_t property_count = config_properties + 32 * voice_state_fields;

struct Settings {
    uint32_t chip_clock_hz = default_clock_hz;
    std::string stream_name = "SWP00";
};

bool parse_settings(const SrhConfig *config, Settings &settings) {
    if (!srz80::sdk::has_field(config, &SrhConfig::config_json) || !config->config_json) return true;
    const auto visit = [](void *opaque, const srz80::sdk::json::Token &token) noexcept {
        auto &value = *static_cast<Settings *>(opaque);
        if (token.name == "chip_clock_hz") {
            uint64_t number = 0;
            if (!srz80::sdk::json::unsigned_value(token, number) || number > 50'000'000) return false;
            value.chip_clock_hz = static_cast<uint32_t>(number);
            return true;
        }
        if (token.name == "stream_name")
            return token.type == srz80::sdk::json::Type::string &&
                   bool(srz80::sdk::json::decode_string(token.value, value.stream_name));
        return false;
    };
    if (config->config_json_size > SIZE_MAX ||
        !srz80::sdk::json::object({config->config_json, size_t(config->config_json_size)}, visit, &settings))
        return false;
    return settings.chip_clock_hz >= 1'000'000 && settings.chip_clock_hz <= 50'000'000 &&
           !settings.stream_name.empty() && settings.stream_name.size() <= 256;
}

struct Card {
    const ShouryoHost *host = nullptr;
    SrhHandle owner = 0, space = 0, mapping = 0, stream = 0;
    uint64_t base = 0;
    int32_t priority = 0;
    uint32_t chip_clock_hz = default_clock_hz;
    uint32_t sample_rate = default_clock_hz / 768;
    srz80::swp00::Engine engine;

    Card(std::span<const uint8_t> image, uint32_t clock)
        : chip_clock_hz(clock), sample_rate((clock + 384) / 768), engine(image) {}

    void reset() {
        engine.reset();
    }

    SrhStatus write(uint64_t address, uint8_t value) {
        if (address < base || address - base >= port_count) return SRH_INVALID;
        engine.write(static_cast<uint32_t>(address - base), value);
        return SRH_OK;
    }
};

SrhStatus SRH_CALL read(void *context, uint64_t address, uint8_t *value) {
    auto &card = *static_cast<Card *>(context);
    if (!value || address < card.base || address - card.base >= port_count) return SRH_INVALID;
    *value = card.engine.read(static_cast<uint32_t>(address - card.base));
    return SRH_OK;
}

SrhStatus SRH_CALL write(void *context, uint64_t address, uint8_t value) {
    return static_cast<Card *>(context)->write(address, value);
}

SrhStatus SRH_CALL peek(void *context, uint64_t address, uint8_t *value) {
    auto &card = *static_cast<Card *>(context);
    if (!value || address < card.base || address - card.base >= port_count) return SRH_INVALID;
    *value = card.engine.read(static_cast<uint32_t>(address - card.base), true);
    return SRH_OK;
}

SrhStatus SRH_CALL render(void *context, uint64_t, uint32_t frames, int16_t *interleaved) {
    if (!interleaved) return SRH_INVALID;
    auto &engine = static_cast<Card *>(context)->engine;
    for (uint32_t frame = 0; frame < frames; ++frame) {
        const auto sample = engine.generate();
        // Make the volume louder
        // edit: that was stupid
        interleaved[static_cast<size_t>(frame) * 2] = static_cast<int16_t>(std::clamp(
            (static_cast<int32_t>(sample[0])*1),
            -32768, 32767));
        interleaved[static_cast<size_t>(frame) * 2 + 1] = static_cast<int16_t>(std::clamp(
            (static_cast<int32_t>(sample[1])*1),
            -32768, 32767));
    }
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
                    part.size > srz80::swp00::Engine::address_space_size - rom.size())
                    return SRH_INVALID;
                rom.insert(rom.end(), part.data, part.data + part.size);
            }
        } else {
            if (!config->image || !config->image_size ||
                config->image_size > srz80::swp00::Engine::address_space_size)
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
        if (!srz80::sdk::valid(audio) || !audio->register_source)
            return SRH_INVALID;
        auto card = std::make_unique<Card>(rom, settings.chip_clock_hz);
        card->host = host;
        card->owner = owner;
        card->space = config->space;
        card->base = config->base;
        card->priority = config->priority;
        SrhMapping mapping{SRH_INIT(SrhMapping), config->space, config->base,
                           config->base + port_count - 1, config->priority, card.get(), read, write, peek, nullptr};
        auto status = host->map(host->context, owner, &mapping, &card->mapping);
        if (status != SRH_OK) return status;
        status = audio->register_source(audio->context, owner, card->sample_rate, 2,
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
        static const char *names[] = {"base", "chip_clock_hz", "MEGControl"};
        static const char *descriptions[] = {"First SWP00 register address", "SWP00 input clock", "MEG control register; bits 7-6 select the variation program"};
        *out = {SRH_INIT(SrhProperty), names[index], "SWP00", descriptions[index], SRH_UNSIGNED,
                index == 0 ? 64u : 32u, index == 1 ? 10u : 16u, 0, nullptr,
                index == 1 ? SRH_PROPERTY_PERSISTENT : SRH_PROPERTY_RUNTIME};
        return SRH_OK;
    }
    const uint32_t relative = index - config_properties;
    const uint32_t voice = relative / voice_state_fields, field = relative % voice_state_fields;
    static thread_local char name[48], group[24];
    std::snprintf(name, sizeof(name), "V%02u.%s", voice, srz80::swp00::Engine::fields[field]);
    std::snprintf(group, sizeof(group), "Voice %u", voice + 1);
    *out = {SRH_INIT(SrhProperty), name, group, srz80::swp00::Engine::fields[field], SRH_UNSIGNED,
            32, 16, 0, nullptr, SRH_PROPERTY_RUNTIME | SRH_PROPERTY_HIDE_UI};
    return SRH_OK;
}
SrhStatus SRH_CALL get(void *context, uint32_t index, SrhValue *out) {
    if (!srz80::sdk::valid(out) || index >= property_count) return SRH_INVALID;
    auto &card = *static_cast<Card *>(context);
    out->unsigned_value = 0;
    out->signed_value = 0;
    if (index == 0) out->unsigned_value = card.base;
    else if (index == 1) out->unsigned_value = card.chip_clock_hz;
    else if (index == 2) out->unsigned_value = card.engine.read(4);
    else out->unsigned_value = card.engine.inspect((index - config_properties) / voice_state_fields,
                                                  (index - config_properties) % voice_state_fields);
    return SRH_OK;
}
SrhStatus SRH_CALL set(void *, uint32_t, const SrhValue *) { return SRH_INVALID; }
SrhStatus SRH_CALL save_payload(void *context, uint8_t *buffer, uint64_t *size) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!size) return SRH_INVALID;
        auto &card = *static_cast<Card *>(context);
        const auto bytes = card.engine.save_state();
        const uint64_t required = bytes.size() + 4;
        if (!buffer) { *size = required; return SRH_OK; }
        if (*size < required) { *size = required; return SRH_UNAVAILABLE; }
        for (unsigned i = 0; i < 4; ++i) buffer[i] = uint8_t(card.chip_clock_hz >> (8 * i));
        std::memcpy(buffer + 4, bytes.data(), bytes.size());
        *size = required;
        return SRH_OK;
    });
}
SrhStatus SRH_CALL load_payload(void *context, const uint8_t *buffer, uint64_t size) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!buffer || size < 8) return SRH_INVALID;
        auto &card = *static_cast<Card *>(context);
        uint32_t clock = 0;
        for (unsigned i = 0; i < 4; ++i) clock |= uint32_t(buffer[i]) << (8 * i);
        if (clock != card.chip_clock_hz) return SRH_INVALID;
        return card.engine.load_state(std::span(buffer + 4, size - 4)) ? SRH_OK : SRH_INVALID;
    });
}

const SrhImageSlotDescriptor image_slots[]{
    {SRH_INIT(SrhImageSlotDescriptor), "ROM 1"},
    {SRH_INIT(SrhImageSlotDescriptor), "ROM 2"},
    {SRH_INIT(SrhImageSlotDescriptor), "ROM 3"},
    {SRH_INIT(SrhImageSlotDescriptor), "ROM 4"},
};
const SrhCardDescriptor descriptor{
    SRH_INIT(SrhCardDescriptor), "Audio", "SWP00",
    "32-voice Yamaha SWP00 with integrated reverb, chorus and variation; up to 16 MiB sample ROM", 0x400000, port_count, 0, 0, 0,
    SRH_CARD_REQUIRES_IMAGE,
    R"({"chip_clock_hz":33868800,"stream_name":"SWP00"})", nullptr, nullptr,
    image_slots, 4};
using State = srz80::sdk::state::Callbacks<save_payload, load_payload, 1>;
const SrhPlugin api{SRH_INIT(SrhPlugin), "swp00", create, destroy, reset, count, info, get, set,
                    State::save, State::load, &descriptor, nullptr, nullptr};
} // namespace

extern "C" SRH_EXPORT const SrhPlugin *SRH_CALL srz80_plugin_init(const ShouryoHost *host) {
    return srz80::sdk::valid(host) ? &api : nullptr;
}
