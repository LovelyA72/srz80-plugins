#include <state.hpp>
#include <algorithm>
#include <array>
#include <boundary.hpp>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {
struct Settings {
    uint32_t width = 80, height = 25;
    uint32_t foreground = 0xFFFFFF, background = 0x000000;
};

class ConfigParser {
  public:
    explicit ConfigParser(const SrhConfig *config) {
        if (srz80::sdk::has_field(config, &SrhConfig::config_json) && config->config_json)
            text_ = {config->config_json, static_cast<size_t>(config->config_json_size)};
        else
            text_ = "{}";
    }
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
            uint64_t number = 0;
            if (!unsigned_value(value, quoted, number))
                return false;
            if (key == "width") {
                if (!number || number > 160)
                    return false;
                settings.width = static_cast<uint32_t>(number);
            } else if (key == "height") {
                if (!number || number > 100)
                    return false;
                settings.height = static_cast<uint32_t>(number);
            } else if (key == "fg") {
                if (number > 0xFFFFFF)
                    return false;
                settings.foreground = static_cast<uint32_t>(number);
            } else if (key == "bg") {
                if (number > 0xFFFFFF)
                    return false;
                settings.background = static_cast<uint32_t>(number);
            } else {
                return false;
            }
            if (take('}'))
                return done();
            if (!take(','))
                return false;
        }
    }

  private:
    void skip() {
        while (position_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[position_])))
            ++position_;
    }
    bool take(char c) {
        skip();
        if (position_ >= text_.size() || text_[position_] != c)
            return false;
        ++position_;
        return true;
    }
    bool string(std::string &out) {
        skip();
        if (position_ >= text_.size() || text_[position_++] != '"')
            return false;
        while (position_ < text_.size() && text_[position_] != '"') {
            if (text_[position_] == '\\')
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
        auto first = position_;
        while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9')
            ++position_;
        if (first == position_)
            return false;
        out.assign(text_.substr(first, position_ - first));
        return true;
    }
    static bool unsigned_value(const std::string &value, bool quoted, uint64_t &out) {
        int base = 10;
        std::string_view digits = value;
        if (quoted && digits.size() > 2 && digits[0] == '0' &&
            (digits[1] == 'x' || digits[1] == 'X')) {
            base = 16;
            digits.remove_prefix(2);
        }
        if (digits.empty())
            return false;
        auto result = std::from_chars(digits.data(), digits.data() + digits.size(), out, base);
        return result.ec == std::errc{} && result.ptr == digits.data() + digits.size();
    }
    bool done() {
        skip();
        return position_ == text_.size();
    }
    std::string_view text_;
    size_t position_ = 0;
};

