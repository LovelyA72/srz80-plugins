#!/usr/bin/env python3
"""Generate a standard uncompressed .ym (YM5) register dump for the AY-3-8913
player example.

The output is a plain AY-3-8910 register dump: a 34-byte YM5 header, three empty
metadata strings, one 16-byte frame per tick, and the "End!" trailer.  Frames
are authored at 60 Hz, matching the player's 60.0 Hz CTC frame interrupt.

Usage: make_music.py [--force] [output]      (default: example.ym)
"""
import os
import struct
import sys

AY_CLOCK_HZ = 2_000_000  # YM5 master clock; must match the card's chip_clock_hz
FRAME_RATE = 60
CHORD_FRAMES = 60        # one second per chord
ARP_FRAMES = 10          # arpeggio step

# (bass note, arpeggio notes, sustained fifth)
CHORDS = [
    ("A2", ["A4", "C5", "E5", "C5"], "E4"),
    ("F2", ["F4", "A4", "C5", "A4"], "C4"),
    ("C3", ["C5", "E5", "G5", "E5"], "G4"),
    ("G2", ["G4", "B4", "D5", "B4"], "D4"),
]

SEMITONES = {"C": 0, "C#": 1, "D": 2, "D#": 3, "E": 4, "F": 5,
             "F#": 6, "G": 7, "G#": 8, "A": 9, "A#": 10, "B": 11}


def note_hz(name):
    pitch = SEMITONES[name[:-1]]
    octave = int(name[-1])
    return 440.0 * 2.0 ** ((pitch + (octave - 4) * 12 - 9) / 12.0)


def period(freq):
    # AY tone frequency: f = clock / (16 * period)
    return max(1, min(0xFFF, round(AY_CLOCK_HZ / (16.0 * freq))))


def frame(arp_note, bass_note, pad_note, arp_volume, bass_volume, pad_volume):
    arp = period(note_hz(arp_note))
    bass = period(note_hz(bass_note))
    pad = period(note_hz(pad_note))
    return [
        arp & 0xFF, (arp >> 8) & 0x0F,     # R0/R1 channel A period
        bass & 0xFF, (bass >> 8) & 0x0F,   # R2/R3 channel B period
        pad & 0xFF, (pad >> 8) & 0x0F,     # R4/R5 channel C period
        0x1F,                              # R6 noise period (unused)
        0x38,                              # R7 mixer: tone A/B/C on, noise off
        arp_volume & 0x0F,                 # R8 channel A volume
        bass_volume & 0x0F,                # R9 channel B volume
        pad_volume & 0x0F,                 # R10 channel C volume
        0x00, 0x00,                        # R11/R12 envelope period
        0x00,                              # R13 envelope shape
    ]


def build_frames():
    frames = []
    for chord in CHORDS:
        bass, arp_notes, pad = chord
        for offset in range(CHORD_FRAMES):
            arp = arp_notes[(offset // ARP_FRAMES) % len(arp_notes)]
            # Slightly accent the first step of each chord.
            accent = 15 if offset < ARP_FRAMES else 13
            frames.append(frame(arp, bass, pad, accent, 11, 8))
    return frames


def build_ym5(frames):
    count = len(frames)
    header = bytearray()
    header += b"YM5!"
    header += b"LeOnArD!"
    header += struct.pack(">I", count)
    header += struct.pack(">I", 1)               # attributes: interleaved
    header += struct.pack(">H", 0)               # digidrum count
    header += struct.pack(">I", AY_CLOCK_HZ)     # master clock
    header += struct.pack(">H", FRAME_RATE)      # player frequency
    header += struct.pack(">I", 0)               # loop frame (0 = start)
    header += struct.pack(">H", 0)               # extra data size
    header += b"\x00\x00\x00"                    # song, author, comment

    body = bytearray()
    for reg in range(16):                        # YM5 stores 16 registers/frame
        for value in frames:
            body.append(value[reg] if reg < 14 else 0)
    return bytes(header) + bytes(body) + b"End!"


def main():
    args = [a for a in sys.argv[1:] if a != "--force"]
    force = "--force" in sys.argv[1:]
    output = args[0] if args else "example.ym"
    if os.path.exists(output) and not force:
        raise SystemExit(f"{output} already exists; pass --force to overwrite it")
    frames = build_frames()
    data = build_ym5(frames)
    with open(output, "wb") as handle:
        handle.write(data)
    print(f"{output}: {len(frames)} frames, {len(data)} bytes, "
          f"{FRAME_RATE} Hz, AY clock {AY_CLOCK_HZ} Hz")


if __name__ == "__main__":
    main()
