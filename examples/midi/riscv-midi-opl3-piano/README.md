# RV32I MIDI OPL3 piano

This example polls the MIDI card from RV32I firmware and drives 72 YMF262
two-operator voices across four OPL3 chips. MIDI channels 1–9 and 11–16 are
accepted; General MIDI's percussion channel 10 is intentionally ignored.
Sustain pedal (CC 64), All Sound Off (CC 120), All Notes Off (CC 123), velocity,
running status, and both forms of note-off are supported.

When polyphony is exhausted, the firmware protects the lowest active pitch and
steals the oldest other voice. The editable instrument is the clearly annotated
`piano_instrument` table near the top of `piano.c`.

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
