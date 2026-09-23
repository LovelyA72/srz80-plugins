# Für Elise: Lua, YM2414, LCD1602

Open `project.json` in SRZ80 and run the simulation. The script card plays a
loop of the opening melody on YM2414 channel 0. The LCD1602 shows the current
note and its position in the phrase. Audio comes from the YM2414 mixer source.

`fur_elise.lua` uses `card.after` for simulated-time note and gate scheduling;
it does not require a CPU or ROM. The card's one-byte mapping at `demo.io:0`
is reserved for the script card. The YM2414 uses address/data ports `0x80` and
`0x81`; the LCD1602 uses command/data ports `0xC0` and `0xC1`.

The script card is listed last so its reset callback configures the YM2414 and
LCD1602 after those cards have reset.
