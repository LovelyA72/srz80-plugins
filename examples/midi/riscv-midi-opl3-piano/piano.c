/* SRZ80 RV32I MIDI-to-OPL3 piano.
 *
 * MIDI channels 1-9 and 11-16 share all 72 two-operator OPL3 voices across
 * four YMF262 chips.
 * Channel 10 (wire channel 9) is deliberately ignored. When all voices are
 * occupied, the oldest voice is stolen, except that the lowest sounding note
 * is protected so the harmony keeps its bass foundation.
 */

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
#include "../midi_receiver/channel.h"

/* This is a bare-metal program: there is no operating system or device
 * driver. The project maps each card directly into the RV32I address space.
 * `volatile` is essential for device registers because every C read or write
 * must become a real bus transaction; the compiler must not cache or remove it.
 *
 *   0x00000000  ROM containing this program and its constant tables
 *   0x00004000  RAM used for Gm (the stack grows down from 0x8000)
 *   0x10000000  YMF262 #1's four address/data ports
 *   0x10000100  YMF262 #2's four address/data ports
 *   0x10000200  YMF262 #3's four address/data ports
 *   0x10000300  YMF262 #4's four address/data ports
 *   0x10000400  MIDI DATA, STATUS, CONTROL, and INFO registers
 */
#define OPL_BASE ((volatile u8 *)0x10000000u)
#define OPL_COUNT 4u
#define OPL_STRIDE 0x100u
#define VOICES_PER_OPL 18u
#define MIDI_DATA (*(volatile u8 *)0x10000400u)
#define MIDI_STATUS (*(volatile u8 *)0x10000401u)
#define MIDI_CONTROL (*(volatile u8 *)0x10000402u)
#define STATE ((Gm *)0x4000u)
#define VOICE_COUNT (OPL_COUNT * VOICES_PER_OPL)
#define MIDI_RX_READY 0x01u
#define MIDI_RX_ENABLE 0x02u

/* Piano instrument
 *
 * Change these bytes to experiment. OPL registers are packed as follows:
 *   reg20: AM | vibrato | sustain | KSR | frequency multiplier
 *   reg40: key-scale level (KSL) | total level (TL, 0 loudest, 63 quietest)
 *   reg60: attack rate | decay rate
 *   reg80: sustain level | release rate
 *   regE0: waveform (0 = sine)
 *   regC0: stereo L/R | feedback | algorithm
 *
 * Screenshot values:
 *   Operator 1 (modulator): MUL=3, KSL=2, TL=25, A=15 D=3 S=15 R=7
 *   Operator 2 (carrier):   MUL=1, KSL=0, TL=0,  A=15 D=3 S=11 R=12
 *   Both sine, AM/vibrato/sustain/KSR off; feedback=7, algorithm=0.
 */
static const u8 piano_instrument[11] = {
    0x03, 0x99, 0xf3, 0xf7, 0x00, /* modulator: 20,40,60,80,E0 */
    0x01, 0x00, 0xf3, 0xbc, 0x00, /* carrier:   20,40,60,80,E0 */
    0x3e                              /* C0: stereo, feedback 7, algorithm 0 */
};

/* OPL channels are numbered 0-8 in each bank, but their two operators are
 * not contiguous in the register map. This table translates a local channel
 * number to its modulator offset; the carrier is always three positions later.
 */
static const u8 operator_offsets[9] = {0, 1, 2, 8, 9, 10, 16, 17, 18};

/* One octave of OPL F-numbers, starting at C. The block field in register B0
 * supplies the octave. Using a table avoids floating point and keeps this
 * firmware within the base RV32I instruction set (no multiply/divide unit).
 */
static const u16 f_numbers[12] = {
    0x157, 0x16b, 0x181, 0x198, 0x1b0, 0x1ca,
    0x1e5, 0x202, 0x220, 0x241, 0x263, 0x287
};

