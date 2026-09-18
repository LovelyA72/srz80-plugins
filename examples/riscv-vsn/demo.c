// SPDX-License-Identifier: MIT
// Four Corners: original tile/sprite art, generated into shared RAM at boot.
#include <stdint.h>

#define VSN_BASE       0x10000000u
#define UART_BASE      0x10000100u
#define MAP_BASE       0x00010000u
#define BG_TILE_BASE   0x00011000u
#define SP_TILE_BASE   0x00012000u
#define OAM_BASE       0x00013000u
#define PALETTE_BASE   0x00013100u
#define OAM_BACK_BASE  0x00013200u
/* Planar4 (mode 2) assets: a 16-bit descriptor map, 4bpp sprite tiles and the
 * RGB444 palette. They live above the NES structures so both modes stay
 * resident and a live toggle only rewrites the MMIO base registers. */
#define PLANAR_MAP_BASE 0x00013300u
#define PLANAR_SP_BASE  0x00015100u
#define RGB444_BASE     0x00015200u
/* Packed modes keep the shared RGB555 palette, two 512-byte extended OAM
 * buffers, per-mode descriptor maps and a small shared sprite bank up here. */
#define RGB555_BASE     0x00015400u
#define EXT_OAM_BASE    0x00015600u
#define EXT_OAM_BACK    0x00015800u
#define P8_MAP_BASE     0x00015a00u
#define P16_MAP_BASE    0x00017800u
#define P8_SP_BASE      0x00018000u
/* Packed backgrounds are per-cell 8bpp slices of one continuous colour field,
 * so packed8 needs 64x60 unique 8x8 tiles and packed16 32x30 unique 16x16
 * tiles. The RAM card is 1 MiB, which leaves room for both and for the stack. */
#define P8_TILE_BASE    0x00020000u
#define P16_TILE_BASE   0x0005c000u
/* The hires showcase draws a fine 1-pixel resolution strip from dedicated
 * tiles. They live right after the 960 packed16 gradient tiles (P16_TILE_BASE
 * + 960*256 = 0x00098000), in the free RAM below the firmware WORK region. */
#define DETAIL_TILE0    960u
#define DETAIL_WHITE    240u  /* PAL_FONT: fixed white for the resolution strip */
#define WORLD_WIDTH    512
#define WORLD_HEIGHT   480
#define VIEW_WIDTH     256
#define VIEW_HEIGHT    240
#define CAMERA_STEP    8

static volatile uint8_t *const vsn = (volatile uint8_t *)VSN_BASE;
static volatile uint8_t *const uart = (volatile uint8_t *)UART_BASE;
static volatile uint8_t *const map = (volatile uint8_t *)MAP_BASE;
static volatile uint8_t *oam;
static uint32_t front_oam, pending_oam;
static unsigned scroll_x, scroll_y;

/* Pattern rows use NES pixel indices 0..3. The encoder below writes the two
 * bitplanes, so the source art stays readable and independent of packing. */
enum { GRASS, TUFT, ROAD, WATER, CANOPY, TRUNK, BRICK, ROOF, WINDOW, DOOR,
       FLOWER, ROCK, EDGE, STAR, BG_TILE_COUNT };
static const char backgrounds[BG_TILE_COUNT][8][9] = {
    {"11111111","11111111","11121111","11111111","11111111","12111111","11111111","11111111"},
    {"11111111","11111111","11212111","11121111","11111111","11111111","12111211","11111111"},
    {"12222221","23323322","22222222","21122112","22222222","23223322","22222222","12222221"},
    {"11111111","11111111","22321111","11112232","11111111","11111111","11223211","11111122"},
    {"11122111","11233211","12333321","23323332","12332321","23323332","12222221","11122111"},
    {"11122111","11132111","11122111","11132111","11122111","11233211","12222221","11111111"},
    {"22212221","33313331","22212221","11111111","22122221","33133331","22122221","11111111"},
    {"11111111","11133111","11322311","13222231","32222223","33333333","22222222","11111111"},
    {"22222222","23333332","23003032","23003032","23333332","23003032","23333332","22222222"},
    {"22333322","23000032","23000032","23000032","23000332","23000032","23000032","23333332"},
    {"11111111","11131111","11323111","11131111","11121111","11221111","11121111","11111111"},
    {"11111111","11222111","12333211","12322221","12222221","11222111","11111111","11111111"},
    {"33333333","31111113","31222213","31211213","31211213","31222213","31111113","33333333"},
    {"11131111","11131111","11232111","33222333","11232111","11131111","11131111","11111111"}
};
enum { GEM, WING, HOUSE_BRICK, HOUSE_ROOF, HOUSE_WINDOW, HOUSE_DOOR,
       SP_TILE_COUNT };
#define PX_WIDE_TILE 6   /* first 16x16 packed sprite tile */
static const char sprites[HOUSE_BRICK][8][9] = {
    {"00030000","00323000","03222300","32222230","03222300","00323000","00030000","00000000"},
    {"03000000","32300000","32230000","03223000","00322300","03223000","03230000","00300000"}
};
/* Planar4 4bpp versions of the gem and wing. Hex digits '0'-'f' encode the
 * four bitplanes directly; 0 is transparent. The gem is a diamond shaded from
 * rim to core, the wing keeps the NES diagonal silhouette but shades it with
 * the smooth sprite ramps that have no 2bpp equivalent. */
static const char sprites4[HOUSE_BRICK][8][9] = {
    {"00022000","00444400","06666660","88888888","88888888","06666660","00444400","00022000"},
    {"09000000","96900000","96690000","09669000","00966900","09669000","09690000","00900000"}
};
/* Universal background is black. The four background banks retain each
 * region's original colors; sprite bank zero supplies the red house tones. */
static const uint8_t colors[32] = {
    0x0f,0x09,0x19,0x29, 0x0f,0x07,0x17,0x37,
    0x0f,0x01,0x11,0x31, 0x0f,0x03,0x13,0x33,
    0x0f,0x06,0x26,0x36, 0x0f,0x02,0x22,0x32,
    0x0f,0x08,0x28,0x38, 0x0f,0x0c,0x2c,0x3c
};
/* Packed modes light the world with one continuous 128-entry RGB555 colour
 * field. Entries 0-127 are a cyclic multi-hue gradient; 128-255 hold the fixed
 * sprite ramps so actors keep their own colours on top of the field. */
