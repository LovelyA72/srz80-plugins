#ifndef MU50_GM_H
#define MU50_GM_H
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

#define GM_VOICES 32u
typedef struct {
    u8 program, bank_msb, bank_lsb, drum, volume, expression, pan, modulation, pressure, sustain;
    u8 reverb, chorus;
    u8 rpn_msb, rpn_lsb, bend_semitones, bend_cents, coarse;
    u16 bend, fine;
} GmChannel;
typedef struct {
    u32 age;
    int pitch_cents;
    u32 sample;
    u8 note, channel, active, held, secondary, attenuation, pan, lfo;
    u8 reverb, chorus;
    u8 release_rate, decay2_rate, decay2_level, stage, choke, note_off, format;
} GmVoice;
typedef struct {
    GmChannel channels[16];
    GmVoice voices[GM_VOICES];
    u32 age;
    u8 running, first, have_first, sysex_length, sysex[9];
    u8 enabled, transport, master, poll_voice, xg_mode;
} Gm;

u8 gm_rom(u32 address);
void gm_global(u16 address, u8 value);
u8 gm_status(u8 voice);
void gm_poll(Gm *gm);
void gm_write(u8 voice, u8 reg, u8 value);
void gm_note_trigger(u8 channel, u8 velocity);
void gm_mode_changed(void);
void gm_program_changed(u8 channel);
void gm_reset(Gm *gm);
void gm_byte(Gm *gm, u8 byte);
void gm_control(Gm *gm, u8 channel, u8 controller, u8 value);
void gm_voice_name(const Gm *gm, u8 channel, char name[9]);
#endif
