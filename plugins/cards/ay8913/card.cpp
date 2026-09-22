#include <state.hpp>
#include <boundary.hpp>
#include <nlohmann/json.hpp>

#include "ay8913.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>

namespace {
using Json = nlohmann::json;

constexpr uint32_t kDefaultClockHz = 1789773; // 1.7897725 MHz (MSX PSG clock)
constexpr uint32_t kDefaultSampleRate = 44100;
// The core's sample() sums three legacy-normalized channels, each in
// [-0.125, 0.375]; the sum spans [-0.375, 1.125].  This gain keeps the
// loudest sensible mix below full scale (1.125 * 20000 = 22500 < 32767).
constexpr float kOutputGain = 20000.0f;

struct Settings {
    uint32_t chip_clock_hz = kDefaultClockHz;
    uint32_t sample_rate = kDefaultSampleRate;
    bool data_first = false;
    std::string stream_name = "AY-3-8913";
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
            if (!read_unsigned(value, number) || number < 1000000 || number > 20000000)
                return false;
            settings.chip_clock_hz = static_cast<uint32_t>(number);
        } else if (key == "sample_rate") {
            if (!read_unsigned(value, number) || number < 8000 || number > 192000)
                return false;
            settings.sample_rate = static_cast<uint32_t>(number);
        } else if (key == "data_first") {
            if (!value.is_boolean())
                return false;
            settings.data_first = value.get<bool>();
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
    return true;
}

struct Card {
    const ShouryoHost *host = nullptr;
    SrhHandle owner = 0, space = 0, mapping = 0, stream = 0;
    uint64_t base = 0;
    uint32_t chip_clock_hz = kDefaultClockHz;
    uint32_t sample_rate = kDefaultSampleRate;
    bool data_first = false;
    uint64_t clock_accum = 0;
    std::atomic<uint8_t> mute_mask{0};
    ay8913::core core;

    uint64_t latch_address() const { return base + (data_first ? 1u : 0u); }
    uint64_t data_address() const { return base + (data_first ? 0u : 1u); }

    SrhStatus read(uint64_t address, uint8_t *value) const {
        if (!value || address < base || address - base >= 2)
            return SRH_INVALID;
        // Reading the address state is high-impedance; the data read returns
        // the latched register (0xff while the latch is inactive).
        *value = address == data_address() ? core.read_data() : 0xff;
        return SRH_OK;
    }
    SrhStatus write(uint64_t address, uint8_t value) {
        if (address < base || address - base >= 2)
            return SRH_INVALID;
        if (address == latch_address())
            core.latch_address(value);
        else
            core.write_data(value);
        return SRH_OK;
    }
    void reset() {
        core.reset();
        clock_accum = 0;
    }
};

SrhStatus SRH_CALL read_register(void *context, uint64_t address, uint8_t *value) {
    return static_cast<Card *>(context)->read(address, value);
}
SrhStatus SRH_CALL write_register(void *context, uint64_t address, uint8_t value) {
    return static_cast<Card *>(context)->write(address, value);
}

SrhStatus SRH_CALL render(void *context, uint64_t, uint32_t frames, int16_t *interleaved) {
    if (!context || !interleaved)
        return SRH_INVALID;
    auto &card = *static_cast<Card *>(context);
    const uint64_t denominator = uint64_t(card.sample_rate) * 8u;
    for (uint32_t frame = 0; frame < frames; ++frame) {
        card.clock_accum += card.chip_clock_hz;
        while (card.clock_accum >= denominator) {
            card.clock_accum -= denominator;
            card.core.step();
        }
        const uint8_t mute_mask = card.mute_mask.load(std::memory_order_relaxed);
        float mix = 0.0f;
        for (unsigned channel = 0; channel < ay8913::core::kChannels; ++channel) {
            if ((mute_mask & (1u << channel)) == 0)
                mix += card.core.channel_sample(channel);
        }
        mix *= kOutputGain;
        const auto clamped = static_cast<int16_t>(std::clamp(
            mix, static_cast<float>(std::numeric_limits<int16_t>::min()),
            static_cast<float>(std::numeric_limits<int16_t>::max())));
        interleaved[static_cast<size_t>(frame) * 2] = clamped;
        interleaved[static_cast<size_t>(frame) * 2 + 1] = clamped;
    }
    return SRH_OK;
}

SrhStatus SRH_CALL create(const ShouryoHost *host, SrhHandle owner, const SrhConfig *config,
                          void **result) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!srz80::sdk::valid(host) || !srz80::sdk::valid(config) || !result || !config->space ||
            !host->map || config->size != 2 || config->base == UINT64_MAX)
            return SRH_INVALID;

        Settings settings;
        if (!parse_settings(config, settings))
            return SRH_INVALID;

        const void *extension = nullptr;
        if (!host->query || host->query(host->context, "host.audio.v1", &extension) != SRH_OK ||
            !extension)
            return SRH_UNAVAILABLE;
        const auto audio = static_cast<const SrhHostAudioV1 *>(extension);
        if (!srz80::sdk::valid(audio) || !audio->register_source ||
            audio->format != SRH_AUDIO_S16_STEREO || audio->channels < 2)
            return SRH_UNAVAILABLE;

        auto card = std::make_unique<Card>();
        card->host = host;
        card->owner = owner;
        card->space = config->space;
        card->base = config->base;
        card->chip_clock_hz = settings.chip_clock_hz;
        card->sample_rate = settings.sample_rate;
        card->data_first = settings.data_first;
        card->reset();

        SrhMapping mapping{SRH_INIT(SrhMapping), config->space, config->base, config->base + 1,
                           config->priority, card.get(), read_register, write_register,
                           read_register, nullptr};
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

