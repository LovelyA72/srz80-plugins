# Z80 PCM DAC FIFO example

This project runs a 3.6864 MHz Z80 and the `dac` card at I/O ports `$D0`–`$D4`.
The program selects FIFO mode, configures its rate to 22,050 Hz, then repeatedly
writes a 32-byte unsigned-PCM waveform. It deliberately writes faster than the
FIFO consumes: excess writes demonstrate the DAC's bounded buffering while the
heard waveform remains stable.

Build the ROM with z88dk before opening `project.json`:

```powershell
.\build_rom.ps1
```

The script defaults to `C:\Software\z88dk`; pass `-Z88dkRoot` if z88dk is
installed elsewhere.

To try direct mode, change the program's control write from `1` to `0` and
write individual values to `$D0`; each value remains at the DAC output until
the next write.
