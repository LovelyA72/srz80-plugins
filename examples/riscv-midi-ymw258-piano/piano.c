/* RV32I/RV32IM board wrapper for the freestanding TG100 GM receiver. */
#include "gm.h"

#define YMW ((volatile u8 *)0x10000000u)
#define UART_DATA (*(volatile u8 *)0x10000100u)
#define UART_STATUS (*(volatile u8 *)0x10000101u)
#define MIDI_DATA (*(volatile u8 *)0x10000200u)
#define MIDI_STATUS (*(volatile u8 *)0x10000201u)
#define MIDI_CONTROL (*(volatile u8 *)0x10000202u)
#define LCD_COMMAND (*(volatile u8 *)0x10000300u)
#define LCD_DATA (*(volatile u8 *)0x10000301u)
#define TG100_ROM ((volatile u8 *)0x20000000u)
#define SAMPLE_ROM ((volatile u8 *)0x20200000u)
#define STATE ((State *)0x4000u)

typedef struct { Gm gm; u16 pending; u8 digits, meter[16]; u32 meter_ticks; } State;
_Static_assert(sizeof(State) < 0x3000, "leave at least 4 KiB for the RV32I stack");

#include "lcd_display.h"
#include "boot_animation.h"

u8 gm_rom(u32 address) { return TG100_ROM[address]; }
u8 gm_sample_rom(u32 address) { return SAMPLE_ROM[address]; }
void gm_write(u8 voice, u8 reg, u8 value) {
    /* Groups of seven slots; hardware slot 7 in each group is omitted. */
    u8 slot = voice;
    if (voice >= 21) slot += 3;
    else if (voice >= 14) slot += 2;
    else if (voice >= 7) ++slot;
    YMW[1] = slot; YMW[2] = reg; YMW[0] = value;
}
void gm_note_trigger(u8 channel, u8 velocity) {
    const u8 height = (u8)((velocity + 15u) >> 4);
    if (height > STATE->meter[channel]) {
        STATE->meter[channel] = height;
        lcd_refresh(&STATE->gm, STATE->meter);
    }
}
static void lcd_decay_tick(void) {
    u8 channel, changed = 0;
    if (++STATE->meter_ticks != 50000u) return;
    STATE->meter_ticks = 0;
    for (channel = 0; channel != 16; ++channel) if (STATE->meter[channel]) {
        u8 voice, held = 0;
        /* The channel's note state is the OR of its individual held notes. */
        for (voice = 0; voice != GM_VOICES; ++voice)
            if (STATE->gm.voices[voice].channel == channel && STATE->gm.voices[voice].held) {
                held = 1;
                break;
            }
        if (!held) { --STATE->meter[channel]; changed = 1; }
    }
    if (changed) lcd_refresh(&STATE->gm, STATE->meter);
}
static void puts_uart(const char *text) { while (*text) UART_DATA = (u8)*text++; }
static void number(u16 value) {
    u8 hundreds = 0, tens = 0;
    while (value >= 100) { value -= 100; ++hundreds; }
    while (value >= 10) { value -= 10; ++tens; }
    if (hundreds) UART_DATA = (u8)('0' + hundreds);
    if (hundreds || tens) UART_DATA = (u8)('0' + tens);
    UART_DATA = (u8)('0' + value);
}
static void show_program(void) {
    puts_uart("\rChannel 1 program "); number(STATE->gm.channels[0].program);
    puts_uart(" selected     ");
    lcd_refresh(&STATE->gm, STATE->meter);
}
static void console_byte(u8 byte) {
    GmChannel *channel = &STATE->gm.channels[0];
    if (byte == 'w' || byte == 's') {
        channel->program = (u8)((channel->program + (byte == 'w' ? 1 : 127)) & 127);
        STATE->pending = 0; STATE->digits = 0; show_program();
    } else if (byte >= '0' && byte <= '9' && STATE->digits < 3) {
        STATE->pending = (u16)(STATE->pending * 10 + byte - '0'); ++STATE->digits;
        UART_DATA = byte;
    } else if (byte == '\r' || byte == '\n') {
        if (STATE->digits && STATE->pending < 128) channel->program = (u8)STATE->pending;
        show_program(); puts_uart("\r\n> "); STATE->pending = 0; STATE->digits = 0;
    } else if ((byte == 8 || byte == 127) && STATE->digits) {
        u16 reduced = STATE->pending, tens = 0;
        while (reduced >= 10) { reduced -= 10; ++tens; }
        STATE->pending = tens; --STATE->digits;
    }
}
__attribute__((noreturn, noinline, used)) void firmware_main(void) {
    u8 channel;
    gm_reset(&STATE->gm); STATE->pending = 0; STATE->digits = 0; STATE->meter_ticks = 0;
    for (channel = 0; channel != 16; ++channel) STATE->meter[channel] = 0;
    /* Two-second piano-roll splash before the panel goes live.  MIDI input is
       still queued by the card during the intro; it is serviced afterwards. */
    lcd_boot_play();
    lcd_define_bars();
    lcd_refresh(&STATE->gm, STATE->meter);
    puts_uart("TG100 GM1 receiver; drums on channel 10\r\nChannel 1 program 0-127 + Return; w next, s previous\r\n> ");
    MIDI_CONTROL = 2;
    for (;;) {
        if (MIDI_STATUS & 1u) gm_byte(&STATE->gm, MIDI_DATA);
        if (UART_STATUS & 1u) console_byte(UART_DATA);
        lcd_decay_tick();
    }
}
__attribute__((naked, section(".text.start"), noreturn)) void _start(void) {
    __asm__ volatile("li sp, 0x8000\n tail firmware_main\n");
}
