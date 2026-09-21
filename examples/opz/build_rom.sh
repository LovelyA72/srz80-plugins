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
temp_dir="$(mktemp -d opz.XXXXXX)"
trap 'rm -rf "$temp_dir"' EXIT HUP INT TERM
# The player paces itself from the cycle CSR, so the build declares Zicsr
# explicitly; the bare rv32imf ISA string does not enable the CSR opcodes.
"$gcc" -march=rv32imf_zicsr -mabi=ilp32 -nostdlib -nostartfiles -Os -ffreestanding \
    -fno-builtin -fno-stack-protector -fomit-frame-pointer \
    -Wl,-T,linker.ld -Wl,--no-relax -Wl,--build-id=none \
    -o "$temp_dir/opz-vgm-player.elf" opz_vgm.c
"$objcopy" -O binary "$temp_dir/opz-vgm-player.elf" opz-vgm-player-rv32imf.rom
