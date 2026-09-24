-- XM 1.04 player for the bundled song, mixed into two PCM DAC FIFOs.
local RATE, BLOCK, PREFILL = 44100, 16, 32
local SPACE, LEFT, RIGHT = "xm.io", 0x10, 0x20
local sine = {0,25,50,74,98,120,142,162,180,197,212,225,235,244,250,254,
  255,254,250,244,235,225,212,197,180,162,142,120,98,74,50,25}
local arp = {1.0,1.059463,1.122462,1.189207,1.259921,1.334840,1.414214,1.498307,
  1.587401,1.681793,1.781797,1.887749,2.0,2.118926,2.244924,2.378414}
local data, channels, orders, patterns, instruments, voices
local order_count, restart, order, row, tick, speed, bpm, started, remaining, deadline

local function clamp(v, lo, hi) return math.max(lo, math.min(hi, v)) end
local function byte(s, p)
  local v = s:byte(p + 1) -- XM offsets are zero based.
  assert(v, "truncated XM data")
  return v
end
local function word(s, p) return byte(s, p) | (byte(s, p + 1) << 8) end
local function dword(s, p) return word(s, p) | (word(s, p + 2) << 16) end
local function signed8(v) return v < 128 and v or v - 256 end
local function slice(s, p, n)
  assert(p >= 0 and n >= 0 and p + n <= #s, "truncated XM data")
  return s:sub(p + 1, p + n)
end
local function decode_sample(s, p, n, sixteen)
  local raw = slice(s, p, n)
  local out, accum = {}, 0
  if sixteen then
    assert(n % 2 == 0, "odd 16-bit XM sample length")
    for i = 0, n / 2 - 1 do
      local delta = word(raw, i * 2)
      if delta >= 32768 then delta = delta - 65536 end
      accum = (accum + delta) & 65535
      out[#out + 1] = string.char((accum >> 8) & 255)
    end
  else
    for i = 0, n - 1 do
      accum = (accum + signed8(byte(raw, i))) & 255
      out[#out + 1] = string.char(accum)
    end
  end
  return table.concat(out)
end
local function parse_pattern(p)
  local header, rows, size = dword(data, p), word(data, p + 5), word(data, p + 7)
  assert(header >= 9 and rows >= 1 and rows <= 256, "invalid XM pattern")
  local packed, offsets, cursor = slice(data, p + header, size), {}, 0
  for r = 1, rows do
    offsets[r] = cursor
    for _ = 1, channels do
      if size ~= 0 then
        local first = byte(packed, cursor)
        cursor = cursor + 1
        if first & 128 ~= 0 then
          for field = 0, 4 do
            if first & (1 << field) ~= 0 then cursor = cursor + 1 end
          end
        else cursor = cursor + 4 end
        assert(cursor <= size, "truncated XM pattern")
      end
    end
  end
  return {rows=rows, packed=packed, offsets=offsets}, p + header + size
end
local function parse_instrument(p)
  local header, count = dword(data, p), word(data, p + 27)
  assert(header >= 29, "invalid XM instrument")
  assert(count <= 32, "too many XM samples in instrument")
  local sample_header_size = count > 0 and dword(data, p + 29) or 0
  assert(count == 0 or (header >= 243 and sample_header_size >= 40), "invalid XM sample header")
  local ins = {keymap=count > 0 and slice(data, p + 33, 96) or nil,
    samples={}, points={}, flags=0, sustain=0, loop_start=0, loop_end=0}
  if count > 0 then
    for i = 0, clamp(byte(data, p + 225), 0, 12) - 1 do
      ins.points[#ins.points + 1] = {word(data, p + 129 + i * 4),
        clamp(word(data, p + 131 + i * 4), 0, 64)}
    end
    ins.flags = byte(data, p + 233)
    ins.sustain = byte(data, p + 227)
    ins.loop_start = byte(data, p + 228)
    ins.loop_end = byte(data, p + 229)
  end
  local sample_header, sample_data = p + header, p + header + count * sample_header_size
  for i = 0, count - 1 do
    local h = sample_header + i * sample_header_size
    local bytes, loop_start, loop_length = dword(data, h), dword(data, h + 4), dword(data, h + 8)
    local kind = byte(data, h + 14)
    local unit = kind & 16 ~= 0 and 2 or 1
    local pcm = decode_sample(data, sample_data, bytes, unit == 2)
    sample_data = sample_data + bytes
    loop_start, loop_length = loop_start // unit, loop_length // unit
    local loop_type = kind & 3
    if loop_type == 0 or loop_length < 2 or loop_start + loop_length > #pcm then
      loop_type, loop_start, loop_length = 0, 0, 0
    end
    ins.samples[i + 1] = {pcm=pcm, length=#pcm, loop_start=loop_start,
      loop_length=loop_length, loop_type=loop_type,
      volume=clamp(byte(data, h + 12), 0, 64),
      finetune=signed8(byte(data, h + 13)), relative=signed8(byte(data, h + 16))}
  end
  return ins, sample_data
end
local function load_song()
  data = project.read("song.xm")
  assert(slice(data, 0, 17) == "Extended Module: ", "not an XM module")
  assert(word(data, 58) == 0x0104, "only XM 1.04 is supported")
  local header = dword(data, 60)
  assert(header >= 276, "invalid XM header")
  order_count, restart, channels = word(data, 64), word(data, 66), word(data, 68)
  local pattern_count, instrument_count = word(data, 70), word(data, 72)
  assert(order_count >= 1 and order_count <= 256 and channels >= 1 and channels <= 32
    and pattern_count <= 256 and instrument_count <= 128, "invalid XM layout")
  assert(word(data, 74) & 1 ~= 0, "this player uses linear XM periods")
  speed, bpm = clamp(word(data, 76), 1, 31), clamp(word(data, 78), 32, 255)
  orders = {}
  for i = 0, order_count - 1 do orders[i + 1] = byte(data, 80 + i) end
  if restart >= order_count then restart = 0 end
  local p = 60 + header
  patterns = {}
  for i = 1, pattern_count do patterns[i], p = parse_pattern(p) end
  instruments = {}
  for i = 1, instrument_count do instruments[i], p = parse_instrument(p) end
  for _, n in ipairs(orders) do assert(n < #patterns, "XM order refers to missing pattern") end
  card.log("XM: " .. slice(data, 17, 20):match("^[^%z]*") .. ", " .. channels ..
    " channels, " .. order_count .. " orders")
end
local function note_period(note, sample)
  local adjusted = clamp(note + sample.relative, 1, 119)
  return 7680 - (adjusted - 1) * 64 - (sample.finetune >> 3) * 4
end
local function restart_envelope(v)
  if not v.sample then return end
  v.volume, v.env_tick, v.keyoff, v.vibrato_phase = v.sample.volume, 0, false, 0
end
local function trigger(v, note)
  local ins = v.instrument
  if not ins or #ins.samples == 0 then return end
  local sample = ins.samples[byte(ins.keymap, clamp(note - 1, 0, 95)) + 1]
  if not sample then return end
  v.sample, v.note, v.period = sample, note, note_period(note, sample)
  v.target, v.position, v.direction = v.period, 0, 1
  restart_envelope(v)
end
local function event(packed, cursor)
  local first = byte(packed, cursor)
  cursor = cursor + 1
  if first & 128 ~= 0 then
    local values = {0,0,0,0,0}
    for i = 0, 4 do
      if first & (1 << i) ~= 0 then
        values[i + 1], cursor = byte(packed, cursor), cursor + 1
      end
    end
    return values, cursor
  end
  return {first, byte(packed, cursor), byte(packed, cursor + 1),
    byte(packed, cursor + 2), byte(packed, cursor + 3)}, cursor + 4
end
local function read_row()
  local pattern = patterns[orders[order + 1] + 1]
  local packed, cursor = pattern.packed, pattern.offsets[row + 1]
  for c = 1, channels do
    local values = {0,0,0,0,0}
    if #packed > 0 then values, cursor = event(packed, cursor) end
    local note, ins, vol, effect, param = table.unpack(values)
    local v = voices[c]
    v.effect, v.param = effect, param
    if ins > 0 and ins <= #instruments then v.instrument = instruments[ins] end
    if note == 97 then
      v.keyoff = true
      if not v.instrument or v.instrument.flags & 1 == 0 then v.volume = 0 end
    elseif note > 0 and note <= 96 then
      if effect == 3 and v.sample then
        v.target = note_period(note, v.sample)
        if ins > 0 then restart_envelope(v) end
      else trigger(v, note) end
    elseif ins > 0 and v.sample then restart_envelope(v) end
    if vol >= 0x10 and vol <= 0x50 then v.volume = vol - 0x10 end
    if effect == 3 and param > 0 then v.porta_speed = param * 4 end
    if effect == 4 then
      if param >> 4 > 0 then v.vibrato_speed = param >> 4 end
      if param & 15 > 0 then v.vibrato_depth = param & 15 end
    end
    if (effect == 6 or effect == 10) and param > 0 then v.volume_slide = param end
    if effect == 12 then v.volume = clamp(param, 0, 64) end
    if effect == 15 and param > 0 then
      if param < 32 then speed = param else bpm = param end
    end
  end
end
local function volume_envelope(v)
  local ins = v.instrument
  if not ins or ins.flags & 1 == 0 or #ins.points == 0 then return 64 end
  local points, t = ins.points, v.env_tick
  local left, value = points[1], points[1][2]
  for _, right in ipairs(points) do
    if t < right[1] then
      if right[1] > left[1] then
        value = left[2] + (right[2] - left[2]) * (t - left[1]) / (right[1] - left[1])
      end
      break
    end
    left, value = right, right[2]
  end
  local next_tick = t + 1
  if ins.flags & 2 ~= 0 and not v.keyoff then
    local sustain = points[ins.sustain + 1]
    if sustain and t >= sustain[1] then next_tick = t end
  end
  if ins.flags & 4 ~= 0 then
    local first, last = points[ins.loop_start + 1], points[ins.loop_end + 1]
    if first and last and next_tick > last[1] then next_tick = first[1] end
  end
  v.env_tick = next_tick
  return clamp(value, 0, 64)
end
local function process_tick()
  for _, v in ipairs(voices) do
    local effect, param = v.effect, v.param
    if tick > 0 then
      if effect == 2 and v.period > 0 then
        v.period = clamp(v.period + param * 4, 1, 7680)
      elseif effect == 3 and v.period > 0 and v.target > 0 then
        if v.period < v.target then v.period = math.min(v.period + v.porta_speed, v.target)
        elseif v.period > v.target then v.period = math.max(v.period - v.porta_speed, v.target) end
      end
      if effect == 6 or effect == 10 then
        local up, down = v.volume_slide >> 4, v.volume_slide & 15
        v.volume = clamp(v.volume + (up > 0 and up or -down), 0, 64)
      end
    end
    v.output_volume = v.volume * volume_envelope(v) / 64
    local period = v.period
    if period > 0 then
      if effect == 4 or effect == 6 then
        local phase = v.vibrato_phase & 63
        local wave = sine[(phase & 31) + 1]
        if phase >= 32 then wave = -wave end
        period = period + wave * v.vibrato_depth / 32
        if tick > 0 then v.vibrato_phase = v.vibrato_phase + v.vibrato_speed end
      end
      local multiplier = 1
      if effect == 0 and param > 0 then
        local semi = tick % 3 == 1 and param >> 4 or (tick % 3 == 2 and param & 15 or 0)
        multiplier = arp[semi + 1]
      end
      v.step = 8363 * 2 ^ ((4608 - period) / 768) * multiplier / RATE
    else v.step = 0 end
  end
end
local function tracker_tick()
  if started then
    tick = tick + 1
    if tick >= speed then
      tick, row = 0, row + 1
      if row >= patterns[orders[order + 1] + 1].rows then
        row, order = 0, order + 1
        if order >= order_count then order = restart end
      end
      read_row()
    end
  else read_row(); started = true end
  process_tick()
end
local function sample_frame(v)
  local sample = v.sample
  if not sample or v.step <= 0 or v.output_volume <= 0 or sample.length == 0 then return 0 end
  local pos, loop_type = v.position, sample.loop_type
  local loop_start, loop_length = sample.loop_start, sample.loop_length
  local loop_end = loop_start + loop_length
  if loop_type == 1 and pos >= loop_end then
    pos = loop_start + (pos - loop_start) % loop_length
  elseif loop_type == 2 then
    if v.direction > 0 and pos >= loop_end then
      pos, v.direction = loop_end * 2 - pos - 1, -1
    elseif v.direction < 0 and pos < loop_start then
      pos, v.direction = loop_start * 2 - pos, 1
    end
  elseif pos >= sample.length then v.sample = nil; return 0 end
  local value = byte(sample.pcm, clamp(math.floor(pos), 0, sample.length - 1))
  if value >= 128 then value = value - 256 end
  v.position = pos + v.step * v.direction
  return value * v.output_volume / 64
end
local function render_frame()
  if remaining <= 0 then tracker_tick(); remaining = remaining + RATE * 2.5 / bpm end
  local left, right = 0, 0
  for _, v in ipairs(voices) do
    local value = sample_frame(v)
    left = left + value * (255 - v.pan) / 255
    right = right + value * v.pan / 255
  end
  card.write(SPACE, LEFT, clamp(math.floor(128 + left / 3), 0, 255))
  card.write(SPACE, RIGHT, clamp(math.floor(128 + right / 3), 0, 255))
  remaining = remaining - 1
end
local function render_block(n) for _ = 1, n do render_frame() end end
local function fill(now)
  render_block(BLOCK)
  deadline = deadline + BLOCK * 1000000000 / RATE
  card.after(math.max(1, math.floor(deadline - now + 0.5)), fill)
end
function on_reset(cold)
  load_song()
  voices = {}
  for i = 1, channels do
    voices[i] = {position=0, direction=1, period=0, target=0, step=0,
      volume=0, output_volume=0, pan=(i - 1) % 2 == 0 and 80 or 175,
      effect=0, param=0, porta_speed=0, vibrato_speed=0, vibrato_depth=0,
      vibrato_phase=0, volume_slide=0, env_tick=0, keyoff=false}
  end
  order, row, tick, started, remaining = 0, 0, 0, false, 0
  for _, base in ipairs({LEFT, RIGHT}) do
    card.write(SPACE, base + 1, 2)
    card.write(SPACE, base, 128)
    card.write(SPACE, base + 3, RATE & 255)
    card.write(SPACE, base + 4, RATE >> 8)
    card.write(SPACE, base + 1, 3)
  end
  deadline = card.time_ns()
  render_block(PREFILL)
  deadline = deadline + BLOCK * 1000000000 / RATE
  card.after(math.max(1, math.floor(deadline - card.time_ns() + 0.5)), fill)
end
function on_read(address) return order or 0 end
