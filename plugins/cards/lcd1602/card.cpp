#include <algorithm>
#include <array>
#include <boundary.hpp>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace {
constexpr uint32_t kColumns = 16;
constexpr uint32_t kRows = 2;
constexpr uint32_t kWidth = 480;
constexpr uint32_t kHeight = 120;
constexpr uint32_t kCellWidth = kWidth / kColumns;
constexpr uint32_t kCellHeight = kHeight / kRows;

using Glyph = std::array<uint8_t, 8>;

// Printable glyphs from the supplied char-lcd.js character ROM.  The source
// format is five bits per row; short source rows remain blank at the bottom.
Glyph glyph(uint8_t character) {
#define G(a, b, c, d, e, f, g, h) return {a, b, c, d, e, f, g, h}
    switch (character) {
    case ' ': G(0,0,0,0,0,0,0,0);
    case '!': G(4,4,4,4,0,0,4,0); case '"': G(10,10,10,0,0,0,0,0);
    case '#': G(10,10,31,10,31,10,10,0); case '$': G(4,15,20,14,5,30,4,0);
    case '%': G(24,25,2,4,8,19,3,0); case '&': G(12,18,20,8,21,18,13,0);
    case '\'': G(12,4,8,0,0,0,0,0); case '(': G(2,4,8,8,8,4,2,0);
    case ')': G(8,4,2,2,2,4,8,0); case '*': G(0,4,21,14,21,4,0,0);
    case '+': G(0,4,4,31,4,4,0,0); case ',': G(0,0,0,0,12,4,8,0);
    case '-': G(0,0,0,31,0,0,0,0); case '.': G(0,0,0,0,0,12,12,0);
    case '/': G(0,1,2,4,8,16,0,0);
    case '0': G(14,17,19,21,25,17,14,0); case '1': G(4,12,4,4,4,4,14,0);
    case '2': G(14,17,1,2,4,8,31,0); case '3': G(31,2,4,2,1,17,14,0);
    case '4': G(2,6,10,18,31,2,2,0); case '5': G(31,16,30,1,1,17,14,0);
    case '6': G(6,8,16,30,17,17,14,0); case '7': G(31,1,2,4,8,8,8,0);
    case '8': G(14,17,17,14,17,17,14,0); case '9': G(14,17,17,15,1,2,12,0);
    case ':': G(0,12,12,0,12,12,0,0); case ';': G(0,12,12,0,12,4,8,0);
    case '<': G(2,4,8,16,8,4,2,0); case '=': G(0,0,31,0,31,0,0,0);
    case '>': G(8,4,2,1,2,4,8,0); case '?': G(14,17,1,2,4,0,4,0);
    case '@': G(14,17,1,13,21,21,14,0);
    case 'A': G(14,17,17,31,17,17,17,0); case 'B': G(30,17,17,30,17,17,30,0);
    case 'C': G(14,17,16,16,16,17,14,0); case 'D': G(28,18,17,17,17,18,28,0);
    case 'E': G(31,16,16,30,16,16,31,0); case 'F': G(31,16,16,30,16,16,16,0);
    case 'G': G(14,17,16,23,17,17,15,0); case 'H': G(17,17,17,31,17,17,17,0);
    case 'I': G(14,4,4,4,4,4,14,0); case 'J': G(14,2,2,2,2,18,12,0);
    case 'K': G(17,18,20,24,20,18,17,0); case 'L': G(16,16,16,16,16,16,31,0);
    case 'M': G(17,27,21,21,17,17,17,0); case 'N': G(17,17,25,21,19,17,17,0);
    case 'O': G(14,17,17,17,17,17,14,0); case 'P': G(30,17,17,30,16,16,16,0);
    case 'Q': G(14,17,17,17,21,18,13,0); case 'R': G(30,17,17,30,20,18,17,0);
    case 'S': G(15,16,16,14,1,1,30,0); case 'T': G(31,4,4,4,4,4,4,0);
    case 'U': G(17,17,17,17,17,17,14,0); case 'V': G(17,17,17,17,17,10,4,0);
    case 'W': G(17,17,17,21,21,21,10,0); case 'X': G(17,10,4,10,17,17,17,0);
    case 'Y': G(17,17,10,4,4,4,4,0); case 'Z': G(31,1,2,4,8,16,31,0);
    case '[': G(14,8,8,8,8,8,14,0); case '\\': G(17,10,31,4,31,4,4,0);
    case ']': G(14,2,2,2,2,2,14,0); case '^': G(4,10,17,0,0,0,0,0);
    case '_': G(0,0,0,0,0,0,31,0); case '`': G(8,4,2,0,0,0,0,0);
    case 'a': G(0,0,14,1,15,17,15,0); case 'b': G(16,16,22,25,17,17,30,0);
    case 'c': G(0,0,14,16,16,17,14,0); case 'd': G(1,1,13,19,17,17,15,0);
    case 'e': G(0,0,14,17,31,16,14,0); case 'f': G(6,9,8,28,8,8,8,0);
    case 'g': G(0,15,17,17,15,1,14,0); case 'h': G(16,16,22,25,17,17,17,0);
    case 'i': G(4,0,12,4,4,4,14,0); case 'j': G(2,0,6,2,2,18,12,0);
    case 'k': G(16,16,18,20,24,20,18,0); case 'l': G(12,4,4,4,4,4,31,0);
    case 'm': G(0,0,26,21,21,17,17,0); case 'n': G(0,0,22,25,17,17,17,0);
    case 'o': G(0,0,14,17,17,17,14,0); case 'p': G(0,0,30,17,30,16,16,0);
    case 'q': G(0,0,13,19,15,1,1,0); case 'r': G(0,0,22,25,16,16,16,0);
    case 's': G(0,0,14,16,14,1,30,0); case 't': G(8,8,28,8,8,9,6,0);
    case 'u': G(0,0,17,17,17,19,13,0); case 'v': G(0,0,17,17,17,10,4,0);
    case 'w': G(0,0,17,17,21,21,10,0); case 'x': G(0,0,17,10,4,10,17,0);
    case 'y': G(0,0,17,17,15,1,14,0); case 'z': G(0,0,31,2,4,8,31,0);
    case '{': G(2,4,4,8,4,4,2,0); case '|': G(4,4,4,4,4,4,4,0);
    case '}': G(8,4,4,2,4,4,8,0); case '~': G(0,4,2,31,2,4,0,0);
    default: G(14,17,1,2,4,0,4,0);
    }
#undef G
}

