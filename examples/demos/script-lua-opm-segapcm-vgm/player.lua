local SPACE, OPZ, DAC_BASE = "vgm.io", 0x80, 0x100
local VGM_RATE, PCM_RATE, PCM_CLOCK = 44100, 31250, 4000000
local data, file_end, cursor, loop_at, samples, start_ns, rom, voices
local pan, fraction, levels, tick_number
local scan_pos, scan_duration, scan_loop_samples, scan_fm, scan_pcm, first_command
local play, pcm_tick

local function byte(pos)
  assert(pos >= 0 and pos < file_end, string.format("truncated VGM at 0x%X", pos))
  return data:byte(pos + 1)
end

local function word(pos)
  return byte(pos) | (byte(pos + 1) << 8)
end

local function dword(pos)
  return word(pos) | (word(pos + 2) << 16)
end

local function command(pos)
  local op = byte(pos)
  local size, wait = 1, 0
  if op == 0x66 then return size, wait end
  if op == 0x61 then size, wait = 3, word(pos + 1)
  elseif op == 0x62 then wait = 735
  elseif op == 0x63 then wait = 882
  elseif op >= 0x70 and op <= 0x7F then wait = (op & 15) + 1
  elseif op == 0x54 then size = 3
  elseif op == 0xC0 then size = 4
  elseif op == 0x67 then
    assert(byte(pos + 1) == 0x66 and byte(pos + 2) == 0x80,
      string.format("unsupported data block at 0x%X", pos))
    local count = dword(pos + 3)
    assert(count >= 8, "short SegaPCM ROM block")
    size = 7 + count
  else
    error(string.format("unsupported VGM command 0x%02X at 0x%X", op, pos))
  end
  assert(pos + size <= file_end, string.format("truncated command at 0x%X", pos))
  return size, wait
end

