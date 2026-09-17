#include <boundary.hpp>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "core.hpp"

/* A standalone NES audio processing unit card.  It owns the two pulse channels,
   the triangle, the noise channel, the delta channel and the frame sequencer,
   and it presents them as one ordinary memory window plus one audio stream.  The
   delta channel reads its samples through the host's own read path, so sample
   data lives in whatever RAM or ROM card the machine already has. */

namespace {
using nesapu::NesApu;
using nesapu::Region;

constexpr uint32_t kDefaultClockHz = 1789773; /* NTSC CPU clock */
constexpr uint32_t kDefaultSampleRate = 44100;
constexpr uint32_t kDefaultGainMilli = 400;
constexpr uint32_t kDefaultChannelVolume = 100;
constexpr uint32_t kPropertyCount = 13;

/* A brief reason for a refused configuration, when the host offers somewhere to
   put it.  The buffer is host-owned and only valid for the duration of the
   call. */
void report(const SrhConfig *config, const char *message) {
    if (!config || !srz80::sdk::has_field(config, &SrhConfig::error_message) || !config->error_message ||
        config->error_message_capacity == 0)
        return;
    std::snprintf(config->error_message, config->error_message_capacity, "%s", message);
}

struct Settings {
    uint32_t clock_hz = kDefaultClockHz;
    uint32_t sample_rate = kDefaultSampleRate;
    uint32_t gain_milli = kDefaultGainMilli;
    Region region = Region::ntsc;
    std::string stream_name = "NES APU";
    /* Space the delta channel fetches samples from.  Empty uses the card's own
       register space, which is what a machine with the unit on the CPU bus
       wants; samples then come out of ordinary RAM or ROM. */
    std::string sample_space;
    uint64_t sample_base = 0;
    uint32_t volume[nesapu::kChannelCount] = {kDefaultChannelVolume, kDefaultChannelVolume,
                                              kDefaultChannelVolume, kDefaultChannelVolume,
                                              kDefaultChannelVolume};
};

/* Minimal JSON object reader: the host hands the card an object of scalars and
   nothing else has to be understood. */
class Json {
  public:
    explicit Json(const SrhConfig *config)
        : text_(config && srz80::sdk::has_field(config, &SrhConfig::config_json) && config->config_json
                    ? std::string(config->config_json, config->config_json_size)
                    : "{}") {}

    bool parse(Settings &settings) {
        if (!take('{'))
            return false;
        skip();
        if (take('}'))
            return done();
        while (true) {
            std::string key, value;
            bool quoted = false;
            if (!string(key) || !take(':') || !scalar(value, quoted))
                return false;
            if (key == "clock_hz") {
                uint64_t number = 0;
                if (!unsigned_value(value, quoted, number) || number < 1000000 || number > 4000000)
                    return false;
                settings.clock_hz = static_cast<uint32_t>(number);
            } else if (key == "sample_rate") {
                uint64_t number = 0;
                if (!unsigned_value(value, quoted, number) || number < 8000 || number > 192000)
                    return false;
                settings.sample_rate = static_cast<uint32_t>(number);
            } else if (key == "gain_milli") {
                uint64_t number = 0;
                if (!unsigned_value(value, quoted, number) || number < 1 || number > 20000)
                    return false;
                settings.gain_milli = static_cast<uint32_t>(number);
            } else if (key == "region") {
                if (!quoted)
                    return false;
                if (value == "ntsc")
                    settings.region = Region::ntsc;
                else if (value == "pal")
                    settings.region = Region::pal;
                else if (value == "dendy")
                    settings.region = Region::dendy;
                else
                    return false;
            } else if (key == "stream_name") {
                if (!quoted || value.empty() || value.size() > 256)
                    return false;
                settings.stream_name = value;
            } else if (key == "sample_space") {
                if (!quoted || value.size() > 256)
                    return false;
                settings.sample_space = value;
            } else if (key == "sample_base") {
                if (!unsigned_value(value, quoted, settings.sample_base))
                    return false;
            } else {
                uint32_t index = nesapu::kChannelCount;
                if (key == "volume_pulse1")
                    index = 0;
                else if (key == "volume_pulse2")
                    index = 1;
                else if (key == "volume_triangle")
                    index = 2;
                else if (key == "volume_noise")
                    index = 3;
                else if (key == "volume_delta")
                    index = 4;
                else
                    return false;
                uint64_t number = 0;
                if (!unsigned_value(value, quoted, number) || number > 200)
                    return false;
                settings.volume[index] = static_cast<uint32_t>(number);
            }
            if (take('}'))
                return done();
            if (!take(','))
                return false;
        }
    }

