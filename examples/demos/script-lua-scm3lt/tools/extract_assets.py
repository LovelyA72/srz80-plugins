"""Extract the SCM3LT music assets used by the Lua player from the game ROM.

The player needs the ROM's music region (song pointers, song bytecode and the
sample bank), the mixer's pitch and gain tables, and the per-song attenuation
the driver loads into 0x03001550.

Usage:

    python3 tools/extract_assets.py --rom /path/to/BDKJ.gba
    python3 tools/extract_assets.py --rom rom.bin --minigsf-dir /path/to/music

Only a raw 16 MiB ROM dump, already decompressed out of the .minigsf container,
is accepted. Assets are written next to this example unless --out is given.
"""

from __future__ import annotations

import argparse
import struct
import zlib
from pathlib import Path

ROM_BASE = 0x08000000
ROM_SIZE = 0x01000000

# Music region the player reads: song tables, song data, sample records and the
# raw samples. The record table lives at bank + 0x6010, 16 bytes per record.
BANK_START = 0x084D62C0
BANK_END = 0x08AA0000

# Mixer lookup tables in the ROM: 768 sixteens-of-a-semitone pitch steps and 32
# volume multipliers.
PITCH_TABLE = 0x08FA8A70
PITCH_ENTRIES = 768
GAIN_TABLE = 0x08FAAE7C
GAIN_ENTRIES = 32

# Per-song attenuation for the 56 song ids the album uses. The driver copies
# the selected value into 0x03001550; zero means "not a song in this album".
SONG_ATTENUATION = bytes([
    0x04, 0x06, 0x05, 0x05, 0x02, 0x02, 0x02, 0x02, 0x01, 0x04, 0x02, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x04, 0x04, 0x04,
    0x04, 0x04, 0x00, 0x00, 0x04, 0x04, 0x03, 0x03, 0x03, 0x04, 0x04, 0x04,
    0x06, 0x07, 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x04, 0x04, 0x00, 0x04, 0x04,
])


def read_rom(path: Path) -> bytes:
    rom = path.read_bytes()
    if len(rom) != ROM_SIZE:
        raise SystemExit(f"{path}: expected a 16 MiB ROM dump, got {len(rom)} bytes")
    game_code = rom[0xAC:0xB0].decode("ascii", "replace")
    if game_code != "BDKJ":
        print(f"warning: game code is {game_code!r}, expected 'BDKJ'")
    return rom


def slice_at(rom: bytes, address: int, size: int) -> bytes:
    start = address - ROM_BASE
    end = start + size
    if start < 0 or end > len(rom):
        raise SystemExit(f"0x{address:08X}+0x{size:X} is outside the ROM")
    return rom[start:end]


def write_minigsf_index(minigsf_dir: Path, out_dir: Path) -> None:
    titles: list[tuple[int, str]] = []
    for path in sorted(minigsf_dir.glob("*.minigsf")):
        raw = path.read_bytes()
        compressed_size = struct.unpack_from("<I", raw, 8)[0]
        body = zlib.decompress(raw[16:16 + compressed_size])
        if len(body) != 13:
            continue
        song = body[12]
        tag = raw.split(b"[TAG]", 1)[1].decode("utf-8")
        title = next(line[6:] for line in tag.splitlines() if line.startswith("title="))
        titles.append((song, title))
    lines = ["# Available songs", "", "Set `song.txt` to the hex index.", ""]
    lines += [f"- `{song:02X}` \u2014 {title}" for song, title in sorted(titles)]
    (out_dir / "TRACKS.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"wrote {out_dir / 'TRACKS.md'}: {len(titles)} tracks")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rom", required=True, type=Path, help="raw 16 MiB BDKJ ROM")
    parser.add_argument("--out", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--minigsf-dir", type=Path,
                        help="optional .minigsf directory used to rebuild TRACKS.md")
    args = parser.parse_args()

    rom = read_rom(args.rom)
    args.out.mkdir(parents=True, exist_ok=True)

    assets = {
        "music_bank.bin": slice_at(rom, BANK_START, BANK_END - BANK_START),
        "pitch_table.bin": slice_at(rom, PITCH_TABLE, PITCH_ENTRIES * 4),
        "gain_table.bin": slice_at(rom, GAIN_TABLE, GAIN_ENTRIES * 4),
        "song_attenuation.bin": SONG_ATTENUATION,
    }
    for name, data in assets.items():
        (args.out / name).write_bytes(data)
        print(f"wrote {args.out / name}: {len(data)} bytes")

    if args.minigsf_dir:
        write_minigsf_index(args.minigsf_dir, args.out)


if __name__ == "__main__":
    main()