void SRH_CALL destroy(void *context) { delete static_cast<Card *>(context); }

SrhStatus SRH_CALL reset(void *context, uint32_t) {
    if (!context)
        return SRH_INVALID;
    static_cast<Card *>(context)->reset();
    return SRH_OK;
}

// Property layout: config, raw registers, bus interface, per-channel
// observables, global observables.
constexpr uint32_t kConfigCount = 4; // base, chip_clock_hz, sample_rate, data_first
constexpr uint32_t kRawStart = kConfigCount;
constexpr uint32_t kRawCount = 16;
constexpr uint32_t kInterfaceStart = kRawStart + kRawCount;
constexpr uint32_t kInterfaceCount = 2; // Address, Data
constexpr uint32_t kChanStart = kInterfaceStart + kInterfaceCount;
constexpr uint32_t kChanCount = 3;
constexpr uint32_t kChanFieldCount = 4; // Period, Tone, Mix, Mute
constexpr uint32_t kGlobalStart = kChanStart + kChanCount * kChanFieldCount;
constexpr uint32_t kGlobalCount = 3; // Envelope, Noise, RNG
constexpr uint32_t kPropertyCount = kGlobalStart + kGlobalCount;

uint32_t SRH_CALL property_count(void *) { return kPropertyCount; }

