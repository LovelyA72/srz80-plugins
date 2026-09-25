/* RV32IMF X1-010 wavetable and PCM console.
 *
 * UART commands (each terminated by Return):
 *   sine, saw, tri, square
 *   draw <16 signed values from -128 through 127>
 *   pcm
 *
 * MIDI Note On/Off messages play the selected wavetable on X1-010 voices.
 */

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

#define X1_BASE       ((volatile u8 *)0x10000000u)
#define UART_DATA     (*(volatile u8 *)0x10002000u)
#define UART_STATUS   (*(volatile u8 *)0x10002001u)
#define MIDI_DATA     (*(volatile u8 *)0x10002100u)
#define MIDI_STATUS   (*(volatile u8 *)0x10002101u)
#define MIDI_CONTROL  (*(volatile u8 *)0x10002102u)
#define SAMPLE_RAM    ((volatile u8 *)0x00100000u)
#define PCM_ROM       ((volatile const u8 *)0x20000000u)

#define X1_WAVE_RAM   0x1000u
#define X1_ENVELOPE   0x0080u
#define PCM_BYTES     0x100000u
#define PCM_LENGTH    0x80000u
#define UART_LINE_MAX 160u

static u8 wave_index;
static u8 active_note[16];
static u8 active[16];
static u8 uart_line[UART_LINE_MAX];
static u32 uart_length;

static void uart_putc(u8 value) { UART_DATA = value; }

static void uart_puts(const char *text) {
    while (*text != 0)
        uart_putc((u8)*text++);
}

static u8 string_equal(const u8 *text, const char *literal) {
    u32 i = 0;
    while (literal[i] != 0 && text[i] == (u8)literal[i])
        ++i;
    return literal[i] == 0 && text[i] == 0;
}

static u8 is_space(u8 value) { return value == ' ' || value == '\t'; }

static void x1_write(u8 voice, u8 reg, u8 value) {
    X1_BASE[(u32)voice * 8u + reg] = value;
}

static void x1_set_frequency(u8 voice, u16 frequency) {
    x1_write(voice, 2, (u8)frequency);
    x1_write(voice, 3, (u8)(frequency >> 8));
}

static void x1_stop_all(void) {
    u8 voice;
    for (voice = 0; voice != 16u; ++voice) {
        x1_write(voice, 0, 0);
        active[voice] = 0;
    }
}

static void write_wave_step(u8 step, u8 value) {
    u8 repeat;
    const u32 offset = X1_WAVE_RAM + (u32)step * 8u;
    for (repeat = 0; repeat != 8u; ++repeat)
        X1_BASE[offset + repeat] = value;
}

static void select_wave(u8 new_wave) {
    wave_index = new_wave;
    for (u8 voice = 0; voice != 16u; ++voice)
        if (active[voice])
            x1_write(voice, 1, wave_index & 31u);
}

static void install_wave(const char *name, const u8 *steps) {
    u8 step;
    for (step = 0; step != 16u; ++step)
        write_wave_step(step, steps[step]);
    select_wave(0);
    uart_puts("\r\nWaveform: ");
    uart_puts(name);
    uart_puts("\r\n> ");
}

static const u8 sine_steps[16] = {
    0, 49, 90, 117, 127, 117, 90, 49,
    0, 207, 166, 139, 129, 139, 166, 207
};
static const u8 saw_steps[16] = {
    0, 17, 34, 51, 68, 85, 102, 119,
    136, 153, 170, 187, 204, 221, 238, 255
};
static const u8 tri_steps[16] = {
    0, 32, 64, 96, 128, 160, 192, 224,
    255, 224, 192, 160, 128, 96, 64, 32
};
static const u8 square_steps[16] = {
    127, 127, 127, 127, 127, 127, 127, 127,
    129, 129, 129, 129, 129, 129, 129, 129
};

static u8 parse_number(const u8 **cursor, int *value) {
    const u8 *text = *cursor;
    int sign = 1;
    int number = 0;
    u8 digits = 0;
    while (is_space(*text))
        ++text;
    if (*text == '-') {
        sign = -1;
        ++text;
    } else if (*text == '+') {
        ++text;
    }
    while (*text >= '0' && *text <= '9') {
        number = number * 10 + (int)(*text - '0');
        ++text;
        digits = 1;
    }
    if (!digits)
        return 0;
    *value = sign * number;
    *cursor = text;
    return 1;
}

static u8 parse_draw(const u8 *line) {
    const u8 *cursor = line + 4;
    u8 steps[16];
    u8 count;
    int value;
    for (count = 0; count != 16u; ++count) {
        if (!parse_number(&cursor, &value) || value < -128 || value > 127)
            return 0;
        steps[count] = (u8)value;
    }
    while (is_space(*cursor))
        ++cursor;
    if (*cursor != 0)
        return 0;
    for (count = 0; count != 16u; ++count)
        write_wave_step(count, steps[count]);
    select_wave(0);
    uart_puts("\r\nWaveform: custom 16-step table\r\n> ");
    return 1;
}

static void pcm_demo(void) {
    u32 address;
    x1_stop_all();
    uart_puts("\r\nCopying PCM demo into shared 1 MiB RAM...\r\n");
    for (address = 0; address != PCM_BYTES; ++address)
        SAMPLE_RAM[address] = PCM_ROM[address];

    // PCM start/end are expressed in 4 KiB units. 0x80 means the first 512 KiB
    // of the shared 1 MiB window; the remaining space stays available for
    // future samples.
    x1_write(0, 1, 0xff);
    // PCM byte rate = chip_clock * frequency / 8192. At 16 MHz, 25 is the
    // nearest integer setting to the source's 48 kHz rate (48,828.125 Hz).
    x1_set_frequency(0, 25);
    x1_write(0, 4, 0);
    x1_write(0, 5, 0x80);
    x1_write(0, 0, 1);
    active[0] = 1;
    uart_puts("PCM demo playing\r\n> ");
}

