#include <boundary.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

namespace {
constexpr uint32_t kVoiceCount = 2;
constexpr uint32_t kRegistersPerVoice = 4;
constexpr uint32_t kVolumeRegister = kVoiceCount * kRegistersPerVoice;
constexpr uint32_t kRegisterCount = kVolumeRegister + 1;
constexpr uint32_t kDefaultClockHz = 1'000'000;
constexpr uint32_t kDefaultSampleRate = 44'100;

struct Settings {
    uint32_t clock_hz = kDefaultClockHz;
    uint32_t sample_rate = kDefaultSampleRate;
    std::string stream_name = "Intro Tone";
};

// This deliberately accepts only the small flat JSON object used by this card.
class Json {
  public:
    explicit Json(const SrhConfig *config)
        : text_(config && srz80::sdk::has_field(config, &SrhConfig::config_json) && config->config_json
                    ? std::string(config->config_json, config->config_json_size)
                    : "{}") {}

    bool parse(Settings &settings) {
        if (!take('{')) return false;
        if (take('}')) return done();
        while (true) {
            std::string key, value;
            bool quoted = false;
            if (!string(key) || !take(':') || !scalar(value, quoted)) return false;
            uint64_t number = 0;
            if (key == "clock_hz") {
                if (!number_value(value, quoted, number) || number == 0 || number > 100'000'000) return false;
                settings.clock_hz = static_cast<uint32_t>(number);
            } else if (key == "sample_rate") {
                if (!number_value(value, quoted, number) || number < 8'000 || number > 192'000) return false;
                settings.sample_rate = static_cast<uint32_t>(number);
            } else if (key == "stream_name") {
                if (!quoted || value.empty() || value.size() > 256) return false;
                settings.stream_name = value;
            } else return false;
            if (take('}')) return done();
            if (!take(',')) return false;
        }
    }

  private:
    void skip() { while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\r' || text_[pos_] == '\n')) ++pos_; }
    bool take(char expected) { skip(); if (pos_ == text_.size() || text_[pos_] != expected) return false; ++pos_; return true; }
    bool string(std::string &out) {
        skip(); if (pos_ == text_.size() || text_[pos_++] != '"') return false;
        while (pos_ < text_.size() && text_[pos_] != '"') {
            if (text_[pos_] == '\\' || static_cast<unsigned char>(text_[pos_]) < 0x20) return false;
            out += text_[pos_++];
        }
        if (pos_ == text_.size()) return false;
        ++pos_; return true;
    }
    bool scalar(std::string &out, bool &quoted) {
        skip();
        if (pos_ < text_.size() && text_[pos_] == '"') { quoted = true; return string(out); }
        const size_t first = pos_;
        while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
        if (first == pos_) return false;
        out.assign(text_, first, pos_ - first); return true;
    }
    static bool number_value(const std::string &value, bool quoted, uint64_t &out) {
        std::string digits = value;
        uint32_t base = 10;
        if (quoted && digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) { base = 16; digits.erase(0, 2); }
        if (digits.empty()) return false;
        out = 0;
        for (char c : digits) {
            const uint32_t digit = c >= '0' && c <= '9' ? static_cast<uint32_t>(c - '0') : c >= 'a' && c <= 'f' ? static_cast<uint32_t>(c - 'a' + 10) : c >= 'A' && c <= 'F' ? static_cast<uint32_t>(c - 'A' + 10) : base;
            if (digit >= base || out > (UINT64_MAX - digit) / base) return false;
            out = out * base + digit;
        }
        return true;
    }
    bool done() { skip(); return pos_ == text_.size(); }
    std::string text_;
    size_t pos_ = 0;
};

struct Voice {
    uint8_t frequency_msb = 0;
    uint8_t frequency_lsb = 0;
    uint8_t decay = 0;
    uint8_t trigger = 0xff;
    bool gated = false;
    uint64_t phase_clocks = 0;
    uint64_t clock_remainder = 0;
    uint64_t envelope = 0;
};

struct Tone {
    SrhHandle owner = 0, space = 0, mapping = 0, stream = 0;
    uint64_t base = 0;
    uint32_t clock_hz = kDefaultClockHz;
    uint32_t sample_rate = kDefaultSampleRate;
    uint8_t volume = 0xff;
    Voice voices[kVoiceCount];

