#include <state.hpp>
#include <boundary.hpp>
#include <json.hpp>

#include "core.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

/* A standalone Konami VRC6 expansion audio card.  The chip is a pure sound
   generator - two pulse channels and one sawtooth channel clocked by the
   machine's processor - so the card exposes one register window and one audio
   stream and reaches the machine for nothing else. */

namespace {
using vrc6::Core;

constexpr uint32_t kDefaultClockHz = 1789773; /* NTSC CPU clock */
constexpr uint32_t kDefaultSampleRate = 44100;
constexpr uint32_t kDefaultGainMilli = 500;
constexpr uint32_t kDefaultChannelVolume = 100;
/* The mixer sums the two 4-bit pulse DACs and the saw's top five bits, a
   maximum of 61.  This step maps a maximum-strength mix at gain 1000 to just
   under full scale (61 * 537 = 32757). */
constexpr int32_t kFullScaleStep = 537;

/* A brief reason for a refused configuration, when the host offers somewhere
   to put it.  The buffer is host-owned and only valid for the call. */
void report(const SrhConfig *config, const char *message) {
    if (!config || !srz80::sdk::has_field(config, &SrhConfig::error_message) || !config->error_message ||
        config->error_message_capacity == 0)
        return;
    std::snprintf(config->error_message, config->error_message_capacity, "%s", message);
}

struct Settings {
    uint32_t chip_clock_hz = kDefaultClockHz;
    uint32_t sample_rate = kDefaultSampleRate;
    uint32_t gain_milli = kDefaultGainMilli;
    std::string stream_name = "VRC6";
    uint32_t volume[vrc6::kChannelCount] = {kDefaultChannelVolume, kDefaultChannelVolume,
                                            kDefaultChannelVolume};
};

/* Parses the card's configuration object.  Returns nullptr on success or a
   short reason for the first offending key. */
const char *parse_settings(const SrhConfig *config, Settings &settings) {
    if (!srz80::sdk::has_field(config, &SrhConfig::config_json) || !config->config_json ||
        config->config_json_size == 0)
        return nullptr;
    struct Parse { Settings *settings; const char *reason = nullptr; } parse{&settings};
    const auto visit = [](void *opaque, const srz80::sdk::json::Token &value) noexcept {
        auto &parse = *static_cast<Parse *>(opaque);
        auto &settings = *parse.settings;
        uint64_t number = 0;
        if (value.name == "chip_clock_hz") {
            if (!srz80::sdk::json::unsigned_value(value, number) || number < 1000000 || number > 20000000)
                return parse.reason = "chip_clock_hz must be 1000000..20000000", false;
            settings.chip_clock_hz = static_cast<uint32_t>(number);
        } else if (value.name == "sample_rate") {
            if (!srz80::sdk::json::unsigned_value(value, number) || number < 8000 || number > 192000)
                return parse.reason = "sample_rate must be 8000..192000", false;
            settings.sample_rate = static_cast<uint32_t>(number);
        } else if (value.name == "gain_milli") {
            if (!srz80::sdk::json::unsigned_value(value, number) || number < 1 || number > 20000)
                return parse.reason = "gain_milli must be 1..20000", false;
            settings.gain_milli = static_cast<uint32_t>(number);
        } else if (value.name == "stream_name") {
            if (value.type != srz80::sdk::json::Type::string ||
                !srz80::sdk::json::decode_string(value.value, settings.stream_name))
                return parse.reason = "stream_name must be a string", false;
            if (settings.stream_name.empty() || settings.stream_name.size() > 256)
                return parse.reason = "stream_name must be 1..256 characters", false;
        } else if (value.name == "volume_pulse1" || value.name == "volume_pulse2" || value.name == "volume_saw") {
            const uint32_t channel = value.name == "volume_pulse1" ? 0u : value.name == "volume_pulse2" ? 1u : 2u;
            if (!srz80::sdk::json::unsigned_value(value, number) || number > 200)
                return parse.reason = "channel volumes must be 0..200 percent", false;
            settings.volume[channel] = static_cast<uint32_t>(number);
        } else {
            return parse.reason = "unknown configuration key", false;
        }
        return true;
    };
    if (config->config_json_size > SIZE_MAX) return "config is too large";
    const auto result = srz80::sdk::json::object(
        {config->config_json, size_t(config->config_json_size)}, visit, &parse);
    return result ? nullptr : parse.reason ? parse.reason : "config must be a JSON object";
}

struct Card {
    const ShouryoHost *host = nullptr;
    SrhHandle owner = 0, space = 0, mapping = 0, stream = 0;
    uint64_t base = 0;
    uint32_t chip_clock_hz = kDefaultClockHz;
    uint32_t sample_rate = kDefaultSampleRate;
    uint32_t gain_milli = kDefaultGainMilli;
    uint32_t volume[vrc6::kChannelCount] = {kDefaultChannelVolume, kDefaultChannelVolume,
                                            kDefaultChannelVolume};
    std::string stream_name = "VRC6";
    int32_t priority = 0;
    /* Fractional chip clocks carried across rendered frames, so the average
       rate is exact and does not depend on how the host chunks its output. */
    uint64_t clock_accum = 0;
    Core core;

