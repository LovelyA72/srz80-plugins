# PCM DAC card

`dac` is a mono 8-bit unsigned PCM DAC. Its mono signal is copied into both
host stereo channels. It offers a direct DAC latch for the simplest possible
PCM output and a 32-byte FIFO whose consumption speed is programmable.

The default card occupies five I/O addresses beginning at `$D0`. The host
audio rate defines the maximum FIFO rate; with the normal 44,100 Hz host
output, every rate from 1 Hz through 65,535 Hz is valid, including rates above
the host output rate.

## Registers

| Offset | Name | Read | Write |
| --- | --- | --- | --- |
| `+0` | `DATA` | current DAC latch | In direct mode, set the DAC latch. In FIFO mode, append a byte to the FIFO. |
| `+1` | `CONTROL` | bit 0: FIFO mode | bit 0 selects direct (`0`) or FIFO (`1`) mode; bit 1 clears the FIFO. A mode change also clears the FIFO. |
| `+2` | `STATUS` | bits 0–5: FIFO count; bit 6: underflow; bit 7: overflow | Write bit 6 and/or 7 as `1` to clear the corresponding sticky flag. |
| `+3` | `RATE_LO` | FIFO rate low byte | FIFO rate low byte, little endian. |
| `+4` | `RATE_HI` | FIFO rate high byte | FIFO rate high byte, little endian. |

PCM is unsigned: `$80` is silence, `$00` is full negative amplitude, and
`$FF` is full positive amplitude. Direct writes hold their value until replaced.

In FIFO mode, a linear resampler converts the FIFO rate into the host output
rate. It linearly blends adjacent unsigned-PCM bytes, so a 22,050 Hz FIFO at a
44,100 Hz host output produces a midpoint between each pair, while a 65,535 Hz
FIFO remains meaningful even with a 44,100 Hz host. The resampler keeps one
look-ahead byte internally; `STATUS` reports the bytes still resident in the
32-byte FIFO, not that look-ahead byte. An empty FIFO supplies silence and
latches underflow; a write to a full FIFO is discarded and latches overflow.
A written rate of zero is clamped to 1 Hz.

Reset selects direct mode, clears the FIFO and status flags, and writes silence
(`$80`) into the DAC latch. The execution-state format preserves the latch,
mode, FIFO contents and positions, resampler samples and fractional phase, rate,
and flags.

## Project configuration

The optional configuration object currently accepts only a display name for the
host audio mixer:

```json
"config": {"stream_name": "My PCM DAC"}
```
