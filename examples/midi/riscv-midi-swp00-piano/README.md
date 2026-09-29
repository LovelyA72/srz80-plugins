# RISC-V MIDI SWP00 piano

A freestanding RISC-V GM1 receiver for the Yamaha SWP00 card, based on
[`riscv-midi-ymw258-piano`](../riscv-midi-ymw258-piano). It reads the MU50's
factory instrument and sample tables directly from its controller ROM and
plays the original compressed/PCM wave ROMs through the SWP00.

## Run

**You must provide your own Yamaha MU50 ROM dumps.** I do not want Yamaha lawyers
to bust open my door and find my collection of YM2414 and Taiwanese YM2413 clones.

How about this... I can give you a hint. (whisper) `mu50.7z`

| File | Size | Purpose |
| --- | ---: | --- |
| `xr174c0.ic7` | 512 KiB | MU50 v1.05 controller ROM and instrument tables |
| `xq057c0.ic18` | 2 MiB | First wave ROM |
| `xq058c0.ic19` | 2 MiB | Second wave ROM |

The v1.02 and v1.04 controller dumps are not supported.
Do not word-swap the files yourself:
`gm_rom()` handles the controller dump's byte order, and the SWP00 card
concatenates the wave ROMs in the order listed above.

With Python 3 and GNU RISC-V bare-metal tools installed:

```sh
python3 verify_roms.py
./build_rom.sh
```

`RISCV_PREFIX` overrides the default `riscv64-unknown-elf-` tool prefix.
The build produces `piano-rv32i.rom` and `piano-rv32imf.rom`, with Zicsr for
the boot animation's cycle counter. The linker rejects firmware that overlaps
Work RAM. ROM files and build products are git-ignored.

Open `project.json` in SRZ80 with the `swp00`, `riscv`, `rom`, `ram`, `midi`,
`uart_console`, and `lcd1602` plugins available. The project selects the
RV32IMF image; change the firmware card image to `piano-rv32i.rom` to use
the base-integer build.

After the two-second LCD intro:

- Send MIDI to `swp00-piano.midi` using the MIDI tool's keyboard, MIDI-file
  player, or connected input. The project opens Yamaha's original MU50 demo,
  `Demo MIDI_Yamaha MU Series_MU50_TheMusithm.mid`, by default.
- Use the UART console endpoint `swp00-piano.uart` to control channel 1. Enter
  **0–127** and Return to select its program, `R0`–`R127` and Return to set its
  reverb send, or `C0`–`C127` and Return to set its chorus send. Use `w` / `s`
  for the next / previous program.
- The LCD shows channel 1's eight-character MU50 ROM voice name (for example,
  `GrandPno`), the current `GM`/`XG` operating mode in the last two cells of
  the first line, and activity for all 16 channels.
- In GM mode, channel 10 is fixed to the MU50 XG Standard Kit's GM percussion
  assignments. In XG mode, bank selection and part mode may switch it between
  rhythm and melodic voices.

## Receiver behavior

- 128 melodic programs, keyboard/velocity splits, up to two preset layers,
  and 32 hardware voices. Released voices are reused before held notes;
  optional layers are reclaimed before primary voices.
- XG bank select and Program Change, including MU50 variation voices, SFX
  voices, drum kits, and SFX kits on any MIDI channel.
- Velocity, pitch bend, modulation, channel pressure, volume, expression, pan,
  sustain, GM reverb send (CC 91), and GM chorus send (CC 93). Hall 1 and
  Chorus 1 are fixed MU50-compatible MEG programs; controller changes also
  affect voices which are already sounding. Reset All Controllers restores
  the GM defaults (reverb 40, chorus 0).
- RPN pitch-bend range, fine tuning and coarse tuning; Reset All Controllers,
  All Notes Off and immediate All Sound Off.
- Running status, interleaved real-time messages, Yamaha XG System On and part
  mode, GM System On/Off, Roland GS Reset (treated as GM mode), System Reset,
  and Universal Master Volume (MSB resolution).
- Drum alternate groups (including hi-hats), single/multiple assignment,
  and per-drum Note Off reception. Drum tuning follows the v1.05 firmware:
  subtract the sample root note and apply the factory semitone correction.

This is an XG-compatible example receiver, **not a complete MU50 firmware
emulation**. Amplitude envelopes and controller response
are simplified. Factory filter/pitch envelopes, velocity curves, envelope
key-on delays and vibrato delay/fade are not reproduced. The filter is open and
output uses the initialized dry mixer
at approximately −8 dB (before preset, envelope and MIDI volume attenuation).
Reverb and chorus use fixed Hall 1 and Chorus 1 programs. Selectable XG effect
programs, variation and insertion effects, effect-parameter SysEx, and GS
effect messages are not yet implemented; unsupported messages are safely ignored.
Factory sample data, sample formats, sample splits, loop parameters and tuning
come from the ROMs. SWP00 core emulation limits also apply.

## Memory map

| Address | Device |
| --- | --- |
| `0x00000000`–`0x00003fff` | Firmware ROM |
| `0x00004000`–`0x00007fff` | Work RAM; stack starts at `0x8000` |
| `0x10000000`–`0x100007ff` | SWP00 registers |
| `0x10001000`–`0x10001001` | UART console |
| `0x10001100`–`0x10001103` | MIDI input |
| `0x10001200`–`0x10001201` | LCD1602 |
| `0x20000000`–`0x2007ffff` | Raw MU50 controller dump |

The peripherals sit outside the SWP00's 2 KiB register window. CPU clock is
4 MHz; SWP00 clock is 33.8688 MHz, giving 44.1 kHz synthesis. The two wave
ROMs reside in the card's private sample memory.

## References

- [MUTable `mu50.cpp`](https://github.com/TaleTN/MUTable/blob/main/mu50.cpp),
  Theo Niessink: controller byte order, table offsets and record layouts.
  Reference revision `6bae60c547cb9e04793cfed51e5e773f6cf27419` (WTFPL v2). The receiver is implemented
  locally; it does not require MUTable or WDL at build/run time.
- [MAME 0.289 MU50 driver](https://github.com/mamedev/mame/blob/mame0289/src/mame/yamaha/ymmu50.cpp):
  ROM identities, ordering and machine configuration.
- MU50 v1.05 controller routines `0x0240a6` (drum tuning), `0x04ce1c`
  (pitch encoding), `0x04cf66` (voice register writes), and its cent/volume
  lookup tables. Drum record byte `+18` is a root note; `+28` and `+29` are
  alternative semitone corrections for factory and neutral-XG pitch bases,
  respectively. They must not be combined as coarse/fine tuning.
- [SWP00 card](../../plugins/cards/swp00/README.md): register interface,
  core provenance and emulation limits.