#define GRADIENT_ENTRIES 128
#define PAL_REGION_STRIDE 16
#define PAL_HOUSE    128
#define PAL_GEM      144
#define PAL_WING     160
#define PAL_CRYSTAL  176   /* four ramps at 176/192/208/224 */
#define PAL_FONT     240
/* 5x7 uppercase font for in-world signs. Each character occupies one tile. */
static const uint8_t font[26][7] = {
    {14,17,17,31,17,17,17},{30,17,17,30,17,17,30},{14,17,16,16,16,17,14},
    {30,17,17,17,17,17,30},{31,16,16,30,16,16,31},{31,16,16,30,16,16,16},
    {14,17,16,23,17,17,15},{17,17,17,31,17,17,17},{14,4,4,4,4,4,14},
    {7,2,2,2,18,18,12},{17,18,20,24,20,18,17},{16,16,16,16,16,16,31},
    {17,27,21,21,17,17,17},{17,25,21,19,17,17,17},{14,17,17,17,17,17,14},
    {30,17,17,30,16,16,16},{14,17,17,17,21,18,13},{30,17,17,30,20,18,17},
    {15,16,16,14,1,1,30},{31,4,4,4,4,4,4},{17,17,17,17,17,17,14},
    {17,17,17,17,17,10,4},{17,17,17,21,21,21,10},{17,17,10,4,10,17,17},
    {17,17,10,4,4,4,4},{31,1,2,4,8,16,31}
};

static int target_x, target_y;
static float camera_x, camera_y;
static uint8_t next_sprite;
static uint32_t animation;
static uint8_t mode;          /* 0 NES, 2 planar4 */
static uint8_t mode_pending;  /* set when a toggle must be applied in vblank */

static void uart_text(const char *text) {
    while (*text) uart[0] = (uint8_t)*text++;
}
static void uart_number(unsigned value) {
    uart[0] = (uint8_t)('0' + value / 100);
    uart[0] = (uint8_t)('0' + (value / 10) % 10);
    uart[0] = (uint8_t)('0' + value % 10);
}
static void camera_report(void) {
    uart_text("Camera target: "); uart_number((unsigned)target_x);
    uart_text(", "); uart_number((unsigned)target_y); uart_text("\r\n");
}
static void vsn_write(unsigned offset, uint32_t value, unsigned bytes) {
    /* All VSN fields are byte-addressed, little-endian. */
    for (unsigned i = 0; i < bytes; ++i) vsn[offset + i] = (uint8_t)(value >> (i * 8));
}
static void encode_tile(volatile uint8_t *destination, const char rows[8][9],
                        unsigned transparent) {
    for (unsigned y = 0; y < 8; ++y) {
        uint8_t low = 0, high = 0;
        for (unsigned x = 0; x < 8; ++x) {
            unsigned pixel = (unsigned)(rows[y][x] - '0');
            if (pixel == transparent) pixel = 0;
            low |= (uint8_t)((pixel & 1) << (7 - x));
            high |= (uint8_t)(((pixel >> 1) & 1) << (7 - x));
        }
        destination[y] = low;
        destination[y + 8] = high;
    }
}
static unsigned hex_value(char c) {
    if (c >= '0' && c <= '9') return (unsigned)(c - '0');
    if (c >= 'a' && c <= 'f') return (unsigned)(c - 'a' + 10);
    return 0;
}
/* Planar4 tiles store four consecutive 8-byte bitplanes; plane 0 is the LSB. */
static void encode_tile4(volatile uint8_t *destination, const char rows[8][9],
                         unsigned transparent) {
    for (unsigned y = 0; y < 8; ++y) {
        uint8_t planes[4] = {0, 0, 0, 0};
        for (unsigned x = 0; x < 8; ++x) {
            unsigned pixel = hex_value(rows[y][x]);
            if (pixel == transparent) pixel = 0;
            for (unsigned p = 0; p < 4; ++p)
                planes[p] |= (uint8_t)(((pixel >> p) & 1) << (7 - x));
        }
        for (unsigned p = 0; p < 4; ++p) destination[p * 8 + y] = planes[p];
    }
}
static void rgb444_entry(volatile uint8_t *palette, unsigned index,
                         uint8_t r, uint8_t g, uint8_t b) {
    uint16_t word = (uint16_t)(b & 15) | (uint16_t)(g & 15) << 4 | (uint16_t)(r & 15) << 8;
    palette[index * 2] = (uint8_t)word;
    palette[index * 2 + 1] = (uint8_t)(word >> 8);
}
static void rgb444_ramp(volatile uint8_t *palette, unsigned base,
                        uint8_t r0, uint8_t g0, uint8_t b0,
                        uint8_t r1, uint8_t g1, uint8_t b1) {
    for (unsigned i = 0; i < 16; ++i)
        rgb444_entry(palette, base + i,
                     (uint8_t)(r0 + (r1 - r0) * i / 15),
                     (uint8_t)(g0 + (g1 - g0) * i / 15),
                     (uint8_t)(b0 + (b1 - b0) * i / 15));
}
static void rgb555_entry(volatile uint8_t *palette, unsigned index,
                         uint8_t r, uint8_t g, uint8_t b) {
    uint16_t word = (uint16_t)(b & 31) | (uint16_t)(g & 31) << 5 | (uint16_t)(r & 31) << 10;
    palette[index * 2] = (uint8_t)word;
    palette[index * 2 + 1] = (uint8_t)(word >> 8);
}
/* Packed 8bpp tiles index the palette directly. The tiles reuse the NES shapes
 * as a three-level height field, but an ordered dither spreads each level over
 * a 15-step ramp, so the same art gains the smooth shading that only 8bpp and
 * RGB555 can show. */
