#ifndef TG100_LCD_DISPLAY_H
#define TG100_LCD_DISPLAY_H

/* LCD_COMMAND and LCD_DATA are supplied by each board wrapper. */
static const char *const gm_program_names[128] = {
    "Acoustic Grand", "Bright Acoustic", "Electric Grand", "Honky-tonk", "Electric Piano 1", "Electric Piano 2", "Harpsichord", "Clavinet",
    "Celesta", "Glockenspiel", "Music Box", "Vibraphone", "Marimba", "Xylophone", "Tubular Bells", "Dulcimer",
    "Drawbar Organ", "Percussive Organ", "Rock Organ", "Church Organ", "Reed Organ", "Accordion", "Harmonica", "Tango Accordion",
    "Nylon Guitar", "Steel Guitar", "Jazz Guitar", "Clean Guitar", "Muted Guitar", "Overdrive Guitar", "Distortion Guitar", "Guitar Harmonics",
    "Acoustic Bass", "Finger Bass", "Pick Bass", "Fretless Bass", "Slap Bass 1", "Slap Bass 2", "Synth Bass 1", "Synth Bass 2",
    "Violin", "Viola", "Cello", "Contrabass", "Tremolo Strings", "Pizzicato", "Orchestral Harp", "Timpani",
    "String Ensemble 1", "String Ensemble 2", "Synth Strings 1", "Synth Strings 2", "Choir Aahs", "Voice Oohs", "Synth Voice", "Orchestra Hit",
    "Trumpet", "Trombone", "Tuba", "Muted Trumpet", "French Horn", "Brass Section", "Synth Brass 1", "Synth Brass 2",
    "Soprano Sax", "Alto Sax", "Tenor Sax", "Baritone Sax", "Oboe", "English Horn", "Bassoon", "Clarinet",
    "Piccolo", "Flute", "Recorder", "Pan Flute", "Blown Bottle", "Shakuhachi", "Whistle", "Ocarina",
    "Lead Square", "Lead Sawtooth", "Lead Calliope", "Lead Chiff", "Lead Charang", "Lead Voice", "Lead Fifths", "Lead Bass",
    "Pad New Age", "Pad Warm", "Pad Polysynth", "Pad Choir", "Pad Bowed", "Pad Metallic", "Pad Halo", "Pad Sweep",
    "FX Rain", "FX Soundtrack", "FX Crystal", "FX Atmosphere", "FX Brightness", "FX Goblins", "FX Echoes", "FX Sci-fi",
    "Sitar", "Banjo", "Shamisen", "Koto", "Kalimba", "Bagpipe", "Fiddle", "Shanai",
    "Tinkle Bell", "Agogo", "Steel Drums", "Woodblock", "Taiko Drum", "Melodic Tom", "Synth Drum", "Reverse Cymbal",
    "Fret Noise", "Breath Noise", "Seashore", "Bird Tweet", "Telephone Ring", "Helicopter", "Applause", "Gunshot"
};

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
    u8 channel;
    lcd_line(0x00, gm_program_names[gm->channels[0].program & 127]);
    LCD_COMMAND = 0xc0;
    for (channel = 0; channel != 16; ++channel) {
        /* CGRAM character 0 is the meter's custom idle baseline. */
        LCD_DATA = meter[channel] ? (u8)(meter[channel] - 1u) : 0;
    }
}

#endif