struct Lcd1602 {
    uint64_t base = 0;
    std::array<uint8_t, 80> ddram{};
    std::array<uint8_t, 64> cgram{};
    std::vector<uint8_t> pixels = std::vector<uint8_t>(kWidth * kHeight * 4u);
    uint8_t address = 0;
    bool cgram_selected = false;
    bool increment = true;
    bool display_on = true;
    bool cursor_on = false;
    bool dirty = true;

    void step() { address = static_cast<uint8_t>((address + (increment ? 1 : 0x7F)) & 0x7F); }
    uint8_t &selected() { return cgram_selected ? cgram[address & 0x3F] : ddram[address % ddram.size()]; }
    void command(uint8_t value) {
        if (value == 0x01) { ddram.fill(0x20); address = 0; cgram_selected = false; }
        else if (value == 0x02) { address = 0; cgram_selected = false; }
        else if ((value & 0xFC) == 0x04) increment = value & 0x02;
        else if ((value & 0xF8) == 0x08) { display_on = value & 0x04; cursor_on = value & 0x02; }
        else if (value & 0x80) { address = value & 0x7F; cgram_selected = false; }
        else if (value & 0x40) { address = value & 0x3F; cgram_selected = true; }
        dirty = true;
    }
    void dot(uint32_t x, uint32_t y, bool on) {
        const uint32_t rgb = on ? 0x082700 : 0x6CAF00;
        for (uint32_t dy = 0; dy < 4; ++dy) for (uint32_t dx = 0; dx < 4; ++dx) {
            const size_t pixel = (static_cast<size_t>(y + dy) * kWidth + x + dx) * 4u;
            pixels[pixel] = static_cast<uint8_t>(rgb >> 16); pixels[pixel + 1] = static_cast<uint8_t>(rgb >> 8);
            pixels[pixel + 2] = static_cast<uint8_t>(rgb); pixels[pixel + 3] = 0xFF;
        }
    }
    void refresh() {
        if (!dirty) return;
        for (size_t pixel = 0; pixel < pixels.size(); pixel += 4) {
            pixels[pixel] = 0x76; pixels[pixel + 1] = 0xB8; pixels[pixel + 2] = 0x00; pixels[pixel + 3] = 0xFF;
        }
        if (display_on) for (uint32_t row = 0; row < kRows; ++row) for (uint32_t column = 0; column < kColumns; ++column) {
            const uint8_t ddram_address = static_cast<uint8_t>(row * 0x40 + column);
            const uint8_t character = ddram[ddram_address];
            const Glyph rows = character < 8 ? custom(character) : glyph(character);
            for (uint32_t y = 0; y < 8; ++y) for (uint32_t x = 0; x < 5; ++x)
                dot(column * kCellWidth + 3 + x * 5, row * kCellHeight + 10 + y * 5,
                    rows[y] & (1u << (4u - x)));
            if (cursor_on && !cgram_selected && address == ddram_address)
                for (uint32_t x = 0; x < 5; ++x) dot(column * kCellWidth + 3 + x * 5, row * kCellHeight + 50, true);
        }
        dirty = false;
    }
    Glyph custom(uint8_t character) const {
        Glyph rows{};
        for (uint32_t row = 0; row < 8; ++row) rows[row] = cgram[character * 8u + row] & 0x1F;
        return rows;
    }
    SrhStatus render(uint64_t offset, uint8_t *out, uint32_t *size, uint32_t *total) {
        if (!size || !total || (!out && *size)) return SRH_INVALID;
        *total = static_cast<uint32_t>(pixels.size());
        if (offset >= *total) { *size = 0; return SRH_OK; }
        refresh();
        const uint32_t count = static_cast<uint32_t>(std::min<uint64_t>(*size, *total - offset));
        std::memcpy(out, pixels.data() + offset, count); *size = count;
        return SRH_OK;
    }
};

