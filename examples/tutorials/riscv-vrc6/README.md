# Middle C: RISC-V + VRC6

An RV32I firmware writes a four-bar C major phrase directly to the VRC6 audio
registers. ROM starts at `0x00000000`, RAM occupies `0x00004000`-`0x00007fff`,
and the VRC6 window is mapped at `0x10000000`.

The phrase is `c d e f | g - - - | g f e d | c - - -`. Each VRC6 channel
plays it in turn: pulse 1, pulse 2, then sawtooth. Pulse 1 is one octave above
the written pitch, pulse 2 uses the written pitch, and the saw sounds one
octave below it with an accumulator rate of 32. Each quarter note lasts four
62.5 ms ticks, giving a tempo of 240 bpm.

Open `project.json` and start the rack to listen.

Rebuild the checked-in ROM after editing the source with:

```sh
./build_rom.sh
```

The script uses `riscv64-unknown-elf-gcc` by default. Set `RISCV_PREFIX` to use
a differently named GNU RISC-V cross toolchain.
