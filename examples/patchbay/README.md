# Patchbay: Z80 + RAM

This project contains a Z80 reset at `0x0000`, a full 64 KiB RAM card, and a
logical GPIO / patchbay card on I/O ports `0x40` through `0x45`.

Open `project.json`, stop the rack, open **Tools → Z80 assembler**, and load
`gpio.asm` into `cpu0.memory` at `0x0000` with **Load + Cold Reset**. Then open
**Tools → I/O → Patchbay** and run the rack. The program configures P1–P5
as outputs and initially shifts a zero glyph into the connected 74HC595 and
displays. Each press of the button on P0 advances the hexadecimal display from
0 through F, then wraps back to 0.
