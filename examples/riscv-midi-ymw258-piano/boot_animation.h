#ifndef TG100_BOOT_ANIMATION_H
#define TG100_BOOT_ANIMATION_H

/*
 * Two-second piano-roll power-on animation for the LCD1602 surface.
 *
 * The board wrapper supplies LCD_COMMAND and LCD_DATA, exactly as
 * lcd_display.h does.  The animation runs from the CPU's own cycle counter so
 * it lasts LCD_BOOT_TICKS regardless of how expensive the drawing code turns
 * out to be.  It borrows all eight CGRAM slots during the intro and the board
 * wrapper re-defines them as the meter glyphs once the animation is over.
 *
 * Storyboard (100 frames of 20 ms):
 *   0- 7   the sixteen piano keys are laid down left to right
 *   8-83   one drop per banner character falls, spread across the keys, and
 *          leaves its character behind in the very cell it struck
 *  84-87   the scattered characters slide together into the banner line
 *  88-99   a win chase sweeps the keyboard under the resting banner
 * 100     the display hands over to the live program/meter view
 */

/* ---------------------------------------------------------------------------
 * OEM banner.  Replace this string (ASCII only: the LCD font has no others)
 * and keep the lcd_boot_columns table the same length.  Entry i is the keyboard
 * column where character i's drop lands and its character is left behind; the
 * characters then slide to LCD_BOOT_TITLE_COLUMN + i so the banner reads
 * straight.  Shorter banners just need a shorter table.
 * It can also be overridden from the build with -DLCD_BOOT_TITLE='"..."'.
 * ------------------------------------------------------------------------- */
#ifndef LCD_BOOT_TITLE
#define LCD_BOOT_TITLE "TG100 Piano"
#endif
#define LCD_BOOT_TITLE_LEN (sizeof(LCD_BOOT_TITLE) - 1u)
#define LCD_BOOT_TITLE_COLUMN 0u
_Static_assert(LCD_BOOT_TITLE_LEN >= 1u && LCD_BOOT_TITLE_LEN <= 16u,
               "LCD_BOOT_TITLE must fit the 16-column LCD1602");
_Static_assert(LCD_BOOT_TITLE_COLUMN + LCD_BOOT_TITLE_LEN <= 16u,
               "LCD_BOOT_TITLE_COLUMN pushes LCD_BOOT_TITLE off the LCD1602");

#define LCD_BOOT_TICKS 8000000u /* 2 s at the project's 4 MHz RISC-V clock */
#define LCD_BOOT_FRAMES 100u
#define LCD_BOOT_TICKS_PER_FRAME (LCD_BOOT_TICKS / LCD_BOOT_FRAMES)
#define LCD_BOOT_NOTE_FALL 6u /* frames spent in each falling glyph stage */
#define LCD_BOOT_CHASE 88u    /* first frame of the keyboard win chase */
#define LCD_BOOT_SLIDE 84u    /* first frame the banner gathers together */
/* Deal the drops out between the key reveal and the slide. */
#define LCD_BOOT_DROP_SPAN (LCD_BOOT_SLIDE - 8u - LCD_BOOT_NOTE_FALL * 4u)
#define LCD_BOOT_NOTE_STEP \
    (LCD_BOOT_DROP_SPAN / (LCD_BOOT_TITLE_LEN > 1u ? LCD_BOOT_TITLE_LEN - 1u : 1u))

enum {
    LCD_BOOT_BLANK = 0,
    LCD_BOOT_WHITE_KEY,
    LCD_BOOT_BLACK_KEY,
    LCD_BOOT_NOTE_HIGH,
    LCD_BOOT_NOTE_MID,
    LCD_BOOT_NOTE_LOW,
    LCD_BOOT_BLOCK,
    LCD_BOOT_SPARKLE
};

/* Keyboard column (0..15) each banner character's drop lands on, spread across
   the whole keyboard.  Character i is left here and later slides to its place
   in the straight banner line. */
static const u8 lcd_boot_columns[] = {
    0, 1, 2, 4, 5, 7, 8, 10, 11, 13, 14
};
_Static_assert(sizeof(lcd_boot_columns) == LCD_BOOT_TITLE_LEN,
               "lcd_boot_columns needs one keyboard column per LCD_BOOT_TITLE character");

/* One 5x8 glyph per CGRAM slot, eight rows of five bits. */
static const u8 lcd_boot_shapes[8][8] = {
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /* blank */
    {0x1F, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1F}, /* white key */
    {0x0E, 0x0E, 0x0E, 0x00, 0x1F, 0x11, 0x11, 0x1F}, /* black key over a white key */
    {0x04, 0x04, 0x04, 0x0E, 0x1F, 0x1F, 0x0E, 0x00}, /* note, high in the cell */
    {0x00, 0x04, 0x04, 0x04, 0x0E, 0x1F, 0x0E, 0x00}, /* note, middle */
    {0x00, 0x00, 0x04, 0x04, 0x04, 0x0E, 0x1F, 0x0E}, /* note, about to land */
    {0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F}, /* struck key */
    {0x04, 0x15, 0x0E, 0x1F, 0x0E, 0x15, 0x04, 0x00}  /* sparkle */
};

