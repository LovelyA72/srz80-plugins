# RV32IMF X1-010 wavetable lab

Build the firmware and PCM image with the supplied RISC-V toolchain:

```powershell
.\build_rom.ps1
```

The script defaults to `C:\msys64\ucrt64\bin`. Override it with:

```powershell
.\build_rom.ps1 -RiscvBin C:\path\to\riscv\bin
```

Open `project.json` in SRZ80, then use the UART console endpoint. Enter one of
these commands followed by Return:

- `sine`, `saw`, `tri`, or `square` selects a built-in 16-step waveform.
- `draw -128 -64 0 64 127 64 0 -64 -128 -64 0 64 127 64 0 -64` installs a custom table.
- `pcm` copies the demo into the shared 1 MiB RAM card and plays it as X1-010 PCM.

MIDI Note On/Off messages play the selected wavetable across the X1-010's 16
voices. Select the `wavetable.midi` endpoint in **Tools > I/O > MIDI**.

The CPU bus map keeps the devices separate: X1-010 at `0x10000000`-`0x10001fff`,
UART at `0x10002000`, and MIDI at `0x10002100`.

To inspect the current wavetable, select `rv.memory` and set the Memory
inspector's **base** field to `10001000`. Wave 0 occupies
`0x10001000`-`0x1000107f`; the next 31 waves follow in 128-byte blocks. This is
the X1-010's private wavetable RAM, not the shared PCM RAM at `0x00100000`.
With base `10000000` and length 4096, the display stops at `10000fff`, one byte
before the wavetable starts.
