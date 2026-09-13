#include "samples.h"

static volatile unsigned char *const card = (volatile unsigned char *)0x10000000;
// Native demo test and the Memory panel can inspect these RAM words.
static volatile unsigned *const progress = (volatile unsigned *)0x10000;

static void word(unsigned offset, unsigned value) {
    for (unsigned byte = 0; byte != 4; ++byte) {
        card[offset + byte] = (unsigned char)value;
        value >>= 8;
    }
}

static void play(unsigned mode, unsigned start, unsigned length, unsigned rate, unsigned stage) {
    card[1] = 2;
    card[0] = (unsigned char)mode;
    word(4, start);
    word(8, length);
    word(12, rate);
    progress[0] = stage;
    card[1] = 1;
    while (card[1] & 1) {}
    if (card[1] & 128) {
        progress[3] = 0xBAD;
        for (;;) {}
    }
    progress[1] |= 1u << stage;
}

void main(void) {
    progress[0] = progress[1] = progress[2] = progress[3] = 0;
    card[2] = 192;
    for (;;) {
        play(samples[0][0], samples[0][1], samples[0][2], samples[0][3], 0);
        ++progress[2];
    }
}
