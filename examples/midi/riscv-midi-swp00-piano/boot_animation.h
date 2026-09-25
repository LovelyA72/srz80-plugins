#ifndef MU50_BOOT_ANIMATION_H
#define MU50_BOOT_ANIMATION_H

/* Restrained two-second MU50-style power-on sequence for the 16x2 LCD. */
#define LCD_BOOT_TICKS 8000000u
#define LCD_BOOT_FRAMES 80u
#define LCD_BOOT_TICKS_PER_FRAME (LCD_BOOT_TICKS / LCD_BOOT_FRAMES)

static const char lcd_boot_title[17] = "SRZ80 MAME SWP00";
static const char lcd_boot_subtitle[17] = " GM1 PIANO DEMO ";
static const u8 lcd_boot_bars[8][8] = {
    {0,0,0,0,0,0,0,0x1f}, {0,0,0,0,0,0,0x1f,0x1f},
    {0,0,0,0,0,0x1f,0x1f,0x1f}, {0,0,0,0,0x1f,0x1f,0x1f,0x1f},
    {0,0,0,0x1f,0x1f,0x1f,0x1f,0x1f}, {0,0,0x1f,0x1f,0x1f,0x1f,0x1f,0x1f},
    {0,0x1f,0x1f,0x1f,0x1f,0x1f,0x1f,0x1f},
    {0x1f,0x1f,0x1f,0x1f,0x1f,0x1f,0x1f,0x1f}
};

static u32 lcd_boot_cycle_count(void) {
    u32 value;
    __asm__ volatile("csrr %0, cycle" : "=r"(value));
    return value;
}
static void lcd_boot_define_glyphs(void) {
    u8 glyph, row;
    for (glyph = 0; glyph != 8u; ++glyph) {
        LCD_COMMAND = (u8)(0x40u + glyph * 8u);
        for (row = 0; row != 8u; ++row) LCD_DATA = lcd_boot_bars[glyph][row];
    }
}
static void lcd_boot_line(u8 address, const u8 *cells) {
    u8 column;
    LCD_COMMAND = (u8)(0x80u | address);
    for (column = 0; column != 16u; ++column) LCD_DATA = cells[column];
}
static u8 lcd_boot_abs(int value) { return (u8)(value < 0 ? -value : value); }
static u8 lcd_boot_scale12(u8 value, u8 factor) {
    volatile u8 sum = 6u;
    u8 result = 0;
    while (factor--) sum = (u8)(sum + value);
    while (sum >= 12u) { sum = (u8)(sum - 12u); ++result; }
    return result;
}

static void lcd_boot_draw(u8 frame) {
    u8 top[16], bottom[16], column;
    for (column = 0; column != 16u; ++column) top[column] = bottom[column] = ' ';
    if (frame < 12u) {
        /* A bright pair meets in the center like a hardware boot scan. */
        static const u8 distance_by_frame[12] = {7,7,6,6,5,4,4,3,2,2,1,0};
        u8 distance = distance_by_frame[frame];
        top[7u-distance] = 7; top[8u+distance] = 7;
        bottom[7u-distance] = 3; bottom[8u+distance] = 3;
    } else {
        /* Reveal both labels symmetrically rather than typing them. */
        u8 radius = frame < 28u ? (u8)((frame - 12u) >> 1) : 8u;
        for (column = 0; column != 16u; ++column)
            if (lcd_boot_abs((int)(column << 1) - 15) <= (u8)((radius << 1) + 1u)) {
                top[column] = (u8)lcd_boot_title[column];
                bottom[column] = (u8)lcd_boot_subtitle[column];
            }
    }
    /* A single centered spectrum breathes once, then the subtitle returns. */
    if (frame >= 32u && frame < 58u) {
        static const u8 profile[16] = {0,1,3,5,7,5,3,2,2,3,5,7,5,3,1,0};
        u8 phase = (u8)(frame - 32u);
        u8 envelope = phase < 13u ? phase : (u8)(25u - phase);
        for (column = 0; column != 16u; ++column) {
            u8 level = lcd_boot_scale12(profile[column], envelope);
            bottom[column] = level > 7u ? 7u : level;
        }
    }
    lcd_boot_line(0x00u, top);
    lcd_boot_line(0x40u, bottom);
}

static void lcd_boot_play(void) {
    const u32 start = lcd_boot_cycle_count();
    u32 deadline = LCD_BOOT_TICKS_PER_FRAME;
    u8 frame = 0;
    lcd_boot_define_glyphs(); lcd_boot_draw(0);
    for (;;) {
        u32 elapsed = lcd_boot_cycle_count() - start;
        if (elapsed >= LCD_BOOT_TICKS) return;
        if (elapsed >= deadline && frame < LCD_BOOT_FRAMES - 1u) {
            do { deadline += LCD_BOOT_TICKS_PER_FRAME; ++frame; }
            while (elapsed >= deadline && frame < LCD_BOOT_FRAMES - 1u);
            lcd_boot_draw(frame);
        }
    }
}
#endif
