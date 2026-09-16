# SWP00 card

Yamaha SWP00 AWM2 sampler and integrated MEG effects, adapted from MAME 0.289.
Provides 32 voices, 16-bit / packed 12-bit / 8-bit / differential compressed
samples, envelopes, filters, LFOs, reverb, chorus and four variation programs.

## Rack configuration

Plugin ID: `swp00`. Build output: `audio_swp00.dll` (Windows) or
`libaudio_swp00.so` (Linux). The card maps **2048 bytes**, by default at
`0x400000`, in the selected address space. Use a space large enough for this
mapping, or choose a different base.

```json
{
  "plugin": "swp00",
  "space": "cpu0.memory",
  "base": "0x400000",
  "size": 2048,
  "image": "wave.bin",
  "config": {"chip_clock_hz": 33868800, "stream_name": "SWP00"}
}
```

One nonempty sample ROM is required. Alternatively, `images` accepts up to four
ordered ROM parts concatenated into the chip's private sample memory, with a
combined maximum of 16 MiB. Empty trailing slots are allowed. Unpopulated bytes
read as `0xff`; addresses wrap at 24 bits. No Yamaha ROM is included.

`chip_clock_hz` accepts integers from 1,000,000 to 50,000,000. Audio runs at the
nearest integer rate to clock / 768 (44,100 Hz at the default clock). The host
audio scheduler advances synthesis; no CPU clock slot is consumed. `stream_name`
must be a nonempty string of at most 256 bytes. Other config keys are rejected.

This is a register-level chip. Guest software must load voice parameters,
sample addresses, and MEG coefficients. It does not parse MIDI, implement the
MU50 firmware, or select instruments from a wave ROM automatically. Reset clears
MEG coefficients, including dry output gains, so key-on alone is silent.

## Register interface

All addresses below are relative to the card base; accesses are 8-bit.

| Address | Function |
| --- | --- |
| `0x001` | Write internal-state selector; read selected voice state |
| `0x002` | Wave ROM access control; status reads ready |
| `0x003` | Read ROM at voice 31's address, then increment that address |
| `0x004` | MEG control; bits 7–6 select variation program 0–3 |
| `0x008`–`0x00b` | Key-on bitmasks for voices 24–31, 16–23, 8–15, 0–7 |
| `0x180`–`0x1ff` | 64 MEG delay offsets, high byte first |
| `0x200`–`0x37f` | 192 MEG coefficient registers, high byte first |

Voice register address: `((slot & 0x3e) << 5) | (voice << 1) | (slot & 1)`.
Voice indices are 0–31. Slots `0x08`–`0x0b` hold phase and pre-loop length;
`0x20`–`0x29` hold filter/LFO/envelope parameters; `0x2a`–`0x2f` hold effect
sends, dry level, global level and pan; `0x30`–`0x37` hold format, sample
address, pitch and loop length. The source header documents the field layout.
Unsupported registers read zero and ignore writes. Peeks never advance ROM
addresses or synthesis. Both cold and warm reset restore the same chip defaults.

State snapshots include all voice histories, LFOs, delay memories, coefficients,
and output meters in a versioned little-endian format. ROM contents come from
the project's images. Restore requires the same configured chip clock.

## Inspector

`swp00_inspector` appears under **Audio → SWP00 Inspector**. It follows the
YMW258 inspector's card selector, scrolling voice table, miniature keyboards,
envelope and volume meters. Its columns show SWP00 pitch, sample format/address,
pre-loop and loop lengths, envelope rates, LFO parameters and effect sends.

The keyboard shows playback pitch relative to C4 at unity rate; it cannot infer
the recorded sample's root note. Pan shows left/right attenuation nibbles; zero
means no attenuation and `F` mutes that side. Send/global values also represent
attenuation. The volume meter measures the voice's dry contribution before MEG
mixing, so a voice routed only to effects can have a zero dry meter.

## Emulation limits and attribution

MEG is MAME's reconstruction of fixed internal programs, not an instruction
interpreter. The original implementation does not apply the control register's
bit 1 mute behavior; this port preserves that limitation. Hardware-exact effects
and all MU50 factory presets have not been verified.

The [MAME](https://github.com/mamedev/mame) SWP00 emulation code is copyright
Olivier Galibert, modified for SRZ80.
