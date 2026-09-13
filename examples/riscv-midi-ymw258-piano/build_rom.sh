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
# The boot animation paces itself from the cycle CSR, so both targets declare
# Zicsr explicitly; the bare ISA string does not enable the CSR opcodes.
# The firmware ROM shares its low 16 KiB with the Work RAM window, so piano.c
# is built at -O2: -O3 expands the splash drawing past that limit.  The GM
# decoder in gm.c keeps -O3 and is linked in from its own object.
for target in rv32i:rv32i_zicsr rv32imf:rv32imf_zicsr; do
    name="${target%%:*}"
    march="${target##*:}"
    cflags="-march=$march -mabi=ilp32 -ffreestanding -fno-builtin -fno-stack-protector -fomit-frame-pointer"
    ldflags="-nostdlib -nostartfiles -Wl,-T,linker.ld -Wl,--no-relax -Wl,--build-id=none"
    "$gcc" $cflags -O3 -c gm.c -o "$temp_dir/gm-$name.o"
    "$gcc" $cflags -O2 -c piano.c -o "$temp_dir/piano-$name.o"
    "$gcc" $cflags $ldflags -o "$temp_dir/piano-$name.elf" \
        "$temp_dir/piano-$name.o" "$temp_dir/gm-$name.o"
    "$objcopy" -O binary "$temp_dir/piano-$name.elf" "piano-$name.rom"
done
