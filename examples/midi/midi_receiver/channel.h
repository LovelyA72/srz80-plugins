#ifndef MIDI_CHANNEL_H
#define MIDI_CHANNEL_H

typedef struct {
    u8 program, volume, expression, pan, modulation, pressure, sustain, sostenuto, soft;
    u8 rpn_msb, rpn_lsb, bend_semitones, bend_cents, coarse;
    u16 bend, fine;
} GmChannel;

#endif
