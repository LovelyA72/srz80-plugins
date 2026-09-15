#include <boundary.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>

namespace {
constexpr uint32_t kRegisterCount = 5;
constexpr uint32_t kFifoCapacity = 32;
constexpr uint32_t kDefaultRate = 22'050;
constexpr uint64_t kPhaseOne = uint64_t{1} << 32;

struct Settings {
    uint32_t sample_rate = 44'100;
    std::string stream_name = "PCM DAC";
};

bool parse_settings(const SrhConfig *config, Settings &settings) {
    if (!config || !srz80::sdk::has_field(config, &SrhConfig::config_json) || !config->config_json)
        return true;
    const auto json = nlohmann::json::parse(config->config_json,
        config->config_json + config->config_json_size, nullptr, false);
    if (!json.is_object()) return false;
    for (auto it = json.begin(); it != json.end(); ++it)
        if (it.key() != "stream_name" && it.key() != "sample_rate") return false;
    if (json.contains("stream_name") && !json["stream_name"].is_string()) return false;
    if (json.contains("sample_rate") && !json["sample_rate"].is_number_unsigned()) return false;
    if (json.contains("stream_name")) settings.stream_name = json["stream_name"].get<std::string>();
    if (json.contains("sample_rate")) {
        const auto value = json["sample_rate"].get<uint64_t>();
        if (value > UINT32_MAX) return false;
        settings.sample_rate = static_cast<uint32_t>(value);
    }
    return !settings.stream_name.empty() && settings.stream_name.size() <= 256 &&
           settings.stream_name.find('\0') == std::string::npos &&
           settings.sample_rate >= 8'000 && settings.sample_rate <= 192'000;
}

struct Dac {
    uint32_t output_rate = 0;
    uint32_t fifo_rate = kDefaultRate;
    uint64_t phase = 0;
    std::array<uint8_t, kFifoCapacity> fifo{};
    uint8_t read_index = 0, write_index = 0, count = 0;
    uint8_t held = 0x80;
    uint8_t current = 0x80, next = 0x80;
    bool resampler_primed = false;
    bool fifo_mode = false, underflow = false, overflow = false;