SrhStatus SRH_CALL property_info(void *, uint32_t index, SrhProperty *out) {
    if (!srz80::sdk::valid(out) || index >= kPropertyCount)
        return SRH_INVALID;
    static const char *config_names[] = {"base", "chip_clock_hz", "sample_rate", "data_first"};
    static const char *config_descriptions[] = {
        "First mapped I/O port", "AY-3-8913 input clock", "Native output rate",
        "Data port first (true) or address port first (false)"};
    static const char *chan_fields[] = {"Period", "Tone", "Mix", "Mute"};
    static const char *chan_descriptions[] = {
        "Tone period (coarse<<8 | fine)", "Tone generator output", "Channel mix enable", ""};
    static const char *global_names[] = {"Envelope", "Noise", "RNG"};
    static const char *global_descriptions[] = {
        "Envelope generator volume", "Noise generator output", "Noise LFSR state"};
    static thread_local char name[128]{}, group[128]{}, description[256]{};
    uint32_t kind = SRH_UNSIGNED;
    uint32_t bits = 8;
    uint32_t base = 10;
    uint32_t editable = 0;
    uint32_t ui_flags = 0;

    if (index < kConfigCount) {
        std::snprintf(name, sizeof(name), "%s", config_names[index]);
        std::snprintf(group, sizeof(group), "%s", "AY-3-8913");
        std::snprintf(description, sizeof(description), "%s", config_descriptions[index]);
        if (index == 0) {
            bits = 16;
            base = 16;
        } else if (index == 3) {
            kind = SRH_BOOLEAN;
            bits = 1;
        } else {
            bits = 32;
        }
    } else if (index < kInterfaceStart) {
        const uint32_t reg = index - kRawStart;
        std::snprintf(name, sizeof(name), "R%02X", reg);
        std::snprintf(group, sizeof(group), "%s", "Raw");
        std::snprintf(description, sizeof(description), "AY-3-8913 register $%02X", reg);
        base = 16;
        editable = 1;
        ui_flags = SRH_PROPERTY_HIDE_UI | SRH_PROPERTY_LIVE_EDIT;
    } else if (index < kChanStart) {
        const bool is_address = index == kInterfaceStart;
        std::snprintf(name, sizeof(name), "%s", is_address ? "Address" : "Data");
        std::snprintf(description, sizeof(description), "%s",
                      is_address ? "Current register-select latch"
                                 : "Read/write the latched register");
        std::snprintf(group, sizeof(group), "%s", "Interface");
        base = 16;
        editable = 1;
        ui_flags = SRH_PROPERTY_HIDE_UI | SRH_PROPERTY_LIVE_EDIT;
        if (is_address)
            bits = 4;
    } else if (index < kGlobalStart) {
        const uint32_t chan = (index - kChanStart) / kChanFieldCount;
        const uint32_t field = (index - kChanStart) % kChanFieldCount;
        std::snprintf(name, sizeof(name), "C%u.%s", chan, chan_fields[field]);
        std::snprintf(group, sizeof(group), "Channel %u", chan);
        std::snprintf(description, sizeof(description), "%s", chan_descriptions[field]);
        if (field == 0) {
            bits = 12;
        } else {
            kind = SRH_BOOLEAN;
            bits = 1;
        }
        if (field == 3) {
            editable = 1;
            ui_flags = SRH_PROPERTY_LIVE_EDIT | SRH_PROPERTY_RUNTIME;
        } else {
            ui_flags = SRH_PROPERTY_HIDE_UI;
        }
    } else {
        const uint32_t g = index - kGlobalStart;
        std::snprintf(name, sizeof(name), "%s", global_names[g]);
        std::snprintf(group, sizeof(group), "%s", "Global");
        std::snprintf(description, sizeof(description), "%s", global_descriptions[g]);
        if (g == 1) {
            kind = SRH_BOOLEAN;
            bits = 1;
        } else if (g == 2) {
            bits = 32;
        }
        ui_flags = SRH_PROPERTY_HIDE_UI;
    }

    *out = {SRH_INIT(SrhProperty), name, group, description, kind, bits, base, editable, nullptr,
            ui_flags};
    return SRH_OK;
}

SrhStatus SRH_CALL property_get(void *context, uint32_t index, SrhValue *out) {
    if (!srz80::sdk::valid(out) || index >= kPropertyCount || !context)
        return SRH_INVALID;
    auto &card = *static_cast<Card *>(context);
    out->unsigned_value = 0;
    out->signed_value = 0;
    if (index < kConfigCount) {
        out->unsigned_value = index == 0   ? card.base
                              : index == 1 ? card.chip_clock_hz
                              : index == 2 ? card.sample_rate
                                           : (card.data_first ? 1u : 0u);
        return SRH_OK;
    }
    if (index < kInterfaceStart) {
        out->unsigned_value = card.core.register_value(index - kRawStart);
        return SRH_OK;
    }
    if (index < kChanStart) {
        out->unsigned_value =
            index == kInterfaceStart ? card.core.latch() : card.core.register_value(card.core.latch());
        return SRH_OK;
    }
    if (index < kGlobalStart) {
        const uint32_t chan = (index - kChanStart) / kChanFieldCount;
        const uint32_t field = (index - kChanStart) % kChanFieldCount;
        out->unsigned_value = field == 0   ? card.core.tone_period(chan)
                              : field == 1 ? (card.core.tone_output(chan) ? 1u : 0u)
                              : field == 2 ? (card.core.channel_enabled(chan) ? 1u : 0u)
                                           : ((card.mute_mask.load(std::memory_order_relaxed) >> chan) & 1u);
        return SRH_OK;
    }
    const uint32_t g = index - kGlobalStart;
    out->unsigned_value = g == 0 ? card.core.envelope_volume()
                          : g == 1 ? (card.core.noise_output() ? 1u : 0u)
                                   : card.core.rng_value();
    return SRH_OK;
}

