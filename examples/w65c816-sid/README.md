# Empty: W65C816 + RAM + SID

This project plays 4mat's 512-byte **Empty** Tiny SID Competition tune with a
W65C816.  The processor starts in its 6502-emulation mode, so `empty.asm` uses
only baseline 6502 instructions.  RAM occupies `$0000`–`$7fff`, the program
ROM is mapped at `$8000`, and the MOS 8580 SID register block is at `$d400`.

Open `project.json` and start the rack to play the tune.  The original player
waited for the C64 VIC raster register; SRZ80 deliberately has no C64 VIC, so
this port uses a 50 Hz busy-wait calibrated for the project's 1 MHz CPU clock.

Rebuild the checked-in ROM after editing the source with:

```powershell
& 'C:\Users\Kashouryo\.local\bin\64tass.exe' --nostart --output=empty.rom empty.asm
```
