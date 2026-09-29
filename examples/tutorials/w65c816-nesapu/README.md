# Middle C: W65C816 + RAM + NES APU

A four-bar C major phrase written straight to the NES APU registers by a
W65C816. The processor enters native mode with `XCE`, initializes player state
with a 16-bit accumulator, and uses 8-bit writes for the APU registers. RAM
occupies `$0000`-`$7fff`, the program ROM is mapped at `$8000`, and the APU
register block answers at `$4000`.

The phrase is `c d e f | g - - - | g f e d | c - - -`. Each channel plays the
full phrase in turn: pulse 1, pulse 2, triangle, then noise.

| Voice | Setup |
| --- | --- |
| Pulse 1 | 50% duty, one octave above the written pitch, constant volume 15 |
| Pulse 2 | 25% duty, at pitch, constant volume 15 |
| Triangle | At pitch, control flag holding the linear counter reloaded |
| Noise | Short mode at rate `$f`, constant volume 8, adding percussion |

The score has sixteen quarter-note slots. A tick is a sixteenth note, about
62.5 ms at the project's 1789773 Hz clock, so four ticks make a quarter note at
240 bpm. The player holds each note for its full duration and disables the
channel during a rest.

Open `project.json` and start the rack to listen.

Rebuild the checked-in ROM after editing the source with:

```powershell
& 'C:\Users\Kashouryo\.local\bin\64tass.exe' --nostart --output=nesapu.rom nesapu.asm
```

The tables play C4 to D5 on pulse 2 and triangle, with pulse 1 one octave above.
The pulse values account for the NES APU card's pulse timer clocking; triangle
uses its own timer divisor. Noise plays the same rests and note durations as a
percussion line.
