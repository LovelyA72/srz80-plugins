# LCD1602 card

`lcd1602` emulates the two-register interface of an HD44780-style 16×2
character LCD.  It publishes a 480×120 RGBA surface, with a green 5×8 dot-grid
rendering of the supplied `char-lcd.js` glyph design.

The default mapping is two bytes at `0xC0`:

| Offset | Read | Write |
| --- | --- | --- |
| `+0` | Status (busy flag is always clear; low seven bits are the address counter) | Instruction |
| `+1` | Data at the address counter | Data at the address counter |

Implemented instructions are clear display (`0x01`), return home (`0x02`),
entry mode (`0x04`–`0x07`), display/cursor enable (`0x08`–`0x0f`), CGRAM address
selection (`0x40`–`0x7f`), and DDRAM address selection (`0x80`–`0xff`).  DDRAM
line starts are `0x00` and `0x40`; CGRAM supplies eight programmable 5×8
characters through character codes `0x00`–`0x07`.