    void clear_fifo() {
        read_index = write_index = count = 0;
        current = next = 0x80;
        phase = 0;
        resampler_primed = false;
    }
    void reset() {
        fifo_mode = false; underflow = overflow = false; held = 0x80;
        fifo_rate = output_rate < kDefaultRate ? output_rate : kDefaultRate;
        clear_fifo();
    }
    uint8_t status() const {
        return count | (underflow ? 0x40 : 0) | (overflow ? 0x80 : 0);
    }
    void push(uint8_t value) {
        if (count == kFifoCapacity) { overflow = true; return; }
        fifo[write_index] = value;
        write_index = (write_index + 1) % kFifoCapacity;
        ++count;
    }
    uint8_t consume() {
        if (count == 0) { underflow = true; return 0x80; }
        const uint8_t value = fifo[read_index];
        read_index = (read_index + 1) % kFifoCapacity;
        --count;
        return value;
    }
    void write(uint32_t offset, uint8_t value) {
        switch (offset) {
        case 0: if (fifo_mode) push(value); else held = value; break;
        case 1: {
            const bool next_mode = (value & 1) != 0;
            if (next_mode != fifo_mode) { fifo_mode = next_mode; clear_fifo(); }
            if (value & 2) clear_fifo();
            break;
        }
        case 2:
            if (value & 0x40) underflow = false;
            if (value & 0x80) overflow = false;
            break;
        case 3: fifo_rate = (fifo_rate & 0xFF00u) | value; break;
        case 4: fifo_rate = (fifo_rate & 0x00FFu) | (uint32_t(value) << 8); break;
        }
        if (!fifo_rate) fifo_rate = 1;
    }
    uint8_t read(uint32_t offset) const {
        switch (offset) {
        case 0: return held;
        case 1: return fifo_mode ? 1 : 0;
        case 2: return status();
        case 3: return static_cast<uint8_t>(fifo_rate);
        case 4: return static_cast<uint8_t>(fifo_rate >> 8);
        default: return 0;
        }
    }
    void render(uint32_t frames, int16_t *out) {
        for (uint32_t frame = 0; frame < frames; ++frame) {
            if (fifo_mode) {
                // This follows the YMW258-style linear source resampler. It makes every
                // 16-bit FIFO rate representable, including rates above host output.
                if (!resampler_primed) {
                    current = consume();
                    next = consume();
                    phase = 0;
                    resampler_primed = true;
                }
                const int64_t current_sample = (int32_t(current) - 128) << 8;
                const int64_t next_sample = (int32_t(next) - 128) << 8;
                const int16_t sample = static_cast<int16_t>(current_sample +
                    ((next_sample - current_sample) * static_cast<int64_t>(phase) >> 32));
                out[size_t(frame) * 2] = sample;
                out[size_t(frame) * 2 + 1] = sample;
                phase += (static_cast<uint64_t>(fifo_rate) << 32) / output_rate;
                while (phase >= kPhaseOne) {
                    phase -= kPhaseOne;
                    current = next;
                    next = consume();
                }
                continue;
            }
            const int16_t sample = static_cast<int16_t>((int32_t(held) - 128) << 8);
            out[size_t(frame) * 2] = sample;
            out[size_t(frame) * 2 + 1] = sample;
        }
    }
};

struct Card { uint64_t base = 0; SrhHandle mapping = 0, stream = 0; Dac dac; };

SrhStatus access(void *context, uint64_t address, uint8_t *value, bool peek) {
    auto &card = *static_cast<Card *>(context);
    if (!value || address < card.base || address - card.base >= kRegisterCount) return SRH_INVALID;
    *value = card.dac.read(uint32_t(address - card.base));
    (void)peek;
    return SRH_OK;
}
SrhStatus SRH_CALL read(void *c, uint64_t a, uint8_t *v) { return access(c, a, v, false); }
SrhStatus SRH_CALL peek(void *c, uint64_t a, uint8_t *v) { return access(c, a, v, true); }
SrhStatus SRH_CALL write(void *context, uint64_t address, uint8_t value) {
    auto &card = *static_cast<Card *>(context);
    if (address < card.base || address - card.base >= kRegisterCount) return SRH_INVALID;
    card.dac.write(uint32_t(address - card.base), value);
    return SRH_OK;
}
SrhStatus SRH_CALL render(void *context, uint64_t, uint32_t frames, int16_t *out) {
    if (!out) return SRH_INVALID;
    static_cast<Card *>(context)->dac.render(frames, out);
    return SRH_OK;
}
SrhStatus SRH_CALL create(const ShouryoHost *host, SrhHandle owner, const SrhConfig *config, void **out) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!srz80::sdk::valid(host) || !srz80::sdk::valid(config) || !out || !host->map ||
            !config->space || config->size != kRegisterCount || config->base > UINT64_MAX - 4)
            return SRH_INVALID;
        Settings settings;
        if (!parse_settings(config, settings)) return SRH_INVALID;
        const void *extension = nullptr;
        if (!host->query || host->query(host->context, "host.audio.v1", &extension) != SRH_OK || !extension)
            return SRH_UNAVAILABLE;
        const auto *audio = static_cast<const SrhHostAudioV1 *>(extension);
        if (!srz80::sdk::valid(audio) || !audio->register_source ||
            audio->channels != 2 || audio->format != SRH_AUDIO_S16_STEREO)
            return SRH_INVALID;
        auto card = std::make_unique<Card>();
        card->base = config->base; card->dac.output_rate = settings.sample_rate; card->dac.reset();
        SrhMapping mapping{SRH_INIT(SrhMapping), config->space, config->base, config->base + 4,
            config->priority, card.get(), read, write, peek, nullptr};
        auto status = host->map(host->context, owner, &mapping, &card->mapping);
        if (status != SRH_OK) return status;
        status = audio->register_source(audio->context, owner, settings.sample_rate, 2,
            SRH_AUDIO_S16_STEREO, settings.stream_name.c_str(), render, card.get(), &card->stream);
        if (status != SRH_OK) return status;
        *out = card.release();
        return SRH_OK;
    });
}
void SRH_CALL destroy(void *context) { delete static_cast<Card *>(context); }
SrhStatus SRH_CALL reset(void *context, uint32_t) { static_cast<Card *>(context)->dac.reset(); return SRH_OK; }

constexpr uint32_t kPropertyCount = 5;
uint32_t SRH_CALL property_count(void *) { return kPropertyCount; }
SrhStatus SRH_CALL property_info(void *, uint32_t index, SrhProperty *out) {
    if (!srz80::sdk::valid(out) || index >= kPropertyCount) return SRH_INVALID;
    static constexpr const char *names[] = {"Mode", "DAC value", "FIFO level", "FIFO rate", "Status"};
    static constexpr const char *descriptions[] = {"0: direct write; 1: 32-byte FIFO", "Current unsigned PCM value", "Queued FIFO bytes", "FIFO samples per second", "Sticky underflow and overflow flags"};
    const uint32_t bits[] = {1, 8, 6, 16, 8};
    *out = {SRH_INIT(SrhProperty), names[index], "PCM DAC", descriptions[index],
            index == 0 ? SRH_ENUM : SRH_UNSIGNED, bits[index], 10, 0,
            index == 0 ? "direct|fifo" : nullptr, SRH_PROPERTY_RUNTIME};
    return SRH_OK;
}
SrhStatus SRH_CALL property_get(void *context, uint32_t index, SrhValue *out) {
    if (!srz80::sdk::valid(out) || index >= kPropertyCount) return SRH_INVALID;
    const auto &dac = static_cast<Card *>(context)->dac;
    *out = {SRH_INIT(SrhValue), 0, 0, {}};
    switch (index) {
    case 0: out->unsigned_value = dac.fifo_mode; break;
    case 1: out->unsigned_value = dac.held; break;
    case 2: out->unsigned_value = dac.count; break;
    case 3: out->unsigned_value = dac.fifo_rate; break;
    case 4: out->unsigned_value = dac.status(); break;
    }
    return SRH_OK;
}
SrhStatus SRH_CALL property_set(void *, uint32_t, const SrhValue *) { return SRH_INVALID; }

