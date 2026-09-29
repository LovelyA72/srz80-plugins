#ifndef MU50_LCD_DISPLAY_H
#define MU50_LCD_DISPLAY_H

/* LCD_COMMAND and LCD_DATA are supplied by each board wrapper. */
static void lcd_line(u8 address, const char *text) {
    u8 column = 0;
    LCD_COMMAND = (u8)(0x80u | address);
    while (column != 16) {
        u8 character = text && *text ? (u8)*text++ : (u8)' ';
        LCD_DATA = character;
        ++column;
    }
}

static void lcd_define_bars(void) {
    u8 glyph, row;
    for (glyph = 0; glyph != 8; ++glyph) {
        LCD_COMMAND = (u8)(0x40u + glyph * 8u);
        for (row = 0; row != 8; ++row)
            LCD_DATA = row >= 7u - glyph ? 0x1fu : 0;
    }
}

static void lcd_refresh(const Gm *gm, const u8 *meter) {
    u8 channel, column;
    char name[9];
    gm_voice_name(gm, 0, name);
    LCD_COMMAND = 0x80;
    for (column = 0; column != 14; ++column)
        LCD_DATA = column < 8 ? (u8)name[column] : (u8)' ';
    LCD_DATA = gm->xg_mode ? (u8)'X' : (u8)'G';
    LCD_DATA = gm->xg_mode ? (u8)'G' : (u8)'M';
    LCD_COMMAND = 0xc0;
    for (channel = 0; channel != 16; ++channel) {
        /* CGRAM character 0 is the meter's custom idle baseline. */
        LCD_DATA = meter[channel] ? (u8)(meter[channel] - 1u) : 0;
    }
}

#endif
