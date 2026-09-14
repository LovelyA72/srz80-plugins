# X1-010 card

This card exposes the Seta/Allumer X1-010 as an audio device. Its bus mapping
is exactly `0x2000` bytes:

- `0x0000..0x007f`: 16 voice register blocks
- `0x0080..0x0fff`: envelope RAM
- `0x1000..0x1fff`: wavetable RAM

The card owns that register/envelope/wavetable RAM. PCM playback uses the
X1-010's 20-bit sample address and reads the bytes through the SRZ80 host bus.
Therefore the sample memory should be supplied by a separate RAM card, in the
same memory space as the CPU. For an RV32IMF machine, a typical arrangement is
1 MiB of RAM at `0x00100000`, with the X1-010's `sample_base` set to the same
address:

```json
{
  "plugin": "ram",
  "space": "rv.memory",
  "base": "0x00100000",
  "size": "0x100000"
},
{
  "plugin": "x1_010",
  "space": "rv.memory",
  "base": "0x00020000",
  "size": "0x2000",
  "config": {
    "chip_clock_hz": 16000000,
    "sample_rate": 44100,
    "sample_base": "0x00100000",
    "sample_space": "rv.memory",
    "stream_name": "X1-010"
  }
}
```

`sample_space` is optional. When omitted, it uses the X1-010 register mapping's
space. `sample_base` identifies the first byte of the shared sample window; the
card does not map or allocate a second copy of that 1 MiB.

## Third-party core and license

`x1_010/x1_010.cpp` and `x1_010/x1_010.hpp` are modified copies of the
[vgsound_emu X1-010 core](https://gitlab.com/cam900/vgsound_emu/) from
`C:\github\furnace\extern\vgsound_emu-modified\vgsound_emu\src\x1_010`.
The original core is copyright (C) 2022-present cam900 and contributors and is
licensed under the Zlib License.

SRZ80's changes add explicit, little-endian save/load-state serialization,
state-size declarations, and flag packing helpers so the core can participate
in the card ABI's state persistence. These altered source files are not the
original vgsound_emu sources; their Zlib license notices are retained in the
files.
