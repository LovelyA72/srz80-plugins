# AY-3-8913

General Instrument AY-3-8913 programmable sound generator (PSG). Three square
tone channels, one noise generator, and one shared envelope generator. The
8913 is the audio-only variant: BC2 is tied high internally and it exposes no
GPIO ports (registers `$0E`/`$0F` are stored but inert).

## Contract

- **Plugin id:** `ay8913`
- **Address space:** `base` … `base+1` (2 bytes, I/O)
- **Bus (BC2 high):**
  - write `base+0` → latch register select (BDIR=1, BC1=1)
  - write `base+1` → write data (BDIR=1, BC1=0)
  - read  `base+1` → read latched register (BDIR=0, BC1=1); `0xff` when inactive
  - read  `base+0` → `0xff` (address state is high-impedance)
- **`data_first`:** when `true`, the data port is `base+0` and the latch port is
  `base+1` (MAME `data_address_w`, BC1 wired to A0). Default `false` matches
  MSX/CPC/Spectrum-128 and the YM2413 card (`address_data_w`).
- **Register select:** upper nibble must be `0000` (A7–A4 mask); otherwise the
  chip goes inactive and further data writes are ignored.
- **Clock:** `chip_clock_hz`, internally divided by 8. Default `1789773`
  (1.7897725 MHz).
- **Audio:** mono source duplicated to stereo, `sample_rate` default `44100`.
- **Channel mute:** the Device Inspector exposes a live `Mute` control for each
  channel. Muting is applied only while mixing audio; it does not change the
  PSG register file and remains effective while the simulation is running.

## Config keys

`chip_clock_hz` (1000000..20000000, default 1789773), `sample_rate`
(8000..192000, default 44100), `data_first` (bool, default false),
`stream_name` (string, default `"AY-3-8913"`).

## Fidelity notes

- Ported from MAME `ay8910.cpp` (`PSG_TYPE_AY`): 16-step envelope
  (`m_step = 2`), `zero_is_off`, and the `ay8910_param` DAC curve. Tone
  period 0 is treated as 1; the envelope period 0 is not halved.
- Output mixes the three legacy-normalized channels by summing them into a
  single mono stream. The 3-channel resistor-network (`mix_3D` / 3D table) is
  not modeled; a future upgrade could replace the sum with that table.
- The register file is readable through the data port for debug/state parity
  (MAME does the same); on real 8913 hardware the only defined reads are the
  nonexistent I/O ports, so this read path is an emulation convenience.
- State save/load serializes latch, activity, registers, tone phase, envelope
  phase, noise LFSR, and the render fractional accumulator.
