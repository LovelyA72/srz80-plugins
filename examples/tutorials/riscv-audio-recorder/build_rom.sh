#!/usr/bin/env sh
set -eu
prefix="${RISCV_PREFIX:-riscv64-unknown-elf-}"
gcc="${prefix}gcc"
objcopy="${prefix}objcopy"
if ! command -v "$gcc" >/dev/null 2>&1 || ! command -v "$objcopy" >/dev/null 2>&1; then
    echo "GNU RISC-V tools not found (tried ${gcc} and ${objcopy})" >&2
    exit 1
fi
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
scratch_root="${SRZ80_SCRATCH_DIR:-$script_dir/../../scratch/riscv-audio-recorder}"
mkdir -p "$scratch_root"
temp_dir=$(mktemp -d "$scratch_root/build.XXXXXX")
trap 'rm -rf "$temp_dir"' EXIT HUP INT TERM
"$gcc" -march=rv32i -mabi=ilp32 -msmall-data-limit=0 -nostdlib -nostartfiles -Os -ffreestanding \
    -fno-builtin -fno-stack-protector -fomit-frame-pointer \
    -Wl,-T,"$script_dir/linker.ld" -Wl,--no-relax -Wl,--build-id=none \
    -o "$temp_dir/recorder.elf" "$script_dir/recorder.c"
"$objcopy" -O binary "$temp_dir/recorder.elf" "$script_dir/recorder.rom"
