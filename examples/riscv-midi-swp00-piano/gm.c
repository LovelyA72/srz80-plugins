#include "gm.h"

/* MU50 v1.05 table locations in CPU byte order. Layout documented by
 * Theo Niessink's MUTable (mu50.cpp); see README for provenance. */
#define PROGRAMS 425734u
#define BANK_LISTS 443910u
#define DRUM_BANKS 141914u
#define INSTRUMENTS 323584u
#define SAMPLE_SETS 479030u
#define SAMPLES 460774u
#define DRUM_KITS 454886u
#define DRUMS 444296u

static int signed_byte(u32 address) {
    u8 b = gm_rom(address);
    return b < 128 ? b : (int)b - 256;
}
static void word(u8 voice, u8 reg, u16 value) {
    gm_write(voice, reg, (u8)(value >> 8));
    gm_write(voice, (u8)(reg + 1), (u8)value);
}
static u16 be16(u32 address) {
    return (u16)((u16)gm_rom(address) << 8 | gm_rom(address + 1u));
}
/* MU50 bank lists translate XG's sparse bank numbers to the packed ROM
 * program banks.  SFX voices use bank MSB 64; normal voices use LSB. */
static u8 program_bank(const GmChannel *c) {
    if (c->bank_msb == 64) return gm_rom(BANK_LISTS + 256u + c->bank_msb);
    return gm_rom(BANK_LISTS + 128u + c->bank_lsb);
}
static u32 instrument_address(const GmChannel *c) {
    u32 entry = PROGRAMS + (u32)program_bank(c) * 256u + (u32)c->program * 2u;
    return INSTRUMENTS + (u32)be16(entry) * 2u;
}
static u32 drum_map(const GmChannel *c) {
    u32 list = DRUM_BANKS + (c->bank_msb == 126 ? 256u : 128u);
    return DRUM_KITS + (u32)gm_rom(list + c->program) * 256u;
}
void gm_voice_name(const Gm *s, u8 channel, char name[9]) {
    const GmChannel *c = &s->channels[channel & 15u];
    u8 i;
    if (c->drum) {
        const char *text = c->bank_msb == 126 ? "SFX Kit " : "Drum Kit";
        for (i = 0; i < 8; ++i) name[i] = text[i];
    } else {
        u32 instrument = instrument_address(c);
        for (i = 0; i < 8; ++i) name[i] = (char)gm_rom(instrument + 2u + i);
    }
    name[8] = 0;
}
/* Preserve RV32I compatibility while allowing the RV32IM image to use M. */
static u32 mul(u32 a, u32 b) {
#if defined(__riscv_mul)
    return a * b;
#else
    u32 value = 0;
    while (b) { if (b & 1u) value += a; a <<= 1; b >>= 1; }
    return value;
#endif
}
static u32 divide(u32 value, u32 divisor) {
#if defined(__riscv_div)
    return value / divisor;
#else
    u32 bit = 1, result = 0;
    if (divisor == 1) return value;
    if (divisor == 8192) return value >> 13;
    while (divisor <= (value >> 1)) { divisor <<= 1; bit <<= 1; }
    while (bit) {
        if (value >= divisor) { value -= divisor; result |= bit; }
        divisor >>= 1; bit >>= 1;
    }
    return result;
#endif
}
static int scale(int value, u32 factor, u32 divisor) {
    u32 magnitude = value < 0 ? (u32)-value : (u32)value;
    int result = (int)divide(mul(magnitude, factor), divisor);
    return value < 0 ? -result : result;
}
static int clamp(int value, int low, int high) {
    return value < low ? low : value > high ? high : value;
}

/* SWP00 global attenuation: 16 steps halve amplitude. The controller ROM
 * contains Yamaha's squared MIDI gain curve in half-resolution units. */