std::array<uint8_t, 7> glyph(uint8_t input) {
    char c = static_cast<char>(input);
    if (c >= 'a' && c <= 'z')
        c = static_cast<char>(c - 'a' + 'A');
#define G(a, b, c, d, e, f, g) return {0b##a, 0b##b, 0b##c, 0b##d, 0b##e, 0b##f, 0b##g}
    switch (c) {
    case ' ': G(00000,00000,00000,00000,00000,00000,00000);
    case 'A': G(01110,10001,10001,11111,10001,10001,10001);
    case 'B': G(11110,10001,10001,11110,10001,10001,11110);
    case 'C': G(01111,10000,10000,10000,10000,10000,01111);
    case 'D': G(11110,10001,10001,10001,10001,10001,11110);
    case 'E': G(11111,10000,10000,11110,10000,10000,11111);
    case 'F': G(11111,10000,10000,11110,10000,10000,10000);
    case 'G': G(01111,10000,10000,10111,10001,10001,01111);
    case 'H': G(10001,10001,10001,11111,10001,10001,10001);
    case 'I': G(11111,00100,00100,00100,00100,00100,11111);
    case 'J': G(00111,00010,00010,00010,10010,10010,01100);
    case 'K': G(10001,10010,10100,11000,10100,10010,10001);
    case 'L': G(10000,10000,10000,10000,10000,10000,11111);
    case 'M': G(10001,11011,10101,10101,10001,10001,10001);
    case 'N': G(10001,11001,10101,10011,10001,10001,10001);
    case 'O': G(01110,10001,10001,10001,10001,10001,01110);
    case 'P': G(11110,10001,10001,11110,10000,10000,10000);
    case 'Q': G(01110,10001,10001,10001,10101,10010,01101);
    case 'R': G(11110,10001,10001,11110,10100,10010,10001);
    case 'S': G(01111,10000,10000,01110,00001,00001,11110);
    case 'T': G(11111,00100,00100,00100,00100,00100,00100);
    case 'U': G(10001,10001,10001,10001,10001,10001,01110);
    case 'V': G(10001,10001,10001,10001,10001,01010,00100);
    case 'W': G(10001,10001,10001,10101,10101,10101,01010);
    case 'X': G(10001,10001,01010,00100,01010,10001,10001);
    case 'Y': G(10001,10001,01010,00100,00100,00100,00100);
    case 'Z': G(11111,00001,00010,00100,01000,10000,11111);
    case '0': G(01110,10001,10011,10101,11001,10001,01110);
    case '1': G(00100,01100,00100,00100,00100,00100,01110);
    case '2': G(01110,10001,00001,00010,00100,01000,11111);
    case '3': G(11110,00001,00001,01110,00001,00001,11110);
    case '4': G(00010,00110,01010,10010,11111,00010,00010);
    case '5': G(11111,10000,10000,11110,00001,00001,11110);
    case '6': G(01110,10000,10000,11110,10001,10001,01110);
    case '7': G(11111,00001,00010,00100,01000,01000,01000);
    case '8': G(01110,10001,10001,01110,10001,10001,01110);
    case '9': G(01110,10001,10001,01111,00001,00001,01110);
    case '.': G(00000,00000,00000,00000,00000,00110,00110);
    case ':': G(00000,00110,00110,00000,00110,00110,00000);
    case '-': G(00000,00000,00000,11111,00000,00000,00000);
    case '/': G(00001,00010,00010,00100,01000,01000,10000);
    case '!': G(00100,00100,00100,00100,00100,00000,00100);
    default:  G(11111,10001,00110,00100,00110,10001,11111);
    }
#undef G
}

struct Video {
    const ShouryoHost *host = nullptr;
    SrhHandle owner = 0, scanout_event = 0;
    uint64_t frame_number = 0, clock_remainder = 0;

    // Field-granularity scanout at 60000/1001 Hz, independent of CPU clocks.
    // Carry the fractional nanoseconds so the clock does not drift.
    SrhStatus arm_scanout() {
        constexpr uint64_t numerator = 1'001'000'000'000ull;
        const uint64_t ticks = numerator + clock_remainder;
        clock_remainder = ticks % 60000;
        return host->schedule(host->context, owner, ticks / 60000, scanout, this, &scanout_event);
    }
    static SrhStatus SRH_CALL scanout(void *context) {
        return srz80::sdk::guard([&] {
            auto &v = *static_cast<Video *>(context);
            v.scanout_event = 0;
            v.refresh();
            ++v.frame_number;
            return v.arm_scanout();
        });
    }
    static SrhStatus SRH_CALL timing(void *context, SrhVideoTiming *out) {
        if (!srz80::sdk::valid(out)) return SRH_INVALID;
        // This card scans a whole field per event, so there is one scanout unit.
        *out = {SRH_INIT(SrhVideoTiming), static_cast<Video *>(context)->frame_number, 0, 1};
        return SRH_OK;
    }

    uint64_t base = 0;
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> pixels;
    std::vector<uint8_t> dirty_cells;
    std::array<uint32_t, 16> palette{
        0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
        0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF};

    void render_cell(size_t cell) {
        const uint32_t cell_x = static_cast<uint32_t>(cell % width);
        const uint32_t cell_y = static_cast<uint32_t>(cell / width);
        const uint8_t character = bytes[cell * 2u];
        const uint8_t attribute = bytes[cell * 2u + 1u];
        const auto rows = glyph(character);
        const uint32_t pixel_width = width * 8u;
        for (uint32_t local_y = 0; local_y < 16u; ++local_y) {
            for (uint32_t local_x = 0; local_x < 8u; ++local_x) {
                const bool ink = local_y >= 1u && local_y <= 14u && local_x >= 1u &&
                                 local_x <= 5u &&
                                 (rows[(local_y - 1u) / 2u] & (1u << (5u - local_x)));
                const uint32_t rgb = palette[ink ? attribute & 0x0Fu : attribute >> 4u];
                const size_t pixel =
                    ((static_cast<size_t>(cell_y) * 16u + local_y) * pixel_width +
                     cell_x * 8u + local_x) *
                    4u;
                pixels[pixel] = static_cast<uint8_t>(rgb >> 16u);
                pixels[pixel + 1u] = static_cast<uint8_t>(rgb >> 8u);
                pixels[pixel + 2u] = static_cast<uint8_t>(rgb);
                pixels[pixel + 3u] = 0xFF;
            }
        }
    }

