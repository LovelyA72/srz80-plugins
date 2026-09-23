local SPACE, YM, LCD = "demo.io", 0x80, 0xC0
local EIGHTH_NS = 250000000

local melody = {
  {"E5",1},{"D#5",1},{"E5",1},{"D#5",1},{"E5",1},{"B4",1},{"D5",1},{"C5",1},{"A4",2},
  {"C4",1},{"E4",1},{"A4",1},{"B4",2},{"E4",1},{"G#4",1},{"B4",1},{"C5",2},
  {"E4",1},{"E5",1},{"D#5",1},{"E5",1},{"D#5",1},{"E5",1},{"B4",1},{"D5",1},{"C5",1},{"A4",2},
  {"C4",1},{"E4",1},{"A4",1},{"B4",2},{"E4",1},{"C5",1},{"B4",1},{"A4",3}
}

-- key codes for the OPZ at 3,579,545 Hz
local semitone = {C=-2, ["C#"]=0, D=1, ["D#"]=2, E=4, F=5,
                  ["F#"]=6, G=7, ["G#"]=9, A=10, ["A#"]=11, B=13}

local function reg(address, value)
  card.write(SPACE, YM, address)
  card.write(SPACE, YM + 1, value)
end

local function lcd_line(row, text)
  card.write(SPACE, LCD, 0x80 + (row == 2 and 0x40 or 0))
  text = (text .. string.rep(" ", 16)):sub(1, 16)
  for i = 1, 16 do card.write(SPACE, LCD + 1, text:byte(i)) end
end

local function key_off() reg(0x08, 0x00) end

local function play_note()
  local entry = melody[state.note]
  local name, beats = entry[1], entry[2]
  local pitch, octave = name:match("^([A-G]#?)(%d)$")
  local key_code = tonumber(octave) * 16 + semitone[pitch]
  key_off()
  reg(0x28, key_code) -- channel 0 key code
  reg(0x30, 0x01)     -- no fractional pitch; send audio to both outputs
  reg(0x08, 0x78)     -- key on all four operators, channel 0
  lcd_line(2, string.format("Note %-3s %02d/%02d", name, state.note, #melody))

  local duration = beats * EIGHTH_NS
  card.after(duration * 0.86, key_off)
  state.note = state.note % #melody + 1
  card.after(duration, play_note)
end

function on_reset(cold)
  state = {note = 1}
  key_off()
  reg(0x15, 0x01)
  reg(0x20, 0x2D)
  local operators = {
    {slot=0,  multiple=4, detune=7, level=49, attack=31, decay=0, sustain=15, release=3, waveform=3}, -- O1: Cut Triangle
    {slot=16, multiple=1, detune=0, level=10,  attack=15, decay=4, sustain=15, release=9, waveform=0}, -- O2
    {slot=8,  multiple=8, detune=3, level=24,  attack=14, decay=7, sustain=13, release=9, waveform=5}, -- O3: Squished Triangle
    {slot=24, multiple=2, detune=3, level=10,  attack=15, decay=4, sustain=15, release=9, waveform=5}  -- O4: Squished Triangle
  }
  for _, op in ipairs(operators) do
    local slot = op.slot
    reg(0x40 + slot, op.detune * 16 + op.multiple)
    reg(0x40 + slot, 0x80 + op.waveform * 16)
    reg(0x60 + slot, op.level)
    reg(0x80 + slot, op.attack)
    reg(0xA0 + slot, op.decay)
    reg(0xC0 + slot, 0x00) -- decay 2 rate and detune 2
    reg(0xE0 + slot, op.sustain * 16 + op.release)
  end
  card.write(SPACE, LCD, 0x01) -- clear
  card.write(SPACE, LCD, 0x06) -- increment DDRAM address
  card.write(SPACE, LCD, 0x0C) -- display on, cursor off
  lcd_line(1, "Fur Elise - OPZ")
  play_note()
end