    void reset() {
        core.power_on();
        clock_accum = 0;
    }

    void advance() {
        clock_accum += chip_clock_hz;
        while (clock_accum >= sample_rate) {
            clock_accum -= sample_rate;
            core.clock();
        }
    }

    int16_t mixed_output() const {
        uint32_t level = 0;
        for (uint32_t channel = 0; channel < vrc6::kChannelCount; ++channel)
            level += core.channel_output(channel) * volume[channel] / 100u;
        const auto scaled = static_cast<int64_t>(level) * gain_milli * kFullScaleStep / 1000;
        return static_cast<int16_t>(
            std::clamp(scaled, static_cast<int64_t>(INT16_MIN), static_cast<int64_t>(INT16_MAX)));
    }

    SrhStatus access(uint64_t address, uint32_t *offset) const {
        if (address < base || address - base >= vrc6::kRegisterSpan)
            return SRH_INVALID;
        *offset = static_cast<uint32_t>(address - base);
        return SRH_OK;
    }
};

SrhStatus SRH_CALL read_register(void *context, uint64_t address, uint8_t *value) {
    if (!context || !value)
        return SRH_INVALID;
    auto &card = *static_cast<Card *>(context);
    uint32_t offset = 0;
    const auto status = card.access(address, &offset);
    if (status != SRH_OK)
        return status;
    /* Every register is write-only; reads see open bus, which the card reports
       as a zero for the bus to override. */
    (void)offset;
    *value = 0;
    return SRH_OK;
}
SrhStatus SRH_CALL write_register(void *context, uint64_t address, uint8_t value) {
    if (!context)
        return SRH_INVALID;
    auto &card = *static_cast<Card *>(context);
    uint32_t offset = 0;
    const auto status = card.access(address, &offset);
    if (status != SRH_OK)
        return status;
    card.core.write(offset, value);
    return SRH_OK;
}

SrhStatus SRH_CALL render(void *context, uint64_t, uint32_t frames, int16_t *interleaved) {
    if (!context || !interleaved)
        return SRH_INVALID;
    auto &card = *static_cast<Card *>(context);
    for (uint32_t frame = 0; frame < frames; ++frame) {
        card.advance();
        const int16_t sample = card.mixed_output();
        interleaved[static_cast<size_t>(frame) * 2] = sample;
        interleaved[static_cast<size_t>(frame) * 2 + 1] = sample;
    }
    return SRH_OK;
}

SrhStatus SRH_CALL create(const ShouryoHost *host, SrhHandle owner, const SrhConfig *config,
                          void **result) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (result)
            *result = nullptr;
        if (!srz80::sdk::valid(host) || !srz80::sdk::valid(config) || !result || !config->space ||
            !host->map || !host->query) {
            report(config, "missing host callbacks or memory space");
            return SRH_INVALID;
        }
        /* An unset size means the descriptor default; anything else has to be
           the whole register window, because the chip decodes all of it. */
        if (config->size != 0 && config->size != vrc6::kRegisterSpan) {
            report(config, "register window must be 0x2003 bytes at the chip's $9000 block");
            return SRH_INVALID;
        }
        if (config->base > UINT64_MAX - (vrc6::kRegisterSpan - 1)) {
            report(config, "register window would wrap the address space");
            return SRH_INVALID;
        }
        Settings settings;
        if (const char *reason = parse_settings(config, settings)) {
            report(config, reason);
            return SRH_INVALID;
        }

        const void *extension = nullptr;
        if (host->query(host->context, "host.audio.v1", &extension) != SRH_OK || !extension) {
            report(config, "the host offers no audio sources");
            return SRH_UNAVAILABLE;
        }
        const auto audio = static_cast<const SrhHostAudioV1 *>(extension);
        if (!srz80::sdk::valid(audio) || !audio->register_source || audio->channels < 2 ||
            audio->format != SRH_AUDIO_S16_STEREO) {
            report(config, "the host audio source table is not usable");
            return SRH_UNAVAILABLE;
        }