static u8 attenuation(u8 value) { return value ? gm_rom(0xc67u + value) : 127; }
static u8 send_attenuation(u8 preset, u8 controller) {
    int amount;
    if (!preset || !controller) return 255;
    amount = 2 * (attenuation(preset) + attenuation(controller));
    return (u8)clamp(amount, 0, 255);
}
static void release(Gm *s, u8 i) {
    gm_write(i, 0x29, 255);
    gm_write(i, 0x28, (u8)(0x80u | s->voices[i].release_rate));
    s->voices[i].active = s->voices[i].held = 0;
    s->voices[i].stage = 0;
}
static void silence(Gm *s, u8 i) {
    gm_write(i, 0x2b, 255); /* Dry send is immediate, unlike interpolated gain. */
    gm_write(i, 0x29, 255);
    gm_write(i, 0x28, 0xff);
    s->voices[i].active = s->voices[i].held = s->voices[i].stage = 0;
    s->voices[i].note = 255;
}
/* Advance the second decay after the chip reaches the first decay target.
 * Poll one voice per main-loop iteration to keep MIDI input responsive. */
void gm_poll(Gm *s) {
    u8 i = s->poll_voice;
    GmVoice *v = &s->voices[i];
    s->poll_voice = (u8)((i + 1u) & 31u);
    if (v->stage && (gm_status(i) & 0xc0u) == 0xc0u) {
        gm_write(i, 0x29, v->decay2_level);
        gm_write(i, 0x28, (u8)(0x80u | v->decay2_rate));
        v->stage = 0;
    }
}
static void all_sound_off(Gm *s, u8 channel) {
    u8 i;
    for (i = 0; i < GM_VOICES; ++i)
        if (s->voices[i].note != 255 && s->voices[i].channel == channel) silence(s, i);
}
static void stop(Gm *s) {
    u8 i;
    for (i = 0; i < GM_VOICES; ++i) silence(s, i);
    for (i = 0; i < 16; ++i) s->channels[i].sustain = 0;
    s->transport = 0;
}
static void controllers(GmChannel *c) {
    c->expression = 127; c->modulation = c->pressure = c->sustain = 0;
    c->reverb = 40; c->chorus = 0;
    c->bend = 8192; c->rpn_msb = c->rpn_lsb = 127;
}
static void meg_word(u16 address, u16 value) {
    gm_global(address, (u8)(value >> 8));
    gm_global((u16)(address + 1u), (u8)value);
}
static void meg_table(u32 record, u32 registers, u8 constants, u8 delays) {
    u8 i;
    for (i = 0; i < constants; ++i)
        meg_word((u16)(0x200u + (u16)gm_rom(registers + i) * 2u),
                 be16(record + (u32)i * 2u));
    record += (u32)constants * 2u;
    registers += constants;
    for (i = 0; i < delays; ++i)
        meg_word((u16)(0x180u + (u16)gm_rom(registers + i) * 2u),
                 be16(record + (u32)i * 2u));
}
static void effects(void) {
    /* MU50 v1.05 routine 0x03cfd4 loads Hall 1 from algorithm record
     * 0x01f93e through coefficient map 0x021a8e and delay map 0x021abb.
     * It loads Chorus 1 from 0x01fb3a through maps 0x021ad6/0x021af2.
     * The records originate at 0x01f8ae (0x90 bytes each) and 0x01faee
     * (0x4c bytes each); type maps 0x03af04/0x03af11 select record one.
     * Keep the dry coefficients at 0x202/0x37c untouched.  The remaining
     * values are the fixed input and return gains used by this receiver;
     * cross-effect inputs stay zero, so chorus-to-reverb is disabled. */
    meg_table(0x1f93eu, 0x21a8eu, 45, 27);
    meg_table(0x1fb3au, 0x21ad6u, 28, 10);
    meg_word(0x222, 0x0080); /* Reverb input, coefficient 0x11: unity. */
    meg_word(0x216, 0x0100); /* Reverb return, coefficient 0x0b: 0.5. */
    meg_word(0x218, 0x0000); /* Reverb return smoothing coefficient. */
    meg_word(0x204, 0x0080); /* Chorus left input, coefficient 0x02: unity. */
    meg_word(0x20a, 0x0080); /* Chorus right input, coefficient 0x05: unity. */
    meg_word(0x2ae, 0x0100); /* Chorus return, coefficient 0x57: 0.5. */
    meg_word(0x2b0, 0x0000); /* Chorus return smoothing coefficient. */
    meg_word(0x224, 0x0000); /* No chorus-left to reverb routing. */
    meg_word(0x226, 0x0000); /* No chorus-right to reverb routing. */
}
__attribute__((weak)) void gm_mode_changed(void) {}
static void reset(Gm *s, u8 xg_mode) {
    u8 i;
    stop(s);
    for (i = 0; i < 16; ++i) {
        GmChannel *c = &s->channels[i];
        controllers(c);
        c->program = 0; c->bank_msb = c->bank_lsb = 0; c->drum = 0;
        c->volume = 100; c->pan = 64;
        c->bend_semitones = 2; c->bend_cents = 0; c->fine = 8192; c->coarse = 64;
    }
    s->channels[9].bank_msb = 127; s->channels[9].drum = 1;
    s->age = 0; s->running = s->have_first = s->sysex_length = 0;
    s->enabled = 1; s->master = 127; s->poll_voice = 0; s->xg_mode = xg_mode;
    /* MEG dry output gains: 204 / 128 followed by the core's /4 output
     * scaling gives 204 / 512, approximately -8 dB on both channels. */
    gm_global(0x202, 0); gm_global(0x203, 204);
    gm_global(0x37c, 0); gm_global(0x37d, 204);
    effects();
    gm_mode_changed();
}
void gm_reset(Gm *s) { reset(s, 0); }
static int channel_pitch(const GmChannel *c) {
    return scale((int)c->bend - 8192, c->bend_semitones * 100u + c->bend_cents, 8192) +
           scale((int)c->fine - 8192, 100, 8192) + ((int)c->coarse - 64) * 100;
}
static void pitch(Gm *s, u8 i) {
    GmVoice *v = &s->voices[i];
    int cents = clamp(v->pitch_cents + channel_pitch(&s->channels[v->channel]), -9600, 9599);
    int octave = 1;
    u16 increment;
    while (cents >= 1200) { cents -= 1200; ++octave; }
    while (cents < 0) { cents += 1200; --octave; }
    /* Yamaha's descending cent table uses [1024,2048] mantissas. */
    increment = be16(0x100u + (1200u-(u32)cents) * 2u);
    /* The original MU50 limits compressed samples to octave +3. */
    if ((v->format & 0xc0u) == 0xc0u && octave > 3) octave = 3;
    if (octave > 7) octave = 7;
    word(i, 0x34, (u16)(((u32)octave & 15u) << 12 | increment));
}
static void level(Gm *s, u8 i) {
    GmVoice *v = &s->voices[i];
    GmChannel *c = &s->channels[v->channel];
    int amount = v->attenuation + 2 * (attenuation(c->volume) + attenuation(c->expression) + attenuation(s->master));
    int pan = clamp((int)c->pan + (int)v->pan - 64, 0, 127);
    u8 left = pan <= 64 ? 0 : (u8)clamp(scale(pan - 64, 15, 63), 0, 15);
    u8 right = pan >= 64 ? 0 : (u8)clamp(scale(64 - pan, 15, 64), 0, 15);
    gm_write(i, 0x2e, (u8)clamp(amount, 0, 255));
    gm_write(i, 0x2f, (u8)((left << 4) | right));
    gm_write(i, 0x2b, (!c->volume || !c->expression || !s->master) ? 255 : 0);
    gm_write(i, 0x2a, send_attenuation(v->reverb, c->reverb));
    gm_write(i, 0x2c, send_attenuation(v->chorus, c->chorus));
    gm_write(i, 0x2d, 255);
}
static void modulation(Gm *s, u8 i) {
    GmVoice *v = &s->voices[i];
    GmChannel *c = &s->channels[v->channel];
    u8 amount = c->modulation > c->pressure ? c->modulation : c->pressure;
    gm_write(i, 0x25, (u8)clamp(v->lfo + (amount >> 2), 0, 127));
}
static void update(Gm *s, u8 channel, u8 what) {
    u8 i;
    for (i = 0; i < GM_VOICES; ++i) if (s->voices[i].note != 255 && s->voices[i].channel == channel) {
        if (what & 1u) pitch(s, i);
        if (what & 2u) level(s, i);
        if (what & 4u) modulation(s, i);
    }
}
static void pedal_up(Gm *s, u8 channel) {
    u8 i;
    for (i = 0; i < GM_VOICES; ++i)
        if (s->voices[i].active && !s->voices[i].held && s->voices[i].channel == channel) release(s, i);
}
static void notes_off(Gm *s, u8 channel) {
    u8 i;
    for (i = 0; i < GM_VOICES; ++i) if (s->voices[i].note != 255 && s->voices[i].channel == channel) {
        s->voices[i].held = 0;
        if (!s->channels[channel].sustain && s->voices[i].active) release(s, i);
    }
}
static void note_off(Gm *s, u8 channel, u8 note) {
    u8 i, oldest = 255;
    for (i = 0; i < GM_VOICES; ++i) {
        GmVoice *v = &s->voices[i];
        if (v->active && v->held && v->channel == channel && v->note == note &&
            (oldest == 255 || s->age - v->age > s->age - s->voices[oldest].age)) oldest = i;
    }
    if (s->channels[channel].drum && oldest != 255 && !s->voices[oldest].note_off) {
        /* Percussion is one-shot: keep the hardware sounding, but free the
         * software slot once its MIDI note has ended. */
        if (oldest != 255) {
            s->voices[oldest].active = s->voices[oldest].held = 0;
        }
        return;
    }
    if (oldest != 255) {
        u32 age = s->voices[oldest].age;
        for (i = 0; i < GM_VOICES; ++i) if (s->voices[i].active && s->voices[i].age == age) {
            s->voices[i].held = 0;
            if (!s->channels[channel].sustain) release(s, i);
        }
    }
}
/* True when candidate was activated before current; current 255 means never. */
static u8 older(Gm *s, u8 candidate, u8 current) {
    return current == 255 || s->age - s->voices[candidate].age > s->age - s->voices[current].age;
}
static u8 allocate(Gm *s, u8 secondary) {
    u8 i, releasing = 255, layered = 255, oldest = 255;
    for (i = 0; i < GM_VOICES; ++i) {
        GmVoice *v = &s->voices[i];
        if (v->note == 255) return i; /* Never used or already silenced. */
        /* Every reusable slot is taken oldest-first, so a run of short notes
         * keeps rotating through the whole chip instead of parking every
         * release on the lowest-numbered voice while the rest age out. */
        if (!v->active && older(s, i, releasing)) releasing = i;
        if (v->secondary && older(s, i, layered)) layered = i;
        if (older(s, i, oldest)) oldest = i;
    }
    if (releasing != 255) { silence(s, releasing); return releasing; }
    if (secondary) return 255;
    /* Preserve independent notes on the 32-slot chip: optional
     * second layers are reclaimed before any held primary voice. */
    if (layered != 255) { silence(s, layered); return layered; }
    silence(s, oldest); /* All 32 are held: kill the one activated first. */
    return oldest;
}
static u32 region(u32 element, int note) {
    u16 wave = (u16)((u16)gm_rom(element) << 7 | gm_rom(element + 1));
    u32 first, end, address;
    if (wave >= 246) return 0;
    first = be16(SAMPLE_SETS + wave * 2u);
    end = be16(SAMPLE_SETS + (wave + 1u) * 2u);
    if (first >= end || end > 1141u * 16u) return 0;
    for (address = SAMPLES + first; address < SAMPLES + end; address += 16u)
        if (note <= (gm_rom(address + 15) & 127)) return address;
    return 0;
}
/* Approximate the MU50's multi-stage envelope with SWP00's attack + two
 * polled decay stages. Rate scaling and levels come from the actual preset. */
