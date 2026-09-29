/* SRZ80 RV32IMF VGM player for the YM2414 (OPZ) card.
 *
 * This is a bare-metal program with no operating system, C library, or device
 * driver. The rack in project.json maps every card straight into the RV32IMF
 * address space, and `volatile` device pointers make each C access a real bus
 * transaction that the compiler is forbidden to cache or delete.
 *
 *   0x00000000  ROM0  this firmware (VGM reader + OPZ driver)
 *   0x00004000  RAM   16 KiB of work RAM; the stack grows down from 0x8000
 *   0x00100000  ROM1  the raw test.vgm file, loaded byte-for-byte
 *   0x10000000  OPZ   YM2414 address latch (+0) and data write (+1)
 *
 * The VGM file is a recording of the register writes that originally drove a
 * YM2151 (which shares the OPZ write protocol). This firmware walks the command
 * stream, forwards each register write to the YM2414, and advances the clock
 * exactly as the file's timestamps request. When the stream ends it rewinds to
 * the header's loop point and keeps playing, so the tune repeats forever.
 */

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

#define VGM_ROM ((const u8 *)0x00100000u)

/* The YM2414 exposes a two-byte host interface: latch a register index, then
 * write the data byte. The card maps those two ports at base + 0 and base + 1.
 */
#define OPZ_ADDR (*(volatile u8 *)0x10000000u)
#define OPZ_DATA (*(volatile u8 *)0x10000001u)

/* VGM command bytes. The 0x54 group is "write a register on the chip that used
 * the YM2151 protocol"; test.vgm uses it exclusively.
 */
#define VGM_WRITE 0x54u
#define VGM_WAIT  0x62u /* wait 735 samples; takes no operand */
#define VGM_END   0x66u /* end of the command stream */

/* Header fields, each holding an offset relative to the field itself: the
 * command stream starts at 0x34 + data_offset, and the tune repeats from
 * 0x1C + loop_offset. A zero loop offset means "play once".
 */
#define VGM_HEADER_LOOP_OFFSET 0x1Cu
#define VGM_HEADER_DATA_OFFSET 0x34u

/* Timing model.
 *
 * VGM timestamps are counted in 44100 Hz samples. The short wait command 0x62
 * advances time by exactly 735 samples (one 60 Hz field), and the file's header
 * total_sample_count equals the sum of every wait, so playback costs a known
 * number of samples end to end.
 *
 * project.json runs the CPU at clocks[0] = 705600 Hz = 44100 * 16, so each
 * output sample spans exactly CYCLES_PER_SAMPLE CPU cycles and the wait above
 * becomes 735 * 16 = 11760 cycles. Pacing from the `cycle` CSR keeps playback
 * locked to simulated time instead of to how fast the host happens to run the
 * emulator, so the music is identical at any speed. (16 was chosen with margin:
 * the densest run in the file is 321 back-to-back register writes, which still
 * fits inside a single 735-sample window at this rate.)
 */
#define CYCLES_PER_SAMPLE 16u
#define WAIT_SAMPLES      735u

/* Read the machine cycle counter, which advances once per executed
 * instruction. It requires the Zicsr extension and is declared explicitly in
 * build_rom.sh.
 */
static u32 cycle_count(void) {
    u32 value;
    __asm__ volatile("csrr %0, cycle" : "=r"(value));
    return value;
}

/* VGM is little-endian; read the header fields byte by byte so this works at
 * any alignment without needing unaligned loads.
 */
static u32 read_u32_le(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static void opz_write(u8 reg, u8 value) {
    /* Latch the register index first, then commit the data byte. */
    OPZ_ADDR = reg;
    OPZ_DATA = value;
}

__attribute__((noreturn, noinline, used)) void firmware_main(void) {
    const u8 *header = VGM_ROM;
    /* The stream does not begin at a fixed offset. Field 0x34 holds the number
     * of bytes from 0x34 itself to the first command; a zero value is the
     * pre-1.51 default of 0x40. Reading it lets the same firmware accept any
     * VGM 1.51+ file, not just this one.
     */
    const u32 data_offset = read_u32_le(header + VGM_HEADER_DATA_OFFSET);
    const u8 *cursor =
        header + (data_offset ? VGM_HEADER_DATA_OFFSET + data_offset : 0x40u);

    /* A VGM with a non-zero loop offset repeats from that point forever: the
     * loop section follows the end of the stream as a seamless musical
     * continuation, so playback returns here instead of stopping.
     */
    const u32 loop_offset = read_u32_le(header + VGM_HEADER_LOOP_OFFSET);
    /* Null for a file with no loop point (a zero offset), which plays once. */
    const u8 *loop_point = 0;
    if (loop_offset) {
        loop_point = header + VGM_HEADER_LOOP_OFFSET + loop_offset;
    }

    /* Absolute cycle deadline for the next wait. Playback starts at time 0, so
     * the opening register writes are applied before the first spin. The
     * deadline never resets, which is what makes the loop point seamless.
     */
    u32 deadline = 0;
    for (;;) {
        const u8 command = *cursor++;
        if (command == VGM_WRITE) {
            const u8 reg = *cursor++;
            const u8 value = *cursor++;
            opz_write(reg, value);
        } else if (command == VGM_WAIT) {
            deadline += WAIT_SAMPLES * CYCLES_PER_SAMPLE;
            /* Spin until simulated time catches up with the deadline. The
             * signed difference makes the comparison robust against a wrapping
             * 32-bit counter, and the volatile asm forces a fresh read on every
             * trip so the loop cannot be optimised away.
             */
            while ((int)(cycle_count() - deadline) < 0) {
            }
        } else if (command == VGM_END && loop_point) {
            /* The stream reached its end marker. Rewind to the loop point and
             * keep the deadline running so the repeat is continuous in
             * simulated time, exactly like a looping VGM player.
             */
            cursor = loop_point;
        } else {
            /* A file with no loop point ends here. Any other byte would
             * desynchronise the parser and pour noise into the chip, so stop
             * instead of guessing.
             */
            break;
        }
    }

    /* Nothing left to play. ECALL has no runtime to call, so the CPU card
     * treats it as a clean stop: the rack halts here and reports
     * "RISC-V ECALL" rather than spinning forever.
     */
    __asm__ volatile("ecall");
    for (;;) {
    }
}

__attribute__((naked, section(".text.start"), noreturn)) void _start(void) {
    /* A freestanding image has no C runtime to set up the stack. Place SP at the
     * top of the 16 KiB RAM card and tail-jump into C so the compiler's
     * prologue never touches uninitialised stack. `naked` suppresses that
     * prologue.
     */
    __asm__ volatile("li sp, 0x8000\n"
                     "tail firmware_main\n");
}