typedef struct {
    u32 age;
    u8 note, channel, active, held, latched, attenuation;
} GmVoice;
typedef struct {
    GmChannel channels[16];
    GmVoice voices[VOICE_COUNT];
    u32 age;
    u8 running, first, have_first, sysex_length, sysex[8];
    u8 enabled, transport, master, master_coarse;
    u16 master_fine;
} Gm;
#define GM_VOICES VOICE_COUNT
_Static_assert(sizeof(Gm) < 0x3000, "leave at least 4 KiB for the RV32I stack");

#include "../midi_receiver/math.h"
static void note_off(Gm *s, u8 channel, u8 note);
void gm_reset(Gm *s);

static void opl_write(u8 chip, u16 reg, u8 value) {
    /* Registers 000-0ff use ports 0/1; registers 100-1ff use ports 2/3.
     * First latch the low eight address bits, then write the register value.
     */
    volatile u8 *base = OPL_BASE + (u32)chip * OPL_STRIDE;
    u32 port = (reg & 0x100u) ? 2u : 0u;
    base[port] = (u8)reg;
    base[port + 1u] = value;
}

/* Each YMF262 has local voices 0-8 in register bank 0 and voices 9-17 in
 * bank 1. Keep the mapping explicit so RV32I does not need a division helper.
 */
static u8 voice_chip(u8 voice) {
    return voice >= 54u ? 3u : voice >= 36u ? 2u : voice >= 18u ? 1u : 0u;
}
static u8 local_voice(u8 voice) {
    return voice >= 54u ? (u8)(voice - 54u) : voice >= 36u ? (u8)(voice - 36u) :
           voice >= 18u ? (u8)(voice - 18u) : voice;
}
static u8 local_channel(u8 voice) {
    const u8 local = local_voice(voice);
    return local >= 9u ? (u8)(local - 9u) : local;
}
static u16 voice_bank(u8 voice) { return local_voice(voice) >= 9u ? 0x100u : 0u; }

static void release(Gm *s, u8 voice) {
    opl_write(voice_chip(voice), (u16)(voice_bank(voice) + 0xb0u + local_channel(voice)), 0);
    s->voices[voice].active = s->voices[voice].held = 0;
}
static void silence(Gm *s, u8 voice) {
    u16 base = voice_bank(voice);
    u8 modulator = operator_offsets[local_channel(voice)];
    // Disconnect stereo output so All Sound Off also kills release tails
    opl_write(voice_chip(voice), (u16)(base + 0x40u + modulator), 63);
    opl_write(voice_chip(voice), (u16)(base + 0x43u + modulator), 63);
    opl_write(voice_chip(voice), (u16)(base + 0xc0u + local_channel(voice)),
              (u8)(piano_instrument[10] & 0xcfu));
    release(s, voice);
    s->voices[voice].note = 255;
}
static void note_frequency(u8 note, u16 *f_number, u8 *block) {
    /* Convert MIDI's linear note number into semitone-within-octave and octave
     * using subtraction instead of `% 12`, which could require an RV32M divide
     * helper. The YMF262 accepts a ten-bit F-number plus a three-bit block.
     */
    u8 semitone = note;
    u8 octave = 0;
    u16 value;
    while (semitone >= 12u) {
        semitone -= 12u;
        octave++;
    }
    value = f_numbers[semitone];
    if (octave == 0) {
        value = (u16)((value + 1u) >> 1);
        *block = 0;
    } else {
        octave--;
        *block = octave > 7u ? 7u : octave;
    }
    *f_number = value;
}

static u8 allocate_voice(Gm *s) {
    u8 voice, lowest = 0, oldest = 255;
    for (voice = 0; voice < VOICE_COUNT; ++voice)
        if (!s->voices[voice].active) return voice;
    for (voice = 1; voice < VOICE_COUNT; ++voice)
        if (s->voices[voice].note < s->voices[lowest].note) lowest = voice;
    for (voice = 0; voice < VOICE_COUNT; ++voice)
        if (voice != lowest && (oldest == 255 ||
            s->age - s->voices[voice].age > s->age - s->voices[oldest].age)) oldest = voice;
    release(s, oldest);
    return oldest;
}

