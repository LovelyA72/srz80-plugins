// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>

namespace vsn::layout {
// Storage layout only: deliberately separate from host memory transport and
// pixel composition. Widen BEFORE arithmetic; the core rejects > UINT32_MAX.
constexpr uint64_t packed_map(uint32_t base, uint16_t stride, unsigned x, unsigned y) {
    return uint64_t(base) + uint64_t(y / 8) * stride + (x / 8) * 2;
}
// 16x16-wide map rows: one descriptor per 16-pixel tile column.
constexpr uint64_t packed_map16(uint32_t base, uint16_t stride, unsigned x, unsigned y) {
    return uint64_t(base) + uint64_t(y / 16) * stride + (x / 16) * 2;
}
constexpr uint64_t packed4(uint32_t base, unsigned tile, unsigned x, unsigned y) {
    return uint64_t(base) + uint64_t(tile) * 32 + y * 4 + x / 2;
}
// Packed 8bpp: one byte per pixel, 64 bytes per 8x8 tile.
constexpr uint64_t packed8(uint32_t base, unsigned tile, unsigned x, unsigned y) {
    return uint64_t(base) + uint64_t(tile) * 64 + y * 8 + x;
}
// 16x16 packed tiles: 128 bytes at 4bpp, 256 bytes at 8bpp. Even X is high nibble.
constexpr uint64_t packed4_16x16(uint32_t base, unsigned tile, unsigned x, unsigned y) {
    return uint64_t(base) + uint64_t(tile) * 128 + y * 8 + x / 2;
}
constexpr uint64_t packed8_16x16(uint32_t base, unsigned tile, unsigned x, unsigned y) {
    return uint64_t(base) + uint64_t(tile) * 256 + y * 16 + x;
}
constexpr uint8_t nibble(uint8_t byte, unsigned x) {
    return (byte >> ((x & 1) ? 0 : 4)) & 15;
}
constexpr uint64_t nes_page(uint32_t base, uint16_t stride_x, uint16_t stride_y,
                            unsigned x, unsigned y) {
    return uint64_t(base) + (x / 256) * uint64_t(stride_x) + (y / 240) * uint64_t(stride_y);
}
constexpr uint64_t nes_attribute(uint64_t page, unsigned tile_x, unsigned tile_y) {
    return page + 960 + (tile_y / 4) * 8 + tile_x / 4;
}
constexpr unsigned attribute_shift(unsigned tile_x, unsigned tile_y) {
    return ((tile_y & 2) << 1) | (tile_x & 2);
}
constexpr uint64_t nes_pattern(uint32_t base, unsigned table, unsigned tile, unsigned row) {
    return uint64_t(base) + table * 4096ull + tile * 16ull + row;
}
constexpr uint8_t planar2(uint8_t low, uint8_t high, unsigned x) {
    return ((low >> (7 - x)) & 1) | (((high >> (7 - x)) & 1) << 1);
}
// VT planar tile addressing: `planes` consecutive 8-byte bitplanes per tile.
// Plane 0 is the least-significant bit, plane `planes-1` the most-significant.
constexpr uint64_t planar_tile(uint32_t base, unsigned tile, unsigned plane,
                               unsigned row, unsigned planes) {
    return uint64_t(base) + uint64_t(tile) * planes * 8 + plane * 8 + row;
}
// 4-plane pixel decode, bit 7 leftmost, plane 0 is the LSB.
constexpr uint8_t planar4(uint8_t p0, uint8_t p1, uint8_t p2, uint8_t p3, unsigned x) {
    return ((p0 >> (7 - x)) & 1) | (((p1 >> (7 - x)) & 1) << 1) |
           (((p2 >> (7 - x)) & 1) << 2) | (((p3 >> (7 - x)) & 1) << 3);
}
struct NesSprite {
    unsigned top, tile, palette, x;
    bool behind, flip_x, flip_y;
};
constexpr NesSprite nes_sprite(const uint8_t *p) {
    return {unsigned(p[0]) + 1, p[1], unsigned(p[2] & 3), p[3],
            bool(p[2] & 32), bool(p[2] & 64), bool(p[2] & 128)};
}
// Extended 16-byte record: signed X and Y top-left corners, 20-bit tile index,
// palette, flip/priority/enable flags and a size selector. X/Y may be negative.
struct ExtSprite {
    int x, y;
    unsigned tile, palette, size;
    bool flip_x, flip_y, behind, enable;
};
constexpr int16_t signed16(uint8_t low, uint8_t high) {
    return static_cast<int16_t>(uint16_t(low) | (uint16_t(high) << 8));
}
constexpr ExtSprite ext_sprite(const uint8_t *p) {
    return {signed16(p[0], p[1]), signed16(p[2], p[3]),
            unsigned(p[4]) | (unsigned(p[5]) << 8) | ((unsigned(p[6]) & 15) << 16),
            unsigned(p[8] & 15), unsigned(p[10]),
            bool(p[9] & 1), bool(p[9] & 2), bool(p[9] & 4), bool(p[9] & 8)};
}
} // namespace vsn::layout