  private:
    void skip() {
        while (position_ < text_.size() && (text_[position_] == ' ' || text_[position_] == '\t' ||
                                            text_[position_] == '\r' || text_[position_] == '\n'))
            ++position_;
    }
    bool take(char expected) {
        skip();
        if (position_ >= text_.size() || text_[position_] != expected)
            return false;
        ++position_;
        return true;
    }
    bool string(std::string &out) {
        skip();
        if (position_ >= text_.size() || text_[position_++] != '"')
            return false;
        while (position_ < text_.size() && text_[position_] != '"') {
            if (text_[position_] == '\\' || static_cast<unsigned char>(text_[position_]) < 0x20)
                return false;
            out.push_back(text_[position_++]);
        }
        if (position_ >= text_.size())
            return false;
        ++position_;
        return true;
    }
    bool scalar(std::string &out, bool &quoted) {
        skip();
        if (position_ < text_.size() && text_[position_] == '"') {
            quoted = true;
            return string(out);
        }
        const auto first = position_;
        while (position_ < text_.size() && text_[position_] != ',' && text_[position_] != '}' &&
               text_[position_] != ' ' && text_[position_] != '\t' && text_[position_] != '\r' &&
               text_[position_] != '\n')
            ++position_;
        if (first == position_)
            return false;
        out.assign(text_, first, position_ - first);
        return true;
    }
    static bool unsigned_value(const std::string &value, bool quoted, uint64_t &out) {
        auto digits = value;
        int base = 10;
        if (quoted && digits.size() > 2 && digits[0] == '0' &&
            (digits[1] == 'x' || digits[1] == 'X')) {
            base = 16;
            digits.erase(0, 2);
        }
        if (digits.empty())
            return false;
        out = 0;
        for (char c : digits) {
            unsigned digit = c >= '0' && c <= '9' ? static_cast<unsigned>(c - '0')
                            : c >= 'a' && c <= 'f' ? static_cast<unsigned>(c - 'a' + 10)
                            : c >= 'A' && c <= 'F' ? static_cast<unsigned>(c - 'A' + 10)
                                                   : 99;
            if (digit >= static_cast<unsigned>(base) || out > (UINT64_MAX - digit) / base)
                return false;
            out = out * static_cast<unsigned>(base) + digit;
        }
        return true;
    }
    bool done() {
        skip();
        return position_ == text_.size();
    }
    std::string text_;
    size_t position_ = 0;
};

/* The console's non-linear mixer, as a pair of lookup tables.  Levels in, an
   amplitude around the resting level out; the caller scales that to the output
   range.  Both tables are shifted so that silence is a zero sample rather than
   the mixer curve's non-zero floor. */
const int16_t *pulse_table() {
    static const int16_t *table = [] {
        auto *values = new int16_t[31];
        for (int level = 0; level < 31; ++level) {
            const double volts = 95.52 / (8128.0 / level + 100.0);
            values[level] = static_cast<int16_t>(volts * 32768.0);
        }
        return values;
    }();
    return table;
}
const int16_t *tnd_table() {
    static const int16_t *table = [] {
        auto *values = new int16_t[203];
        for (int level = 0; level < 203; ++level) {
            const double volts = 163.67 / (24329.0 / level + 100.0);
            values[level] = static_cast<int16_t>(volts * 32768.0);
        }
        return values;
    }();
    return table;
}
/* The mixer's output for a silent machine.  It is not zero, because the DAC
   curves are not linear, so it is subtracted to put silence at a zero sample. */
constexpr int32_t kMixerRest = 203;

struct Card {
    const ShouryoHost *host = nullptr;
    SrhHandle owner = 0;
    SrhHandle space = 0;
    SrhHandle sample_space = 0;
    SrhHandle mapping = 0;
    SrhHandle stream = 0;
    uint64_t base = 0;
    uint64_t sample_base = 0;
    uint32_t clock_hz = kDefaultClockHz;
    uint32_t sample_rate = kDefaultSampleRate;
    uint32_t gain_milli = kDefaultGainMilli;
    uint32_t volume[nesapu::kChannelCount] = {kDefaultChannelVolume, kDefaultChannelVolume,
                                              kDefaultChannelVolume, kDefaultChannelVolume,
                                              kDefaultChannelVolume};
    std::string stream_name = "NES APU";
    std::string sample_space_name;
    int32_t priority = 0;
    /* Set when the machine could not answer a delta channel fetch, so the audio
       callback can report it without printing anything itself. */
    SrhStatus sample_error = SRH_OK;
    NesApu apu;