local function validate()
  assert(#data >= 0x40 and data:sub(1, 4) == "Vgm ", "expected an uncompressed VGM")
  file_end = #data
  file_end = 4 + dword(0x04)
  assert(file_end >= 0x40 and file_end <= #data, "invalid VGM EOF offset")
  local version = dword(0x08)
  assert(version >= 0x151, "SegaPCM needs VGM 1.51 or newer")
  assert(dword(0x30) == PCM_CLOCK and dword(0x38) == PCM_CLOCK,
    "project chip clocks must match the VGM header (4 MHz)")
  assert(dword(0x3C) == 0x0C, "this player needs SegaPCM interface 0x0C")
  local first = dword(0x34) == 0 and 0x40 or 0x34 + dword(0x34)
  loop_at = dword(0x1C) == 0 and nil or 0x1C + dword(0x1C)
  assert(first < file_end and (not loop_at or loop_at >= first and loop_at < file_end),
    "invalid VGM data or loop offset")
  return first
end

local function scan()
  for _ = 1, 512 do
    assert(scan_pos < file_end, "VGM end command missing")
    if scan_pos == loop_at then scan_loop_samples = scan_duration end
    local op = byte(scan_pos)
    local size, wait = command(scan_pos)
    if op == 0x66 then
      assert(scan_fm > 0 and scan_pcm > 0, "VGM needs YM2151 and SegaPCM writes")
      assert(dword(0x18) == 0 or dword(0x18) == scan_duration, "VGM sample count mismatch")
      assert(not loop_at or scan_loop_samples and scan_loop_samples < scan_duration,
        "loop offset is not a command boundary or has no wait")
      card.log(string.format("VGM: %.2f s, %d YM2151 writes, %d SegaPCM writes",
        scan_duration / VGM_RATE, scan_fm, scan_pcm))
      samples, tick_number = 0, 0
      start_ns = card.time_ns()
      cursor = first_command
      play()
      pcm_tick()
      return
    end
    if op == 0x54 then scan_fm = scan_fm + 1 end
    if op == 0xC0 then
      assert(word(scan_pos + 1) <= 0xFF, "second SegaPCM chip is unsupported")
      scan_pcm = scan_pcm + 1
    end
    if op == 0x67 then
      local total, start = dword(scan_pos + 7), dword(scan_pos + 11)
      assert(total == 0x80000 and start <= total and size - 15 <= total - start,
        "SegaPCM ROM block lies outside the 512 KiB ROM")
    end
    scan_duration = scan_duration + wait
    scan_pos = scan_pos + size
  end
  card.after(1, scan)
end

local function opz_write(reg, value)
  card.write(SPACE, OPZ, reg)
  card.write(SPACE, OPZ + 1, value)
end

local function opm_write(reg, value)
  if reg >= 0x20 and reg <= 0x27 then
    local ch = reg & 7
    local old = pan[ch]
    pan[ch] = value >> 6
    opz_write(reg, (value & 0x3F) | ((pan[ch] & 2) ~= 0 and 0x80 or 0))
    opz_write(0x30 + ch, (fraction[ch] & 0xFE) | (pan[ch] == 3 and 1 or 0))
    if (old == 0) ~= (pan[ch] == 0) then
      for op = 0, 3 do
        local index = ch + op * 8
        opz_write(0x60 + index, pan[ch] == 0 and 0x7F or levels[index])
      end
    end
  elseif reg >= 0x30 and reg <= 0x37 then
    local ch = reg & 7
    fraction[ch] = value
    opz_write(reg, (value & 0xFE) | (pan[ch] == 3 and 1 or 0))
  elseif reg >= 0x60 and reg <= 0x7F then
    local index = reg - 0x60
    levels[index] = value
    opz_write(reg, pan[index & 7] == 0 and 0x7F or value)
  else
    opz_write(reg, value)
  end
end

local function pcm_write(address, value)
  local ch = (address >> 3) & 15
  local v = voices[ch]
  local reg = address & 0x87
  if reg == 0x02 or reg == 0x03 then
    if reg == 0x02 then v.left = value & 0x7F else v.right = value & 0x7F end
    local balance = 127
    if v.left > v.right then
      balance = math.floor(v.right * 127 / v.left + 0.5)
    elseif v.right > v.left then
      balance = 255 - math.floor(v.left * 128 / v.right + 0.5)
    end
    card.write(SPACE, DAC_BASE + ch * 8 + 5, balance)
  elseif reg == 0x04 then v.loop = (v.loop & 0xFF00) | value
  elseif reg == 0x05 then v.loop = (v.loop & 0x00FF) | (value << 8)
  elseif reg == 0x06 then v.finish = value
  elseif reg == 0x07 then v.step = value
  elseif reg == 0x84 then v.address = (v.address & 0xFF00FF) | (value << 8)
  elseif reg == 0x85 then v.address = (v.address & 0x00FFFF) | (value << 16)
  elseif reg == 0x86 then
    v.control = value
    if value & 1 ~= 0 then card.write(SPACE, DAC_BASE + ch * 8, 0x80) end
  end
end

local function rom_block(pos)
  local count = dword(pos + 3)
  local start = dword(pos + 11)
  rom = rom:sub(1, start) .. data:sub(pos + 16, pos + 7 + count) ..
    rom:sub(start + count - 7)
end

play = function()
  while true do
    local due = start_ns + math.floor(samples * 1000000000 / VGM_RATE + 0.5)
    local delay = due - card.time_ns()
    if delay > 0 then
      card.after(delay, play)
      return
    end
    local op = byte(cursor)
    local size, wait = command(cursor)
    if op == 0x66 then
      if not loop_at then
        card.log("VGM playback complete")
        return
      end
      cursor = loop_at
    else
      if op == 0x54 then opm_write(byte(cursor + 1), byte(cursor + 2))
      elseif op == 0xC0 then pcm_write(word(cursor + 1), byte(cursor + 3))
      elseif op == 0x67 then rom_block(cursor) end
      samples = samples + wait
      cursor = cursor + size
    end
  end
end

pcm_tick = function()
  for ch = 0, 15 do
    local v = voices[ch]
    if v.control & 1 == 0 then
      if v.address >> 16 == ((v.finish + 1) & 0xFF) then
        if v.control & 2 ~= 0 then
          v.control = v.control | 1
          card.write(SPACE, DAC_BASE + ch * 8, 0x80)
        else
          v.address = v.loop << 8
        end
      end
      if v.control & 1 == 0 then
        local offset = ((v.control & 0x70) << 12) + (v.address >> 8)
        local sample = rom:byte(offset + 1) or 0x80
        local volume = math.max(v.left, v.right)
        local output = 0x80 + math.floor((sample - 0x80) * volume / 127 + 0.5)
        card.write(SPACE, DAC_BASE + ch * 8, output)
        v.address = (v.address + v.step) & 0xFFFFFF
      end
    else
      v.address = v.address & 0xFFFF00
    end
  end
  tick_number = tick_number + 1
  local due = start_ns + math.floor(tick_number * 1000000000 / PCM_RATE + 0.5)
  card.after(math.max(1, due - card.time_ns()), pcm_tick)
end

function on_reset(cold)
  data = project.read("outrun.vgm")
  first_command = validate()
  rom = string.rep("\128", 0x80000)
  voices = {}
  for ch = 0, 15 do
    voices[ch] = {address = 0xFFFF00, loop = 0xFFFF, finish = 0xFF,
      step = 0xFF, left = 0x7F, right = 0x7F, control = 0xFF}
  end
  pan, fraction, levels = {}, {}, {}
  for ch = 0, 7 do pan[ch], fraction[ch] = 0, 0 end
  for index = 0, 31 do levels[index] = 0 end
  scan_pos, scan_duration, scan_loop_samples, scan_fm, scan_pcm =
    first_command, 0, nil, 0, 0
  scan()
end
