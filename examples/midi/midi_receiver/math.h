#ifndef MIDI_MATH_H
#define MIDI_MATH_H

// Use hardware multiply and divide when the target has them
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

static int channel_pitch(const GmChannel *c) {
    return scale((int)c->bend - 8192, c->bend_semitones * 100u + c->bend_cents, 8192) +
           scale((int)c->fine - 8192, 100, 8192) + ((int)c->coarse - 64) * 100;
}

#endif