static u8 rate(int value) { return (u8)(2 * clamp(value, 1, 63)); }
/* Drum decay-1 is already a seven-bit SWP00-domain value. Doubling it makes
 * snares and toms reach the first target almost instantly, leaving a click.
 * The second stage retains the compressed six-bit approximation used to make
 * long looping percussion terminate on the SWP00's two-stage envelope. */
static u8 drum_decay1_rate(u32 drum) {
    u8 value = gm_rom(drum+14);
    return value < 40 ? 40 : value;
}
static u8 envelope_level(u8 value) { return value ? (u8)(127 - value) : 255; }
static int key_scale(u32 points, u32 offsets, int note) {
    u8 j;
    int low = gm_rom(points), y = (int)gm_rom(offsets) - 64;
    if (note <= low) return y;
    for (j = 1; j < 4; ++j) {
        int high = gm_rom(points+j), next = (int)gm_rom(offsets+j) - 64;
        if (note < high && high > low) return y + scale(next-y, (u32)(note-low), (u32)(high-low));
        low = high; y = next;
    }
    return y;
}
static void start(Gm *s, u8 channel, u8 note, u8 velocity, u8 secondary,
                  u32 sample, u32 element, u32 drum, int cents, int base_level, u8 pan,
                  u8 reverb, u8 chorus) {
    u8 i = allocate(s, secondary), j;
    GmVoice *v;
    u32 header = drum ? drum + 19u : sample + 4u;
    int scaling = 0;
    if (element) {
        int depth = (int)gm_rom(element+68)-64;
        scaling = scale((int)note-gm_rom(element+69), (u32)(depth < 0 ? -depth : depth), 8);
        if (depth < 0) scaling = -scaling;
    }
    u16 attack = be16(header), loop = be16(header+3);
    u32 address = ((u32)(gm_rom(header+5) & 63u) << 16) | be16(header+6);
    if (i == 255) return;
    v = &s->voices[i];
    v->age = s->age; v->note = note; v->channel = channel; v->sample = sample;
    v->active = v->held = 1; v->secondary = secondary; v->pitch_cents = cents;
    v->attenuation = (u8)clamp(base_level + 2 * attenuation(velocity), 0, 255);
    v->pan = pan; v->format = gm_rom(header+8);
    v->reverb = reverb; v->chorus = chorus;
    v->lfo = element ? gm_rom(element+12) : 0;
    v->choke = drum ? gm_rom(drum+3) : 0;
    v->note_off = drum ? gm_rom(drum+9) : 1;
    v->release_rate = element ? rate(gm_rom(element+74) + scaling) : 64;
    v->decay2_rate = element ? rate(gm_rom(element+73) + scaling) : rate(gm_rom(drum+15));
    v->decay2_level = element ? envelope_level(gm_rom(element+76)) : 255;
    v->stage = loop != 0;
    /* Reset every per-voice register: stolen voices must not retain settings. */
    for (j = 0x20; j <= 0x37; ++j) gm_write(i, j, 0);
    word(i, 8, 0);
    if (element) {
        u16 skip = (u16)((u16)gm_rom(element+77) << 7 | gm_rom(element+78));
        attack = attack > skip ? (u16)(attack-skip) : 0;
    }
    word(i, 0x0a, attack);
    gm_write(i, 0x30, v->format);
    gm_write(i, 0x31, (u8)(address >> 16));
    word(i, 0x32, (u16)address);
    word(i, 0x36, loop);
    /* Open, non-resonant low-pass. Full MU50 filter/PEG modulation is outside
     * this receiver; the original wave samples and sample formats are intact. */
    word(i, 0x20, 0x7ff);
    gm_write(i, 0x24, element ? (u8)(gm_rom(element+9) << 1) : 0);
    gm_write(i, 0x23, element ? gm_rom(element+14) : 0);
    gm_write(i, 0x26, (u8)(0x80u | (element ? rate(gm_rom(element+71) + scaling) : 127)));
    gm_write(i, 0x27, element && gm_rom(element+71) < 63 ? 0xff : 0);
    gm_write(i, 0x28, element ? rate(gm_rom(element+72) + scaling) : drum_decay1_rate(drum));
    gm_write(i, 0x29, !loop ? 255 : element ? envelope_level(gm_rom(element+75)) : 64);
    gm_write(i, 0x2d, 255);
    pitch(s, i); level(s, i); modulation(s, i);
    gm_global((u16)(0x0bu - (i >> 3)), (u8)(1u << (i & 7u)));
}
__attribute__((weak)) void gm_note_trigger(u8 channel, u8 velocity) {
    (void)channel; (void)velocity;
}
static void note_on(Gm *s, u8 channel, u8 note, u8 velocity) {
    u8 element;
    u32 instrument;
    if (!velocity) { note_off(s, channel, note); return; }
    if (!s->enabled) return;
    ++s->age;
    if (s->channels[channel].drum) {
        u16 offset = be16(drum_map(&s->channels[channel]) + note * 2u);
        u32 d;
        u8 i, group;
        if (offset >= 353u * 30u) return;
        d = DRUMS + offset;
        if (!gm_rom(d+10) || be16(d+16) != 65535u) return;
        group = gm_rom(d+3);
        for (i = 0; i < GM_VOICES; ++i)
            if (s->voices[i].note != 255 && s->voices[i].channel == channel &&
                ((group && s->voices[i].choke == group) || (!gm_rom(d+8) && s->voices[i].note == note))) silence(s, i);
        start(s, channel, note, velocity, 0, 0, 0, d,
              /* MU50 v1.05 0x0240a6: +18 is the sample root note,
               * +28 is a signed SEMITONE correction for the factory pitch.
               * +29 is the alternative correction for neutral XG pitch 64;
               * it is not a second tuning term. */
              ((int)gm_rom(d) + signed_byte(d+28) - (int)gm_rom(d+18))*100 +
              (int)gm_rom(d+1)-64,
              2 * attenuation(gm_rom(d+2)), gm_rom(d+4) ? gm_rom(d+4) : 64,
              gm_rom(d+5), gm_rom(d+6));
        gm_note_trigger(channel, velocity);
        return;
    }
    instrument = instrument_address(&s->channels[channel]);
    for (element = 0; element < ((gm_rom(instrument+1) & 2u) ? 2 : 1); ++element) {
        static const u8 scaling[6] = {100,50,20,10,5,0};
        u32 e = instrument + 10u + element * 80u, sample;
        int key = (int)note + gm_rom(e+15) - 64;
        int center = gm_rom(e+18), depth = gm_rom(e+17), cents;
        int gain = clamp((int)gm_rom(e+57) + key_scale(e+58,e+62,note), 0, 127);
        u8 pan = gm_rom(e+67);
        if (note < gm_rom(e+2) || note > gm_rom(e+3) || velocity < gm_rom(e+4) || velocity > gm_rom(e+5)) continue;
        key = clamp(key,0,127);
        sample = region(e,key);
        if (!sample) continue;
        cents = center * 100 + scale(key-center, scaling[depth < 6 ? depth : 0], 1);
        cents -= signed_byte(sample+1) * 100;
        cents += signed_byte(sample+2) + (int)gm_rom(e+16)-64;
        start(s, channel, note, velocity, element, sample, e, 0, cents,
              2 * (attenuation(gm_rom(instrument)) + attenuation((u8)gain) + gm_rom(sample)),
              pan == 15 ? note : (u8)scale(pan, 127, 14), 127, 127);
    }
    gm_note_trigger(channel, velocity);
}