SrhStatus SRH_CALL property_set(void *context, uint32_t index, const SrhValue *in) {
    if (!srz80::sdk::valid(in) || index >= kPropertyCount || !context)
        return SRH_INVALID;
    auto &card = *static_cast<Card *>(context);
    if (index < kConfigCount)
        return SRH_INVALID;
    if (index < kInterfaceStart) {
        if (in->unsigned_value > 0xff)
            return SRH_INVALID;
        card.core.set_register(index - kRawStart, static_cast<uint8_t>(in->unsigned_value));
        return SRH_OK;
    }
    if (index < kChanStart) {
        if (index == kInterfaceStart) {
            if (in->unsigned_value > 0x0f)
                return SRH_INVALID;
            card.core.latch_address(static_cast<uint8_t>(in->unsigned_value));
        } else {
            if (in->unsigned_value > 0xff)
                return SRH_INVALID;
            card.core.set_register(card.core.latch(), static_cast<uint8_t>(in->unsigned_value));
        }
        return SRH_OK;
    }
    if (index < kGlobalStart) {
        const uint32_t channel = (index - kChanStart) / kChanFieldCount;
        const uint32_t field = (index - kChanStart) % kChanFieldCount;
        if (field != 3 || in->unsigned_value > 1)
            return SRH_INVALID;
        const uint8_t bit = static_cast<uint8_t>(1u << channel);
        if (in->unsigned_value != 0)
            card.mute_mask.fetch_or(bit, std::memory_order_relaxed);
        else
            card.mute_mask.fetch_and(static_cast<uint8_t>(~bit), std::memory_order_relaxed);
        return SRH_OK;
    }
    return SRH_INVALID; // observables are read-only
}

SrhStatus SRH_CALL save_payload(void *context, uint8_t *buffer, uint64_t *size) {
    if (!size || !context)
        return SRH_INVALID;
    constexpr uint64_t required = sizeof(uint64_t) + ay8913::core::serialized_size;
    if (!buffer) {
        *size = required;
        return SRH_OK;
    }
    if (*size < required) {
        *size = required;
        return SRH_UNAVAILABLE;
    }
    auto &card = *static_cast<Card *>(context);
    srz80::sdk::state::put(buffer, card.clock_accum);
    card.core.save_state(buffer + sizeof(card.clock_accum));
    *size = required;
    return SRH_OK;
}

SrhStatus SRH_CALL load_payload(void *context, const uint8_t *buffer, uint64_t size) {
    constexpr uint64_t required = sizeof(uint64_t) + ay8913::core::serialized_size;
    if (!context || !buffer || size != required)
        return SRH_INVALID;
    auto &card = *static_cast<Card *>(context);
    const auto accumulator = srz80::sdk::state::get<uint64_t>(buffer);
    if (accumulator >= uint64_t(card.sample_rate) * 8 || !card.core.load_state(buffer + sizeof(card.clock_accum)))
        return SRH_INVALID;
    card.clock_accum = accumulator;
    return SRH_OK;
}

const SrhCardDescriptor descriptor{
    SRH_INIT(SrhCardDescriptor), "Audio", "AY-3-8913",
    "General Instrument AY-3-8913 programmable sound generator", 0xA0, 2, 0, 0, 0,
    0, // port block maps into the selected space; no separate I/O space
    R"({"chip_clock_hz":1789773,"sample_rate":44100,"stream_name":"AY-3-8913","data_first":false})",
    nullptr, nullptr, nullptr, 0};

using State = srz80::sdk::state::Callbacks<save_payload, load_payload, 1>;
const SrhPlugin api{SRH_INIT(SrhPlugin), "ay8913", create, destroy, reset,
                    property_count, property_info, property_get, property_set,
                    State::save, State::load, &descriptor, nullptr, nullptr};
} // namespace

extern "C" SRH_EXPORT const SrhPlugin *SRH_CALL srz80_plugin_init(const ShouryoHost *host) {
    return srz80::sdk::valid(host) ? &api : nullptr;
}