    void reset() {
        apu.power_on();
        sample_error = SRH_OK;
    }

    bool fetch_sample(uint16_t address, uint8_t *value) {
        if (!value || !host || !host->read)
            return false;
        uint8_t byte = 0;
        const auto status =
            host->read(host->context, owner, sample_space, sample_base + address, &byte);
        if (status != SRH_OK) {
            sample_error = status;
            return false;
        }
        *value = byte;
        return true;
    }

    SrhStatus read_register(uint64_t address, uint8_t *value) {
        if (!value || address < base || address - base >= nesapu::kRegisterWindow)
            return SRH_INVALID;
        const auto offset = static_cast<uint32_t>(address - base);
        /* $4015 is the only readable register; the rest read back as open bus,
           which the card reports as a zero for the bus to override. */
        *value = NesApu::readable(offset) ? apu.read(offset) : 0;
        return SRH_OK;
    }

    SrhStatus write_register(uint64_t address, uint8_t value) {
        if (address < base || address - base >= nesapu::kRegisterWindow)
            return SRH_INVALID;
        apu.write(static_cast<uint32_t>(address - base), value);
        return SRH_OK;
    }

    int16_t mixed_output() const {
        uint32_t level[nesapu::kChannelCount];
        for (uint32_t channel = 0; channel < nesapu::kChannelCount; ++channel)
            level[channel] = (apu.channel_output(channel) * volume[channel]) / 100u;
        const uint32_t pulse = std::min(level[0], 15u) + std::min(level[1], 15u);
        const uint32_t tnd = std::min(level[2], 15u) * 3 + std::min(level[3], 15u) * 2 +
                             std::min(level[4], 127u);
        const int32_t normalized =
            pulse_table()[pulse] + tnd_table()[tnd] - kMixerRest;
        const int32_t scaled = normalized * static_cast<int32_t>(gain_milli) / 1000;
        return static_cast<int16_t>(std::clamp(scaled, static_cast<int32_t>(INT16_MIN),
                                               static_cast<int32_t>(INT16_MAX)));
    }
};

SrhStatus SRH_CALL read_register(void *context, uint64_t address, uint8_t *value) {
    return static_cast<Card *>(context)->read_register(address, value);
}
bool SRH_CALL fetch_sample(void *context, uint16_t address, uint8_t *value) {
    return static_cast<Card *>(context)->fetch_sample(address, value);
}
SrhStatus SRH_CALL write_register(void *context, uint64_t address, uint8_t value) {
    return static_cast<Card *>(context)->write_register(address, value);
}
/* Reading $4015 clears the frame interrupt flag, so a peek answers with the
   same value but is forbidden from doing that. */
SrhStatus SRH_CALL peek_register(void *context, uint64_t address, uint8_t *value) {
    auto &card = *static_cast<Card *>(context);
    if (!value || address < card.base || address - card.base >= nesapu::kRegisterWindow)
        return SRH_INVALID;
    const auto offset = static_cast<uint32_t>(address - card.base);
    *value = NesApu::readable(offset) ? card.apu.peek(offset) : 0;
    return SRH_OK;
}
SrhStatus SRH_CALL render(void *context, uint64_t, uint32_t frames, int16_t *interleaved) {
    if (!context || !interleaved)
        return SRH_INVALID;
    auto &card = *static_cast<Card *>(context);
    card.sample_error = SRH_OK;
    for (uint32_t frame = 0; frame < frames; ++frame) {
        card.apu.advance(card.clock_hz, card.sample_rate);
        const int16_t sample = card.mixed_output();
        interleaved[static_cast<size_t>(frame) * 2] = sample;
        interleaved[static_cast<size_t>(frame) * 2 + 1] = sample;
    }
    return card.sample_error;
}

SrhStatus SRH_CALL create(const ShouryoHost *host, SrhHandle owner, const SrhConfig *config,
                          void **result) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!srz80::sdk::valid(host) || !srz80::sdk::valid(config) || !result || !config->space ||
            !host->map || !host->query) {
            report(config, "missing host callbacks or memory space");
            return SRH_INVALID;
        }
        /* An unset size means the descriptor default; anything else has to be
           exactly the register window, because the unit decodes all of it. */
        if (config->size != 0 && config->size != nesapu::kRegisterWindow) {
            report(config, "register window must be 0x18 bytes at $4000");
            return SRH_INVALID;
        }
        if (config->base > UINT64_MAX - (nesapu::kRegisterWindow - 1)) {
            report(config, "register window would wrap the address space");
            return SRH_INVALID;
        }
        Settings settings;
        if (!Json(config).parse(settings)) {
            report(config, "invalid configuration object");
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

        /* Delta channel samples are fetched through the machine, so the unit
           follows the card into whatever space holds the sample data. */
        SrhHandle sample_space = config->space;
        if (!settings.sample_space.empty()) {
            extension = nullptr;
            if (host->query(host->context, "host.resources.v1", &extension) != SRH_OK || !extension) {
                report(config, "the host cannot resolve named spaces");
                return SRH_UNAVAILABLE;
            }
            const auto resources = static_cast<const SrhHostResourcesV1 *>(extension);
            if (!srz80::sdk::valid(resources) || !resources->lookup) {
                report(config, "the host space table is not usable");
                return SRH_UNAVAILABLE;
            }
            if (resources->lookup(resources->context, "space", settings.sample_space.c_str(),
                                  &sample_space) != SRH_OK ||
                !sample_space) {
                report(config, "the named sample space does not exist");
                return SRH_NOT_FOUND;
            }
        }

        auto card = std::make_unique<Card>();
        card->host = host;
        card->owner = owner;
        card->space = config->space;
        card->sample_space = sample_space;
        card->base = config->base;
        card->sample_base = settings.sample_base;
        card->clock_hz = settings.clock_hz;
        card->sample_rate = settings.sample_rate;
        card->gain_milli = settings.gain_milli;
        card->stream_name = settings.stream_name;
        card->sample_space_name = settings.sample_space;
        card->priority = config->priority;
        for (uint32_t channel = 0; channel < nesapu::kChannelCount; ++channel)
            card->volume[channel] = settings.volume[channel];
        card->apu.set_region(settings.region);
        card->apu.set_sample_reader(fetch_sample, card.get());
        card->reset();

        SrhMapping mapping{SRH_INIT(SrhMapping),
                           config->space,
                           config->base,
                           config->base + nesapu::kRegisterWindow - 1,
                           config->priority,
                           card.get(),
                           read_register,
                           write_register,
                           peek_register,
                           nullptr};
        auto status = host->map(host->context, owner, &mapping, &card->mapping);
        if (status != SRH_OK)
            return status;
        status = audio->register_source(audio->context, owner, card->sample_rate, 2,
                                        SRH_AUDIO_S16_STEREO, card->stream_name.c_str(), render,
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

uint32_t SRH_CALL count(void *) { return kPropertyCount; }

SrhStatus SRH_CALL info(void *, uint32_t index, SrhProperty *out) {
    if (!srz80::sdk::valid(out) || index >= kPropertyCount)
        return SRH_INVALID;
    static const char *names[kPropertyCount] = {
        "base",          "clock_hz",      "region",         "sample_rate",   "gain_milli",
        "stream_name",   "sample_space",  "sample_base",    "volume_pulse1", "volume_pulse2",
        "volume_triangle", "volume_noise", "volume_delta"};
    static const char *descriptions[kPropertyCount] = {
        "First mapped APU register ($4000)",
        "CPU clock the unit is driven at",
        "Console timing for the frame sequencer and the noise and delta periods",
        "Native output rate of the audio stream",
        "Output level, in thousandths",
        "Name of the registered audio stream",
        "Space the delta channel fetches sample data from; empty uses the register space",
        "Offset added to a delta channel sample address before the fetch",
        "Pulse 1 level, in percent",
        "Pulse 2 level, in percent",
        "Triangle level, in percent",
        "Noise level, in percent",
        "Delta level, in percent"};
    static const uint32_t kinds[kPropertyCount] = {SRH_UNSIGNED, SRH_UNSIGNED, SRH_ENUM,
                                                   SRH_UNSIGNED, SRH_UNSIGNED, SRH_TEXT,
                                                   SRH_TEXT,     SRH_UNSIGNED, SRH_UNSIGNED,
                                                   SRH_UNSIGNED, SRH_UNSIGNED, SRH_UNSIGNED,
                                                   SRH_UNSIGNED};
    static const uint32_t bits[kPropertyCount] = {16, 32, 0, 32, 32, 0, 0, 32, 32, 32, 32, 32, 32};
    static const uint32_t bases[kPropertyCount] = {16, 10, 0, 10, 10, 0, 0, 16, 10, 10, 10, 10, 10};
    static const uint32_t flags[kPropertyCount] = {
        SRH_PROPERTY_PERSISTENT,
        SRH_PROPERTY_LIVE_EDIT | SRH_PROPERTY_PERSISTENT,
        SRH_PROPERTY_LIVE_EDIT | SRH_PROPERTY_PERSISTENT,
        SRH_PROPERTY_PERSISTENT,
        SRH_PROPERTY_LIVE_EDIT | SRH_PROPERTY_PERSISTENT,
        SRH_PROPERTY_PERSISTENT,
        SRH_PROPERTY_PERSISTENT,
        SRH_PROPERTY_PERSISTENT,
        SRH_PROPERTY_LIVE_EDIT | SRH_PROPERTY_RUNTIME,
        SRH_PROPERTY_LIVE_EDIT | SRH_PROPERTY_RUNTIME,
        SRH_PROPERTY_LIVE_EDIT | SRH_PROPERTY_RUNTIME,
        SRH_PROPERTY_LIVE_EDIT | SRH_PROPERTY_RUNTIME,
        SRH_PROPERTY_LIVE_EDIT | SRH_PROPERTY_RUNTIME};
    static const char *enum_labels[kPropertyCount] = {nullptr, nullptr, "ntsc|pal|dendy", nullptr,
                                                      nullptr, nullptr, nullptr,         nullptr,
                                                      nullptr, nullptr, nullptr,         nullptr,
                                                      nullptr};
    *out = {SRH_INIT(SrhProperty), names[index], "NES APU", descriptions[index], kinds[index],
            bits[index], bases[index], 1, enum_labels[index], flags[index]};
    return SRH_OK;
}

SrhStatus SRH_CALL get(void *context, uint32_t index, SrhValue *out) {
    if (!srz80::sdk::valid(out) || !context || index >= kPropertyCount)
        return SRH_INVALID;
    auto &card = *static_cast<Card *>(context);
    out->unsigned_value = 0;
    out->signed_value = 0;
    out->text[0] = '\0';
    switch (index) {
    case 0:
        out->unsigned_value = card.base;
        break;
    case 1:
        out->unsigned_value = card.clock_hz;
        break;
    case 2:
        out->unsigned_value = static_cast<uint32_t>(card.apu.region());
        break;
    case 3:
        out->unsigned_value = card.sample_rate;
        break;
    case 4:
        out->unsigned_value = card.gain_milli;
        break;
    case 5:
        std::snprintf(out->text, sizeof(out->text), "%s", card.stream_name.c_str());
        break;
    case 6:
        std::snprintf(out->text, sizeof(out->text), "%s", card.sample_space_name.c_str());
        break;
    case 7:
        out->unsigned_value = card.sample_base;
        break;
    default:
        out->unsigned_value = card.volume[index - 8];
        break;
    }
    return SRH_OK;
}

SrhStatus SRH_CALL set(void *context, uint32_t index, const SrhValue *in) {
    if (!srz80::sdk::valid(in) || !context || index >= kPropertyCount)
        return SRH_INVALID;
    auto &card = *static_cast<Card *>(context);
    switch (index) {
    case 0: {
        if (in->unsigned_value > UINT64_MAX - (nesapu::kRegisterWindow - 1))
            return SRH_INVALID;
        if (in->unsigned_value == card.base)
            return SRH_OK;
        SrhMapping mapping{SRH_INIT(SrhMapping),
                           card.space,
                           in->unsigned_value,
                           in->unsigned_value + nesapu::kRegisterWindow - 1,
                           card.priority,
                           &card,
                           read_register,
                           write_register,
                           peek_register,
                           nullptr};
        SrhHandle replacement = 0;
        const auto status = card.host->map(card.host->context, card.owner, &mapping, &replacement);
        if (status != SRH_OK)
            return status;
        if (!card.host->unmap || card.host->unmap(card.host->context, card.mapping) != SRH_OK) {
            if (card.host->unmap)
                card.host->unmap(card.host->context, replacement);
            return SRH_ERROR;
        }
        card.base = in->unsigned_value;
        card.mapping = replacement;
        return SRH_OK;
    }
    case 1:
        if (in->unsigned_value < 1000000 || in->unsigned_value > 4000000)
            return SRH_INVALID;
        card.clock_hz = static_cast<uint32_t>(in->unsigned_value);
        return SRH_OK;
    case 2:
        if (in->unsigned_value > 2)
            return SRH_INVALID;
        card.apu.set_region(static_cast<Region>(in->unsigned_value));
        return SRH_OK;
    case 4:
        if (in->unsigned_value < 1 || in->unsigned_value > 20000)
            return SRH_INVALID;
        card.gain_milli = static_cast<uint32_t>(in->unsigned_value);
        return SRH_OK;
    case 3:
    case 5:
    case 6:
    case 7:
        /* The stream rate, its name and the delta channel's fetch window are
           fixed when the card is created; only a project reload may change
           them, because the host has to be told about each of them. */
        return SRH_INVALID;
    default:
        if (in->unsigned_value > 200)
            return SRH_INVALID;
        card.volume[index - 8] = static_cast<uint32_t>(in->unsigned_value);
        return SRH_OK;
    }
}

SrhStatus SRH_CALL save_state(void *context, uint8_t *buffer, uint64_t *size) {
    if (!context || !size)
        return SRH_INVALID;
    /* The carried cycle remainder comes first, then the unit's own image. */
    constexpr uint64_t required = 8 + NesApu::state_size();
    if (!buffer) {
        *size = required;
        return SRH_OK;
    }
    if (*size < required) {
        *size = required;
        return SRH_UNAVAILABLE;
    }
    auto &card = *static_cast<Card *>(context);
    const uint64_t accumulator = card.apu.accumulator();
    for (uint32_t byte = 0; byte < 8; ++byte)
        buffer[byte] = static_cast<uint8_t>(accumulator >> (byte * 8));
    card.apu.save_state(buffer + 8);
    *size = required;
    return SRH_OK;
}

SrhStatus SRH_CALL load_state(void *context, const uint8_t *buffer, uint64_t size) {
    constexpr uint64_t required = 8 + NesApu::state_size();
    if (!context || !buffer || size != required)
        return SRH_INVALID;
    auto &card = *static_cast<Card *>(context);
    if (!card.apu.load_state(buffer + 8, NesApu::state_size()))
        return SRH_INVALID;
    uint64_t accumulator = 0;
    for (uint32_t byte = 0; byte < 8; ++byte)
        accumulator |= static_cast<uint64_t>(buffer[byte]) << (byte * 8);
    if (!card.apu.set_accumulator(accumulator, card.sample_rate))
        return SRH_INVALID;
    card.sample_error = SRH_OK;
    return SRH_OK;
}

const SrhCardDescriptor descriptor{SRH_INIT(SrhCardDescriptor),
                                   "Audio",
                                   "NES APU",
                                   "Ricoh 2A03 audio processing unit with sample fetch through "
                                   "the machine bus",
                                   0x4000,
                                   nesapu::kRegisterWindow,
                                   0,
                                   0,
                                   kDefaultClockHz,
                                   0,
                                   R"({"clock_hz":1789773,"region":"ntsc","sample_rate":44100,"gain_milli":400,"stream_name":"NES APU","sample_space":"","sample_base":0,"volume_pulse1":100,"volume_pulse2":100,"volume_triangle":100,"volume_noise":100,"volume_delta":100})",
                                   nullptr,
                                   nullptr,
                                   nullptr,
                                   0};
const SrhPlugin api{SRH_INIT(SrhPlugin), "nesapu", create,    destroy,      reset, count,
                    info,                get,      set,       save_state,   load_state,
                    &descriptor,         nullptr,  nullptr};
} // namespace

extern "C" SRH_EXPORT const SrhPlugin *SRH_CALL srz80_plugin_init(const ShouryoHost *host) {
    return srz80::sdk::valid(host) ? &api : nullptr;
}