static void data_entry(Gm *s, u8 channel, u8 cc, u8 value) {
    GmChannel *c = &s->channels[channel];
    if (c->rpn_msb) return;
    if (c->rpn_lsb == 0) {
        if (cc == 6) c->bend_semitones = value;
        else c->bend_cents = (u8)clamp(value, 0, 99);
    } else if (c->rpn_lsb == 1) {
        if (cc == 6) c->fine = (u16)((c->fine & 127u) | (u16)value << 7);
        else c->fine = (u16)((c->fine & 16256u) | value);
    } else if (c->rpn_lsb == 2 && cc == 6) c->coarse = value;
    update(s, channel, 1);
}
static void control(Gm *s, u8 channel, u8 cc, u8 value) {
    GmChannel *c = &s->channels[channel];
    switch (cc) {
    case 0: c->bank_msb = value; break;
    case 32: c->bank_lsb = value; break;
    case 1: c->modulation = value; update(s, channel, 4); break;
    case 7: c->volume = value; update(s, channel, 2); break;
    case 10: c->pan = value; update(s, channel, 2); break;
    case 11: c->expression = value; update(s, channel, 2); break;
    case 64: c->sustain = value >= 64; if (!c->sustain) pedal_up(s, channel); break;
    case 91: c->reverb = value; update(s, channel, 2); break;
    case 93: c->chorus = value; update(s, channel, 2); break;
    case 100: c->rpn_lsb = value; break;
    case 101: c->rpn_msb = value; break;
    case 98: case 99: c->rpn_msb = c->rpn_lsb = 127; break;
    case 6: case 38: data_entry(s, channel, cc, value); break;
    case 120: all_sound_off(s, channel); break;
    case 121: controllers(c); pedal_up(s, channel); update(s, channel, 7); break;
    case 123: case 124: case 125: case 126: case 127: notes_off(s, channel); break;
    default: break;
    }
}
static void sysex(Gm *s) {
    const u8 *b = s->sysex;
    /* Yamaha XG parameter change: System On, and per-part receive mode. */
    /* XG parameter changes contain seven bytes between F0 and F7. Since
     * sysex_length includes the initial F0 marker, their completed length is
     * eight. (There is no checksum byte in this Yamaha message format.) */
    if (s->sysex_length == 8 && b[0] == 0x43 && (b[1] & 0xf0u) == 0x10 && b[2] == 0x4c) {
        if (b[3] == 0 && b[4] == 0 && b[5] == 0x7e && b[6] == 0) reset(s, 1);
        else if (b[3] == 8 && b[4] < 16 && b[5] == 7 &&
                 (s->xg_mode || b[4] != 9))
            s->channels[b[4]].drum = b[6] != 0;
        return;
    }
    /* Roland GS Reset selects the GM-compatible operating mode. This is the
     * sole supported GS message; GS effect and parameter messages stay ignored. */
    if (s->sysex_length == 10 && b[0] == 0x41 && b[1] <= 0x1fu &&
        b[2] == 0x42 && b[3] == 0x12 && b[4] == 0x40 && b[5] == 0 &&
        b[6] == 0x7f && b[7] == 0 && b[8] == 0x41) {
        reset(s, 0);
        return;
    }
    if (b[1] != 0x7f && b[1] != 0) return; /* device 0 or broadcast */
    if (s->sysex_length == 5 && b[0] == 0x7e && b[2] == 9) {
        if (b[3] == 1) gm_reset(s);
        else if (b[3] == 2) { stop(s); s->enabled = 0; }
    } else if (s->sysex_length == 7 && b[0] == 0x7f && b[2] == 4 && b[3] == 1) {
        u8 i;
        s->master = b[5];
        for (i = 0; i < 16; ++i) update(s, i, 2);
    }
}
void gm_byte(Gm *s, u8 byte) {
    u8 kind, channel;
    if (byte >= 0xf8) {
        if (byte == 0xff) gm_reset(s);
        else if (byte == 0xfc) stop(s);
        else if (byte == 0xfa) { stop(s); s->transport = 1; }
        else if (byte == 0xfb) s->transport = 1;
        return; /* Real-time bytes do not interrupt running status or SysEx. */
    }
    if (s->sysex_length) {
        if (byte == 0xf7) { sysex(s); s->sysex_length = 0; return; }
        if (byte < 128) {
            if (s->sysex_length <= sizeof s->sysex) s->sysex[s->sysex_length - 1] = byte;
            if (s->sysex_length < 255) ++s->sysex_length;
            return;
        }
        s->sysex_length = 0;
    }
    if (byte & 128) {
        s->have_first = 0; s->running = byte < 0xf0 ? byte : 0;
        if (byte == 0xf0) s->sysex_length = 1;
        return;
    }
    if (!s->running) return;
    kind = s->running & 0xf0; channel = s->running & 15;
    if (kind == 0xc0) {
        GmChannel *c = &s->channels[channel];
        c->program = byte;
        c->drum = (!s->xg_mode && channel == 9) ||
                  c->bank_msb == 126 || c->bank_msb == 127;
        return;
    }
    if (kind == 0xd0) { s->channels[channel].pressure = byte; update(s, channel, 4); return; }
    if (!s->have_first) { s->first = byte; s->have_first = 1; return; }
    s->have_first = 0;
    if (kind == 0x90) note_on(s, channel, s->first, byte);
    else if (kind == 0x80) note_off(s, channel, s->first);
    else if (kind == 0xb0) control(s, channel, s->first, byte);
    else if (kind == 0xe0) {
        s->channels[channel].bend = (u16)((u16)byte << 7 | s->first);
        update(s, channel, 1);
    }
}
