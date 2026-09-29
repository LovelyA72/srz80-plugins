# AY-3-8913 `.ym` music player

An RV32IMF machine that plays a standard uncompressed `.ym` AY-3-8910 register
dump through the AY-3-8913 card. Player code lives in ROM0, the tune in ROM1,
and a Z80 CTC supplies the frame interrupt.

## Cards and memory map

| Address | Card | Purpose |
| --- | --- | --- |
| `0x00000000` | rom | `player.rom` — parser, player, trap handler |
| `0x00004000` | ram | 16 KiB work RAM and stack |
| `0x00008000` | ctc | interrupt-acknowledge alias |
| `0x00200000` | rom | `example.ym` — the `.ym` image |
| `0x10000000` | ay8913 | register select at +0, data at +1 |
| `0x10001000` | ctc | four counter/timer channels, frame tick on channel 0 |

Clock 0 runs at 3.072 MHz. The CTC uses the `/256` prescaler and derives its time
constant from the `.ym` player frequency. Both common rates divide exactly:
50 Hz → 240, 60 Hz → 200.

The CTC shares the engine-level `IRQ` signal with the RISC-V machine external
interrupt. Its acknowledge alias is mapped at `0x8000` because the card restricts
`intack_port` to the 16-bit range; the RAM window ends at `0x7fff`, so the alias
does not overlap anything.

## How it runs

`start.S` sets the stack, installs `trap_handler` in `mtvec`, and unmasks
`mie.MEIE`. `mstatus.MIE` stays clear until `main` has parsed the song.

`main` silences the AY, parses the `.ym` header, writes frame 0, programs CTC
channel 0 (interrupt enabled, timer, `/256`, auto-start, rate from the header)
and then sets `mstatus.MIE`. Each machine external interrupt runs
`trap_handler`, which reads `0x8000` to drop the CTC request and writes one frame
of registers `0..13` to the AY.

## Formats

`player.c` accepts the uncompressed `.ym` variants:

- `YM2!` / `YM3!` — 4-byte header, 14 registers per frame, register-interleaved.
- `YM3b` — as `YM3!` with a 4-byte loop-frame footer.
- `YM4!` — 26-byte header, digidrums, strings, 16 registers per frame.
- `YM5!` / `YM6!` — 34-byte header with master clock, frame rate and loop frame.

`YM2`/`YM3`/`YM3b` carry no frame count, so the player needs the image length;
`build_rom.sh` passes it as `-DMUSIC_IMAGE_SIZE=<bytes of example.ym>`. `YM4`+
read the frame count from the header and do not depend on that constant.

The player writes 14 registers per frame. Register 13 with value `0xFF` is
skipped, which is the format's "leave the envelope running" marker.

## The song

`example.ym` is the song, treated as a read-only input. `build_rom.sh` only reads
it; `make_music.py` refuses to overwrite an existing file unless given `--force`.
The checked-in file is *Goldrunner 2* by David Whittaker: YM5, interleaved,
7839 frames at 50 Hz, 2 MHz AY clock.

## Build

```sh
./build_rom.sh
```

Uses `${RISCV_PREFIX:-riscv64-unknown-elf-}`. It fails with a clear message if
`example.ym` is missing; create a sample tune first with
`python3 make_music.py example.ym`.

## Using your own music

Drop any uncompressed `.ym` in place of `example.ym` and re-run `build_rom.sh`.
The frame rate follows the file's header. Set the AY card's `chip_clock_hz` in
`project.json` to the file's YM5 master clock so pitches match.

Set `PLAYER_FRAME_HZ` to ignore the header and force one rate, for example:

```sh
PLAYER_FRAME_HZ=60 ./build_rom.sh
```

`make_music.py` writes a `YM5!` image with an interleaved 16-register frame
layout and three empty metadata strings.

## Limitations

- Digidrum samples and YM4/YM5 timer effects are parsed but not rendered.
- The `0x8000` acknowledge alias is polled from the firmware's trap handler; it
  is not a Z80-style interrupt-acknowledge cycle.
