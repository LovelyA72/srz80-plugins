/*
 * AY-3-8913 ".ym" music player.
 *
 * Platform (see project.json):
 *   ROM0  0x00000000  this firmware
 *   RAM   0x00004000  16 KiB work RAM (stack, song state)
 *   ROM1  0x00200000  example.ym, a standard uncompressed .ym register dump
 *   CTC   0x10001000  frame timer, channel 0 raises "IRQ"
 *        0x00008000  CTC interrupt-acknowledge alias (read drops the request)
 *   AY    0x10000000  AY-3-8913, latch at +0, data at +1
 *
 * The CTC runs from the 3.072 MHz project clock with the /256 prescaler.  Its
 * time constant follows the .ym player frequency, which divides exactly for
 * both common rates: 50 Hz -> 240, 60 Hz -> 200.  Each machine external
 * interrupt acknowledges the CTC and writes one .ym frame of registers 0..13
 * to the AY.
 */
#include <stdint.h>

#define AY_BASE 0x10000000u
#define CTC_BASE 0x10001000u
#define CTC_INTACK 0x00008000u
#define MUSIC_BASE 0x00200000u

/* Build script defines the music image size; it is only needed to derive a
   frame count for the headerless YM2/YM3/YM3b variants. */
#ifndef MUSIC_IMAGE_SIZE
#define MUSIC_IMAGE_SIZE 0u
#endif

/* Frame clock.  The CTC always supplies the frame interrupt; its rate follows
   the .ym header's player frequency (YM4/YM5/YM6) or the 50 Hz YM2/YM3
   convention.  Define PLAYER_FRAME_HZ non-zero to force a single rate. */
#ifndef PLAYER_FRAME_HZ
#define PLAYER_FRAME_HZ 0u
#endif

#define CTC_CLOCK_HZ 3072000u
#define CTC_PRESCALER 256u
#define CTC_FALLBACK_HZ 50u

#define REG8(address) (*(volatile uint8_t *)(uintptr_t)(address))

static void ay_write(uint8_t reg, uint8_t value) {
    REG8(AY_BASE + 0u) = (uint8_t)(reg & 0x0Fu); /* register select */
    REG8(AY_BASE + 1u) = value;                  /* data */
}

/* ------------------------------------------------------------------ */
/* .ym parsing                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    const uint8_t *frames;   /* start of the register dump */
    uint32_t frame_count;
    uint32_t loop_frame;
    uint16_t frame_rate;     /* Hz, informational */
    uint8_t regs_per_frame;  /* 14 for YM2/YM3, 16 for YM4+ */
    uint8_t interleaved;     /* 1 = register-major (all frames of r0, then r1...) */
} ym_song_t;

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint16_t be16(const uint8_t *p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }

static uint32_t ym_magic(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* "YM2!", "YM3!", "YM3b", "YM4!", "YM5!" and "YM6!" as little-endian words. */
#define YM_ID(a, b, c, d)                                                                          \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define YM2 YM_ID('Y', 'M', '2', '!')
#define YM3 YM_ID('Y', 'M', '3', '!')
#define YM3B YM_ID('Y', 'M', '3', 'b')
#define YM4 YM_ID('Y', 'M', '4', '!')
#define YM5 YM_ID('Y', 'M', '5', '!')
#define YM6 YM_ID('Y', 'M', '6', '!')

/* Skip `count` NUL-terminated strings starting at *offset, clamping at size. */
static int ym_skip_strings(const uint8_t *data, uint32_t size, uint32_t *offset, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        while (*offset < size && data[*offset] != 0)
            ++*offset;
        if (*offset >= size)
            return 0;
        ++*offset;
    }
    return 1;
}

