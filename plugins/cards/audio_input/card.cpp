#include <state.hpp>
#include <boundary.hpp>
#include <json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>

namespace {
constexpr uint32_t kRegisterCount = 16;
constexpr uint32_t kFifoCapacity = 4096;
constexpr uint32_t kReadFrames = 512;

struct Settings { uint32_t sample_rate = 16'000, channel = 0; };

bool parse_settings(const SrhConfig *config, Settings &settings) {
    if (!config || !srz80::sdk::has_field(config, &SrhConfig::config_json) ||
        !config->config_json) return true;
    const auto visit = [](void *opaque, const srz80::sdk::json::Token &token) noexcept {
        auto &value = *static_cast<Settings *>(opaque);
        uint64_t number = 0;
        if (!srz80::sdk::json::unsigned_value(token, number)) return false;
        if (token.name == "sample_rate" && number >= 8'000 && number <= 384'000)
            value.sample_rate = static_cast<uint32_t>(number);
        else if (token.name == "channel" && number <= 7)
            value.channel = static_cast<uint32_t>(number);
        else return false;
        return true;
    };
    return config->config_json_size <= SIZE_MAX &&
           bool(srz80::sdk::json::object(
               {config->config_json, size_t(config->config_json_size)}, visit, &settings));
}

struct Card {
    const ShouryoHost *host = nullptr;
    const SrhHostAudioInputV1 *input = nullptr;
    SrhHandle owner = 0;
    uint64_t base = 0;
    uint32_t rate = 16'000, pending_rate = 16'000, channel = 0;
    std::array<uint8_t, kFifoCapacity> fifo{};
    uint32_t read_index = 0, write_index = 0, count = 0;
    bool enabled = false, underflow = false, overflow = false, channel_missing = false;

