-- Four-channel ProTracker player. All addresses are local to this project.
local SPACE, RATE, BLOCK = "mod.io", 22050, 16
local DAC = {0x10, 0x20, 0x30, 0x40}
local periods = {856,808,762,720,678,640,604,570,538,508,480,453,
  428,404,381,360,339,320,302,285,269,254,240,226,
  214,202,190,180,170,160,151,143,135,127,120,113}
local tuning = {1,1.007246,1.014545,1.021897,1.029302,1.036762,1.044274,1.051842,
  .943874,.950714,.957603,.964542,.971532,.978572,.985663,.992806}
local sine = {0,24,49,74,97,120,141,161,180,197,212,224,235,244,250,253,
  255,253,250,244,235,224,212,197,180,161,141,120,97,74,49,24}
local mod, samples, voices, orders, restart, order, row, tick, speed, bpm
local jump, breakrow, looprow, delayrows, remaining, played, deadline

local function byte(p) return mod:byte(p + 1) end -- MOD offsets are zero based
local function be16(p) return byte(p) * 256 + byte(p + 1) end
local function clamp(n, lo, hi) return math.max(lo, math.min(hi, n)) end
local function nearest(p)
  local best, dist = 1, math.huge
  for i = 1, 36 do
    local d = math.abs(p - periods[i])
    if d < dist then best, dist = i, d end
  end
  return best
end
local function instrument(v, n)
  if n > 0 and n <= 31 then
    v.sample = samples[n]; v.volume = v.sample.volume; v.fine = v.sample.fine
  end
end
local function trigger(v, p, offset)
  if p ~= 0 then v.target = p; v.period = p end
  v.playing = v.sample; v.position = offset
  if v.vibwave & 4 == 0 then v.vibphase = 0 end
  if v.tremwave & 4 == 0 then v.tremphase = 0 end
end
local function slide_volume(v)
  local hi, lo = v.param >> 4, v.param & 15
  v.volume = clamp(v.volume + (hi ~= 0 and hi or -lo), 0, 64)
end
local function portamento(v)
  if v.target == 0 then return end
  if v.period < v.target then v.period = clamp(v.period + v.porta, 1, v.target)
  elseif v.period > v.target then v.period = clamp(v.period - v.porta, v.target, 4095) end
end
local function wave(phase, shape)
  local n
  if shape & 3 == 1 then n = 255 - (phase & 31) * 16
  elseif shape & 3 == 2 then n = 255
  else n = sine[(phase & 31) + 1] end
  return phase & 32 ~= 0 and -n or n
end
local function read_row()
  jump, breakrow, looprow = -1, -1, -1
  for c = 1, 4 do
    local v = voices[c]
    local p = 1084 + byte(952 + order) * 1024 + row * 16 + (c - 1) * 4
    local b0, b2 = byte(p), byte(p + 2)
    local note = ((b0 & 15) << 8) | byte(p + 1)
    local ins, e, x = (b0 & 240) | (b2 >> 4), b2 & 15, byte(p + 3)
    local hi, lo = x >> 4, x & 15
    v.effect, v.param, v.delayed_period, v.delayed_sample = e, x, 0, 0
    if e == 14 and hi == 13 and lo ~= 0 then
      v.delayed_period, v.delayed_sample = note, ins
    else
      instrument(v, ins)
      if e == 14 and hi == 5 then v.fine = lo end
      if e == 9 and x ~= 0 then v.offset = x * 256 end
      if note ~= 0 then
        if e == 3 or e == 5 then
          v.target = note; if v.period == 0 then v.period = note end
        else trigger(v, note, e == 9 and v.offset or 0) end
      end
    end
    if e == 3 and x ~= 0 then v.porta = x end
    if e == 4 then
      if hi ~= 0 then v.vibrato = (v.vibrato & 15) | (hi << 4) end
      if lo ~= 0 then v.vibrato = (v.vibrato & 240) | lo end
    end
    if e == 7 then
      if hi ~= 0 then v.tremolo = (v.tremolo & 15) | (hi << 4) end
      if lo ~= 0 then v.tremolo = (v.tremolo & 240) | lo end
    end
    if e == 11 then jump = x end
    if e == 12 then v.volume = clamp(x, 0, 64) end
    if e == 13 then breakrow = clamp(hi * 10 + lo, 0, 63) end
    if e == 15 and x ~= 0 then
      if x < 32 then speed = x else bpm = x end
    end
    if e == 14 then
      if hi == 1 and v.period ~= 0 then v.period = clamp(v.period - lo, 113, 856)
      elseif hi == 2 and v.period ~= 0 then v.period = clamp(v.period + lo, 113, 856)
      elseif hi == 3 then v.gliss = lo
      elseif hi == 4 then v.vibwave = lo
      elseif hi == 6 then
        if lo == 0 then v.looprow = row
        else
          if v.loops == 0 then v.loops = lo else v.loops = v.loops - 1 end
          if v.loops ~= 0 then looprow = v.looprow end
        end
      elseif hi == 7 then v.tremwave = lo
      elseif hi == 10 then v.volume = clamp(v.volume + lo, 0, 64)
      elseif hi == 11 then v.volume = clamp(v.volume - lo, 0, 64)
      elseif hi == 14 then delayrows = lo end
    end
  end
