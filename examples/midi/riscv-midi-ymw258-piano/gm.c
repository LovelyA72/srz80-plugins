#include "gm.h"

#define PROGRAMS 0x10410u
#define REGIONS 0x176a2u
#define GROUPS 0x1841au
#define PITCHES 0x14e2cu
#define DRUM_MAP 0x18b9eu
#define DRUMS 0x18532u

static u16 be16(u32 address) {
    return (u16)((u16)gm_rom(address) << 8 | gm_rom(address + 1u));
}
#include "../midi_receiver/math.h"

static void note_off(Gm *s, u8 channel, u8 note);

/* MIDI volume/expression use a squared gain curve, converted to the chip's
 * 0.375 dB attenuation steps. Zero additionally selects the chip's mute pan. */
static const u8 attenuation[128] = {
    127,127,127,127,127,127,127,127,127,123,118,113,109,106,102,99,
    96,93,91,88,86,83,81,79,77,75,73,72,70,68,67,65,
    64,62,61,60,58,57,56,55,54,52,51,50,49,48,47,46,
    45,44,43,42,41,40,39,39,38,37,36,35,34,34,33,32,
    32,31,30,30,29,28,28,27,26,26,25,24,24,23,23,22,
    21,21,20,20,19,19,18,18,17,17,16,16,15,15,14,14,
    13,13,12,12,11,11,11,10,10,9,9,8,8,8,7,7,
    6,6,6,5,5,4,4,4,3,3,3,2,2,1,1,0
};