    void reset() { volume = 0xff; for (auto &voice : voices) voice = {}; }
    uint16_t divisor(const Voice &voice) const {
        const uint16_t value = static_cast<uint16_t>((static_cast<uint16_t>(voice.frequency_msb) << 8) | voice.frequency_lsb);
        return value == 0 ? 1 : value;
    }
    SrhStatus write(uint64_t address, uint8_t value) {
        if (address < base || address - base >= kRegisterCount) return SRH_INVALID;
        const uint32_t offset = static_cast<uint32_t>(address - base);
        if (offset == kVolumeRegister) { volume = value; return SRH_OK; }
        Voice &voice = voices[offset / kRegistersPerVoice];
        switch (offset % kRegistersPerVoice) {
        case 0: voice.frequency_msb = value; break;
        case 1: voice.frequency_lsb = value; break;
        case 2: voice.decay = value; break;
        default:
            voice.trigger = value;
            voice.gated = value != 0xff;
            if (voice.gated) { voice.phase_clocks = 0; voice.clock_remainder = 0; voice.envelope = UINT32_MAX; }
            else voice.envelope = 0;
            break;
        }
        return SRH_OK;
    }
};

SrhStatus SRH_CALL read(void *context, uint64_t address, uint8_t *value) {
    auto &tone = *static_cast<Tone *>(context);
    if (!value || address < tone.base || address - tone.base >= kRegisterCount) return SRH_INVALID;
    *value = 0; // The card's registers are write-only; use the Device Inspector for their latched values.
    return SRH_OK;
}
SrhStatus SRH_CALL write(void *context, uint64_t address, uint8_t value) { return static_cast<Tone *>(context)->write(address, value); }
SrhStatus SRH_CALL render(void *context, uint64_t, uint32_t frames, int16_t *interleaved) {
    if (!interleaved) return SRH_INVALID;
    auto &tone = *static_cast<Tone *>(context);
    for (uint32_t frame = 0; frame < frames; ++frame) {
        int64_t mixed = 0;
        for (auto &voice : tone.voices) {
            if (!voice.gated || voice.envelope == 0) continue;
            const uint16_t period = tone.divisor(voice);
            const bool high = voice.phase_clocks < period;
            constexpr int64_t amplitude = 12'000;
            mixed += (high ? amplitude : -amplitude) * static_cast<int64_t>(voice.envelope) / UINT32_MAX;
            voice.clock_remainder += tone.clock_hz;
            const uint64_t clocks = voice.clock_remainder / tone.sample_rate;
            voice.clock_remainder %= tone.sample_rate;
            voice.phase_clocks = (voice.phase_clocks + clocks) % (static_cast<uint64_t>(period) * 2);
            // A nonzero decay fades linearly to silence in 256 / decay seconds.
            if (voice.decay) {
                const uint64_t drop = (static_cast<uint64_t>(UINT32_MAX) * voice.decay) / (256u * tone.sample_rate);
                voice.envelope = drop >= voice.envelope ? 0 : voice.envelope - drop;
            }
        }
        mixed = mixed * tone.volume / 255;
        const auto sample = static_cast<int16_t>(std::clamp(mixed, static_cast<int64_t>(INT16_MIN), static_cast<int64_t>(INT16_MAX)));
        interleaved[static_cast<size_t>(frame) * 2] = sample;
        interleaved[static_cast<size_t>(frame) * 2 + 1] = sample;
    }
    return SRH_OK;
}
SrhStatus SRH_CALL create(const ShouryoHost *host, SrhHandle owner, const SrhConfig *config, void **result) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!srz80::sdk::valid(host) || !srz80::sdk::valid(config) || !result || !config->space || config->size != kRegisterCount || config->base > UINT64_MAX - (kRegisterCount - 1)) return SRH_INVALID;
        Settings settings;
        if (!Json(config).parse(settings)) return SRH_INVALID;
        const void *extension = nullptr;
        if (!host->query || host->query(host->context, "host.audio.v1", &extension) != SRH_OK || !extension) return SRH_UNAVAILABLE;
        const auto *audio = static_cast<const SrhHostAudioV1 *>(extension);
        if (!srz80::sdk::valid(audio) || !audio->register_source) return SRH_INVALID;
        auto tone = std::make_unique<Tone>();
        tone->owner = owner; tone->space = config->space; tone->base = config->base; tone->clock_hz = settings.clock_hz; tone->sample_rate = settings.sample_rate;
        SrhMapping mapping{SRH_INIT(SrhMapping), config->space, config->base,
                           config->base + kRegisterCount - 1, config->priority, tone.get(),
                           read, write, read, nullptr};
        auto status = host->map(host->context, owner, &mapping, &tone->mapping);
        if (status != SRH_OK) return status;
        status = audio->register_source(audio->context, owner, settings.sample_rate, 2, SRH_AUDIO_S16_STEREO, settings.stream_name.c_str(), render, tone.get(), &tone->stream);
        if (status != SRH_OK) return status;
        *result = tone.release(); return SRH_OK;
    });
}
void SRH_CALL destroy(void *context) { delete static_cast<Tone *>(context); }
SrhStatus SRH_CALL reset(void *context, uint32_t) { static_cast<Tone *>(context)->reset(); return SRH_OK; }
uint32_t SRH_CALL count(void *) { return kRegisterCount + 2; }
SrhStatus SRH_CALL info(void *, uint32_t index, SrhProperty *out) {
    if (!srz80::sdk::valid(out) || index >= kRegisterCount + 2) return SRH_INVALID;
    static thread_local char name[32], group[32], description[128];
    if (index == 0) { std::snprintf(name, sizeof(name), "base"); std::snprintf(group, sizeof(group), "Tone"); std::snprintf(description, sizeof(description), "First mapped I/O port"); *out = {SRH_INIT(SrhProperty), name, group, description, SRH_UNSIGNED, 16, 16, 0, nullptr, 0}; return SRH_OK; }
    if (index == 1) { std::snprintf(name, sizeof(name), "clock_hz"); std::snprintf(group, sizeof(group), "Tone"); std::snprintf(description, sizeof(description), "Tone generator clock"); *out = {SRH_INIT(SrhProperty), name, group, description, SRH_UNSIGNED, 32, 10, 0, nullptr, 0}; return SRH_OK; }
    const uint32_t reg = index - 2;
    static const char *names[] = {"Frequency MSB", "Frequency LSB", "Decay", "Trigger"};
    const char *desc = reg == kVolumeRegister ? "Global output volume (FF is 100%, 00 is silent)" : reg % 4 == 0 ? "High byte of frequency divisor; larger values lower pitch" : reg % 4 == 1 ? "Low byte of frequency divisor; larger values lower pitch" : reg % 4 == 2 ? "Decay rate (00 holds; FF fades in about one second)" : "Latched trigger (FF stops; any other value restarts)";
    if (reg == kVolumeRegister) { std::snprintf(name, sizeof(name), "Volume"); std::snprintf(group, sizeof(group), "Global"); }
    else { std::snprintf(name, sizeof(name), "%s", names[reg % 4]); std::snprintf(group, sizeof(group), "Voice %u", reg / 4 + 1); }
    std::snprintf(description, sizeof(description), "%s", desc);
    *out = {SRH_INIT(SrhProperty), name, group, description, SRH_UNSIGNED, 8, 16, 0, nullptr, SRH_PROPERTY_RUNTIME};
    return SRH_OK;
}
SrhStatus SRH_CALL get(void *context, uint32_t index, SrhValue *out) {
    if (!srz80::sdk::valid(out) || index >= kRegisterCount + 2) return SRH_INVALID;
    auto &tone = *static_cast<Tone *>(context); out->unsigned_value = 0; out->signed_value = 0;
    if (index == 0) out->unsigned_value = tone.base;
    else if (index == 1) out->unsigned_value = tone.clock_hz;
    else { const uint32_t reg = index - 2; if (reg == kVolumeRegister) out->unsigned_value = tone.volume; else { const auto &voice = tone.voices[reg / 4]; const uint8_t values[] = {voice.frequency_msb, voice.frequency_lsb, voice.decay, voice.trigger}; out->unsigned_value = values[reg % 4]; } }
    return SRH_OK;
}
SrhStatus SRH_CALL set(void *, uint32_t, const SrhValue *) { return SRH_INVALID; }
SrhStatus SRH_CALL save_state(void *context, uint8_t *buffer, uint64_t *size) {
    constexpr uint64_t kStateSize = 1 + kVoiceCount * 29;
    if (!size) return SRH_INVALID;
    if (!buffer) { *size = kStateSize; return SRH_OK; }
    if (*size < kStateSize) { *size = kStateSize; return SRH_UNAVAILABLE; }
    const auto &tone = *static_cast<Tone *>(context);
    uint8_t *out = buffer;
    *out++ = tone.volume;
    for (const auto &voice : tone.voices) {
        *out++ = voice.frequency_msb; *out++ = voice.frequency_lsb; *out++ = voice.decay;
        *out++ = voice.trigger; *out++ = voice.gated ? 1 : 0;
        for (uint64_t value : {voice.phase_clocks, voice.clock_remainder, voice.envelope})
            for (uint32_t byte = 0; byte < 8; ++byte) *out++ = static_cast<uint8_t>(value >> (byte * 8));
    }
    *size = kStateSize;
    return SRH_OK;
}
SrhStatus SRH_CALL load_state(void *context, const uint8_t *buffer, uint64_t size) {
    constexpr uint64_t kStateSize = 1 + kVoiceCount * 29;
    if (!buffer || size != kStateSize) return SRH_INVALID;
    auto &tone = *static_cast<Tone *>(context);
    const uint8_t *in = buffer;
    tone.volume = *in++;
    for (auto &voice : tone.voices) {
        voice.frequency_msb = *in++; voice.frequency_lsb = *in++; voice.decay = *in++;
        voice.trigger = *in++; voice.gated = *in++ != 0;
        for (uint64_t *value : {&voice.phase_clocks, &voice.clock_remainder, &voice.envelope}) {
            *value = 0;
            for (uint32_t byte = 0; byte < 8; ++byte) *value |= static_cast<uint64_t>(*in++) << (byte * 8);
        }
        voice.phase_clocks %= static_cast<uint64_t>(tone.divisor(voice)) * 2;
        voice.clock_remainder %= tone.sample_rate;
    }
    return SRH_OK;
}

const SrhCardDescriptor descriptor{SRH_INIT(SrhCardDescriptor), "Audio", "Tone", "Basic dual tone melody card", 0xC0, kRegisterCount, 0, 0, 0, 0, R"({"clock_hz":1000000,"sample_rate":44100,"stream_name":"Intro Tone"})", nullptr, nullptr};
const SrhPlugin api{SRH_INIT(SrhPlugin), "tone", create, destroy, reset, count, info, get, set,
                    save_state, load_state, &descriptor};
} // namespace

extern "C" SRH_EXPORT const SrhPlugin *SRH_CALL srz80_plugin_init(const ShouryoHost *host) { return srz80::sdk::valid(host) ? &api : nullptr; }