static u16 note_frequency(u8 note) {
    static const u16 semitone[12] = {
        3691, 3911, 4148, 4397, 4658, 4935,
        5228, 5539, 5868, 6217, 6586, 6976
    };
    u8 index = note % 12u;
    int octave = (int)(note / 12u) - 5;
    u16 frequency = semitone[index];
    while (octave > 0) {
        frequency <<= 1;
        --octave;
    }
    while (octave < 0) {
        frequency >>= 1;
        ++octave;
    }
    return frequency;
}

static void note_off(u8 note) {
    u8 voice;
    for (voice = 0; voice != 16u; ++voice)
        if (active[voice] && active_note[voice] == note) {
            x1_write(voice, 0, 0);
            active[voice] = 0;
            return;
        }
}

static void note_on(u8 note, u8 velocity) {
    u8 voice;
    if (velocity == 0) {
        note_off(note);
        return;
    }
    for (voice = 0; voice != 16u; ++voice)
        if (active[voice] && active_note[voice] == note)
            break;
    if (voice == 16u) {
        for (voice = 0; voice != 16u && active[voice]; ++voice) {}
        if (voice == 16u)
            voice = 0;
    }
    x1_write(voice, 0, 0);
    x1_write(voice, 1, wave_index & 31u);
    x1_set_frequency(voice, note_frequency(note));
    x1_write(voice, 4, 0);
    // Envelope shape 0 aliases the voice-register area on the host bus.
    // Shape 1 begins at X1 offset 0x0080 and is initialized to full volume.
    x1_write(voice, 5, 1);
    x1_write(voice, 0, 3); // wavetable mode + key on
    active_note[voice] = note;
    active[voice] = 1;
}

static u8 midi_running;
static u8 midi_length;
static u8 midi_count;
static u8 midi_message[2];

static void midi_byte(u8 value) {
    if (value >= 0xf8u)
        return;
    if (value & 0x80u) {
        if (value >= 0x80u && value <= 0xefu) {
            midi_running = value;
            midi_count = 0;
            midi_length = ((value & 0xe0u) == 0xc0u || (value & 0xe0u) == 0xd0u) ? 1 : 2;
        } else {
            midi_running = 0;
            midi_count = 0;
        }
        return;
    }
    if (!midi_running)
        return;
    midi_message[midi_count++] = value;
    if (midi_count < midi_length)
        return;
    midi_count = 0;
    if ((midi_running & 0xf0u) == 0x80u)
        note_off(midi_message[0]);
    else if ((midi_running & 0xf0u) == 0x90u)
        note_on(midi_message[0], midi_message[1]);
}

static void poll_midi(void) {
    while (MIDI_STATUS & 1u)
        midi_byte(MIDI_DATA);
}

static void console_line(void) {
    uart_line[uart_length] = 0;
    if (string_equal(uart_line, "sine"))
        install_wave("sine", sine_steps);
    else if (string_equal(uart_line, "saw"))
        install_wave("saw", saw_steps);
    else if (string_equal(uart_line, "tri"))
        install_wave("triangle", tri_steps);
    else if (string_equal(uart_line, "square"))
        install_wave("square", square_steps);
    else if (string_equal(uart_line, "pcm"))
        pcm_demo();
    else if (uart_length >= 4u && uart_line[0] == 'd' && uart_line[1] == 'r' &&
             uart_line[2] == 'a' && uart_line[3] == 'w')
        if (!parse_draw(uart_line))
            uart_puts("\r\nUsage: draw <16 values from -128 through 127>\r\n> ");
    else
        uart_puts("\r\nCommands: sine | saw | tri | square | draw <16 numbers> | pcm\r\n> ");
    uart_length = 0;
}

static void poll_uart(void) {
    while (UART_STATUS & 1u) {
        const u8 value = UART_DATA;
        if (value == '\r' || value == '\n') {
            console_line();
        } else if (value == 8u || value == 127u) {
            if (uart_length != 0) {
                --uart_length;
                uart_puts("\b \b");
            }
        } else if (uart_length + 1u < UART_LINE_MAX) {
            uart_line[uart_length++] = value;
            uart_putc(value);
        }
    }
}

static void init_x1(void) {
    u16 address;
    u8 voice;
    for (address = 0; address != 0x80u; ++address)
        X1_BASE[X1_ENVELOPE + address] = 0xff;
    for (voice = 0; voice != 16u; ++voice) {
        active[voice] = 0;
        x1_write(voice, 0, 0);
    }
    for (u8 step = 0; step != 16u; ++step)
        write_wave_step(step, sine_steps[step]);
    wave_index = 0;
}

__attribute__((naked, noreturn, section(".text.start"))) void _start(void) {
    __asm__ volatile("li sp, 0x10000\n\t j firmware_main");
}

__attribute__((noreturn)) void firmware_main(void) {
    init_x1();
    MIDI_CONTROL = 2; // receive enabled, IRQ disabled; firmware polls it
    uart_puts("RV32IMF X1-010 wavetable lab\r\n");
    uart_puts("Type sine, saw, tri, square, draw <16 numbers>, or pcm.\r\n> ");
    for (;;) {
        poll_uart();
        poll_midi();
    }
}
