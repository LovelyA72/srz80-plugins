# Lua YM2151 and SegaPCM VGM player

Open `project.json` in SRZ80 and resume the rack. The Lua script reads
`test.vgm` from this folder. It sends YM2151 writes to the YM2414 card and
plays SegaPCM's 16 voices through 16 DAC cards.

You can replace `test.vgm` with another uncompressed VGM that uses a 4 MHz
YM2151, a 4 MHz SegaPCM with interface `0x0C`, and the commands supported by
`player.lua`. The player checks the header, command bounds, ROM blocks, and
loop point before playback. The included recording lasts about 358 seconds
before it loops. The script schedules register writes from the VGM's 44,100 Hz
sample timeline and updates the DACs at SegaPCM's 31,250 Hz voice rate.

The script converts each SegaPCM voice's left and right volumes to a DAC level
and `PAN` value. The YM2414 handles OPM panning through its OPZ registers. Its
both-off setting is emulated by muting the channel's operators. You can adjust
the YM2414 and DAC levels in the project mixer.