static void release(Gm *s, u8 i) {
    gm_write(i, 4, 0);
    s->voices[i].active = s->voices[i].held = 0;
}
static void silence(Gm *s, u8 i) {
    gm_write(i, 0, 0x80); /* All Sound Off must also silence release tails. */
    release(s, i);
    s->voices[i].note = 255;
}
static void pitch(Gm *s, u8 i) {
    GmVoice *v = &s->voices[i];
    int cents = v->pitch_cents + channel_pitch(&s->channels[v->channel]) +
                scale((int)s->master_fine - 8192, 100, 8192) +
                ((int)s->master_coarse - 64) * 100;
    int octave = 1;
    u16 fnum;
    /* Encode only the hardware's actual range: nibble 8 means +7, not -9. */
    cents = clamp(cents, -9600, 9599);
    while (cents >= 1200) { cents -= 1200; ++octave; }
    while (cents < 0) { cents += 1200; --octave; }
    fnum = be16(PITCHES + (u32)cents * 2u);
    gm_write(i, 2, (u8)((fnum << 2) | (v->sample >> 8)));
    gm_write(i, 3, (u8)((fnum >> 6) | (((u8)octave & 15u) << 4)));
}
static void level(Gm *s, u8 i) {
    GmVoice *v = &s->voices[i];
    GmChannel *c = &s->channels[v->channel];
    int amount = v->attenuation + (c->soft ? 16 : 0) + attenuation[c->volume] +
                 attenuation[c->expression] + attenuation[s->master];
    int pan = (int)c->pan - 64;
    int base_pan = v->pan >= 9 ? (int)v->pan - 16 : v->pan;
    pan = clamp(scale(pan, 7, pan < 0 ? 64 : 63) + base_pan, -7, 7);
    gm_write(i, 5, (u8)(clamp(amount, 0, 127) * 2) | 1u);
    gm_write(i, 0, (!c->volume || !c->expression || !s->master) ? 0x80 : (u8)((pan & 15) << 4));
}
static void modulation(Gm *s, u8 i) {
    GmVoice *v = &s->voices[i];
    GmChannel *c = &s->channels[v->channel];
    u8 amount = c->modulation > c->pressure ? c->modulation : c->pressure;
    u8 depth = (u8)(amount >> 4);
    if (depth < (v->lfo & 7u)) depth = v->lfo & 7u;
    gm_write(i, 6, (u8)(((v->lfo & 0x38u) ? (v->lfo & 0x38u) : (amount ? 0x28u : 0u)) | depth));
}
static void update(Gm *s, u8 channel, u8 what) {
    u8 i;
    for (i = 0; i < GM_VOICES; ++i) if (s->voices[i].note != 255 && s->voices[i].channel == channel) {
        if (what & 1u) pitch(s, i);
        if (what & 2u) level(s, i);
        if (what & 4u) modulation(s, i);
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
    /* Preserve at least 24 independent notes on a 28-slot chip: optional
     * second layers are reclaimed before any held primary voice. */
    if (layered != 255) { silence(s, layered); return layered; }
    silence(s, oldest); /* All 28 are held: kill the one activated first. */
    return oldest;
}
static u32 region(u32 instrument, u8 element, int note) {
    u32 field = instrument + (element ? 60u : 24u);
    u16 group = (u16)((u16)gm_rom(field) << 4 | gm_rom(field + 1));
    u16 first, end, i;
    if (group >= 140) return 0;
    first = be16(GROUPS + group * 2u); end = be16(GROUPS + (group + 1u) * 2u);
    if (!first || first >= end || end > 383) return 0;
    for (i = first; i < end; ++i) {
        u32 address = REGIONS + i * 9u;
        if (note <= gm_rom(address + 3)) return address;
    }
    return REGIONS + (end - 1u) * 9u; /* Extend the edge sample across MIDI's range. */
}
static void start(Gm *s, u8 channel, u8 note, u8 velocity, u8 secondary,
                  u16 sample, int cents, u8 attenuation_value, u8 pan) {
    u8 i = allocate(s, secondary);
    GmVoice *v;
    if (i == 255) return;
    v = &s->voices[i];
    v->age = s->age; v->note = note; v->channel = channel; v->sample = sample;
    v->active = v->held = 1; v->latched = 0; v->secondary = secondary; v->pitch_cents = cents;
    v->attenuation = (u8)clamp(attenuation_value + ((127 - velocity) >> 1), 0, 127);
    v->pan = pan & 15u;
    /* Header LFO is a sample default; controller changes must not erase it. */
    v->lfo = gm_sample_rom(sample * 12u + 7u);
    gm_write(i, 4, 0);
    gm_write(i, 2, (u8)(sample >> 8)); /* high sample bit is latched by reg 1 */
    gm_write(i, 1, (u8)sample);
    pitch(s, i); level(s, i); modulation(s, i);
    gm_write(i, 4, 0x80);
}
static u8 choke(u8 note) {
    if (note == 42 || note == 44 || note == 46) return 1;
    if (note == 80 || note == 81) return 2;
    if (note == 71 || note == 72) return 3;
    if (note == 73 || note == 74) return 4;
    return 0;
}
__attribute__((weak)) void gm_note_trigger(u8 channel, u8 velocity) {
    (void)channel;
    (void)velocity;
}

static void note_on(Gm *s, u8 channel, u8 note, u8 velocity) {
    u8 element;
    u32 instrument;
    if (!velocity) { note_off(s, channel, note); return; }
    if (!s->enabled) return;
    gm_note_trigger(channel, velocity);
    ++s->age;
    if (channel == 9) {
        u16 index = be16(DRUM_MAP + note * 2u);
        u32 address;
        u8 i, group = choke(note), transpose;
        if (index == 511 || index >= 274) return;
        for (i = 0; i < GM_VOICES; ++i)
            if (s->voices[i].channel == 9 && group && choke(s->voices[i].note) == group) silence(s, i);
        address = DRUMS + index * 6u;
        transpose = gm_rom(address + 2);
        start(s, channel, note, velocity, 0, be16(address),
              (transpose >= 128 ? (int)transpose - 256 : transpose) * 100 + gm_rom(address + 3),
              gm_rom(address + 4), gm_rom(address + 5));
        return;
    }
    instrument = PROGRAMS + s->channels[channel].program * 96u;
    for (element = 0; element <= (gm_rom(instrument) != 0); ++element) {
        u8 key_scale = gm_rom(0x15c68u + gm_rom(instrument + (element ? 14u : 10u)));
        int center = gm_rom(instrument + (element ? 15u : 11u));
        int key_cents = scale((int)note - center, key_scale, 1) + center * 100 +
                        ((int)gm_rom(instrument + (element ? 13u : 12u)) - 64) * 100;
        u32 address = region(instrument, element, scale(key_cents, 1, 100));
        int base_level;
        if (!address) continue;
        base_level = ((127 - gm_rom(instrument + (element ? 2u : 1u))) >> 1) + gm_rom(address + 6);
        start(s, channel, note, velocity, element, be16(address),
              key_cents - gm_rom(address + 4) * 100 + gm_rom(address + 5) +
              (int)gm_rom(instrument + (element ? 4u : 3u)) - 64,
              (u8)clamp(base_level, 0, 127), gm_rom(instrument + (element ? 76u : 40u)));
    }
}

#define GM_PROGRAM_CHANGE 1
#define GM_PERCUSSION_ONESHOT 1
#include "../midi_receiver/receiver.h"
