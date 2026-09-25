# SCM3LT album player (DigiCommunication Nyo)

This demo plays the DigiCommunication Nyo (Digi Charat 2, GBA) soundtrack with
nothing but the stock `script` and `dac` cards. A Lua script decodes M2's SCM3LT
(one of the best GBA music driver. RIP Akira Saito san...) sequence bytecode,
reads the game's sample bank at runtime, resamples each voice
with the driver's own pitch table and mixes the result into two PCM DACs.

The example is a companion to the style used by
[`script-lua-xm`](../script-lua-xm) and [`script-mod-player`](../script-mod-player),
except that the sequence format is a game-specific driver instead of a module
file.

## Extract your own data!

This demo does not come with any copyrighted material from M2. You need to dump
your own ROM of `Di Gi Communication 2: Datou! Black GemaGema Dan!` from your legally
owned GBA cartridge and extract the data using `tools/extract_assets.py`.

## Running it

1. Open `project.json` in SRZ80 with the `script` and `dac` cards installed.
2. Edit `song.txt` and set a two-digit hexadecimal GSF song index.
3. Reload the project (or stop and start the rack) so the script reloads the
   file. `08` is *この指とまれ!*, `0A` is *Happy Day*, `01` is *君とHAPPY!*.

`TRACKS.md` lists all 36 tracks in the album. Both DACs run at 22 919 Hz, which
is what the driver programs into GBA Timer 0. The host mixer sends one DAC to
each stereo output.

## Files

| File | Purpose |
| --- | --- |
| `project.json` | Left and right `dac` cards at `scm3lt.io`+0x10 and +0x20, plus one `script` card. |
| `player.lua` | SCM3LT decoder and mixer. |
| `song.txt` | Selected song index (hex). |
| `TRACKS.md` | Song index to title map. |
| `tools/extract_assets.py` | Regenerates the data files from a copy of the ROM. |

## Format notes

The bank starts with 16 little-endian track pointers for each song; each song
table is 96 bytes. Track bytecode is a stream of commands:

- `0x01..0x7E` note (`+15` = MIDI key), `0x7F` rest, `0x00` end or loop back to
  the last `0xCF` marker, followed by a variable-length count (7 bits per byte,
  high bit set continues).
- `0x80` volume, `0x81` transpose (16-bit signed), `0x82` instrument,
  `0x87` tempo, `0x94` gate/release.
- `0xC0`/`0xC1`/`0xC2` repeat start/break/end, `0xCF` loop marker.

Sample records are 16 bytes at `bank + 0x6010 + index*16`: control flags,
length, loop or shift field, and the byte offset of the raw data. Instruments
`0x01..0x7B` use `index = instrument - 1`; instrument `0xC8` selects a drum
record from the note number. The record flag selects the voice format:

| Flag | Format |
| --- | --- |
| `2` | 8-bit signed, loops over the whole record. |
| `6` | 8-bit signed, one shot. |
| `8` | 8-bit signed block format. |
| `11` | 4-bit delta nibbles, one nibble per output sample. The length field counts nibbles (the drum bank uses up to ~966 000), so the script streams them instead of pre-decoding a record. |
| `12` | 8-bit signed, loops from the record's loop field. |

Two driver quirks matter for the mix and are easy to get wrong:

- The current voice level is `volume - song_attenuation`. Levels `1, 7, 13, 19,
  25, 31` are special: the volume field holds a shift amount and the mixer
  multiplies the sample by `2^shift`, so those levels are `8, 16, 32, 64, 128,
  256`. Treating the shift as a multiplier makes entire string and brass
  sections 8-32x too quiet.
- Note counts are in driver ticks, and the sequencer advances `tempo/128` ticks
  per video frame, so the script derives the DAC block schedule from the GBA
  frame length (280 896 cycles) instead of the host clock.
- Commands `0x98`, `0x99`, and `0x9A` route a track left, right, and center.
  The driver's side channels also contribute to the opposite output at half
  level; the script applies the same weighting before writing the two DACs.

## Regenerating the data

`tools/extract_assets.py` reads a raw GBA ROM (16 MiB) of DigiCommunication Nyo
(game code `BDKJ`) and writes `music_bank.bin`, `pitch_table.bin` and
`gain_table.bin`. The per-song attenuation values are small measured driver
parameters and are embedded in the tool; passing a directory of ripped
`.minigsf` files regenerates `TRACKS.md` from their tags instead.

The music data belongs to M2 and Broccoli and is included only so the example is
runnable, in the same spirit as the third-party module files shipped with the
other player demos.

## Limitations

The mixer is an approximation of the driver: the per-note attack ramp and
release level are not modelled, and commands that only change effect state are
skipped after their operands. Voice formats and the volume model follow the
original mixer, which is what makes the melody, drums and sustained sections
recognizable.

The SCM3LT command grammar was cross-checked against
[loveemu's DC2.java](https://github.com/loveemu/loveemu-lab/blob/master/gba/scm3Mus/src/scm3Mus/DC2.java);
the sample records, pitch/gain tables and volume rules above come from tracing
the driver in the ROM.