        auto card = std::make_unique<Card>();
        card->host = host;
        card->owner = owner;
        card->space = config->space;
        card->base = config->base;
        card->priority = config->priority;
        card->chip_clock_hz = settings.chip_clock_hz;
        card->sample_rate = settings.sample_rate;
        card->gain_milli = settings.gain_milli;
        card->stream_name = settings.stream_name;
        for (uint32_t channel = 0; channel < vrc6::kChannelCount; ++channel)
            card->volume[channel] = settings.volume[channel];
        card->reset();

        SrhMapping mapping{SRH_INIT(SrhMapping),
                           config->space,
                           config->base,
                           config->base + vrc6::kRegisterSpan - 1,
                           config->priority,
                           card.get(),
                           read_register,
                           write_register,
                           read_register,
                           nullptr};
        auto status = host->map(host->context, owner, &mapping, &card->mapping);
        if (status != SRH_OK) {
            report(config, "register window could not be mapped");
            return status;
        }
        status = audio->register_source(audio->context, owner, card->sample_rate, 2,
                                        SRH_AUDIO_S16_STEREO, card->stream_name.c_str(), render,
                                        card.get(), &card->stream);
        if (status != SRH_OK) {
            if (host->unmap)
                host->unmap(host->context, card->mapping);
            report(config, "audio stream could not be registered");
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

// Property layout: configuration, then read-only runtime state per channel,
// then the chip-wide control state.
constexpr uint32_t kConfigCount = 8; /* base .. volume_saw */
constexpr uint32_t kPulseStart = kConfigCount;
constexpr uint32_t kPulseFields = 7; /* Enabled .. Output */
constexpr uint32_t kSawStart = kPulseStart + 2 * kPulseFields;
constexpr uint32_t kSawFields = 5; /* Enabled .. Output */
constexpr uint32_t kGlobalStart = kSawStart + kSawFields;
constexpr uint32_t kGlobalFields = 2; /* Halt, FreqShift */
constexpr uint32_t kPropertyCount = kGlobalStart + kGlobalFields;

uint32_t SRH_CALL property_count(void *) { return kPropertyCount; }

SrhStatus SRH_CALL property_info(void *, uint32_t index, SrhProperty *out) {
    if (!srz80::sdk::valid(out) || index >= kPropertyCount)
        return SRH_INVALID;
    static const char *config_names[kConfigCount] = {
        "base", "chip_clock_hz", "sample_rate", "gain_milli",
        "stream_name", "volume_pulse1", "volume_pulse2", "volume_saw"};
    static const char *config_descriptions[kConfigCount] = {
        "First mapped register (the chip's $9000 block)",
        "Processor clock the oscillators are driven at",
        "Native output rate of the audio stream",
        "Output level, in thousandths",
        "Name of the registered audio stream",
        "Pulse 1 level, in percent",
        "Pulse 2 level, in percent",
        "Sawtooth level, in percent"};
    static const char *pulse_fields[kPulseFields] = {"Enabled", "Mode",    "Duty", "Volume",
                                                     "Period",   "Step",   "Output"};
    static const char *pulse_descriptions[kPulseFields] = {
        "Channel enabled by the period-high register",
        "Mode bit: output the volume regardless of the duty generator",
        "Duty generator setting, (duty + 1) of 16 steps high",
        "DAC volume, 0..15",
        "12-bit period; the duty generator steps every period + 1 clocks",
        "Current duty generator step, counting 15 down to 0",
        "Current DAC level"};
    static const char *saw_fields[kSawFields] = {"Enabled", "Rate", "Period", "Accumulator",
                                                 "Output"};
    static const char *saw_descriptions[kSawFields] = {
        "Channel enabled by the period-high register",
        "Accumulator rate added on each of the six accumulation steps",
        "12-bit period; the ramp clocks every period + 1 clocks",
        "8-bit ramp accumulator",
        "Current DAC level (accumulator bits 7..3)"};
    static thread_local char name[128]{}, group[128]{}, description[256]{};
    const char *enum_labels = nullptr;
    uint32_t kind = SRH_UNSIGNED;
    uint32_t bits = 32;
    uint32_t base = 10;
    uint32_t editable = 0;
    uint32_t ui_flags = 0;

    if (index < kConfigCount) {
        std::snprintf(name, sizeof(name), "%s", config_names[index]);
        std::snprintf(group, sizeof(group), "%s", "VRC6");
        std::snprintf(description, sizeof(description), "%s", config_descriptions[index]);
        if (index == 0) {
            bits = 16;
            base = 16;
        } else if (index == 1 || index == 3 || index >= 5) {
            editable = 1;
            ui_flags = SRH_PROPERTY_LIVE_EDIT | SRH_PROPERTY_PERSISTENT;
        } else if (index == 4) {
            kind = SRH_TEXT;
            bits = 0;
            base = 0;
        }
    } else if (index < kSawStart) {
        const uint32_t channel = (index - kPulseStart) / kPulseFields;
        const uint32_t field = (index - kPulseStart) % kPulseFields;
        std::snprintf(name, sizeof(name), "%s", pulse_fields[field]);
        std::snprintf(group, sizeof(group), "Pulse %u", channel + 1);
        std::snprintf(description, sizeof(description), "%s", pulse_descriptions[field]);
        ui_flags = SRH_PROPERTY_RUNTIME;
        if (field == 0 || field == 1) {
            kind = SRH_BOOLEAN;
            bits = 1;
        } else if (field == 2) {
            bits = 3;
        } else if (field == 3 || field == 6) {
            bits = 4;
        } else if (field == 4) {
            bits = 12;
            base = 16;
        } else {
            bits = 4;
        }
    } else if (index < kGlobalStart) {
        const uint32_t field = index - kSawStart;
        std::snprintf(name, sizeof(name), "%s", saw_fields[field]);
        std::snprintf(group, sizeof(group), "%s", "Saw");
        std::snprintf(description, sizeof(description), "%s", saw_descriptions[field]);
        ui_flags = SRH_PROPERTY_RUNTIME;
        if (field == 0) {
            kind = SRH_BOOLEAN;
            bits = 1;
        } else if (field == 1) {
            bits = 6;
        } else if (field == 2) {
            bits = 12;
            base = 16;
        } else if (field == 3) {
            bits = 8;
            base = 16;
        } else {
            bits = 5;
        }
    } else {
        const bool is_halt = index == kGlobalStart;
        std::snprintf(name, sizeof(name), "%s", is_halt ? "Halt" : "FreqShift");
        std::snprintf(group, sizeof(group), "%s", "VRC6");
        std::snprintf(description, sizeof(description), "%s",
                      is_halt ? "Global halt flag ($9003 bit 0): oscillators frozen in place"
                              : "Global period shift ($9003 bits 2-1): 16x or 256x pitch");
        ui_flags = SRH_PROPERTY_RUNTIME;
        if (is_halt) {
            kind = SRH_BOOLEAN;
            bits = 1;
        } else {
            kind = SRH_ENUM;
            bits = 0;
            base = 0;
            enum_labels = "off|16x|256x";
        }
    }

    *out = {SRH_INIT(SrhProperty), name, group, description, kind, bits, base, editable, enum_labels,
            ui_flags};
    return SRH_OK;
}

SrhStatus SRH_CALL property_get(void *context, uint32_t index, SrhValue *out) {
    if (!srz80::sdk::valid(out) || !context || index >= kPropertyCount)
        return SRH_INVALID;
    auto &card = *static_cast<Card *>(context);
    out->unsigned_value = 0;
    out->signed_value = 0;
    out->text[0] = '\0';
    if (index < kConfigCount) {
        switch (index) {
        case 0:
            out->unsigned_value = card.base;
            break;
        case 1:
            out->unsigned_value = card.chip_clock_hz;
            break;
        case 2:
            out->unsigned_value = card.sample_rate;
            break;
        case 3:
            out->unsigned_value = card.gain_milli;
            break;
        case 4:
            std::snprintf(out->text, sizeof(out->text), "%s", card.stream_name.c_str());
            break;
        default:
            out->unsigned_value = card.volume[index - 5];
            break;
        }
        return SRH_OK;
    }
    const auto &core = card.core;
    if (index < kSawStart) {
        const uint32_t channel = (index - kPulseStart) / kPulseFields;
        const uint32_t field = (index - kPulseStart) % kPulseFields;
        out->unsigned_value = field == 0   ? (core.channel_enabled(channel) ? 1u : 0u)
                              : field == 1 ? (core.pulse_mode(channel) ? 1u : 0u)
                              : field == 2 ? core.pulse_duty(channel)
                              : field == 3 ? core.pulse_volume(channel)
                              : field == 4 ? core.period(channel)
                              : field == 5 ? core.pulse_step(channel)
                                           : core.channel_output(channel);
        return SRH_OK;
    }
    if (index < kGlobalStart) {
        const uint32_t field = index - kSawStart;
        out->unsigned_value = field == 0   ? (core.channel_enabled(2) ? 1u : 0u)
                              : field == 1 ? core.saw_rate()
                              : field == 2 ? core.period(2)
                              : field == 3 ? core.saw_accumulator()
                                           : core.channel_output(2);
        return SRH_OK;
    }
    if (index == kGlobalStart) {
        out->unsigned_value = core.halted() ? 1u : 0u;
        return SRH_OK;
    }
    out->unsigned_value = core.frequency_shift() == 0 ? 0u : core.frequency_shift() == 4 ? 1u : 2u;
    return SRH_OK;
}

SrhStatus SRH_CALL property_set(void *context, uint32_t index, const SrhValue *in) {
    if (!srz80::sdk::valid(in) || !context || index >= kPropertyCount)
        return SRH_INVALID;
    auto &card = *static_cast<Card *>(context);
    switch (index) {
    case 1:
        if (in->unsigned_value < 1000000 || in->unsigned_value > 20000000)
            return SRH_INVALID;
        card.chip_clock_hz = static_cast<uint32_t>(in->unsigned_value);
        return SRH_OK;
    case 3:
        if (in->unsigned_value < 1 || in->unsigned_value > 20000)
            return SRH_INVALID;
        card.gain_milli = static_cast<uint32_t>(in->unsigned_value);
        return SRH_OK;
    case 5:
    case 6:
    case 7:
        if (in->unsigned_value > 200)
            return SRH_INVALID;
        card.volume[index - 5] = static_cast<uint32_t>(in->unsigned_value);
        return SRH_OK;
    default:
        /* Everything else is either fixed at creation (the stream contract),
           read-only metadata (base, sample rate) or runtime state that only
           the register file may change. */
        return SRH_INVALID;
    }
}

SrhStatus SRH_CALL save_payload(void *context, uint8_t *buffer, uint64_t *size) {
    if (!context || !size)
        return SRH_INVALID;
    /* The carried clock remainder comes first, then the unit's own image. */
    constexpr uint64_t required = 8 + Core::state_size();
    if (!buffer) {
        *size = required;
        return SRH_OK;
    }
    if (*size < required) {
        *size = required;
        return SRH_UNAVAILABLE;
    }
    auto &card = *static_cast<Card *>(context);
    for (uint32_t byte = 0; byte < 8; ++byte)
        buffer[byte] = static_cast<uint8_t>(card.clock_accum >> (byte * 8));
    card.core.save_state(buffer + 8);
    *size = required;
    return SRH_OK;
}

SrhStatus SRH_CALL load_payload(void *context, const uint8_t *buffer, uint64_t size) {
    constexpr uint64_t required = 8 + Core::state_size();
    if (!context || !buffer || size != required)
        return SRH_INVALID;
    auto &card = *static_cast<Card *>(context);
    uint64_t accumulator = 0;
    for (uint32_t byte = 0; byte < 8; ++byte)
        accumulator |= static_cast<uint64_t>(buffer[byte]) << (byte * 8);
    if (accumulator >= card.sample_rate)
        return SRH_INVALID;
    if (!card.core.load_state(buffer + 8, Core::state_size()))
        return SRH_INVALID;
    card.clock_accum = accumulator;
    return SRH_OK;
}

const SrhCardDescriptor descriptor{
    SRH_INIT(SrhCardDescriptor),
    "Audio",
    "VRC6",
    "Konami VRC6 expansion audio: two pulse channels and one sawtooth channel",
    0x9000,
    vrc6::kRegisterSpan,
    0,
    0,
    kDefaultClockHz,
    0,
    R"({"chip_clock_hz":1789773,"sample_rate":44100,"gain_milli":500,"stream_name":"VRC6","volume_pulse1":100,"volume_pulse2":100,"volume_saw":100})",
    nullptr,
    nullptr,
    nullptr,
    0};

using State = srz80::sdk::state::Callbacks<save_payload, load_payload, 1>;
const SrhPlugin api{SRH_INIT(SrhPlugin), "vrc6",   create,    destroy,      reset, property_count,
                    property_info,      property_get, property_set, State::save, State::load,
                    &descriptor,        nullptr,  nullptr};
} // namespace

extern "C" SRH_EXPORT const SrhPlugin *SRH_CALL srz80_plugin_init(const ShouryoHost *host) {
    return srz80::sdk::valid(host) ? &api : nullptr;
}
