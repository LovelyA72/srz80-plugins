# Z80 configurable-MBC5 example

This project prints through the UART while it selects two MBC5 ROM banks and
two external-RAM banks. It also uses an ordinary RAM card for the stack and
reads a marker from a separate ROM card, so the project contains `z80`, `mbc5`,
`ram`, `rom`, and `uart_console` cards.

Build the ROM images with the supplied z88dk installation:

```powershell
.\build_rom.ps1
```

The MBC5 card's generic `base` is the first control-register address. Its
`rom_range` and `ram_range` configuration values are inclusive absolute address
pairs. The ROM range is split into equal fixed and switchable windows; each ROM
image bank must have that half-range size. `ram_banks` may be 0 through 16.

This example uses the native MBC5 layout: controls and ROM at `$0000-$7FFF`,
external RAM at `$A000-$BFFF`, ordinary RAM at `$C000-$DFFF`, and an ordinary
ROM at `$E000-$EFFF`. The UART occupies I/O ports `$80-$81`.