static const uint8_t bayer4[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
static const uint8_t bayer8[64] = {
     0, 32,  8, 40,  2, 34, 10, 42, 48, 16, 56, 24, 50, 18, 58, 26,
    12, 44,  4, 36, 14, 46,  6, 38, 60, 28, 52, 20, 62, 30, 54, 22,
     3, 35, 11, 43,  1, 33,  9, 41, 51, 19, 59, 27, 49, 17, 57, 25,
    15, 47,  7, 39, 13, 45,  5, 37, 63, 31, 55, 23, 61, 29, 53, 21
};
static unsigned dither_offset(unsigned x, unsigned y, int fine) {
    return fine ? (bayer8[(y & 7) * 8 + (x & 7)] >> 4) : (bayer4[(y & 3) * 4 + (x & 3)] >> 2);
}
static uint8_t shade8(unsigned level, unsigned x, unsigned y, unsigned base, int fine) {
    unsigned shade = (level - 1) * 5 + 2 + dither_offset(x, y, fine);
    if (shade > 15) shade = 15;
    return (uint8_t)(base + shade);
}
static void encode_shaded8(volatile uint8_t *destination, const char rows[8][9],
                           unsigned base, int fine) {
    for (unsigned y = 0; y < 8; ++y)
        for (unsigned x = 0; x < 8; ++x) {
            unsigned level = (unsigned)(rows[y][x] - '0');
            destination[y * 8 + x] = (level >= 1 && level <= 3) ? shade8(level, x, y, base, fine) : 0;
        }
}
/* The 16x16 bank scales the same height field 2x and shades it with the finer
 * 8x8 dither, so packed16 reads as higher resolution rather than a zoom. */
static void encode_shaded16(volatile uint8_t *destination, const char rows[8][9],
                            unsigned base) {
    for (unsigned y = 0; y < 16; ++y)
        for (unsigned x = 0; x < 16; ++x) {
            unsigned level = (unsigned)(rows[y / 2][x / 2] - '0');
            destination[y * 16 + x] = (level >= 1 && level <= 3) ? shade8(level, x, y, base, 1) : 0;
        }
}
static void world_tile(unsigned x, unsigned y, uint8_t tile) {
    unsigned page = (y / 30) * 2 + x / 32;
    map[page * 1024 + (y % 30) * 32 + x % 32] = tile;
}
static void world_palette(unsigned x, unsigned y, uint8_t palette) {
    unsigned page = (y / 30) * 2 + x / 32;
    unsigned local_x = x % 32, local_y = y % 30;
    unsigned shift = ((local_y % 4) / 2) * 4 + ((local_x % 4) / 2) * 2;
    volatile uint8_t *attribute = map + page * 1024 + 960
                                + (local_y / 4) * 8 + local_x / 4;
    *attribute = (uint8_t)((*attribute & ~(3u << shift)) | (palette << shift));
}
static void sign(unsigned x, unsigned y, const char *text) {
    while (*text) world_tile(x++, y, (uint8_t)(32 + *text++ - 'A'));
}
static void build_planar_map(void);
static void build_planar_sprites(void);
static void build_rgb444(void);
static void build_rgb555(void);
static void build_field_lut(void);
static void build_packed8_tiles(void);
static void build_packed16_tiles(void);
static void build_packed_sprites(void);
static void build_packed_maps(void);
static void build_hires_detail(void);
static void build_world(void) {
    volatile uint8_t *bg = (volatile uint8_t *)BG_TILE_BASE;
    volatile uint8_t *sp = (volatile uint8_t *)SP_TILE_BASE;
    volatile uint8_t *palette = (volatile uint8_t *)PALETTE_BASE;
    /* Rebuild on warm as well as cold reset; no dependency on cleared RAM. */
    for (unsigned i = 0; i < 4096; ++i) { bg[i] = 0; sp[i] = 0; }
    for (unsigned i = 0; i < BG_TILE_COUNT; ++i)
        encode_tile(bg + i * 16, backgrounds[i], 4);
    for (unsigned i = 0; i < HOUSE_BRICK; ++i)
        encode_tile(sp + i * 16, sprites[i], 4);
    encode_tile(sp + HOUSE_BRICK * 16, backgrounds[BRICK], 1);
    encode_tile(sp + HOUSE_ROOF * 16, backgrounds[ROOF], 1);
    encode_tile(sp + HOUSE_WINDOW * 16, backgrounds[WINDOW], 1);
    encode_tile(sp + HOUSE_DOOR * 16, backgrounds[DOOR], 1);
    for (unsigned c = 0; c < 26; ++c) {
        for (unsigned y = 0; y < 7; ++y) {
            bg[(32 + c) * 16 + y] = (uint8_t)(font[c][y] << 2);
            bg[(32 + c) * 16 + y + 8] = (uint8_t)(font[c][y] << 2);
        }
    }
    for (unsigned i = 0; i < 32; ++i) palette[i] = colors[i];
    for (unsigned page = 0; page < 4; ++page)
        for (unsigned i = 960; i < 1024; ++i)
            map[page * 1024 + i] = (uint8_t)(page * 0x55); /* four attribute quadrants */
    for (unsigned y = 0; y < 60; ++y) {
        for (unsigned x = 0; x < 64; ++x) {
            unsigned local_x = x % 32, local_y = y % 30;
            uint8_t tile = ((x * 13 + y * 7) % 11 == 0) ? TUFT : GRASS;
            if (x < 32 && y >= 30) tile = WATER;
            else if ((x * 3 + y * 5) % 29 == 0) tile = FLOWER;
            else if ((x * 7 + y) % 37 == 0) tile = ROCK;
            if (local_x >= 9 && local_x <= 22 && local_y >= 9 && local_y <= 19) tile = ROAD;
            /* Two axes of broad roads join all four screens. */
            if ((x >= 30 && x <= 33) || (y >= 28 && y <= 31)) tile = ROAD;
            if (!x || x == 63 || !y || y == 59) tile = EDGE;
            world_tile(x, y, tile);
        }
    }
    for (unsigned page = 0; page < 4; ++page) {
        unsigned x = (page & 1) * 32, y = (page >> 1) * 30;
        /* Attribute quadrants are 2x2 tiles. Make the observatory clearing
         * green while leaving the rest of each page in its original bank. */
        for (unsigned dy = 8; dy < 20; dy += 2)
            for (unsigned dx = 8; dx < 24; dx += 2)
                world_palette(x + dx, y + dy, 0);
        for (unsigned dy = 0; dy < 3; ++dy) {
            world_tile(x + 6, y + 7 + dy * 6, CANOPY);
            world_tile(x + 6, y + 8 + dy * 6, TRUNK);
        }
    }
    sign(12, 5, "GROVE"); sign(44, 5, "DUNES");
    sign(12, 35, "LAKE"); sign(44, 35, "RUINS");
    world_tile(31, 29, STAR); world_tile(32, 29, STAR);
    world_tile(31, 30, STAR); world_tile(32, 30, STAR);
    for (unsigned i = 0; i < 256; ++i) {
        ((volatile uint8_t *)OAM_BASE)[i] = 255;
        ((volatile uint8_t *)OAM_BACK_BASE)[i] = 255;
    }
    build_planar_map();
    build_planar_sprites();
    build_rgb444();
}
/* The packed banks are the expensive part (~490 KiB of per-cell tiles), so they
 * build after the display is already showing the NES world. */
static void build_packed_assets(void) {
    build_rgb555();
    build_field_lut();
    build_packed_maps();
    build_packed8_tiles();
    build_packed16_tiles();
    build_packed_sprites();
    build_hires_detail();
}
/* Re-emit the NES nametable+attribute world as a planar4 16-bit descriptor map
 * (tile index bits 0-11, palette bank bits 12-15), avoiding a second copy of
 * the world-generation logic. */
static void build_planar_map(void) {
    volatile uint8_t *desc = (volatile uint8_t *)PLANAR_MAP_BASE;
    for (unsigned y = 0; y < 60; ++y) {
        for (unsigned x = 0; x < 64; ++x) {
            unsigned page = (y / 30) * 2 + x / 32;
            unsigned local_x = x % 32, local_y = y % 30;
            uint8_t tile = map[page * 1024 + local_y * 32 + local_x];
            unsigned shift = ((local_y % 4) / 2) * 4 + ((local_x % 4) / 2) * 2;
            volatile uint8_t *attribute = map + page * 1024 + 960
                                        + (local_y / 4) * 8 + local_x / 4;
            unsigned bank = (*attribute >> shift) & 3;
            uint16_t descriptor = (uint16_t)tile | (uint16_t)(bank << 12);
            unsigned off = y * 128 + x * 2; /* 64 tiles/row * 2 bytes */
            desc[off] = (uint8_t)descriptor;
            desc[off + 1] = (uint8_t)(descriptor >> 8);
        }
    }
}
static void build_planar_sprites(void) {
    volatile uint8_t *sp = (volatile uint8_t *)PLANAR_SP_BASE;
    for (unsigned i = 0; i < HOUSE_BRICK; ++i)
        encode_tile4(sp + i * 32, sprites4[i], 0);
    /* House tiles keep their 2bpp art, re-encoded as 4bpp (planes 2-3 zero). */
    encode_tile4(sp + HOUSE_BRICK * 32, backgrounds[BRICK], 1);
    encode_tile4(sp + HOUSE_ROOF * 32, backgrounds[ROOF], 1);
    encode_tile4(sp + HOUSE_WINDOW * 32, backgrounds[WINDOW], 1);
    encode_tile4(sp + HOUSE_DOOR * 32, backgrounds[DOOR], 1);
}
/* 256-entry RGB444 palette. Entries 0-15 are the four 2bpp background banks,
 * one per world region (black plus three shades), so the planar4 background
 * keeps the same green/dune/lake/ruin split as NES mode. Entries 16-63 are
 * three 4bpp sprite banks; planar4 sprites index bank*16+pixel and use banks
 * 1-3, so they never collide with the background's entries 0-15. */
static void build_rgb444(void) {
    volatile uint8_t *p = (volatile uint8_t *)RGB444_BASE;
    for (unsigned i = 0; i < 512; ++i) p[i] = 0;
    rgb444_entry(p, 1, 1, 4, 0);   rgb444_entry(p, 2, 4, 8, 0);   rgb444_entry(p, 3, 8, 12, 4);   /* grass */
    rgb444_entry(p, 5, 6, 2, 0);   rgb444_entry(p, 6, 9, 5, 0);   rgb444_entry(p, 7, 13, 10, 4);  /* dunes */
    rgb444_entry(p, 9, 1, 2, 7);   rgb444_entry(p, 10, 2, 5, 11); rgb444_entry(p, 11, 7, 9, 15);  /* lake */
    rgb444_entry(p, 13, 4, 0, 7);  rgb444_entry(p, 14, 7, 3, 12); rgb444_entry(p, 15, 11, 8, 14); /* ruins */
    rgb444_entry(p, 17, 7, 1, 0);  rgb444_entry(p, 18, 11, 4, 2); rgb444_entry(p, 19, 14, 9, 6);  /* houses */
    rgb444_ramp(p, 32, 7, 3, 0, 15, 15, 4);    /* gems: amber -> gold */
    rgb444_ramp(p, 48, 3, 7, 15, 15, 15, 15);  /* wings: bright azure -> white */
}
/* The packed background is one continuous colour field: entries 0-127 form a
 * cyclic eight-stop RGB555 gradient, and each cell's tile carries the field
 * position at every pixel. Because a tile is a plain slice of the field, the
 * screen shows a real smooth gradient rather than a recoloured tile bank. */
static const uint8_t gradient_stops[8][3] = {
    {2, 6, 20}, {0, 18, 30}, {0, 30, 16}, {20, 31, 4},
    {31, 26, 4}, {31, 10, 6}, {26, 4, 24}, {10, 2, 30}
};
static void rgb555_ramp(volatile uint8_t *palette, unsigned base,
                        uint8_t r0, uint8_t g0, uint8_t b0,
                        uint8_t r1, uint8_t g1, uint8_t b1) {
    for (unsigned i = 1; i <= 15; ++i)
        rgb555_entry(palette, base + i,
                     (uint8_t)(r0 + (r1 - r0) * i / 15),
                     (uint8_t)(g0 + (g1 - g0) * i / 15),
                     (uint8_t)(b0 + (b1 - b0) * i / 15));
}
static void build_rgb555(void) {
    volatile uint8_t *p = (volatile uint8_t *)RGB555_BASE;
    for (unsigned i = 0; i < 512; ++i) p[i] = 0;
    for (unsigned i = 0; i < GRADIENT_ENTRIES; ++i) {
        const unsigned segment = i / (GRADIENT_ENTRIES / 8), step = i % (GRADIENT_ENTRIES / 8);
        const uint8_t *from = gradient_stops[segment];
        const uint8_t *to = gradient_stops[(segment + 1) & 7];
        rgb555_entry(p, i,
                     (uint8_t)(from[0] + ((int)to[0] - from[0]) * (int)step / (GRADIENT_ENTRIES / 8)),
                     (uint8_t)(from[1] + ((int)to[1] - from[1]) * (int)step / (GRADIENT_ENTRIES / 8)),
                     (uint8_t)(from[2] + ((int)to[2] - from[2]) * (int)step / (GRADIENT_ENTRIES / 8)));
    }
    rgb555_ramp(p, PAL_HOUSE, 2, 0, 0, 31, 14, 8);
    rgb555_ramp(p, PAL_GEM, 3, 1, 0, 31, 31, 12);
    rgb555_ramp(p, PAL_WING, 1, 4, 10, 31, 31, 31);
    rgb555_ramp(p, PAL_CRYSTAL +  0, 0, 4, 1, 26, 31, 28); /* emerald */
    rgb555_ramp(p, PAL_CRYSTAL + 16, 5, 0, 0, 31, 26, 26); /* ruby */
    rgb555_ramp(p, PAL_CRYSTAL + 32, 0, 2, 6, 26, 30, 31); /* sapphire */
    rgb555_ramp(p, PAL_CRYSTAL + 48, 4, 0, 6, 30, 26, 31); /* amethyst */
    rgb555_entry(p, PAL_FONT, 31, 31, 31);
}
/* Field position for a world pixel: a slow diagonal sweep through the whole
 * 128-entry gradient, so a viewport spans only a fraction of the cycle. The
 * sweep value is a linear function of the world pixel, so a 4 KiB lookup table
 * turns it into one indexed load per pixel instead of a multiply. */
static uint8_t field_lut[4096];
static const uint8_t relief_lut[5] = {0, 0, 8, 16, 0};
static void build_field_lut(void) {
    for (unsigned raw = 0; raw < 4096; ++raw)
        field_lut[raw] = (uint8_t)(((raw * (GRADIENT_ENTRIES - 1)) >> 12) & (GRADIENT_ENTRIES - 1));
}
static uint8_t cell_tile(unsigned x, unsigned y) {
    unsigned page = (y / 30) * 2 + x / 32;
    return map[page * 1024 + (y % 30) * 32 + x % 32];
}
/* packed8: one unique 8x8 tile per world cell, index y*64+x. */
static void build_packed8_tiles(void) {
    uint8_t *bank = (uint8_t *)P8_TILE_BASE;
    for (unsigned cy = 0; cy < 60; ++cy) {
        for (unsigned cx = 0; cx < 64; ++cx) {
            const uint8_t tile = cell_tile(cx, cy);
            uint8_t *out = bank + (cy * 64 + cx) * 64;
            if (tile >= 32 && tile < 58) {
                const uint8_t *glyph = font[tile - 32];
                for (unsigned ly = 0; ly < 8; ++ly) {
                    unsigned raw = (cx * 8) * 5 + (cy * 8 + ly) * 3;
                    for (unsigned lx = 0; lx < 8; ++lx, raw += 5) {
                        const int set = ly < 7 && lx < 5 && ((glyph[ly] >> (4 - lx)) & 1);
                        out[ly * 8 + lx] = set ? PAL_FONT : field_lut[raw];
                    }
                }
            } else {
                for (unsigned ly = 0; ly < 8; ++ly) {
                    const char *art = backgrounds[tile][ly];
                    unsigned raw = (cx * 8) * 5 + (cy * 8 + ly) * 3;
                    for (unsigned lx = 0; lx < 8; ++lx, raw += 5)
                        out[ly * 8 + lx] =
                            (uint8_t)((field_lut[raw] + relief_lut[(unsigned)(art[lx] - '0')]) &
                                      (GRADIENT_ENTRIES - 1));
                }
            }
        }
    }
}
/* packed16: one unique 16x16 tile per 2x2 cell block, index y*32+x. The source
 * shape is doubled but the field is sampled at the real 16x16 resolution. */
static void build_packed16_tiles(void) {
    uint8_t *bank = (uint8_t *)P16_TILE_BASE;
    for (unsigned by = 0; by < 30; ++by) {
        for (unsigned bx = 0; bx < 32; ++bx) {
            unsigned tile = cell_tile(bx * 2, by * 2);
            if (tile >= 32) tile = GRASS;
            uint8_t *out = bank + (by * 32 + bx) * 256;
            for (unsigned ly = 0; ly < 16; ++ly) {
                const char *art = backgrounds[tile][ly / 2];
                unsigned raw = (bx * 16) * 5 + (by * 16 + ly) * 3;
                for (unsigned lx = 0; lx < 16; ++lx, raw += 5)
                    out[ly * 16 + lx] =
                        (uint8_t)((field_lut[raw] + relief_lut[(unsigned)(art[lx / 2] - '0')]) &
                                  (GRADIENT_ENTRIES - 1));
            }
        }
    }
}
/* Shared packed sprite bank: shaded 8x8 actors and four 16x16 crystals. */
static void build_packed_sprites(void) {
    volatile uint8_t *sp = (volatile uint8_t *)P8_SP_BASE;
    for (unsigned i = 0; i < (PX_WIDE_TILE + 4) * 256; ++i) sp[i] = 0;
    /* The packed sprite order matches the NES GEM/WING/HOUSE_* enum. */
    encode_shaded8(sp + GEM * 64, sprites[GEM], PAL_GEM, 0);
    encode_shaded8(sp + WING * 64, sprites[WING], PAL_WING, 0);
    encode_shaded8(sp + HOUSE_BRICK * 64, backgrounds[BRICK], PAL_HOUSE, 0);
    encode_shaded8(sp + HOUSE_ROOF * 64, backgrounds[ROOF], PAL_HOUSE, 0);
    encode_shaded8(sp + HOUSE_WINDOW * 64, backgrounds[WINDOW], PAL_HOUSE, 0);
    encode_shaded8(sp + HOUSE_DOOR * 64, backgrounds[DOOR], PAL_HOUSE, 0);
    /* One 16x16 crystal per region, each on its own 15-step ramp. */
    for (unsigned region = 0; region < 4; ++region)
        encode_shaded16(sp + (PX_WIDE_TILE + region) * 256, sprites[GEM],
                        PAL_CRYSTAL + region * PAL_REGION_STRIDE);
}
/* The descriptor is just the cell's unique tile index. */
static void build_packed_maps(void) {
    volatile uint8_t *p8 = (volatile uint8_t *)P8_MAP_BASE;
    for (unsigned y = 0; y < 60; ++y)
        for (unsigned x = 0; x < 64; ++x) {
            const uint16_t descriptor = (uint16_t)(y * 64 + x);
            p8[y * 128 + x * 2] = (uint8_t)descriptor;
            p8[y * 128 + x * 2 + 1] = (uint8_t)(descriptor >> 8);
        }
    volatile uint8_t *p16 = (volatile uint8_t *)P16_MAP_BASE;
    for (unsigned by = 0; by < 30; ++by)
        for (unsigned bx = 0; bx < 32; ++bx) {
            const uint16_t descriptor = (uint16_t)(by * 32 + bx);
            p16[by * 64 + bx * 2] = (uint8_t)descriptor;
            p16[by * 64 + bx * 2 + 1] = (uint8_t)(descriptor >> 8);
        }
}
/* Hires showcase: a strip of 1-pixel patterns and 1-pixel-stroke text. These
 * are ordinary 8bpp 16x16 tiles, but they only read as crisp single pixels at
 * the native 512x480 resolution; the 256x240 modes upscale every pixel to a
 * visible 2x2 block, so the same strip doubles as a resolution comparison. */
static void encode_fine_glyph(volatile uint8_t *t, int glyph, unsigned x0, unsigned y0) {
    if (glyph < 0 || glyph > 25) return;
    for (unsigned y = 0; y < 7; ++y)
        for (unsigned x = 0; x < 5; ++x)
            if ((font[glyph][y] >> (4 - x)) & 1)
                t[(y0 + y) * 16 + (x0 + x)] = DETAIL_WHITE;
}
static void fine_text_tile(volatile uint8_t *t, int g0, int g1) {
    for (unsigned i = 0; i < 256; ++i) t[i] = 0;
    encode_fine_glyph(t, g0, 1, 4);
    encode_fine_glyph(t, g1, 7, 4);
}
static void build_hires_detail(void) {
    volatile uint8_t *bank = (volatile uint8_t *)P16_TILE_BASE;
    volatile uint8_t *t = bank + DETAIL_TILE0 * 256;
    for (unsigned y = 0; y < 16; ++y)              /* 1px checkerboard */
        for (unsigned x = 0; x < 16; ++x)
            t[y * 16 + x] = ((x ^ y) & 1) ? DETAIL_WHITE : 0;
    t = bank + (DETAIL_TILE0 + 1) * 256;
    for (unsigned y = 0; y < 16; ++y)              /* 1px horizontal lines */
        for (unsigned x = 0; x < 16; ++x)
            t[y * 16 + x] = (y & 1) ? DETAIL_WHITE : 0;
    t = bank + (DETAIL_TILE0 + 2) * 256;
    for (unsigned y = 0; y < 16; ++y)              /* 1px vertical lines */
        for (unsigned x = 0; x < 16; ++x)
            t[y * 16 + x] = (x & 1) ? DETAIL_WHITE : 0;
    t = bank + (DETAIL_TILE0 + 3) * 256;
    for (unsigned y = 0; y < 16; ++y)              /* 1px diagonal lines */
        for (unsigned x = 0; x < 16; ++x)
            t[y * 16 + x] = ((x + y) & 1) ? DETAIL_WHITE : 0;
    fine_text_tile(bank + (DETAIL_TILE0 + 4) * 256, 7, 8);    /* "HI" */
    fine_text_tile(bank + (DETAIL_TILE0 + 5) * 256, 17, 4);   /* "RE" */
    fine_text_tile(bank + (DETAIL_TILE0 + 6) * 256, 18, -1);  /* "S"  */

    /* Row 10, columns 8-14: clear grass between the tree rows and the central
     * crossroads, and inside packed16's default 256x240 view so the same strip
     * can be compared crisp (hires) versus blocky (2x upscaled packed16). */
    volatile uint8_t *p16 = (volatile uint8_t *)P16_MAP_BASE;
    const unsigned by = 10;
    for (unsigned i = 0; i < 7; ++i) {
        const uint16_t descriptor = (uint16_t)(DETAIL_TILE0 + i);
        const unsigned bx = 8 + i;
        p16[by * 64 + bx * 2] = (uint8_t)descriptor;
        p16[by * 64 + bx * 2 + 1] = (uint8_t)(descriptor >> 8);
    }
}

static void poll_uart(void) {
    /* A character is a command immediately; live-mode CR/LF are separators,
     * never another movement or a command that waits for a line buffer. */
    for (unsigned budget = 0; budget < 32 && (uart[1] & 1); ++budget) {
        uint8_t key = uart[0];
        int previous_x = target_x, previous_y = target_y;
        if (key >= 'A' && key <= 'Z') key = (uint8_t)(key + 'a' - 'A');
        switch (key) {
        case 'w': target_y -= CAMERA_STEP; break;
        case 'a': target_x -= CAMERA_STEP; break;
        case 's': target_y += CAMERA_STEP; break;
        case 'd': target_x += CAMERA_STEP; break;
        case 'r': target_x = 128; target_y = 120; break;
        case 'm':
            mode = mode == 0 ? 2 : mode == 2 ? 3 : mode == 3 ? 4 : mode == 4 ? 5 : 0;
            mode_pending = 1;
            break;
        case '?': uart_text("WASD: pan 8 px | R: center | M: cycle mode | ?: help\r\n"); break;
        default: break; /* including CR, LF, spaces and unknown characters */
        }
        /* Mode 5 shows the whole 512x480 world, so the camera is pinned to the
         * origin; the clamp range shrinks to zero there and widens again when
         * the user cycles back to a 256x240 mode. */
        int view_w = mode == 5 ? WORLD_WIDTH : VIEW_WIDTH;
        int view_h = mode == 5 ? WORLD_HEIGHT : VIEW_HEIGHT;
        if (target_x < 0) target_x = 0;
        if (target_x > WORLD_WIDTH - view_w) target_x = WORLD_WIDTH - view_w;
        if (target_y < 0) target_y = 0;
        if (target_y > WORLD_HEIGHT - view_h) target_y = WORLD_HEIGHT - view_h;
        if (target_x != previous_x || target_y != previous_y) camera_report();
    }
}
static float approach(float current, int target) {
    float difference = (float)target - current;
    if (difference > -0.25f && difference < 0.25f) return (float)target;
    return current + difference * 0.375f; /* real RV32F arithmetic */
}
static int packed_mode(void) { return mode == 3 || mode == 4 || mode == 5; }
static uint32_t oam_primary(void) { return packed_mode() ? EXT_OAM_BASE : OAM_BASE; }
static uint32_t oam_secondary(void) { return packed_mode() ? EXT_OAM_BACK : OAM_BACK_BASE; }
static void put_nes_sprite(int x, int y, uint8_t tile, uint8_t attributes) {
    /* NES OAM cannot represent negative X or top row 0. Cull crossing sprites
     * instead of wrapping byte coordinates to the opposite side of the view. */
    if (next_sprite >= 64 || x < 0 || x > 255 || y < 1 || y > 239) return;
    volatile uint8_t *record = oam + next_sprite++ * 4;
    record[0] = (uint8_t)(y - 1);
    record[1] = tile;
    record[2] = attributes;
    record[3] = (uint8_t)x;
}
/* Extended records carry signed top-left corners, so edge-crossing sprites are
 * kept and clipped by VSN instead of being culled by the guest. */
static void put_ext_sprite(int x, int y, uint8_t tile, uint8_t flags, uint8_t size) {
    if (next_sprite >= 32 || x < -16 || x > 255 || y < -16 || y > 239) return;
    volatile uint8_t *record = oam + next_sprite++ * 16;
    uint16_t ux = (uint16_t)x, uy = (uint16_t)y;
    record[0] = (uint8_t)ux; record[1] = (uint8_t)(ux >> 8);
    record[2] = (uint8_t)uy; record[3] = (uint8_t)(uy >> 8);
    record[4] = tile; record[5] = 0; record[6] = 0; record[7] = 0;
    record[8] = 0;                                        /* 8bpp ignores it */
    record[9] = (uint8_t)(flags | 8);                     /* enable */
    record[10] = size;                                    /* 0=8x8, 1=8x16, 2=16x16 */
    for (unsigned i = 11; i < 16; ++i) record[i] = 0;
}
static void put_object(int x, int y, uint8_t tile, uint8_t attributes) {
    if (!packed_mode()) {
        put_nes_sprite(x, y, tile, attributes);
        return;
    }
    /* Packed sprites bake their palette into the tile, so only the flip and
     * behind-background attribute bits carry over. */
    uint8_t flags = (uint8_t)(((attributes & 64) ? 1 : 0) |
                              ((attributes & 128) ? 2 : 0) |
                              ((attributes & 32) ? 4 : 0));
    put_ext_sprite(x, y, tile, flags, 0);
}
/* Emit every visible object for the current camera without advancing time. */
static void build_scene(int cx, int cy) {
    next_sprite = 0;
    /* planar4 4bpp sprites index bank*16+pixel, so they use banks 1-3 to stay
     * clear of the background's entries 0-15; NES mode keeps its 16-31 sprite
     * region through bank*4 indexing. */
    uint8_t house_bank = mode == 2 ? 1 : 0;
    uint8_t wing_bank = mode == 2 ? 3 : 1;
    int phase = (int)((animation / 4) & 15);
    int bob = (phase < 8 ? phase : 15 - phase) / 2;
    if (packed_mode()) {
        /* The four shaded 16x16 crystals own the lowest records so they always
         * survive the 32-entry table. Signed coordinates let them cross the
         * viewport edge and be clipped by VSN instead of culled by the guest. */
        for (unsigned page = 0; page < 4; ++page) {
            int origin_x = (int)(page & 1) * 256, origin_y = (int)(page >> 1) * 240;
            int corner_x = (page & 1) ? 8 : 232;
            int corner_y = (page >> 1) ? 8 : 216;
            put_ext_sprite(origin_x + corner_x - cx, origin_y + corner_y - cy + bob,
                           (uint8_t)(PX_WIDE_TILE + page), 0, 2);
        }
    }
    /* Static world positions minus the camera keep background and sprites
     * scrolling together. */
    for (unsigned page = 0; page < 4; ++page) {
        int origin_x = (int)(page & 1) * 256, origin_y = (int)(page >> 1) * 240;
        /* Houses use transparent sprite copies of the background art so they
         * can stay red independently of the page and clearing palettes. */
        for (unsigned dy = 0; dy < 5; ++dy) {
            for (unsigned dx = 0; dx < 6; ++dx) {
                uint8_t tile = dy == 0 ? HOUSE_ROOF : HOUSE_BRICK;
                if (dy == 2 && (dx == 1 || dx == 4)) tile = HOUSE_WINDOW;
                if (dx == 3 && (dy == 3 || dy == 4)) tile = HOUSE_DOOR;
                put_object(origin_x + (13 + (int)dx) * 8 - cx,
                           origin_y + (12 + (int)dy) * 8 - cy, tile, house_bank);
            }
        }
        for (unsigned i = 0; i < 5; ++i) {
            int x = origin_x + 88 + (int)i * 16;
            int y = origin_y + 160 + (int)(i & 1) * 12;
            put_object(x - cx, y - cy + bob, GEM,
                       mode == 2 ? 2 : (uint8_t)((page + animation / 24) & 3));
        }
        /* A two-wing butterfly patrols independently of camera motion. */
        int travel = (int)((animation + page * 32) & 127);
        if (travel >= 64) travel = 127 - travel;
        int x = origin_x + 96 + travel - cx;
        int y = origin_y + 64 + bob - cy;
        uint8_t flap = (animation & 8) ? 128 : 0;
        put_object(x, y, WING, (uint8_t)(wing_bank | flap));
        put_object(x + 7, y, WING, (uint8_t)(wing_bank | 64 | flap));
        /* Packed sprites may sit offscreen; stop once the 32-record table is
         * full so the nearest page keeps its objects. */
        if (packed_mode() && next_sprite >= 32) break;
    }
    /* Only unused entries need hiding; visible entries were replaced above. */
    if (packed_mode())
        for (unsigned i = next_sprite; i < 32; ++i) oam[i * 16 + 9] = 0;
    else
        for (unsigned i = next_sprite; i < 64; ++i) oam[i * 4] = 255;
}
static void select_pending_oam(void) {
    /* Build the inactive shared OAM table while the previous one is scanned. */
    pending_oam = front_oam == oam_primary() ? oam_secondary() : oam_primary();
    oam = (volatile uint8_t *)pending_oam;
}
/* Mode 5 renders the full 512x480 world, so its view origin is always (0,0). */
static int camera_cx(void) { return mode == 5 ? 0 : (int)(camera_x + 0.5f); }
static int camera_cy(void) { return mode == 5 ? 0 : (int)(camera_y + 0.5f); }
static void prepare_scene(void) {
    camera_x = approach(camera_x, target_x);
    camera_y = approach(camera_y, target_y);
    int cx = camera_cx(), cy = camera_cy();
    scroll_x = (unsigned)cx; scroll_y = (unsigned)cy;
    select_pending_oam();
    build_scene(cx, cy);
    ++animation;
}
/* Rebuild the inactive table after a live mode change in the vblank window. */
static void rebuild_scene(void) {
    int cx = camera_cx(), cy = camera_cy();
    pending_oam = oam_primary();
    oam = (volatile uint8_t *)pending_oam;
    build_scene(cx, cy);
}

static void publish_scene(void) {
    vsn_write(0x14, scroll_x, 2); vsn_write(0x16, scroll_y, 2);
    vsn_write(0x30, pending_oam, 4);
    front_oam = pending_oam;
}
/* UART bursts may make us miss the leading edge. Wait for an EARLY vblank
 * before writing multi-byte scroll/base registers, never race its last line. */
static void wait_early_vblank(void) {
    for (;;) {
        while (!(vsn[6] & 0x80)) poll_uart();
        unsigned line = (unsigned)vsn[0x54] | ((unsigned)vsn[0x55] << 8);
        if (line >= 242 && line <= 245) return;
        while (vsn[6] & 0x80) poll_uart();
    }
}
/* Point the MMIO base registers at the current mode's assets. Call only inside
 * the vblank window; every mode shares the scroll range and sprite tile order,
 * so only bases, the map geometry and MODE/PLANAR change. */
static const char *mode_report(void) {
    if (mode == 2) return "Mode: planar4 (4bpp planar sprites)\r\n";
    if (mode == 3) return "Mode: packed8 (8bpp + extended sprites)\r\n";
    if (mode == 4) return "Mode: packed16 (16x16 8bpp + extended sprites)\r\n";
    if (mode == 5) return "Mode: hires (native 512x480 16x16 8bpp)\r\n";
    return "Mode: NES (2bpp)\r\n";
}
static void apply_mode(void) {
    if (mode == 2) {
        vsn[5] = 2;                 /* planar4 */
        vsn[0x39] = 0x02;           /* PLANAR: BG 2bpp, sprites 4bpp */
        vsn_write(0x20, PLANAR_MAP_BASE, 4);
        vsn_write(0x24, BG_TILE_BASE, 4);
        vsn_write(0x28, 128, 2);    /* MAP_ROW_STRIDE = 64 tiles * 2 bytes */
        vsn[0x2a] = 64; vsn[0x2b] = 60;
        vsn_write(0x30, OAM_BASE, 4);
        vsn_write(0x34, PLANAR_SP_BASE, 4);
        vsn_write(0x40, RGB444_BASE, 4);
        vsn[0x45] = 0;              /* backdrop = palette entry 0 */
    } else if (mode == 3 || mode == 4 || mode == 5) {
        const int wide = mode == 4 || mode == 5;
        vsn[5] = (uint8_t)mode;     /* packed8, packed16 or hires */
        vsn[0x39] = 0;              /* PLANAR only applies to mode 2 */
        vsn_write(0x20, wide ? P16_MAP_BASE : P8_MAP_BASE, 4);
        vsn_write(0x24, wide ? P16_TILE_BASE : P8_TILE_BASE, 4);
        vsn_write(0x28, wide ? 64 : 128, 2);
        vsn[0x2a] = wide ? 32 : 64; vsn[0x2b] = wide ? 30 : 60;
        vsn_write(0x30, EXT_OAM_BASE, 4);
        vsn_write(0x34, P8_SP_BASE, 4);
        vsn_write(0x40, RGB555_BASE, 4);
        vsn[0x45] = 0;
    } else {
        vsn[5] = 0;                 /* NES */
        vsn_write(0x20, MAP_BASE, 4); vsn_write(0x24, BG_TILE_BASE, 4);
        vsn_write(0x30, OAM_BASE, 4); vsn_write(0x34, SP_TILE_BASE, 4);
        vsn_write(0x40, PALETTE_BASE, 4);
        vsn_write(0x2c, 1024, 2); vsn_write(0x2e, 2048, 2);
        vsn[0x18] = 0; vsn[0x38] = 0;
    }
    front_oam = 0; /* The next prepare picks the mode's primary table. */
}

extern uint8_t _data_load[], _data_start[], _data_end[], _bss_start[], _bss_end[];
__attribute__((noreturn)) void boot(void) {
    for (volatile uint8_t *p = _data_start; p < _data_end; ++p) *p = _data_load[p - _data_start];
    for (volatile uint8_t *p = _bss_start; p < _bss_end; ++p) *p = 0;
    vsn[4] = 0; /* Raster continues while shared assets are constructed. */
    uart_text("\r\nVSN / FOUR CORNERS - RV32IMF\r\n");
    if (vsn[0] != 'V' || vsn[1] != 'S' || vsn[2] != 'N' || vsn[3] != 1) {
        uart_text("VSN revision 1 not found at 0x10000000.\r\n");
        for (;;) {}
    }
    build_world();
    target_x = 128; target_y = 120; camera_x = 128.0f; camera_y = 120.0f;
    mode = 0;
    apply_mode(); /* NES mode: four-screen background + 64-entry OAM. */
    vsn[6] = 7;
    prepare_scene();
    uart_text("WASD: pan 8 px | R: center | M: cycle mode | ?: help\r\n");
    uart_text("Live input accepts each key immediately; CR/LF ignored.\r\n");
    camera_report();
    /* Start at a blanking boundary. Unlimited per-line sprites is intentional:
     * several independently animated objects can overlap the same scanline. */
    wait_early_vblank();
    publish_scene();
    vsn[4] = 0x1f; /* master, BG, sprites, BG-left, sprite-left */
    /* Show the NES world first, then spend the remaining boot time on the
     * per-cell packed banks. The raster keeps scanning a stable frame. */
    build_packed_assets();
    for (;;) {
        while (vsn[6] & 0x80) poll_uart();
        prepare_scene();
        wait_early_vblank();
        if (mode_pending) {
            apply_mode();
            mode_pending = 0;
            rebuild_scene(); /* emit the new mode's records before publishing */
            uart_text(mode_report());
        }
        publish_scene(); /* only eight MMIO bytes change in the vblank window */
        if (vsn[6] & 1) {
            vsn[4] = 0;
            uart_text("VSN memory fault: check the project RAM mapping.\r\n");
            for (;;) {}
        }
    }
}
__attribute__((section(".text.start"), naked, noreturn)) void _start(void) {
    __asm__ volatile(
        ".option push\n.option norelax\n"
        "la sp, _stack_top\n"
        "li t0, 0x6000\ncsrs mstatus, t0\ncsrw fcsr, zero\n"
        "call boot\n1: j 1b\n.option pop\n");
}
