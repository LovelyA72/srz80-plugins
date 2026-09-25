#!/usr/bin/env sh
# Build the AY-3-8913 .ym player ROM.
#
# example.ym is the song: a standard uncompressed .ym image loaded into ROM1.
# This script only ever READS it and never writes or replaces it.  Supply any
# YM2/YM3/YM3b/YM4/YM5/YM6 file.  make_music.py can generate a sample tune, but
# it will not overwrite an existing file.
#
# The CTC frame rate follows the .ym player frequency.  Set PLAYER_FRAME_HZ to
# force one rate instead, e.g. `PLAYER_FRAME_HZ=60 ./build_rom.sh`.
set -eu

prefix="${RISCV_PREFIX:-riscv64-unknown-elf-}"
gcc="${prefix}gcc"
objcopy="${prefix}objcopy"
if ! command -v "$gcc" >/dev/null 2>&1 || ! command -v "$objcopy" >/dev/null 2>&1; then
    echo "GNU RISC-V tools not found (tried ${gcc} and ${objcopy})" >&2
    exit 1
fi

cd "$(dirname "$0")"

if [ ! -f example.ym ]; then
    echo "example.ym is missing.  Provide a .ym image, or create a sample tune with:" >&2
    echo "    python3 make_music.py example.ym" >&2
    exit 1
fi
music_size=$(wc -c < example.ym | tr -d ' ')
frame_hz="${PLAYER_FRAME_HZ:-0}"

temp_dir="$(mktemp -d player.XXXXXX)"
trap 'rm -rf "$temp_dir"' EXIT HUP INT TERM

# rv32imf matches the riscv card's RV32IMF core; zicsr supplies mtvec/mstatus.
cflags="-march=rv32imf_zicsr -mabi=ilp32 -ffreestanding -fno-builtin \
-fno-stack-protector -fomit-frame-pointer -Os \
-DMUSIC_IMAGE_SIZE=${music_size}u -DPLAYER_FRAME_HZ=${frame_hz}u"
ldflags="-nostdlib -nostartfiles -Wl,-T,linker.ld -Wl,--no-relax -Wl,--build-id=none"

"$gcc" $cflags -c player.c -o "$temp_dir/player.o"
"$gcc" $cflags -c start.S -o "$temp_dir/start.o"
"$gcc" $cflags $ldflags -o "$temp_dir/player.elf" "$temp_dir/start.o" "$temp_dir/player.o"
"$objcopy" -O binary "$temp_dir/player.elf" player.rom

echo "player.rom: $(wc -c < player.rom | tr -d ' ') bytes (example.ym: ${music_size} bytes)"
