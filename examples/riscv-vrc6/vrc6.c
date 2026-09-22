/* A C-major phrase played by the three VRC6 channels one at a time. */

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

#define VRC6 ((volatile u8 *)0x10000000u)
#define PULSE1_CTRL VRC6[0x0000]
#define PULSE1_LO   VRC6[0x0001]
#define PULSE1_HI   VRC6[0x0002]
#define FREQ_CTRL   VRC6[0x0003]
#define PULSE2_CTRL VRC6[0x1000]
#define PULSE2_LO   VRC6[0x1001]
#define PULSE2_HI   VRC6[0x1002]
#define SAW_RATE    VRC6[0x2000]
#define SAW_LO      VRC6[0x2001]
#define SAW_HI      VRC6[0x2002]

#define REST 0xffu
#define SCORE_SLOTS 16u

/* C4,D4,E4,F4,G4,A4,B4,C5,D5 at a 1,789,773 Hz VRC6 clock. Pulse 1 uses
 * the octave above the written pitch, pulse 2 uses written pitch, and the
 * saw sounds one octave below it. */
static const u16 pulse1_period[9] = {213, 189, 169, 159, 142, 126, 112, 106, 94};
static const u16 pulse2_period[9] = {427, 380, 338, 319, 284, 253, 225, 213, 189};
static const u16 saw_period[9] = {977, 869, 775, 731, 651, 580, 517, 487, 435};
static const u8 song[SCORE_SLOTS] = {
    0, 1, 2, 3, 4, REST, REST, REST,
    4, 3, 2, 1, 0, REST, REST, REST
};

static void silence(void) {
    PULSE1_HI = 0;
    PULSE2_HI = 0;
    SAW_HI = 0;
}

static void start_note(u8 channel, u8 note) {
    u16 period;
    silence();
    if (note == REST)
        return;
    if (channel == 0u) {
        period = pulse1_period[note];
        PULSE1_LO = (u8)period;
        PULSE1_HI = (u8)(0x80u | (period >> 8));
    } else if (channel == 1u) {
        period = pulse2_period[note];
        PULSE2_LO = (u8)period;
        PULSE2_HI = (u8)(0x80u | (period >> 8));
    } else {
        period = saw_period[note];
        SAW_LO = (u8)period;
        SAW_HI = (u8)(0x80u | (period >> 8));
    }
}

/* The RV32I loop takes two instructions per iteration. At the project clock,
 * 55,930 iterations are approximately one 62.5 ms tick. */
static __attribute__((noinline)) void tick_wait(void) {
    u32 count = 55930u;
    __asm__ volatile(
        "1: addi %0, %0, -1\n"
        "bnez %0, 1b\n"
        : "+r"(count));
}

__attribute__((noreturn, noinline, used)) void firmware_main(void) {
    u8 channel = 0;
    u8 slot = 0;
    u8 ticks_left = 0;
    FREQ_CTRL = 0;
    PULSE1_CTRL = 0x5fu; /* 6/16 duty, constant volume 15. */
    PULSE2_CTRL = 0x2fu; /* 3/16 duty, constant volume 15. */
    SAW_RATE = 32u;
    silence();
    for (;;) {
        if (ticks_left == 0u) {
            start_note(channel, song[slot]);
            ticks_left = 4u;
            if (++slot == SCORE_SLOTS) {
                slot = 0;
                if (++channel == 3u)
                    channel = 0;
            }
        }
        --ticks_left;
        tick_wait();
    }
}

__attribute__((naked, section(".text.start"), noreturn)) void _start(void) {
    __asm__ volatile("li sp, 0x8000\n"
                     "tail firmware_main\n");
}
