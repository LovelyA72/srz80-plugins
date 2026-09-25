#!/usr/bin/env sh
set -eu
prefix="${RISCV_PREFIX:-riscv64-unknown-elf-}"
gcc="${prefix}gcc"
objcopy="${prefix}objcopy"
if ! command -v "$gcc" >/dev/null 2>&1 || ! command -v "$objcopy" >/dev/null 2>&1; then
    echo "GNU RISC-V tools not found (tried ${gcc} and ${objcopy})" >&2
    exit 1
fi
cd "$(dirname "$0")"
temp_dir="$(mktemp -d piano.XXXXXX)"
trap 'rm -rf "$temp_dir"' EXIT HUP INT TERM
"$gcc" -march=rv32i -mabi=ilp32 -nostdlib -nostartfiles -Os -ffreestanding \
    -fno-builtin -fno-stack-protector -fomit-frame-pointer \
    -Wl,-T,linker.ld -Wl,--no-relax -Wl,--build-id=none \
    -o "$temp_dir/piano.elf" piano.c
"$objcopy" -O binary "$temp_dir/piano.elf" piano.rom
