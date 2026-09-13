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

/* This is a bare-metal program: there is no operating system or device
 * driver. The project maps each card directly into the RV32I address space.
 * `volatile` is essential for device registers because every C read or write
 * must become a real bus transaction; the compiler must not cache or remove it.
 *
 *   0x00000000  ROM containing this program and its constant tables
 *   0x00004000  RAM used for SynthState (the stack grows down from 0x8000)
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
#define STATE ((volatile SynthState *)0x4000u)
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
    /* A note value of 0xff marks a free OPL voice. MIDI channel and sustain
     * state let a later Note Off find the correct voice even though MIDI
     * channels and OPL channels are allocated independently.
     */
    u8 note[VOICE_COUNT];
    u8 channel[VOICE_COUNT];
    u8 sustained[VOICE_COUNT];
    u8 sustain[16];
    u32 age[VOICE_COUNT];
    u32 next_age;

    /* MIDI running status permits a stream to omit repeated status bytes.
     * The parser therefore remembers both the last channel status and the
     * first data byte of the message currently being assembled.
     */
    u8 running_status;
    u8 first_data;
    u8 data_count;
} SynthState;

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

static void key_off(u8 voice) {
    /* B0 bit 5 is KEY-ON. Writing zero releases the operators, then 0xff makes
     * the software voice immediately available for another MIDI note.
     */
    u16 base = voice_bank(voice);
    opl_write(voice_chip(voice), (u16)(base + 0xb0u + local_channel(voice)), 0);
    STATE->note[voice] = 0xffu;
    STATE->sustained[voice] = 0;
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

static u8 allocate_voice(void) {
    u8 voice;
    u8 lowest = 0;
    u8 oldest = 0xffu;
    u32 oldest_age = 0xffffffffu;
    /* Prefer a genuinely free voice; this avoids cutting off a sounding note. */
    for (voice = 0; voice < VOICE_COUNT; ++voice)
        if (STATE->note[voice] == 0xffu)
            return voice;
    /* All voices are busy. First identify the lowest pitch to protect. If
     * several voices share it, protecting one is enough to retain the bass.
     */
    for (voice = 1; voice < VOICE_COUNT; ++voice)
        if (STATE->note[voice] < STATE->note[lowest])
            lowest = voice;
    /* Among every other voice, the smallest monotonically assigned age is the
     * earliest note. Release that voice before reusing its OPL channel.
     */
    for (voice = 0; voice < VOICE_COUNT; ++voice) {
        if (voice != lowest && STATE->age[voice] < oldest_age) {
            oldest = voice;
            oldest_age = STATE->age[voice];
        }
    }
    key_off(oldest);
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

static void note_on(u8 channel, u8 note, u8 velocity) {
    u8 voice;
    u16 f_number;
    u8 block;
    if (channel == 9u || note >= 128u || velocity == 0u)
        return;
    /* MIDI channels select message ownership, not fixed OPL voices. A fresh
     * note obtains whichever hardware voice is free (or selected for theft).
     */
    voice = allocate_voice();
    program_voice(voice, velocity);
    note_frequency(note, &f_number, &block);
    STATE->channel[voice] = channel;
    STATE->note[voice] = note;
    STATE->sustained[voice] = 0;
    STATE->age[voice] = ++STATE->next_age;
    /* A0 holds FNUM bits 7:0. B0 holds FNUM bits 9:8, block in bits 4:2,
     * and KEY-ON in bit 5. Writing B0 last starts the note at the new pitch.
     */
    opl_write(voice_chip(voice), (u16)(voice_bank(voice) + 0xa0u + local_channel(voice)), (u8)f_number);
    opl_write(voice_chip(voice), (u16)(voice_bank(voice) + 0xb0u + local_channel(voice)),
              (u8)((f_number >> 8) | (block << 2) | 0x20u));
}

static void note_off(u8 channel, u8 note) {
    u8 voice;
    if (channel == 9u)
        return;
    for (voice = 0; voice < VOICE_COUNT; ++voice) {
        if (STATE->note[voice] == note && STATE->channel[voice] == channel) {
            /* With the pedal down, remember the release but leave KEY-ON set.
             * Releasing CC 64 later turns off every such deferred voice.
             */
            if (STATE->sustain[channel])
                STATE->sustained[voice] = 1;
            else
                key_off(voice);
        }
    }
}

static void release_channel(u8 channel, u8 sustained_only) {
    u8 voice;
    for (voice = 0; voice < VOICE_COUNT; ++voice)
        if (STATE->note[voice] != 0xffu && STATE->channel[voice] == channel &&
            (!sustained_only || STATE->sustained[voice]))
            key_off(voice);
}

static void control_change(u8 channel, u8 controller, u8 value) {
    if (channel == 9u)
        return;
    /* This small example intentionally implements only controllers that affect
     * note lifetime. Program, modulation, volume, and pan messages are parsed
     * correctly but do not replace the single learner-editable piano patch.
     */
    if (controller == 64u) {
        u8 was_on = STATE->sustain[channel];
        STATE->sustain[channel] = value >= 64u;
        if (was_on && !STATE->sustain[channel])
            release_channel(channel, 1);
    } else if (controller == 120u || controller == 123u) {
        release_channel(channel, 0);
    }
}

static void dispatch(u8 status, u8 first, u8 second) {
    /* MIDI displays channels as 1-16, while the low status nibble is 0-15.
     * Consequently nibble 9 is displayed channel 10, the ignored drum channel.
     */
    u8 kind = status & 0xf0u;
    u8 channel = status & 0x0fu;
    if (channel == 9u)
        return;
    if (kind == 0x80u || (kind == 0x90u && second == 0u))
        note_off(channel, first);
    else if (kind == 0x90u)
        note_on(channel, first, second);
    else if (kind == 0xb0u)
        control_change(channel, first, second);
}

static void midi_byte(u8 value) {
    u8 kind;
    /* Real-time messages may appear between any two bytes and do not disturb
     * running status. This piano has no clock/transport behavior, so skip them.
     */
    if (value >= 0xf8u)
        return;
    /* Bit 7 distinguishes a status byte from a data byte. System messages
     * (f0-f7) cancel running status; their following data is ignored until the
     * next channel status. Channel Voice messages retain their status.
     */
    if (value & 0x80u) {
        STATE->data_count = 0;
        STATE->running_status = value < 0xf0u ? value : 0;
        return;
    }
    if (!STATE->running_status)
        return;
    kind = STATE->running_status & 0xf0u;
    /* Program Change and Channel Pressure have one data byte. The other
     * Channel Voice messages have two, so collect the first before dispatch.
     */
    if (STATE->data_count == 0) {
        STATE->first_data = value;
        if (kind == 0xc0u || kind == 0xd0u)
            dispatch(STATE->running_status, value, 0);
        else
            STATE->data_count = 1;
    } else {
        dispatch(STATE->running_status, STATE->first_data, value);
        STATE->data_count = 0;
    }
}

__attribute__((noreturn, noinline, used)) void firmware_main(void) {
    u8 voice;
    /* RAM has just been cold-reset, but initialize every field explicitly so
     * the sentinel values and hardware state are obvious and deterministic.
     */
    for (voice = 0; voice < VOICE_COUNT; ++voice) {
        STATE->note[voice] = 0xffu;
        STATE->age[voice] = 0;
        opl_write(voice_chip(voice), (u16)(voice_bank(voice) + 0xb0u + local_channel(voice)), 0);
        opl_write(voice_chip(voice), (u16)(voice_bank(voice) + 0xc0u + local_channel(voice)), 0x30);
    }
    for (voice = 0; voice < 16u; ++voice)
        STATE->sustain[voice] = 0;
    STATE->next_age = 0;
    STATE->running_status = 0;
    STATE->data_count = 0;
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
     * one byte from the card FIFO. At 100 kHz this simple loop is comfortably
     * faster than a normal MIDI stream and needs no CPU interrupt support.
     */
    MIDI_CONTROL = MIDI_RX_ENABLE;
    for (;;)
        if (MIDI_STATUS & MIDI_RX_READY)
            midi_byte(MIDI_DATA);
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