end
local function effects()
  for c = 1, 4 do
    local v, p, volume = voices[c], nil, nil
    local e, x = v.effect, v.param
    if tick ~= 0 then
      if e == 1 and v.period ~= 0 then v.period = clamp(v.period - x, 113, 856)
      elseif e == 2 and v.period ~= 0 then v.period = clamp(v.period + x, 113, 856)
      elseif e == 3 then portamento(v)
      elseif e == 5 then portamento(v); slide_volume(v)
      elseif e == 6 or e == 10 then slide_volume(v) end
    end
    if e == 14 then
      local hi, lo = x >> 4, x & 15
      if hi == 9 and lo ~= 0 and tick ~= 0 and tick % lo == 0 then trigger(v, 0, 0) end
      if hi == 12 and tick == lo then v.volume = 0 end
      if hi == 13 and tick == lo and lo ~= 0 then
        instrument(v, v.delayed_sample)
        if v.delayed_period ~= 0 then trigger(v, v.delayed_period, 0) end
      end
    end
    p, volume = v.period, v.volume
    if v.gliss ~= 0 and (e == 3 or e == 5) and p ~= 0 then p = periods[nearest(p)] end
    if e == 0 and x ~= 0 and p ~= 0 then
      local semi = tick % 3 == 1 and (x >> 4) or (tick % 3 == 2 and (x & 15) or 0)
      if semi ~= 0 then p = periods[clamp(nearest(p) + semi, 1, 36)] end
    end
    if e == 4 or e == 6 then
      p = p + math.modf(wave(v.vibphase, v.vibwave) * (v.vibrato & 15) / 128)
      if tick ~= 0 then v.vibphase = (v.vibphase + (v.vibrato >> 4)) & 63 end
    end
    if e == 7 then
      volume = clamp(volume + math.modf(wave(v.tremphase, v.tremwave) * (v.tremolo & 15) / 64), 0, 64)
      if tick ~= 0 then v.tremphase = (v.tremphase + (v.tremolo >> 4)) & 63 end
    end
    v.output_volume = volume
    v.step = p > 0 and (3546894.6 / RATE) * tuning[(v.fine & 15) + 1] / p or 0
  end
end
local function advance_row()
  local old = order
  if jump >= 0 or breakrow >= 0 then
    order = jump >= 0 and jump or order + 1
    row = breakrow >= 0 and breakrow or 0
  elseif looprow >= 0 then row = looprow
  else row = row + 1; if row == 64 then row = 0; order = order + 1 end end
  if order >= orders then order = restart end
  if order ~= old then
    for c = 1, 4 do voices[c].looprow = 0; voices[c].loops = 0 end
  end
