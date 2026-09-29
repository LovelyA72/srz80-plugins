#!/usr/bin/env python3
"""Build vdp.rom from vdp.asm.

    1. generate deterministic star positions
    2. assemble vdp.asm with the z88dk Z80 assembler
    3. pad the image to the 32 KiB the ROM card expects

The assembler is z88dk's z80asm.  Point SRZ80_Z80ASM at it if it is not on PATH,
for example:

    SRZ80_Z80ASM=C:\\Software\\z88dk\\bin\\z80asm.exe python build.py

Run from this directory.
"""

import os
import shutil
import subprocess
import sys
from pathlib import Path
import tempfile

ROM_SIZE = 0x8000       # the image is mapped at 0x0000-0x7FFF, below the RAM
SOURCE = 'vdp.asm'
OUTPUT = 'vdp.rom'


def assembler():
    override = os.environ.get('SRZ80_Z80ASM')
    if override:
        return override
    for candidate in ('z80asm', 'z80asm.exe'):
        found = shutil.which(candidate)
        if found:
            return found
    for candidate in (r'C:\Software\z88dk\bin\z80asm.exe', '/usr/local/bin/z80asm'):
        if os.path.exists(candidate):
            return candidate
    return None


def main():
    source = Path(__file__).resolve().parent
    tool = assembler()
    if not tool:
        print('z80asm was not found.  Set SRZ80_Z80ASM to the z88dk assembler, or put it on PATH.',
              file=sys.stderr)
        return 1

    tool = str(Path(tool).resolve())
    with tempfile.TemporaryDirectory(prefix='srz80-stars-') as scratch:
        directory = Path(scratch)
        shutil.copyfile(source / SOURCE, directory / SOURCE)
        seed = 0x5A17
        records = []
        for i in range(128):
            seed = (seed * 25173 + 13849) & 0xFFFF
            speed = 1 + ((seed >> 8) % 3)
            records.append(f'        defb {seed >> 8},{(i * 73) % 192},{speed},{speed}\n')
        (directory / 'stars.inc').write_text(''.join(records), encoding='ascii')
        subprocess.run([tool, '-b', SOURCE], cwd=directory, check=True)
        image = bytearray((directory / 'vdp.bin').read_bytes())
    used = len(image)
    if len(image) > ROM_SIZE:
        print(f'the image is {len(image)} bytes, which does not fit in {ROM_SIZE}',
              file=sys.stderr)
        return 1
    image.extend(bytes(ROM_SIZE - len(image)))

    with open(source / OUTPUT, 'wb') as handle:
        handle.write(image)
    print(f'wrote {OUTPUT}: {len(image)} bytes, code and data using {used}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
