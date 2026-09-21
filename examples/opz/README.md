# RV32IMF VGM player for the YM2414 (OPZ)

This example plays `test.vgm` on a YM2414 (OPZ) card using only bare-metal
RISC-V firmware. Five cards cooperate:

| Card | Plugin | Purpose |
| --- | --- | --- |
| ROM0 | `rom` | The firmware: VGM command reader plus OPZ driver |
| RAM | `ram` | 16 KiB of work RAM; the stack grows down from `0x8000` |
| ROM1 | `rom` | `test.vgm`, loaded byte-for-byte as a memory image |
| CPU | `riscv` | RV32IMF hart that runs the firmware |
| Chip | `ym2414` | Yamaha YM2414 (OPZ) FM synthesizer |

`test.vgm` is a recording of the register writes that originally drove a
YM2151; the YM2414 shares the OPZ write protocol, so the firmware simply
forwards every logged register write to the card while keeping the file's
timing.

## Run

With GNU RISC-V bare-metal tools installed:

```sh
./build_rom.sh
../../build/gcc-debug/bin/srz80_gui project.json
```

`RISCV_PREFIX` overrides the default `riscv64-unknown-elf-` tool prefix.
The build writes `opz-vgm-player-rv32imf.rom`, which the linker refuses to
place if it would overlap Work RAM.

Resume the rack and the tune plays from start to finish. There is no
interactive control: the firmware boots, replays the file, and stops the rack
with an `ECALL` when the stream ends.

The same rack runs headless, which is also how it is tested:

```sh
cd ../../scratch/card-test                                   # loader + plugins
./srz80 ../../examples/opz/project.json --run 5s --wav out.wav
```

`--run 100s` stops early at `~96.02 s` with the message
`Card 11 requested stop: RISC-V ECALL`; the recorded WAV holds exactly
4234335 frames, matching the VGM's declared sample count.

## Memory map

| Address | Device |
| --- | --- |
| `0x00000000`–`0x00003fff` | ROM0, the firmware |
| `0x00004000`–`0x00007fff` | Work RAM; the stack starts at `0x8000` |
| `0x00100000`–`0x0013523b` | ROM1, the raw `test.vgm` image (217660 bytes) |
| `0x10000000`–`0x10000001` | YM2414 address latch and data write |

## The VGM reader

The firmware reads the stream offset from header field `0x34` (`0x40` when the
field is zero, per the pre-1.51 default) and then walks three commands:

| Byte | Meaning |
| --- | --- |
| `0x54 rr vv` | Write `vv` to chip register `rr` |
| `0x62` | Wait 735 samples (one 60 Hz field), no operand |
| `0x66` | End of stream |

Any other byte would desynchronise the parser, so the firmware stops rather
than feeding the chip garbage. `test.vgm` uses 70475 `0x54` writes, 5761 `0x62`
waits and a single `0x66`; 5761 × 735 = 4234335 samples, exactly the
`total_sample_count` in the header — about 96.02 seconds at 44100 Hz.

## Timing

VGM timestamps count 44100 Hz samples. The rack runs the CPU at
`clocks[0] = 705600` Hz = `44100 × 16`, so one output sample spans exactly 16
CPU cycles and one `0x62` wait is 11760 cycles. The firmware paces itself from
the `cycle` CSR (`CYCLES_PER_SAMPLE` in `opz_vgm.c`), so playback is locked to
simulated time rather than to host speed: the music is identical whether the
emulator runs faster or slower than real time. The busiest run in the file is
321 back-to-back register writes, which still fits inside a single 735-sample
window at 16 cycles per sample.

`CYCLES_PER_SAMPLE` and `clocks[0]` must stay in step; changing one without the
other retimes the music.

## Credits

Test VGMs: SnugglyBun, Yousuke Yasui
Exported from Furnace
