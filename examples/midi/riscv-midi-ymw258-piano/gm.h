#ifndef TG100_GM_H
#define TG100_GM_H
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

#define GM_VOICES 28u
typedef struct {
    u8 program, volume, expression, pan, modulation, pressure, sustain;
    u8 rpn_msb, rpn_lsb, bend_semitones, bend_cents, coarse;
    u16 bend, fine;
} GmChannel;
typedef struct {
    u32 age;
    int pitch_cents;
    u16 sample;
    u8 note, channel, active, held, secondary, attenuation, pan, lfo;
} GmVoice;
typedef struct {
    GmChannel channels[16];
    GmVoice voices[GM_VOICES];
    u32 age;
    u8 running, first, have_first, sysex_length, sysex[8];
    u8 enabled, transport, master;
} Gm;

u8 gm_rom(u32 address);
u8 gm_sample_rom(u32 address);
void gm_write(u8 voice, u8 reg, u8 value);
void gm_note_trigger(u8 channel, u8 velocity);
void gm_reset(Gm *gm);
void gm_byte(Gm *gm, u8 byte);
#endif