    void refresh() {
        for (size_t cell = 0; cell < dirty_cells.size(); ++cell) {
            if (!dirty_cells[cell])
                continue;
            render_cell(cell);
            dirty_cells[cell] = 0;
        }
    }

    SrhStatus render(uint64_t offset, uint8_t *out, uint32_t *size, uint32_t *total) {
        if (!size || !total || (!out && *size))
            return SRH_INVALID;
        *total = static_cast<uint32_t>(pixels.size());
        if (offset >= *total) {
            *size = 0;
            return SRH_OK;
        }
        uint32_t count = static_cast<uint32_t>(std::min<uint64_t>(*size, *total - offset));
        if (count) {
            std::memcpy(out, pixels.data() + offset, count);
        }
        *size = count;
        return SRH_OK;
    }
};

SrhStatus SRH_CALL read(void *p, uint64_t address, uint8_t *value) {
    auto &video = *static_cast<Video *>(p);
    if (!value || address < video.base || address - video.base >= video.bytes.size())
        return SRH_INVALID;
    *value = video.bytes[static_cast<size_t>(address - video.base)];
    return SRH_OK;
}
SrhStatus SRH_CALL write(void *p, uint64_t address, uint8_t value) {
    auto &video = *static_cast<Video *>(p);
    if (address < video.base || address - video.base >= video.bytes.size())
        return SRH_INVALID;
    const auto index = static_cast<size_t>(address - video.base);
    if (video.bytes[index] != value) {
        video.bytes[index] = value;
        const auto cell = index / 2u;
        if (cell < video.dirty_cells.size())
            video.dirty_cells[cell] = 1;
    }
    return SRH_OK;
}
SrhStatus SRH_CALL query(void *p, uint64_t offset, uint8_t *out, uint32_t *size, uint32_t *total) {
    return srz80::sdk::guard(
        [&] { return static_cast<Video *>(p)->render(offset, out, size, total); });
}
SrhStatus SRH_CALL create(const ShouryoHost *host, SrhHandle owner, const SrhConfig *config,
                          void **result) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!srz80::sdk::valid(host) || !srz80::sdk::valid(config) || !result || !config->space ||
            !host->query || !host->schedule || !host->cancel)
            return SRH_INVALID;
        Settings settings;
        if (!ConfigParser(config).parse(settings))
            return SRH_INVALID;
        uint64_t cells = uint64_t(settings.width) * settings.height * 2u;
        if (config->size < cells || config->size > 1024 * 1024 ||
            config->base > UINT64_MAX - (config->size - 1))
            return SRH_INVALID;
        const void *extension = nullptr;
        if (host->query(host->context, "host.video.v1", &extension) != SRH_OK || !extension)
            return SRH_UNAVAILABLE;
        auto video_api = static_cast<const SrhHostVideoV1 *>(extension);
        if (!srz80::sdk::valid(video_api) || !srz80::sdk::has_field(video_api, &SrhHostVideoV1::set_video_timing) ||
            !video_api->register_video_ex || !video_api->set_video_timing)
            return SRH_UNAVAILABLE;
        auto video = std::make_unique<Video>();
        video->host = host;
        video->owner = owner;
        video->base = config->base;
        video->width = settings.width;
        video->height = settings.height;
        video->bytes.resize(static_cast<size_t>(config->size));
        video->pixels.resize(static_cast<size_t>(settings.width) * 8u * settings.height * 16u * 4u);
        video->dirty_cells.resize(static_cast<size_t>(settings.width) * settings.height, 1);
        video->palette[0] = settings.background;
        video->palette[15] = settings.foreground;
        SrhMapping mapping{SRH_INIT(SrhMapping), config->space, config->base,
                           config->base + config->size - 1, config->priority, video.get(), read,
                           write, read};
        SrhHandle mapping_id = 0;
        auto status = host->map(host->context, owner, &mapping, &mapping_id);
        if (status != SRH_OK)
            return status;
        SrhHandle surface_id = 0;
        status = video_api->register_video_ex(video_api->context, owner, settings.width * 8u,
                                              settings.height * 16u, SRH_VIDEO_RGBA8, query,
                                              video.get(), SRH_VIDEO_ALLOW_SHADER, &surface_id);
        if (status != SRH_OK)
            return status;
        status = video_api->set_video_timing(video_api->context, surface_id, Video::timing, video.get());
        if (status != SRH_OK) return status;
        video->refresh();
        status = video->arm_scanout();
        if (status != SRH_OK) return status;
        *result = video.release();
        return SRH_OK;
    });
}
void SRH_CALL destroy(void *p) { delete static_cast<Video *>(p); }
SrhStatus SRH_CALL reset(void *p, uint32_t cold) {
    auto &video = *static_cast<Video *>(p);
    if (video.scanout_event) video.host->cancel(video.host->context, video.scanout_event);
    video.scanout_event = 0;
    video.frame_number = video.clock_remainder = 0;
    if (cold) {
        std::fill(video.bytes.begin(), video.bytes.end(), 0);
        std::fill(video.dirty_cells.begin(), video.dirty_cells.end(), 1);
    }
    video.refresh();
    return video.arm_scanout();
}
uint32_t SRH_CALL count(void *) { return 0; }
SrhStatus SRH_CALL info(void *, uint32_t, SrhProperty *) { return SRH_NOT_FOUND; }
SrhStatus SRH_CALL get(void *, uint32_t, SrhValue *) { return SRH_NOT_FOUND; }
SrhStatus SRH_CALL set(void *, uint32_t, const SrhValue *) { return SRH_NOT_FOUND; }
SrhStatus SRH_CALL save_payload(void *p, uint8_t *buffer, uint64_t *size) {
    if (!size) return SRH_INVALID;
    const auto &v = *static_cast<Video *>(p);
    // Preserve the displayed field separately from pending VRAM writes.
    const uint64_t required = 16 + v.bytes.size() + v.pixels.size();
    if (!buffer) { *size = required; return SRH_OK; }
    if (*size < required) { *size = required; return SRH_UNAVAILABLE; }
    srz80::sdk::state::put(buffer, v.frame_number);
    srz80::sdk::state::put(buffer + 8, v.clock_remainder);
    std::memcpy(buffer + 16, v.bytes.data(), v.bytes.size());
    std::memcpy(buffer + 16 + v.bytes.size(), v.pixels.data(), v.pixels.size());
    *size = required;
    return SRH_OK;
}
SrhStatus SRH_CALL load_payload(void *p, const uint8_t *buffer, uint64_t size) {
    auto &v = *static_cast<Video *>(p);
    if (!buffer || size != 16 + v.bytes.size() + v.pixels.size()) return SRH_INVALID;
    const auto remainder = srz80::sdk::state::get<uint64_t>(buffer + 8);
    if (remainder >= 60000) return SRH_INVALID;
    v.frame_number = srz80::sdk::state::get<uint64_t>(buffer);
    v.clock_remainder = remainder;
    std::memcpy(v.bytes.data(), buffer + 16, v.bytes.size());
    std::memcpy(v.pixels.data(), buffer + 16 + v.bytes.size(), v.pixels.size());
    std::fill(v.dirty_cells.begin(), v.dirty_cells.end(), 1);
    return SRH_OK;
}
const SrhCardDescriptor descriptor{SRH_INIT(SrhCardDescriptor), "Video", "Text video",
                                   "Host-backed text video surface", 0x8000, 4096, 0, 0, 0, 0,
                                   R"({"width":80,"height":25,"fg":"0xFFFFFF","bg":"0x000000"})",
                                   nullptr, nullptr};
using State = srz80::sdk::state::Callbacks<save_payload, load_payload, 1>;
const SrhPlugin api{SRH_INIT(SrhPlugin), "video", create, destroy, reset, count, info, get, set,
                    State::save, State::load, &descriptor};
} // namespace

extern "C" SRH_EXPORT const SrhPlugin *SRH_CALL srz80_plugin_init(const ShouryoHost *host) {
    return srz80::sdk::valid(host) ? &api : nullptr;
}
