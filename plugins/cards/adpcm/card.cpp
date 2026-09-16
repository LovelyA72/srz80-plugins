#include <boundary.hpp>
#include "device.hpp"
#include <cstring>
#include <memory>
#include <nlohmann/json.hpp>

namespace {
using srz80::adpcm::Device;
struct Card {
    Device device;
    uint64_t base = 0;
    SrhHandle mapping = 0, stream = 0;
};
SrhStatus access(void *context, uint64_t address, uint8_t *value, bool peek) {
    auto &card = *static_cast<Card *>(context);
    if (!value || address < card.base || address - card.base >= Device::register_count) return SRH_INVALID;
    *value = card.device.read(uint32_t(address - card.base), peek);
    return SRH_OK;
}
SrhStatus SRH_CALL read(void *c, uint64_t a, uint8_t *v) { return access(c, a, v, false); }
SrhStatus SRH_CALL peek(void *c, uint64_t a, uint8_t *v) { return access(c, a, v, true); }
SrhStatus SRH_CALL write(void *context, uint64_t address, uint8_t value) {
    auto &card = *static_cast<Card *>(context);
    if (address < card.base || address - card.base >= Device::register_count) return SRH_INVALID;
    card.device.write(uint32_t(address - card.base), value);
    return SRH_OK;
}
SrhStatus SRH_CALL render(void *context, uint64_t, uint32_t frames, int16_t *out) {
    if (!out) return SRH_INVALID;
    static_cast<Card *>(context)->device.render(frames, out);
    return SRH_OK;
}
SrhStatus SRH_CALL create(const ShouryoHost *host, SrhHandle owner, const SrhConfig *config, void **out) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!srz80::sdk::valid(host) || !srz80::sdk::valid(config) || !out || !host->map ||
            !config->space || config->size != Device::register_count ||
            config->base > UINT64_MAX - (Device::register_count - 1)) return SRH_INVALID;
        *out = nullptr;
        std::string name = "ADPCM";
        uint32_t sample_rate = 44'100;
        if (srz80::sdk::has_field(config, &SrhConfig::config_json) && config->config_json) {
            const auto json = nlohmann::json::parse(config->config_json, config->config_json + config->config_json_size,
                                                   nullptr, false);
            if (!json.is_object()) return SRH_INVALID;
            for (auto it = json.begin(); it != json.end(); ++it)
                if (it.key() != "stream_name" && it.key() != "sample_rate") return SRH_INVALID;
            if (json.contains("stream_name")) {
                if (!json["stream_name"].is_string()) return SRH_INVALID;
                name = json["stream_name"].get<std::string>();
            }
            if (json.contains("sample_rate")) {
                if (!json["sample_rate"].is_number_unsigned()) return SRH_INVALID;
                const auto value = json["sample_rate"].get<uint64_t>();
                if (value > UINT32_MAX) return SRH_INVALID;
                sample_rate = static_cast<uint32_t>(value);
            }
        }
        if (name.empty() || name.size() > 256 || name.find('\0') != std::string::npos ||
            sample_rate < 8'000 || sample_rate > 192'000) return SRH_INVALID;
        const uint8_t *image = config->image;
        uint64_t size = config->image_size;
        if (srz80::sdk::has_field(config, &SrhConfig::image_count) && config->image_count) {
            if (config->image_count != 1 || !config->images || !srz80::sdk::valid(config->images)) return SRH_INVALID;
            image = config->images[0].data; size = config->images[0].size;
        }
        if (size > Device::ram_size || (size && !image)) return SRH_INVALID;
        const void *extension = nullptr;
        if (!host->query || host->query(host->context, "host.audio.v1", &extension) != SRH_OK || !extension)
            return SRH_UNAVAILABLE;
        const auto *audio = static_cast<const SrhHostAudioV1 *>(extension);
        if (!srz80::sdk::valid(audio) || !audio->register_source) return SRH_INVALID;
        auto card = std::make_unique<Card>();
        card->base = config->base; card->device.output_rate = sample_rate;
        if (size) std::memcpy(card->device.ram.data(), image, size_t(size));
        SrhMapping mapping{SRH_INIT(SrhMapping), config->space, config->base,
            config->base + Device::register_count - 1, config->priority, card.get(), read, write, peek, nullptr};
        auto status = host->map(host->context, owner, &mapping, &card->mapping);
        if (status != SRH_OK) return status;
        status = audio->register_source(audio->context, owner, sample_rate, 2, SRH_AUDIO_S16_STEREO,
                                         name.c_str(), render, card.get(), &card->stream);
        if (status != SRH_OK) return status;
        *out = card.release(); return SRH_OK;
    });
}
void SRH_CALL destroy(void *c) { delete static_cast<Card *>(c); }
SrhStatus SRH_CALL reset(void *c, uint32_t) { static_cast<Card *>(c)->device.reset(); return SRH_OK; }
constexpr const char *names[] = {"Mode", "Status", "Volume", "Start", "Length", "Speed", "Upload address", "Position", "Remaining"};
constexpr uint32_t offsets[] = {0, 1, 2, 4, 8, 12, 16, 24, 28};
uint32_t SRH_CALL count(void *) { return 9; }
SrhStatus SRH_CALL info(void *, uint32_t i, SrhProperty *out) {
    if (!srz80::sdk::valid(out) || i >= 9) return SRH_INVALID;
    *out = {SRH_INIT(SrhProperty), names[i], "ADPCM", names[i], i == 0 ? SRH_ENUM : SRH_UNSIGNED,
            i < 3 ? 8u : 32u, 10, 0, i == 0 ? "1-bit DPCM|4-bit PCM|8-bit PCM|IMA ADPCM|G.711 mu-law" : nullptr,
            SRH_PROPERTY_RUNTIME};
    return SRH_OK;
}
SrhStatus SRH_CALL get(void *c, uint32_t i, SrhValue *out) {
    if (!srz80::sdk::valid(out) || i >= 9) return SRH_INVALID;
    auto &d = static_cast<Card *>(c)->device;
    *out = {SRH_INIT(SrhValue), 0, 0, {}};
    for (unsigned b = 0; b < (i < 3 ? 1u : 4u); ++b)
        out->unsigned_value |= uint64_t(d.read(offsets[i] + b, true)) << (8 * b);
    return SRH_OK;
}
SrhStatus SRH_CALL set(void *, uint32_t, const SrhValue *) { return SRH_INVALID; }
// Fixed, versioned little-endian state; no native struct layout is persisted.
constexpr uint64_t state_size = 4 + 13 * 4 + Device::register_count + Device::ram_size;
SrhStatus SRH_CALL save(void *c, uint8_t *buffer, uint64_t *size) {
    if (!size) return SRH_INVALID;
    if (!buffer) { *size = state_size; return SRH_OK; }
    if (*size < state_size) { *size = state_size; return SRH_UNAVAILABLE; }
    auto &d = static_cast<Card *>(c)->device;
    std::memcpy(buffer, "ADP1", 4);
    uint8_t *p = buffer + 4;
    for (uint32_t value : {d.output_rate, d.start, d.length, d.rate, d.position, d.mode, d.phase,
                           uint32_t(d.predictor + 32768), uint32_t(d.index), uint32_t(d.sample + 32768),
                           uint32_t(d.playing), uint32_t(d.error), uint32_t(0)})
        for (unsigned b = 0; b < 4; ++b) *p++ = uint8_t(value >> (8 * b));
    std::memcpy(p, d.regs.data(), d.regs.size()); p += d.regs.size();
    std::memcpy(p, d.ram.data(), d.ram.size());
    *size = state_size; return SRH_OK;
}
SrhStatus SRH_CALL load(void *c, const uint8_t *buffer, uint64_t size) {
    if (!buffer || size != state_size || std::memcmp(buffer, "ADP1", 4)) return SRH_INVALID;
    auto &d = static_cast<Card *>(c)->device;
    uint32_t v[13]{};
    const uint8_t *p = buffer + 4;
    for (auto &value : v) for (unsigned b = 0; b < 4; ++b) value |= uint32_t(*p++) << (8 * b);
    if (v[0] != d.output_rate || v[4] > v[2] || v[6] >= d.output_rate || v[7] > 65535 ||
        v[8] > 88 || v[9] > 65535 || v[10] > 1 || v[11] > 1 || v[12]) return SRH_INVALID;
    if (v[10] && (v[5] > Device::mulaw || !v[3] || v[3] > 192000 || !v[2] || !v[4] ||
        v[1] >= Device::ram_size || Device::bytes_needed(v[5], v[2]) > Device::ram_size - v[1] ||
        (v[5] == Device::dpcm && (v[7] < 32704 || v[7] > 32831)))) return SRH_INVALID;
    d.start = v[1]; d.length = v[2]; d.rate = v[3]; d.position = v[4]; d.mode = v[5]; d.phase = v[6];
    d.predictor = int32_t(v[7]) - 32768; d.index = int32_t(v[8]); d.sample = int32_t(v[9]) - 32768;
    d.playing = v[10] != 0; d.error = v[11] != 0;
    std::memcpy(d.regs.data(), p, d.regs.size()); p += d.regs.size();
    std::memcpy(d.ram.data(), p, d.ram.size()); return SRH_OK;
}
const SrhImageSlotDescriptor slots[]{{SRH_INIT(SrhImageSlotDescriptor), "Sample RAM bank (raw, optional, up to 2 MiB)"}};
const SrhCardDescriptor descriptor{SRH_INIT(SrhCardDescriptor), "Audio", "ADPCM",
    "Single mono voice: DPCM, PCM4/8, IMA and G.711 mu-law; private 2 MiB RAM",
    0x1000, Device::register_count, 0, 0, 0, 0,
    R"({"sample_rate":44100,"stream_name":"ADPCM"})", nullptr, nullptr, slots, 1};
const SrhPlugin api{SRH_INIT(SrhPlugin), "adpcm", create, destroy, reset, count, info, get, set,
                    save, load, &descriptor, nullptr, nullptr};
}
extern "C" SRH_EXPORT const SrhPlugin *SRH_CALL srz80_plugin_init(const ShouryoHost *host) {
    return srz80::sdk::valid(host) ? &api : nullptr;
}