constexpr uint64_t kStateSize = 4 + 1 + 1 + 1 + 1 + 1 + 1 + 4 + 8 + 1 + 1 + 1 + kFifoCapacity;
void put_u32(uint8_t *&p, uint32_t value) { for (unsigned i = 0; i < 4; ++i) *p++ = uint8_t(value >> (8 * i)); }
uint32_t take_u32(const uint8_t *&p) { uint32_t value = 0; for (unsigned i = 0; i < 4; ++i) value |= uint32_t(*p++) << (8 * i); return value; }
void put_u64(uint8_t *&p, uint64_t value) { for (unsigned i = 0; i < 8; ++i) *p++ = uint8_t(value >> (8 * i)); }
uint64_t take_u64(const uint8_t *&p) { uint64_t value = 0; for (unsigned i = 0; i < 8; ++i) value |= uint64_t(*p++) << (8 * i); return value; }
SrhStatus SRH_CALL save_state(void *context, uint8_t *buffer, uint64_t *size) {
    if (!size) return SRH_INVALID;
    if (!buffer) { *size = kStateSize; return SRH_OK; }
    if (*size < kStateSize) { *size = kStateSize; return SRH_UNAVAILABLE; }
    const auto &dac = static_cast<Card *>(context)->dac;
    std::memcpy(buffer, "DAC2", 4); uint8_t *p = buffer + 4;
    *p++ = dac.fifo_mode; *p++ = dac.held; *p++ = dac.read_index; *p++ = dac.write_index;
    *p++ = dac.count; *p++ = (dac.underflow ? 1 : 0) | (dac.overflow ? 2 : 0);
    put_u32(p, dac.fifo_rate); put_u64(p, dac.phase); *p++ = dac.current; *p++ = dac.next;
    *p++ = dac.resampler_primed ? 1 : 0; std::memcpy(p, dac.fifo.data(), dac.fifo.size());
    *size = kStateSize;
    return SRH_OK;
}
SrhStatus SRH_CALL load_state(void *context, const uint8_t *buffer, uint64_t size) {
    if (!buffer || size != kStateSize || std::memcmp(buffer, "DAC2", 4)) return SRH_INVALID;
    const uint8_t *p = buffer + 4;
    Dac next = static_cast<Card *>(context)->dac;
    const uint8_t mode = *p++, held = *p++, read_index = *p++, write_index = *p++, count = *p++, flags = *p++;
    const uint32_t rate = take_u32(p);
    const uint64_t phase = take_u64(p);
    const uint8_t current = *p++, next_sample = *p++, primed = *p++;
    if (mode > 1 || read_index >= kFifoCapacity || write_index >= kFifoCapacity || count > kFifoCapacity ||
        flags > 3 || !rate || rate > UINT16_MAX || phase >= kPhaseOne || primed > 1) return SRH_INVALID;
    next.fifo_mode = mode != 0; next.held = held; next.read_index = read_index; next.write_index = write_index;
    next.count = count; next.underflow = (flags & 1) != 0; next.overflow = (flags & 2) != 0;
    next.fifo_rate = rate; next.phase = phase; next.current = current; next.next = next_sample;
    next.resampler_primed = primed != 0; std::memcpy(next.fifo.data(), p, next.fifo.size());
    static_cast<Card *>(context)->dac = next;
    return SRH_OK;
}

const SrhCardDescriptor descriptor{SRH_INIT(SrhCardDescriptor), "Audio", "PCM DAC",
    "8-bit unsigned PCM DAC with direct and programmable-rate 32-byte FIFO modes",
    0xD0, kRegisterCount, 0, 0, 0, 0,
    R"({"sample_rate":44100,"stream_name":"PCM DAC"})", nullptr, nullptr,
    nullptr, 0};
const SrhPlugin api{SRH_INIT(SrhPlugin), "dac", create, destroy, reset, property_count, property_info,
                    property_get, property_set, save_state, load_state, &descriptor, nullptr, nullptr};
} // namespace

extern "C" SRH_EXPORT const SrhPlugin *SRH_CALL srz80_plugin_init(const ShouryoHost *host) {
    return srz80::sdk::valid(host) ? &api : nullptr;
}