    void clear_fifo() { read_index = write_index = count = 0; }
    void stop() {
        if (enabled) (void)input->request(input->context, owner, 0);
        enabled = false;
        clear_fifo();
    }
    SrhStatus start() {
        if (pending_rate < 8'000 || pending_rate > 384'000) return SRH_INVALID;
        const bool rate_changed = rate != pending_rate;
        const auto status = input->request(input->context, owner, pending_rate);
        if (status != SRH_OK) return status;
        rate = pending_rate;
        enabled = true;
        channel_missing = false;
        if (rate_changed) clear_fifo();
        return SRH_OK;
    }
    void reset() {
        stop();
        underflow = overflow = channel_missing = false;
        pending_rate = rate;
    }
    static uint8_t quantize(float sample) {
        if (!std::isfinite(sample)) sample = 0.0f;
        const float scaled = std::floor((std::clamp(sample, -1.0f, 1.0f) + 1.0f) * 128.0f);
        return static_cast<uint8_t>(std::clamp(scaled, 0.0f, 255.0f));
    }
    void service() {
        if (!enabled) return;
        std::array<float, kReadFrames * 8> samples{};
        uint32_t actual_rate = 0, channels = 0;
        const uint32_t frames = input->read(input->context, owner, samples.data(),
            kReadFrames, &actual_rate, &channels);
        if (!frames) return;
        if (actual_rate != rate || channels == 0 || channels > 8 || frames > kReadFrames ||
            channel >= channels) {
            channel_missing = channel >= channels;
            return;
        }
        channel_missing = false;
        for (uint32_t frame = 0; frame < frames; ++frame) {
            if (count == kFifoCapacity) { overflow = true; continue; }
            fifo[write_index] = quantize(samples[size_t(frame) * channels + channel]);
            write_index = (write_index + 1) % kFifoCapacity;
            ++count;
        }
    }
    uint8_t status() const {
        return (enabled ? 0x01 : 0) | (count ? 0x02 : 0) |
               (count == kFifoCapacity ? 0x04 : 0) | (channel_missing ? 0x20 : 0) |
               (underflow ? 0x40 : 0) | (overflow ? 0x80 : 0);
    }
    uint8_t pop() {
        if (!count) { underflow = true; return 0x80; }
        const uint8_t value = fifo[read_index];
        read_index = (read_index + 1) % kFifoCapacity;
        --count;
        return value;
    }
};

SrhStatus access(void *context, uint64_t address, uint8_t *value, bool peek) {
    auto &card = *static_cast<Card *>(context);
    if (!value || address < card.base || address - card.base >= kRegisterCount) return SRH_INVALID;
    const uint32_t offset = static_cast<uint32_t>(address - card.base);
    // Polling STATUS or COUNT_LO is the explicit bounded service point. DATA
    // reads only consume locally queued bytes, avoiding one host call per CPU
    // load and keeping the two-byte count stable through COUNT_HI.
    if (!peek && (offset == 2 || offset == 7)) card.service();
    switch (offset) {
    case 0: *value = peek ? (card.count ? card.fifo[card.read_index] : 0x80) : card.pop(); break;
    case 1: *value = card.enabled ? 1 : 0; break;
    case 2: *value = card.status(); break;
    case 3: case 4: case 5: case 6: *value = uint8_t(card.pending_rate >> (8 * (offset - 3))); break;
    case 7: *value = uint8_t(card.count); break;
    case 8: *value = uint8_t(card.count >> 8); break;
    case 9: *value = uint8_t(card.channel); break;
    case 10: case 11: case 12: case 13: {
        uint64_t ns = 0;
        if (!card.host->time_ns || card.host->time_ns(card.host->context, &ns) != SRH_OK) ns = 0;
        const uint32_t ms = static_cast<uint32_t>(ns / 1'000'000);
        *value = uint8_t(ms >> (8 * (offset - 10)));
        break;
    }
    default: *value = 0; break;
    }
    return SRH_OK;
}
SrhStatus SRH_CALL read(void *c, uint64_t a, uint8_t *v) { return access(c, a, v, false); }
SrhStatus SRH_CALL peek(void *c, uint64_t a, uint8_t *v) { return access(c, a, v, true); }
SrhStatus SRH_CALL write(void *context, uint64_t address, uint8_t value) {
    auto &card = *static_cast<Card *>(context);
    if (address < card.base || address - card.base >= kRegisterCount) return SRH_INVALID;
    const uint32_t offset = static_cast<uint32_t>(address - card.base);
    switch (offset) {
    case 1:
        if (value & 0x02) card.clear_fifo();
        if (value & 0x04) {
            const auto status = card.start();
            if (status != SRH_OK) return status;
        }
        if (value & 0x01) {
            if (!card.enabled) { const auto status = card.start(); if (status != SRH_OK) return status; }
        } else if (card.enabled) card.stop();
        break;
    case 2:
        if (value & 0x20) card.channel_missing = false;
        if (value & 0x40) card.underflow = false;
        if (value & 0x80) card.overflow = false;
        break;
    case 3: case 4: case 5: case 6: {
        const auto shift = 8 * (offset - 3);
        card.pending_rate = (card.pending_rate & ~(uint32_t(0xFF) << shift)) | (uint32_t(value) << shift);
        break;
    }
    case 9: if (value > 7) return SRH_INVALID; card.channel = value; card.clear_fifo(); break;
    default: break;
    }
    return SRH_OK;
}

SrhStatus SRH_CALL create(const ShouryoHost *host, SrhHandle owner, const SrhConfig *config, void **out) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!srz80::sdk::valid(host) || !srz80::sdk::valid(config) || !out || !host->map ||
            !host->time_ns || config->size != kRegisterCount ||
            config->base > UINT64_MAX - (kRegisterCount - 1)) return SRH_INVALID;
        if (!srz80::sdk::has_field(host, &ShouryoHost::query) || !host->query)
            return SRH_UNAVAILABLE;
        Settings settings;
        if (!parse_settings(config, settings)) return SRH_INVALID;
        const void *extension = nullptr;
        if (host->query(host->context, "host.audio_input.v1", &extension) != SRH_OK || !extension)
            return SRH_UNAVAILABLE;
        const auto *input = static_cast<const SrhHostAudioInputV1 *>(extension);
        if (!srz80::sdk::valid(input) ||
            !srz80::sdk::has_field(input, &SrhHostAudioInputV1::read) ||
            !input->request || !input->read) return SRH_UNAVAILABLE;
        auto card = std::make_unique<Card>();
        card->host = host; card->input = input; card->owner = owner; card->base = config->base;
        card->rate = card->pending_rate = settings.sample_rate; card->channel = settings.channel;
        SrhMapping mapping{SRH_INIT(SrhMapping), config->space, config->base,
            config->base + kRegisterCount - 1, config->priority, card.get(), read, write, peek, nullptr};
        SrhHandle handle = 0;
        const auto status = host->map(host->context, owner, &mapping, &handle);
        if (status != SRH_OK) return status;
        *out = card.release();
        return SRH_OK;
    });
}
void SRH_CALL destroy(void *context) { auto *card = static_cast<Card *>(context); card->stop(); delete card; }
SrhStatus SRH_CALL reset(void *context, uint32_t) { static_cast<Card *>(context)->reset(); return SRH_OK; }
uint32_t SRH_CALL property_count(void *) { return 4; }
SrhStatus SRH_CALL property_info(void *, uint32_t index, SrhProperty *out) {
    if (!srz80::sdk::valid(out) || index >= 4) return SRH_INVALID;
    static const char *names[] = {"Capture enabled", "FIFO level", "Sample rate", "Status"};
    static const char *descriptions[] = {"Guest-controlled live input subscription", "Queued mono PCM bytes", "Requested host delivery rate", "Input FIFO and error flags"};
    const uint32_t kinds[] = {SRH_BOOLEAN, SRH_UNSIGNED, SRH_UNSIGNED, SRH_UNSIGNED};
    const uint32_t bits[] = {1, 13, 32, 8};
    *out = {SRH_INIT(SrhProperty), names[index], "Audio input", descriptions[index], kinds[index], bits[index], 10, 0, nullptr, SRH_PROPERTY_RUNTIME};
    return SRH_OK;
}
SrhStatus SRH_CALL property_get(void *context, uint32_t index, SrhValue *out) {
    if (!srz80::sdk::valid(out) || index >= 4) return SRH_INVALID;
    const auto &card = *static_cast<Card *>(context);
    *out = {SRH_INIT(SrhValue), 0, 0, {}};
    const uint64_t values[] = {card.enabled, card.count, card.rate, card.status()};
    out->unsigned_value = values[index]; return SRH_OK;
}
SrhStatus SRH_CALL property_set(void *, uint32_t, const SrhValue *) { return SRH_INVALID; }
constexpr uint64_t kStateSize = 9;
SrhStatus SRH_CALL save_payload(void *context, uint8_t *buffer, uint64_t *size) {
    if (!size) return SRH_INVALID;
    if (!buffer) { *size = kStateSize; return SRH_OK; }
    if (*size < kStateSize) { *size = kStateSize; return SRH_UNAVAILABLE; }
    const auto &card = *static_cast<Card *>(context);
    srz80::sdk::state::put(buffer, card.rate);
    srz80::sdk::state::put(buffer + 4, card.pending_rate);
    buffer[8] = uint8_t(card.channel); *size = kStateSize; return SRH_OK;
}
SrhStatus SRH_CALL load_payload(void *context, const uint8_t *buffer, uint64_t size) {
    if (!buffer || size != kStateSize) return SRH_INVALID;
    const auto rate = srz80::sdk::state::get<uint32_t>(buffer);
    const auto pending = srz80::sdk::state::get<uint32_t>(buffer + 4);
    if (rate < 8'000 || rate > 384'000 || pending < 8'000 || pending > 384'000 || buffer[8] > 7) return SRH_INVALID;
    auto &card = *static_cast<Card *>(context); card.stop(); card.rate = rate; card.pending_rate = pending;
    card.channel = buffer[8]; card.underflow = card.overflow = card.channel_missing = false; return SRH_OK;
}
const SrhCardDescriptor descriptor{SRH_INIT(SrhCardDescriptor), "Audio", "Audio input",
    "Live host audio input as a mono unsigned 8-bit PCM FIFO", 0x10000000, kRegisterCount,
    0, 0, 0, 0, R"({"sample_rate":16000,"channel":0})", nullptr, nullptr, nullptr, 0};
using State = srz80::sdk::state::Callbacks<save_payload, load_payload, 1>;
const SrhPlugin api{SRH_INIT(SrhPlugin), "audio_input", create, destroy, reset,
    property_count, property_info, property_get, property_set, State::save, State::load,
    &descriptor, nullptr, nullptr};
}
extern "C" SRH_EXPORT const SrhPlugin *SRH_CALL srz80_plugin_init(const ShouryoHost *host) {
    return srz80::sdk::valid(host) ? &api : nullptr;
}
