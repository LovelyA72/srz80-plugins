# VRC6

Konami VRC6 expansion audio: two pulse channels and one sawtooth channel,
driven directly by the machine's processor clock. The unit is a pure sound
generator - it has no interrupts, no DMA and no bus reads - so the card exposes
one register window and one audio stream.

## Contract

- **Plugin id:** `vrc6`
- **Address space:** `base` … `base+0x2002` (0x2003 bytes), default base
  `$9000`, so the window covers `$9000..$B002` when placed at the chip's own
  addresses. `base` is the address of the chip's `$9000` block.
- **Register decode:** the chip decodes only A0/A1 inside a block and A12/A13
  between blocks, so each block mirrors its registers every four bytes.
  `$B003` (the mapper's mirroring control) is outside the window and is not
  implemented.
- **Reads:** every register is write-only; reads and peeks return `0x00` (open
  bus) and have no side effects.
- **Audio:** mono source duplicated to stereo, `SRH_AUDIO_S16_STEREO`.
- **Reset:** cold and warm reset both restore the power-on state (the chip has
  no reset-sensitive state of its own).

| Address | Register | Bits |
| --- | --- | --- |
| `$9000` | Pulse 1 duty and volume | `MDDD VVVV` (mode, duty, volume) |
| `$9001` | Pulse 1 period low | `FFFFFFFF` |
| `$9002` | Pulse 1 period high | `E... FFFF` (enable, period bits 11..8) |
| `$9003` | Frequency control | `.... .ABH` (256x, 16x, halt) |
| `$A000` | Pulse 2 duty and volume | `MDDD VVVV` |
| `$A001` | Pulse 2 period low | `FFFFFFFF` |
| `$A002` | Pulse 2 period high | `E... FFFF` |
| `$B000` | Saw accumulator rate | `..AA AAAA` |
| `$B001` | Saw period low | `FFFFFFFF` |
| `$B002` | Saw period high | `E... FFFF` |

Pulse frequencies are `clock / (16 * (period + 1))`; the saw divides by 14
instead of 16. Clearing a channel's enable bit silences it and re-enabling
restarts its sequencer from the beginning, which is how software retriggers a
channel.

## Config keys

`chip_clock_hz` (1000000..20000000, default 1789773), `sample_rate`
(8000..192000, default 44100), `gain_milli` (1..20000, default 500),
`stream_name` (string, default `"VRC6"`), `volume_pulse1`, `volume_pulse2`,
`volume_saw` (0..200 percent, default 100). Unknown keys and wrong types are
rejected. `gain_milli` and the three volumes are live-editable from the Device
Inspector; the stream contract (`sample_rate`, `stream_name`) is fixed at
creation.

## Properties

Configuration mirrors the config keys above. Read-only runtime state is
exposed for inspection: per pulse `Enabled`, `Mode`, `Duty`, `Volume`,
`Period`, `Step`, `Output`; for the saw `Enabled`, `Rate`, `Period`,
`Accumulator`, `Output`; and chip-wide `Halt` and `FreqShift`
(`off|16x|256x`). Runtime properties are never written to the project.

## Fidelity notes

Written clean-room from the published chip description (NESdev wiki,
[VRC6 audio](https://www.nesdev.org/wiki/VRC6_audio), which also cites Kevin
Horton's VRCVI chip notes) and behaviourally cross-checked against the
nsfplay VRC6 core. No code from either source is used here.

- Pulse duty: the generator walks 16 steps and emits the volume for the last
  `(duty + 1)` of them; the mode bit emits the volume unconditionally. The
  resulting duty ratios are 1/16 .. 8/16 as tabulated on the wiki.
- Saw ramp: 14 ramp clocks per waveform. The 8-bit accumulator only reacts on
  every second ramp clock: the 6-bit rate is added six times and the seventh
  reaction clears the accumulator. Only accumulator bits 7..3 are output (a
  0..31 DAC). Rates above 42 wrap the accumulator and distort, as on the
  hardware.
- `$9003`: halt freezes every sequencer in place while the outputs keep their
  current levels; the 16x/256x flags shift every period right by 4/8 bits
  (256x wins over 16x), raising pitch by 16x/256x.
- The period divider is a per-channel clock accumulator compared against the
  programmed period, so it steps the sequencer every `period + 1` clocks and
  tracks pitch bends immediately. When a period shrinks below the accumulated
  count the divider is clamped to the new period, so a shrunk period retriggers
  promptly instead of bursting out several steps.
- While a channel is disabled its divider and sequencer are held in place and
  its output is forced to zero (the wiki leaves the divider's behaviour here
  open; this matches the nsfplay core).
- The DAC is linear: the mixer sums the two 4-bit pulse levels and the saw's
  5-bit level (a 0..61 sum), scales by the channel volumes and `gain_milli`,
  and emits that magnitude in the same polarity the reference mixer sums with
  its 2A03. On the real chip the DAC is inverted relative to the 2A03.
- Save state uses the shared SDK envelope with a 45-byte little-endian payload:
  the render's eight-byte carried clock remainder and 37 bytes of core fields.
  Loading validates the image before changing the unit
  on any bad byte. This card models the audio unit only - the VRC6's mapper,
  banking and mirroring-control registers are not implemented.

## Example

```json
{
  "plugin": "vrc6",
  "space": "cpu0.memory",
  "base": "0x9000",
  "size": "0x2003",
  "config": {"chip_clock_hz": 1789773, "sample_rate": 44100, "stream_name": "VRC6"}
}
```
