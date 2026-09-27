-- VGM 1.70 playback. All offsets are zero based; VGM waits count 44,100 Hz samples.
local SPACE, APU, AY = "vgm.io", 0x4000, 0x5000
local SAMPLE_RATE = 44100
local data, cursor, loop_at, samples, start_ns, ay_regs, dpcm_memory

local function byte(pos)
  local value = data:byte(pos + 1)
  assert(value, string.format("truncated VGM at 0x%X", pos))
  return value
end

local function word(pos)
  return byte(pos) | (byte(pos + 1) << 8)
end

local function dword(pos)
  return word(pos) | (word(pos + 2) << 16)
end

local function validate()
  assert(#data >= 0x100 and data:sub(1, 4) == "Vgm ", "expected an uncompressed VGM")
  assert(dword(0x08) >= 0x170, "VGM 1.70 or newer is required")
  local file_end = 4 + dword(0x04)
  assert(file_end <= #data, "truncated VGM file")
  assert(dword(0x74) == 1789773 and dword(0x84) == 1789773,
    "project chip clocks do not match VGM header")
  local first = dword(0x34) == 0 and 0x40 or 0x34 + dword(0x34)
  loop_at = dword(0x1C) == 0 and nil or 0x1C + dword(0x1C)
  assert(first < file_end and (not loop_at or loop_at >= first), "invalid VGM data or loop offset")

  local pos, duration, loop_samples = first, 0, nil
  while pos < file_end do
    if pos == loop_at then loop_samples = duration end
    local command = byte(pos)
    if command == 0x66 then
      assert(duration == dword(0x18), "VGM sample count mismatch")
      assert(not loop_at or (loop_samples and loop_samples < duration),
        "loop offset is invalid or loop has no wait")
      card.log(string.format("VGM: %.2f s, loop %s", duration / SAMPLE_RATE,
        loop_at and string.format("0x%X", loop_at) or "none"))
      return first
    elseif command == 0x61 then
      assert(pos + 3 <= file_end, "truncated wait command")
      duration = duration + word(pos + 1)
      pos = pos + 3
    elseif command == 0x62 or command == 0x63 then
      duration = duration + (command == 0x62 and 735 or 882)
      pos = pos + 1
    elseif command >= 0x70 and command <= 0x7F then
      duration = duration + (command & 15) + 1
      pos = pos + 1
    elseif command == 0xA0 or command == 0xB4 then
      assert(pos + 3 <= file_end, "truncated register write")
      local reg = byte(pos + 1)
      assert((command == 0xA0 and reg <= 15) or
             (command == 0xB4 and reg <= 0x17), "unsupported chip register")
      local value = byte(pos + 2)
      if command == 0xA0 then
        -- This AY bridge only translates the 5B's tone divider.  The supplied
        -- recording keeps noise disabled and does not select envelope volume.
        assert(reg ~= 7 or (value & 0x38) == 0x38, "YM2149 noise needs a divider")
        assert(reg < 8 or reg > 10 or (value & 0x10) == 0,
          "YM2149 envelope needs a divider")
      end
      pos = pos + 3
    elseif command == 0x67 then
      assert(pos + 9 <= file_end, "truncated DPCM data block header")
      assert(byte(pos + 1) == 0x66 and byte(pos + 2) == 0xC2,
        "unsupported VGM data block")
      local size = dword(pos + 3)
      assert(size >= 2 and pos + 7 + size <= file_end, "truncated DPCM data block")
      local address = word(pos + 7)
      assert(address >= 0xC000 and address + size - 2 <= 0x10000,
        "DPCM data block lies outside the sample window")
      pos = pos + 7 + size
    else
      error(string.format("unsupported VGM command 0x%02X at 0x%X", command, pos))
    end
  end
  error("VGM end command missing")
end

local function ay_write(reg, value)
  ay_regs[reg] = value
  if reg <= 5 then
    local low = reg & 0xFE
    local period = (ay_regs[low] or 0) | (((ay_regs[low + 1] or 0) & 15) << 8)
    -- Sunsoft 5B holds YM2149 SEL low. Doubling a tone period at the AY's
    -- unmodified input clock gives the same pitch for the bundled recording.
    local adjusted = math.min(0xFFF, math.max(1, period) * 2)
    card.write(SPACE, AY, low)
    card.write(SPACE, AY + 1, adjusted & 255)
    card.write(SPACE, AY, low + 1)
    card.write(SPACE, AY + 1, (adjusted >> 8) & 15)
  else
    card.write(SPACE, AY, reg)
    card.write(SPACE, AY + 1, value)
  end
end

local function play()
  while true do
    -- Absolute deadlines prevent timer rounding from accumulating drift.
    local due = start_ns + math.floor(samples * 1000000000 / SAMPLE_RATE + 0.5)
    local delay = due - card.time_ns()
    if delay > 0 then
      card.after(delay, play)
      return
    end

    local command = byte(cursor)
    if command == 0xA0 then
      ay_write(byte(cursor + 1), byte(cursor + 2))
      cursor = cursor + 3
    elseif command == 0xB4 then
      local reg, value = byte(cursor + 1), byte(cursor + 2)
      card.write(SPACE, APU + reg, value)
      cursor = cursor + 3
    elseif command == 0x67 then
      local size = dword(cursor + 3)
      local first = word(cursor + 7) - 0xC000
      dpcm_memory = dpcm_memory:sub(1, first) ..
        data:sub(cursor + 10, cursor + 7 + size) ..
        dpcm_memory:sub(first + size - 1)
      cursor = cursor + 7 + size
    elseif command == 0x61 then
      samples = samples + word(cursor + 1)
      cursor = cursor + 3
    elseif command == 0x62 or command == 0x63 then
      samples = samples + (command == 0x62 and 735 or 882)
      cursor = cursor + 1
    elseif command >= 0x70 and command <= 0x7F then
      samples = samples + (command & 15) + 1
      cursor = cursor + 1
    elseif command == 0x66 then
      if not loop_at then
        card.log("VGM playback complete")
        return
      end
      cursor = loop_at
    end
  end
end

function on_reset(cold)
  data = project.read("test.vgm")
  cursor = validate()
  ay_regs = {}
  dpcm_memory = string.rep("\0", 0x4000)
  samples = 0
  start_ns = card.time_ns()
  play()
end

function on_read(address)
  return dpcm_memory:byte(address - 0xC000 + 1)
end