/* The five sharps of each seven-white-key octave, drawn over the white key.
   Kept as a table so the rv32i build never needs a modulo libcall. */
static const u8 lcd_boot_keys[16] = {
    LCD_BOOT_BLACK_KEY, LCD_BOOT_BLACK_KEY, LCD_BOOT_WHITE_KEY, LCD_BOOT_BLACK_KEY,
    LCD_BOOT_BLACK_KEY, LCD_BOOT_BLACK_KEY, LCD_BOOT_WHITE_KEY, LCD_BOOT_BLACK_KEY,
    LCD_BOOT_BLACK_KEY, LCD_BOOT_WHITE_KEY, LCD_BOOT_BLACK_KEY, LCD_BOOT_BLACK_KEY,
    LCD_BOOT_BLACK_KEY, LCD_BOOT_WHITE_KEY, LCD_BOOT_BLACK_KEY, LCD_BOOT_BLACK_KEY
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
        for (row = 0; row != 8u; ++row) LCD_DATA = lcd_boot_shapes[glyph][row];
    }
}

static void lcd_boot_line(u8 address, const u8 *cells) {
    u8 column;
    LCD_COMMAND = (u8)(0x80u | address);
    for (column = 0; column != 16u; ++column) LCD_DATA = cells[column];
}

static void lcd_boot_draw(u16 frame) {
    u8 top[16], bottom[16], index;
    const u8 revealed = frame < 8u ? (u8)(frame * 2u) : 16u;

    for (index = 0; index != 16u; ++index) {
        top[index] = LCD_BOOT_BLANK;
        bottom[index] = (u8)(index < revealed ? lcd_boot_keys[index] : LCD_BOOT_BLANK);
    }
    /* A landed drop leaves its banner character in the very cell it struck; the
       characters then slide to their place in the straight banner line. */
    for (index = 0; index != LCD_BOOT_TITLE_LEN; ++index) {
        const u16 impact = (u16)(8u + index * LCD_BOOT_NOTE_STEP + LCD_BOOT_NOTE_FALL * 3u);
        const int target = (int)(LCD_BOOT_TITLE_COLUMN + index);
        int column, slide;
        if (frame < impact) continue;
        column = (int)lcd_boot_columns[index];
        slide = frame > LCD_BOOT_SLIDE ? (int)(frame - LCD_BOOT_SLIDE) : 0;
        if (column > target) {
            const int distance = column - target;
            column -= distance < slide ? distance : slide;
        } else {
            const int distance = target - column;
            column += distance < slide ? distance : slide;
        }
        top[column] = (u8)LCD_BOOT_TITLE[index];
    }
    /* The drops fall onto the keys they will leave their character on, so a
       drop and its character always share a column. */
    for (index = 0; index != LCD_BOOT_TITLE_LEN; ++index) {
        const u8 column = lcd_boot_columns[index];
        const u16 start = (u16)(8u + index * LCD_BOOT_NOTE_STEP);
        const u16 impact = (u16)(start + LCD_BOOT_NOTE_FALL * 3u);
        u16 age;
        if (frame < start) continue;
        age = (u16)(frame - start);
        if (age < LCD_BOOT_NOTE_FALL) top[column] = LCD_BOOT_NOTE_HIGH;
        else if (age < LCD_BOOT_NOTE_FALL * 2u) top[column] = LCD_BOOT_NOTE_MID;
        else if (age < LCD_BOOT_NOTE_FALL * 3u) top[column] = LCD_BOOT_NOTE_LOW;
        if (age >= impact && age < impact + 4u) bottom[column] = LCD_BOOT_BLOCK;
        else if (age >= impact + 4u && age < impact + LCD_BOOT_NOTE_FALL) bottom[column] = LCD_BOOT_SPARKLE;
    }
    if (frame >= LCD_BOOT_CHASE) {
        const u8 lit = (u8)((frame - LCD_BOOT_CHASE + 1u) * 2u);
        for (index = 0; index != 16u; ++index)
            if (index < lit)
                bottom[index] = (u8)((index & 1u) ? LCD_BOOT_SPARKLE : LCD_BOOT_BLOCK);
    }
    lcd_boot_line(0x00u, top);
    lcd_boot_line(0x40u, bottom);
}

/* Blocks for LCD_BOOT_TICKS, then leaves the CGRAM/DDRAM to the caller. */
static void lcd_boot_play(void) {
    const u32 start = lcd_boot_cycle_count();
    u32 deadline = start + LCD_BOOT_TICKS_PER_FRAME;
    u16 frame = 0;
    lcd_boot_define_glyphs();
    lcd_boot_draw(frame);
    for (;;) {
        const u32 elapsed = lcd_boot_cycle_count() - start;
        if (elapsed >= LCD_BOOT_TICKS) return;
        while (elapsed >= deadline && frame < LCD_BOOT_FRAMES - 1u) {
            deadline += LCD_BOOT_TICKS_PER_FRAME;
            lcd_boot_draw(++frame);
        }
    }
}

#endif