end
local function tracker_tick()
  if played ~= 0 then
    tick = tick + 1
    if tick >= speed then
      tick = 0
      if delayrows ~= 0 then delayrows = delayrows - 1
      else advance_row(); read_row() end
    end
  else read_row() end
  effects(); played = played + 1
end
local function voice_frame(v)
  local s = v.playing
  if not s or s.length == 0 or v.period == 0 then return 128 end
  local last = s.replen ~= 0 and s.loop + s.replen or s.length
  if v.position >= last then
    if s.replen == 0 then v.playing = nil; return 128 end
    v.position = v.position - (math.floor((v.position - last) / s.replen) + 1) * s.replen
  end
  local value = byte(s.start + math.floor(v.position))
  if value >= 128 then value = value - 256 end
  v.position = v.position + v.step
  return clamp(128 + math.modf(value * v.output_volume / 256), 0, 255)
end
local function render_frame()
  if remaining <= 0 then tracker_tick(); remaining = remaining + RATE * 2.5 / bpm end
  for c = 1, 4 do card.write(SPACE, DAC[c], voice_frame(voices[c])) end
  remaining = remaining - 1
end
local function fill(now)
  for _ = 1, BLOCK do render_frame() end
  deadline = deadline + BLOCK * 1000000000 / RATE
  card.after(math.max(1, math.floor(deadline - now + 0.5)), fill)
end
function on_reset(cold)
  mod = project.read("chromag_-_abyss.mod")
  assert(#mod >= 1084, "MOD header is truncated")
  local signature = mod:sub(1081, 1084)
  assert(signature == "M.K." or signature == "M!K!" or signature == "4CHN" or signature == "FLT4",
    "MOD must have four channels")
  orders = byte(950)
  assert(orders > 0 and orders <= 128, "invalid MOD order count")
  local patterns = 0
  for i = 0, 127 do
    local n = byte(952 + i)
    assert(n <= 127, "invalid MOD pattern number")
    patterns = math.max(patterns, n + 1)
  end
  local pos = 1084 + patterns * 1024
  assert(pos <= #mod, "MOD patterns are truncated")
  samples = {}
  for i = 1, 31 do
    local h = 20 + (i - 1) * 30
    local length = be16(h + 22) * 2
    assert(length <= #mod - pos, "MOD sample data is truncated")
    local s = {start = pos, length = length, fine = byte(h + 24) & 15,
      volume = clamp(byte(h + 25), 0, 64), loop = be16(h + 26) * 2,
      replen = be16(h + 28) * 2}
    pos = pos + length
    if s.loop >= length then s.replen = 0
    elseif s.replen > length - s.loop then s.replen = length - s.loop end
    if s.replen <= 2 then s.replen = 0 end
    samples[i] = s
  end
  restart = byte(951) < orders and byte(951) or 0
  voices = {}
  for c = 1, 4 do
    voices[c] = {position=0, step=0, period=0, target=0, volume=0, output_volume=0,
      fine=0, effect=0, param=0, porta=0, offset=0, vibrato=0, tremolo=0,
      vibphase=0, tremphase=0, vibwave=0, tremwave=0, gliss=0,
      looprow=0, loops=0, delayed_period=0, delayed_sample=0}
    local base = DAC[c]
    card.write(SPACE, base + 1, 2)
    card.write(SPACE, base, 128)
    card.write(SPACE, base + 3, RATE & 255)
    card.write(SPACE, base + 4, RATE >> 8)
    card.write(SPACE, base + 1, 3)
  end
  order, row, tick, speed, bpm = 0, 0, 0, 6, 125
  jump, breakrow, looprow, delayrows, remaining, played = -1, -1, -1, 0, 0, 0
  deadline = card.time_ns()
  -- Prime each FIFO and then refill at the DAC consumption rate.
  for _ = 1, 32 do render_frame() end
  deadline = deadline + BLOCK * 1000000000 / RATE
  card.after(math.floor(deadline - card.time_ns() + 0.5), fill)
end