SrhStatus SRH_CALL read(void *context, uint64_t address, uint8_t *value) {
    auto &lcd = *static_cast<Lcd1602 *>(context);
    if (!value || address < lcd.base || address - lcd.base > 1) return SRH_INVALID;
    if (address - lcd.base == 0) *value = lcd.address & 0x7F; // Busy flag is always clear.
    else { *value = lcd.selected(); lcd.step(); }
    return SRH_OK;
}
SrhStatus SRH_CALL write(void *context, uint64_t address, uint8_t value) {
    auto &lcd = *static_cast<Lcd1602 *>(context);
    if (address < lcd.base || address - lcd.base > 1) return SRH_INVALID;
    if (address - lcd.base == 0) lcd.command(value);
    else { lcd.selected() = value; lcd.step(); lcd.dirty = true; }
    return SRH_OK;
}
SrhStatus SRH_CALL query(void *context, uint64_t offset, uint8_t *out, uint32_t *size, uint32_t *total) {
    return srz80::sdk::guard([&] { return static_cast<Lcd1602 *>(context)->render(offset, out, size, total); });
}
SrhStatus SRH_CALL create(const ShouryoHost *host, SrhHandle owner, const SrhConfig *config, void **result) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!srz80::sdk::valid(host) || !srz80::sdk::valid(config) || !result || !config->space ||
            config->size < 2 || config->base > UINT64_MAX - (config->size - 1) || !host->query)
            return SRH_INVALID;
        const void *extension = nullptr;
        if (host->query(host->context, "host.video.v1", &extension) != SRH_OK || !extension) return SRH_UNAVAILABLE;
        const auto *video = static_cast<const SrhHostVideoV1 *>(extension);
        if (!srz80::sdk::valid(video) ||
            !srz80::sdk::has_field(video, &SrhHostVideoV1::register_video_ex) ||
            !video->register_video_ex) return SRH_UNAVAILABLE;
        auto lcd = std::make_unique<Lcd1602>(); lcd->base = config->base; lcd->ddram.fill(0x20);
        SrhMapping mapping{SRH_INIT(SrhMapping), config->space, config->base,
                           config->base + config->size - 1, config->priority, lcd.get(), read,
                           write, read, nullptr};
        SrhHandle mapping_id = 0;
        auto status = host->map(host->context, owner, &mapping, &mapping_id);
        if (status != SRH_OK) return status;
        SrhHandle surface_id = 0;
        status = video->register_video_ex(video->context, owner, kWidth, kHeight,
                                          SRH_VIDEO_RGBA8, query, lcd.get(),
                                          SRH_VIDEO_ALLOW_SHADER, &surface_id);
        if (status != SRH_OK) return status;
        *result = lcd.release(); return SRH_OK;
    });
}
void SRH_CALL destroy(void *context) { delete static_cast<Lcd1602 *>(context); }
SrhStatus SRH_CALL reset(void *context, uint32_t cold) {
    if (cold) { auto &lcd = *static_cast<Lcd1602 *>(context); lcd.ddram.fill(0x20); lcd.cgram.fill(0); lcd.address = 0; lcd.cgram_selected = false; lcd.increment = true; lcd.display_on = true; lcd.cursor_on = false; lcd.dirty = true; }
    return SRH_OK;
}
uint32_t SRH_CALL property_count(void *) { return 0; }
SrhStatus SRH_CALL property_info(void *, uint32_t, SrhProperty *) { return SRH_NOT_FOUND; }
SrhStatus SRH_CALL property_get(void *, uint32_t, SrhValue *) { return SRH_NOT_FOUND; }
SrhStatus SRH_CALL property_set(void *, uint32_t, const SrhValue *) { return SRH_NOT_FOUND; }
SrhStatus SRH_CALL save_state(void *context, uint8_t *buffer, uint64_t *size) {
    constexpr uint64_t kStateSize = 147; if (!size) return SRH_INVALID; if (!buffer) { *size = kStateSize; return SRH_OK; }
    if (*size < kStateSize) { *size = kStateSize; return SRH_UNAVAILABLE; } auto &lcd = *static_cast<Lcd1602 *>(context);
    std::memcpy(buffer, lcd.ddram.data(), lcd.ddram.size()); std::memcpy(buffer + 80, lcd.cgram.data(), lcd.cgram.size());
    buffer[144] = lcd.address; buffer[145] = static_cast<uint8_t>(lcd.cgram_selected | (lcd.increment << 1)); buffer[146] = static_cast<uint8_t>(lcd.display_on | (lcd.cursor_on << 1)); *size = kStateSize; return SRH_OK;
}
SrhStatus SRH_CALL load_state(void *context, const uint8_t *buffer, uint64_t size) {
    if (!buffer || size != 147)
        return SRH_INVALID;
    auto &lcd = *static_cast<Lcd1602 *>(context);
    std::memcpy(lcd.ddram.data(), buffer, lcd.ddram.size());
    std::memcpy(lcd.cgram.data(), buffer + 80, lcd.cgram.size());
    lcd.address = buffer[144] & 0x7F;
    lcd.cgram_selected = buffer[145] & 1;
    lcd.increment = buffer[145] & 2;
    lcd.display_on = buffer[146] & 1;
    lcd.cursor_on = buffer[146] & 2;
    lcd.dirty = true;
    return SRH_OK;
}
const SrhCardDescriptor descriptor{SRH_INIT(SrhCardDescriptor), "Video", "LCD1602",
    "HD44780-compatible 16 by 2 character LCD with a 480 by 120 dot-grid surface", 0xC0, 2,
    0, 0, 0, 0, "{}", nullptr, nullptr, nullptr, 0};
const SrhPlugin api{SRH_INIT(SrhPlugin), "lcd1602", create, destroy, reset, property_count,
                    property_info, property_get, property_set, save_state, load_state,
                    &descriptor, nullptr, nullptr};
} // namespace

extern "C" SRH_EXPORT const SrhPlugin *SRH_CALL srz80_plugin_init(const ShouryoHost *host) { return srz80::sdk::valid(host) ? &api : nullptr; }
