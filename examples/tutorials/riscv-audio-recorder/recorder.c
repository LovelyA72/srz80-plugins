#include <stdint.h>

#define INPUT_BASE 0x10000000u
#define DAC_BASE   0x10000100u
#define SAMPLE_RATE 16000u
#define SAMPLE_COUNT (SAMPLE_RATE * 5u)

static volatile uint8_t *const input = (volatile uint8_t *)INPUT_BASE;
static volatile uint8_t *const dac = (volatile uint8_t *)DAC_BASE;
static uint8_t recording[SAMPLE_COUNT];

__attribute__((noreturn)) void main(void);

static uint32_t time_ms_once(void) {
    return (uint32_t)input[10] | ((uint32_t)input[11] << 8) |
           ((uint32_t)input[12] << 16) | ((uint32_t)input[13] << 24);
}

static uint32_t time_ms(void) {
    uint32_t a, b;
    do { a = time_ms_once(); b = time_ms_once(); } while (a != b);
    return a;
}

static int before(uint32_t now, uint32_t deadline) {
    return (int32_t)(now - deadline) < 0;
}

static void record_phase(void) {
    for (uint32_t i = 0; i < SAMPLE_COUNT; ++i) recording[i] = 0x80;
    input[2] = 0xE0;
    input[1] = 0x03; /* clear, enable */
    const uint32_t deadline = time_ms() + 5000u;
    uint32_t position = 0;
    while (before(time_ms(), deadline)) {
        uint16_t available = (uint16_t)input[7] | ((uint16_t)input[8] << 8);
        while (available-- && position < SAMPLE_COUNT) recording[position++] = input[0];
    }
    input[1] = 0; /* badge off, discard captured tail */
}

static void play_phase(void) {
    dac[0] = 0x80;
    dac[3] = (uint8_t)SAMPLE_RATE;
    dac[4] = (uint8_t)(SAMPLE_RATE >> 8);
    dac[2] = 0xC0;
    dac[1] = 0x03; /* FIFO mode, clear */
    const uint32_t deadline = time_ms() + 5000u;
    uint32_t position = 0;
    while (before(time_ms(), deadline)) {
        uint8_t count = dac[2] & 0x3Fu;
        while (count < 30 && position < SAMPLE_COUNT) {
            dac[0] = recording[position++];
            ++count;
        }
    }
    dac[1] = 0x02; /* direct mode and clear residual FIFO/lookahead */
    dac[0] = 0x80;
}

__attribute__((section(".text.start"), naked, noreturn))
void _start(void) {
    __asm__ volatile("la sp, _stack_top\ncall main\n1: j 1b");
}

__attribute__((noreturn)) void main(void) {
    input[3] = (uint8_t)SAMPLE_RATE;
    input[4] = (uint8_t)(SAMPLE_RATE >> 8);
    input[5] = input[6] = 0;
    input[9] = 0;
    input[1] = 0;
    dac[0] = 0x80;
    for (;;) { record_phase(); play_phase(); }
}