static void program_voice(u8 voice, u8 velocity) {
    u16 base = voice_bank(voice);
    u8 modulator = operator_offsets[local_channel(voice)];
    u8 carrier = (u8)(modulator + 3u);
    /* OPL total level is attenuation, the inverse of MIDI velocity. Preserve
     * the instrument's fixed modulator level (which shapes the timbre), while
     * changing only the carrier level (which mainly changes loudness).
     */
    u8 velocity_attenuation = (u8)((127u - velocity) >> 1);
    if (velocity_attenuation > 63u)
        velocity_attenuation = 63u;
    /* Program the modulator's five operator registers. */
    opl_write(voice_chip(voice), (u16)(base + 0x20u + modulator), piano_instrument[0]);
    opl_write(voice_chip(voice), (u16)(base + 0x40u + modulator), piano_instrument[1]);
    opl_write(voice_chip(voice), (u16)(base + 0x60u + modulator), piano_instrument[2]);
    opl_write(voice_chip(voice), (u16)(base + 0x80u + modulator), piano_instrument[3]);
    opl_write(voice_chip(voice), (u16)(base + 0xe0u + modulator), piano_instrument[4]);
    /* Program the carrier, substituting its TL with the velocity-derived one. */
    opl_write(voice_chip(voice), (u16)(base + 0x20u + carrier), piano_instrument[5]);
    opl_write(voice_chip(voice), (u16)(base + 0x40u + carrier), velocity_attenuation);
    opl_write(voice_chip(voice), (u16)(base + 0x60u + carrier), piano_instrument[7]);
    opl_write(voice_chip(voice), (u16)(base + 0x80u + carrier), piano_instrument[8]);
    opl_write(voice_chip(voice), (u16)(base + 0xe0u + carrier), piano_instrument[9]);
    /* C0 belongs to the channel rather than either individual operator. */
    opl_write(voice_chip(voice), (u16)(base + 0xc0u + local_channel(voice)), piano_instrument[10]);
}

