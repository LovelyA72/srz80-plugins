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

Short YM2151 key-off pulses are held for two audio samples so the YM2414
renderer sees them before the next key-on. This keeps repeated bass notes
from fading when a pulse falls between audio samples. Only that key-on is
delayed. Later commands keep their original VGM timing.

The script converts each SegaPCM voice's left and right volumes to a DAC level
and `PAN` value. The YM2414 handles OPM panning through its OPZ registers. Its
both-off setting is emulated by muting the channel's operators. You can adjust
the YM2414 and DAC levels in the project mixer.

You can reuse `opm2opz.lua` in another Lua player. Each converter keeps its own
panning, key fraction, operator levels, and key-off timing. It translates those
registers and passes other writes through. It doesn't emulate YM2151 noise or
guarantee identical sound for every chip feature.

```lua
local opm2opz = require("opm2opz.lua")
local opm = opm2opz.new({
  write = function(reg, value)
    card.write("vgm.io", 0x80, reg)
    card.write("vgm.io", 0x81, value)
  end,
  time_ns = card.time_ns,
  after = card.after,
  sample_rate = 44100,
})

opm:write(0x20, 0xC7)
opm:reset()
```

`write(reg, value)` takes YM2151 register and value bytes. The `write` callback
receives converted YM2414 writes. `time_ns()` returns simulated nanoseconds,
and `after(delay_ns, callback)` schedules the callback on the same timeline.
Set `sample_rate` to your YM2414 renderer's rate. It defaults to 44,100 Hz.
Call `reset()` after resetting your chip. It clears the converter's state and
discards pending key-ons without writing hardware registers.
