# RV32I MIDI OPL3 piano

This example polls the MIDI card from RV32I firmware and drives 72 YMF262
two-operator voices across four OPL3 chips. MIDI channels 1–9 and 11–16 are
accepted; General MIDI's percussion channel 10 is intentionally ignored.
The fixed piano patch ignores Program Change and bank selection.

The MIDI parser is shared with the YMW258 example. It handles running status,
interleaved real-time bytes, both forms of Note Off, sustain, sostenuto, soft
pedal, Reset All Controllers, All Notes Off and immediate All Sound Off.
Repeated notes release oldest-first. All Notes Off respects the pedals.
Start and Stop silence the voices, and System Reset restores defaults.

Volume, expression, pan, pitch bend, RPN bend range and fine/coarse tuning,
modulation and channel pressure also affect sounding voices. Pan uses the
OPL3's left/both/right routing, and modulation and pressure enable its fixed
vibrato. Universal Master Volume uses its MSB, and Universal Master Fine/Coarse
Tuning is supported. GM1 System On resets the receiver, and GM System Off
silences it and disables new notes until reset. GS, XG, GM2, reverb and chorus
messages are ignored.

When polyphony is exhausted, the firmware protects the lowest active pitch and
steals the oldest other voice. Edit the
`piano_instrument` table near the top of `piano.c` to change the sound.

Build and run:

```sh
./build_rom.sh
../../build/gcc-debug/bin/srz80_gui project.json
```

Open **Tools > I/O > MIDI**, load `piano.mid` in the MIDI File Player if it is
not already selected, press Play, then resume the rack. The MIDI tool schedules
the file against simulated time, so pausing the rack also pauses the music.

The included `piano.mid` is the original supplied file. It contains an
exporter-added tail after its three declared track chunks; SRZ80 follows common
player behavior and ignores data outside the tracks declared by the SMF header.