static int ym_parse(const uint8_t *data, uint32_t size, ym_song_t *song) {
    if (size < 4)
        return 0;

    song->loop_frame = 0;
    song->frame_rate = 50;

    const uint32_t magic = ym_magic(data);
    if (magic == YM2 || magic == YM3 || magic == YM3B) {
        uint32_t payload = size - 4;
        if (magic == YM3B) {
            if (size < 8)
                return 0;
            payload = size - 8;
            song->loop_frame = be32(data + size - 4);
        }
        if (payload < 14 || payload % 14 != 0)
            return 0;
        song->frames = data + 4;
        song->regs_per_frame = 14;
        song->interleaved = 1;
        song->frame_count = payload / 14;
        if (song->loop_frame >= song->frame_count)
            song->loop_frame = 0;
        return 1;
    }

    if (magic != YM4 && magic != YM5 && magic != YM6)
        return 0;
    if (size < 26)
        return 0;
    {
        static const char check[8] = "LeOnArD!";
        for (uint32_t i = 0; i < 8; ++i) {
            if (data[4 + i] != (uint8_t)check[i])
                return 0;
        }
    }

    const uint32_t frame_count = be32(data + 12);
    const uint32_t attributes = be32(data + 16);
    const uint16_t digidrums = be16(data + 20);
    if (frame_count == 0 || frame_count > 100000u)
        return 0;

    uint32_t offset;
    if (magic == YM4) {
        song->loop_frame = be32(data + 22);
        offset = 26;
    } else {
        if (size < 34)
            return 0;
        song->frame_rate = be16(data + 26);
        song->loop_frame = be32(data + 28);
        offset = 34u + be16(data + 32);
    }
    if (offset > size)
        return 0;

    for (uint16_t i = 0; i < digidrums; ++i) {
        if (offset + 4 > size)
            return 0;
        offset += 4u + be32(data + offset);
        if (offset > size)
            return 0;
    }
    if (!ym_skip_strings(data, size, &offset, 3))
        return 0;

    const uint32_t available = size - offset;
    uint8_t regs = 16;
    if ((uint64_t)frame_count * 16u > available) {
        if ((uint64_t)frame_count * 14u > available)
            return 0;
        regs = 14;
    }

    song->frames = data + offset;
    song->regs_per_frame = regs;
    song->interleaved = (attributes & 1u) ? 1 : 0;
    song->frame_count = frame_count;
    if (song->frame_rate == 0)
        song->frame_rate = 50;
    if (song->loop_frame >= frame_count)
        song->loop_frame = 0;
    return 1;
}

static uint8_t ym_register(const ym_song_t *song, uint32_t frame, uint8_t reg) {
    if (song->interleaved)
        return song->frames[(uint32_t)reg * song->frame_count + frame];
    return song->frames[frame * song->regs_per_frame + reg];
}

/* ------------------------------------------------------------------ */
/* Player                                                             */
/* ------------------------------------------------------------------ */

static ym_song_t song;
static uint32_t position;

/* Write one .ym frame of the 14 AY registers.  Register 13 uses 0xFF in the
   format to mean "leave the envelope running", so it is not rewritten. */
static void play_frame(uint32_t frame) {
    for (uint8_t reg = 0; reg < 14; ++reg) {
        const uint8_t value = ym_register(&song, frame, reg);
        if (reg == 13 && value == 0xFF)
            continue;
        ay_write(reg, value);
    }
}

/* Machine external interrupt: acknowledge the CTC, then advance one frame. */
void __attribute__((interrupt("machine"), aligned(4))) trap_handler(void) {
    (void)REG8(CTC_INTACK);

    if (song.frame_count == 0)
        return;

    play_frame(position);
    if (++position >= song.frame_count)
        position = song.loop_frame;
}

static void ay_silence(void) {
    for (uint8_t reg = 0; reg < 14; ++reg)
        ay_write(reg, 0);
    ay_write(7, 0x3Fu); /* all tone and noise disabled */
}

/* Channel 0: interrupt enabled, timer, /256 prescaler, auto-start.  The time
   constant is derived from the frame rate, so the CTC follows the song.  A
   constant of 256 is written as 0, which the card reads back as 256. */
static void ctc_start(uint32_t frame_hz) {
    uint32_t constant = CTC_CLOCK_HZ / (CTC_PRESCALER * frame_hz);
    if (constant < 1u)
        constant = 1u;
    if (constant > 256u)
        constant = 256u;

    REG8(CTC_BASE + 0u) = 0x00u; /* interrupt vector base */
    REG8(CTC_BASE + 0u) = 0xA5u; /* INT + timer + /256 + next word is the constant */
    REG8(CTC_BASE + 0u) = (uint8_t)(constant & 0xFFu);
}

__attribute__((noreturn)) void main(void) {
    ay_silence();

    if (!ym_parse((const uint8_t *)MUSIC_BASE, MUSIC_IMAGE_SIZE, &song)) {
        /* No usable .ym image: stay silent. */
        for (;;) {
        }
    }

    /* Show the first frame immediately so playback does not wait one tick, and
       advance past it so the first interrupt plays frame 1, not frame 0 twice. */
    position = 0;
    play_frame(0);
    if (++position >= song.frame_count)
        position = song.loop_frame;

    uint32_t frame_hz = PLAYER_FRAME_HZ;
    if (frame_hz == 0)
        frame_hz = song.frame_rate ? song.frame_rate : CTC_FALLBACK_HZ;
    ctc_start(frame_hz);

    __asm__ volatile("csrs mstatus, %0" ::"r"(0x8u)); /* mstatus.MIE = 1 */

    for (;;) {
    }
}
