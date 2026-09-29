#!/usr/bin/env sh
# SPDX-License-Identifier: MIT
set -eu
prefix="${RISCV_PREFIX:-riscv64-unknown-elf-}"
gcc="${prefix}gcc"
objcopy="${prefix}objcopy"
if ! command -v "$gcc" >/dev/null 2>&1 || ! command -v "$objcopy" >/dev/null 2>&1; then
    echo "GNU RISC-V tools not found (tried $gcc and $objcopy)" >&2
    exit 1
fi
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
scratch_root="${SRZ80_SCRATCH_DIR:-$script_dir/../../scratch/riscv-vsn}"
mkdir -p "$scratch_root"
build_dir=$(mktemp -d "$scratch_root/build.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM
"$gcc" -march=rv32imf_zicsr -mabi=ilp32f -msmall-data-limit=0 \
    -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-stack-protector \
    -fno-pic -fno-pie -nostdlib -nostartfiles \
    -Wl,-T,"$script_dir/linker.ld" -Wl,--no-relax -Wl,--build-id=none \
    -o "$build_dir/demo.elf" "$script_dir/demo.c"
"$objcopy" -O binary "$build_dir/demo.elf" "$script_dir/demo.rom"
echo "Built $script_dir/demo.rom ($(wc -c < "$script_dir/demo.rom") bytes, RV32IMF)"