// MIDI gain uses the existing velocity level and keeps CC 7's default unchanged
static const u8 gain_attenuation[128] = {
    63,63,63,63,63,63,63,63,63,61,59,57,55,53,51,49,
    48,47,45,44,43,42,41,40,39,38,37,36,35,34,33,33,
    32,31,31,30,29,29,28,27,27,26,26,25,25,24,24,23,
    23,22,22,21,21,20,20,19,19,19,18,18,17,17,17,16,
    16,16,15,15,14,14,14,13,13,13,13,12,12,12,11,11,
    11,10,10,10,10,9,9,9,8,8,8,8,7,7,7,7,
    6,6,6,6,6,5,5,5,5,4,4,4,4,4,3,3,
    3,3,3,2,2,2,2,2,1,1,1,1,1,0,0,0,
};
static void pitch(Gm *s, u8 voice) {
    GmVoice *v = &s->voices[voice];
    int cents = clamp((int)v->note * 100 + channel_pitch(&s->channels[v->channel]) +
                      scale((int)s->master_fine - 8192, 100, 8192) +
                      ((int)s->master_coarse - 64) * 100, 0, 12799);
    u8 note = (u8)divide((u32)cents, 100), semitone = note, block;
    u16 fnum, next;
    while (semitone >= 12) semitone -= 12;
    note_frequency(note, &fnum, &block);
    next = semitone == 11 ? (u16)(f_numbers[0] << 1) : f_numbers[semitone + 1];
    if (note < 12) next = (u16)((next + 1u) >> 1);
    fnum = (u16)(fnum + scale((int)next - fnum, (u32)(cents - note * 100), 100));
    opl_write(voice_chip(voice), (u16)(voice_bank(voice) + 0xa0u + local_channel(voice)), (u8)fnum);
    opl_write(voice_chip(voice), (u16)(voice_bank(voice) + 0xb0u + local_channel(voice)),
              (u8)((fnum >> 8) | (block << 2) | (v->active ? 0x20u : 0u)));
}
static void level(Gm *s, u8 voice) {
    GmVoice *v = &s->voices[voice];
    GmChannel *c = &s->channels[v->channel];
    u16 base = voice_bank(voice);
    u8 carrier = (u8)(operator_offsets[local_channel(voice)] + 3u);
    int amount = v->attenuation + (int)gain_attenuation[c->volume] - gain_attenuation[100] +
                 gain_attenuation[c->expression] + gain_attenuation[s->master] + (c->soft ? 8 : 0);
    u8 stereo = c->pan < 32 ? 0x10 : c->pan > 95 ? 0x20 : 0x30;
    if (!c->volume || !c->expression || !s->master) { amount = 63; stereo = 0; }
    opl_write(voice_chip(voice), (u16)(base + 0x40u + carrier), (u8)clamp(amount, 0, 63));
    opl_write(voice_chip(voice), (u16)(base + 0xc0u + local_channel(voice)),
              (u8)((piano_instrument[10] & 0xcfu) | (piano_instrument[10] & stereo)));
}
static void modulation(Gm *s, u8 voice) {
    GmChannel *c = &s->channels[s->voices[voice].channel];
    u16 base = voice_bank(voice);
    u8 modulator = operator_offsets[local_channel(voice)];
    u8 vibrato = c->modulation || c->pressure ? 0x40 : 0;
    opl_write(voice_chip(voice), (u16)(base + 0x20u + modulator), (u8)(piano_instrument[0] | vibrato));
    opl_write(voice_chip(voice), (u16)(base + 0x23u + modulator), (u8)(piano_instrument[5] | vibrato));
}
static void update(Gm *s, u8 channel, u8 what) {
    u8 i;
    for (i = 0; i < VOICE_COUNT; ++i) if (s->voices[i].note != 255 && s->voices[i].channel == channel) {
        if (what & 1u) pitch(s, i);
        if (what & 2u) level(s, i);
        if (what & 4u) modulation(s, i);
    }
}
static void note_on(Gm *s, u8 channel, u8 note, u8 velocity) {
    u8 voice;
    GmVoice *v;
    if (!velocity) { note_off(s, channel, note); return; }
    if (!s->enabled || channel == 9) return;
    voice = allocate_voice(s);
    v = &s->voices[voice];
    v->channel = channel; v->note = note; v->active = v->held = 1; v->latched = 0;
    v->age = ++s->age; v->attenuation = (u8)((127u - velocity) >> 1);
    program_voice(voice, velocity);
    level(s, voice); modulation(s, voice); pitch(s, voice);
}

#define GM_PROGRAM_CHANGE 0
#define GM_PERCUSSION_ONESHOT 0
#include "../midi_receiver/receiver.h"

__attribute__((noreturn, noinline, used)) void firmware_main(void) {
    u8 voice;
    gm_reset(STATE);
    /* Global YMF262 setup. The second register bank is usable only after NEW
     * mode is enabled. Keeping four-op and rhythm modes off yields 18 uniform
     * two-operator voices per chip, which makes allocation straightforward.
     */
    for (voice = 0; voice < OPL_COUNT; ++voice) {
        opl_write(voice, 0x105, 0x01); /* OPL3 mode: expose the second bank. */
        opl_write(voice, 0x104, 0x00); /* Disable four-operator pairing. */
        opl_write(voice, 0x0bd, 0x00); /* Disable rhythm mode. */
    }
    /* Enable MIDI receive, then poll STATUS bit 0. Reading DATA pops exactly
     * one byte from the card FIFO. At 300 kHz this loop is comfortably
     * faster than a normal MIDI stream and needs no CPU interrupt support.
     */
    MIDI_CONTROL = MIDI_RX_ENABLE;
    for (;;)
        if (MIDI_STATUS & MIDI_RX_READY)
            gm_byte(STATE, MIDI_DATA);
}

__attribute__((naked, section(".text.start"), noreturn)) void _start(void) {
    /* A freestanding C program has no C runtime to initialize the stack or call
     * main(). This tiny RV32I entry point places SP at the top of the 16 KiB RAM
     * card and tail-jumps to C. `naked` prevents a compiler-generated prologue
     * from touching the uninitialized stack first.
     */
    __asm__ volatile("li sp, 0x8000\n"
                     "tail firmware_main\n");
}
