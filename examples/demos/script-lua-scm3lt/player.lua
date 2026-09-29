-- Digi Charat 2 SCM3LT player demo
--
--         ._                __.
--        / \"-.          ,-",'/
--       (   \ ,"--.__.--".,' /
--       =---Y(_i.-'  |-.i_)---=
--      f ,  "..'/\\v/|/|/\  , l
--      l//  ,'|/   V / /||  \\j
--       "--; / db     db|/---"
--          | \ YY   , YY//
--          '.\>_   (_),"' __
--        .-"    "-.-." I,"  `.
--        \.-""-. ( , ) ( \   |
--        (     l  `"'  -'-._j
-- __,---_ '._." .  .    \
--(__.--_-'.  ,  :  '  \  '-.
--    ,' .'  /   |   \  \  \ "-
--     "--.._____t____.--'-""'
--            /  /  `. ".
--           / ":     \' '.
--         .'  (       \   :
--         |    l      j    "-.
--         l_;_;I      l____;_I
--                      cgmm  
--
-- Set song.txt to the two-digit hexadecimal GSF song index, TRACKS.md lists
-- the album. The mixer is an approximation: no per-note attack/release envelope.
local SPACE, DAC_LEFT, DAC_RIGHT = "scm3lt.io", 0x10, 0x20
local RATE, BLOCK = 22919, 16
local DATA_OFFSET = 0x84DD3D0 - 0x84D62C0
local bank, pitch_table, gain_table, attenuation, tracks, tempo
local tempo_fraction, video_fraction, deadline

local function u8(offset)
  return assert(bank:byte(offset + 1), "music data read out of range")
end

local function u32(offset)
  return u8(offset) | (u8(offset + 1) << 8) |
         (u8(offset + 2) << 16) | (u8(offset + 3) << 24)
end

local function pitch_step(note, transpose)
  local pitch = (note - 1) * 64 + transpose
  if pitch < 0 or pitch > 0x2000 then return 0 end
  local octave = pitch // 768
  local offset = (pitch % 768) * 4
  local a, b, c, d = pitch_table:byte(offset + 1, offset + 4)
  local raw = a | (b << 8) | (c << 16) | (d << 24)
  if octave <= 3 then return (raw >> (4 - octave)) / 4096 end
  return (raw << (octave - 4)) / 4096
end

local function data_byte(offset) return u8(DATA_OFFSET + offset) end
local function gain(level)
  level = math.max(0, math.min(31, level))
  -- The driver handles levels 1,7,13,19,25,31 with a shift amount in the
  -- volume field, the mixer scales the sample by 2^shift.
  if level % 6 == 1 then return 8 << ((level - 1) // 6) end
  local a, b, c, d = gain_table:byte(level * 4 + 1, level * 4 + 4)
  return a | (b << 8) | (c << 16) | (d << 24)
end
local function sample_record(instrument, note)
  local index
  if instrument >= 1 and instrument <= 0x7B then
    index = instrument - 1
  elseif instrument == 0xC8 then
    -- The drum bank chooses a sample record by the note byte.
    index = note + 0x82
  else
    return nil
  end
  local offset = 0x6010 + index * 16
  local flags, length, loop_start, data = u32(offset), u32(offset + 4),
    u32(offset + 8), u32(offset + 12)
  local raw_length = flags == 11 and ((length + 1) // 2) or length
  if length == 0 or data + raw_length > #bank then return nil end
  return {flags=flags, length=length, loop_start=loop_start, data=data,
          delta=flags == 11}
end
local function next_byte(track)
  local value = data_byte(track.pos)
  track.pos = track.pos + 1
  return value
end

local function next_be16(track)
  return (next_byte(track) << 8) | next_byte(track)
end

local command_lengths = {
  [0x88]=1, [0x90]=6, [0x96]=1, [0x97]=1,
  [0xA0]=4, [0xA1]=4, [0xA5]=2, [0xA6]=2,
  [0xF1]=1, [0xF2]=1, [0xF3]=2,
}

local function tick_track(track)
  if track.finished then return end
  if track.remaining > 0 then
    track.remaining = track.remaining - 1
    if track.remaining > 0 then return end
  end
  for _ = 1, 64 do
    local command = next_byte(track)
    if command == 0 then
      if track.loop_start then
        track.pos = track.loop_start
      else
        track.finished, track.note = true, nil
        return
      end
    elseif command <= 0x7F then
      local length, byte, chain = 0, next_byte(track), 0
      while byte >= 128 do
        chain = chain + 1
        if chain > 6 then error(string.format("bad note length at +%X", track.pos)) end
        length = (length << 7) | (byte & 0x7F)
        byte = next_byte(track)
      end
      length = (length << 7) | byte
      if command == 0x7F then
        track.note, track.sample = nil, nil
      else
        track.note = command + 15
        track.sample = sample_record(track.instrument or 0, command)
      end
      -- Reproduce the driver's 20.12 sample position and pitch table lookup.
      -- Drum voices and 4-bit delta streams run at a fixed one step per
      -- output sample, everything else uses the pitch table.
      local fixed = track.instrument == 0xC8 or (track.sample and track.sample.delta)
      track.step = track.note and
        (fixed and 1 or pitch_step(command, track.pitch or 0)) or 0
      track.remaining = math.max(1, length)
      if track.note then
        track.position, track.delta, track.delta_count = 0, 0, 0
      end
      return
    elseif command == 0x80 then
      track.volume = next_byte(track)
      track.gain = gain(track.volume - attenuation)
    elseif command == 0x81 then
      track.pitch = next_byte(track) * 256 + next_byte(track)
      if track.pitch >= 0x8000 then track.pitch = track.pitch - 0x10000 end
    elseif command == 0x82 then
      track.instrument = next_byte(track)
    elseif command == 0x87 then
      tempo = next_byte(track)
    elseif command == 0x94 then
      track.gate = next_byte(track)
      track.release = next_byte(track)
    elseif command == 0xCF then
      track.loop_start = track.pos
    elseif command == 0xC0 then
      local repeats = next_byte(track)
      track.loops = track.loops or {}
      assert(#track.loops < 5, "SCM3LT loop nesting exceeds five")
      track.loops[#track.loops + 1] = repeats
    elseif command == 0xC1 then
      local distance = next_be16(track)
      if distance >= 0x8000 then distance = distance - 0x10000 end
      if track.loops and track.loops[#track.loops] == 1 then
        track.pos = track.pos + distance
        track.loops[#track.loops] = nil
      end
    elseif command == 0xC2 then
      local distance = next_be16(track)
      local loops = track.loops
      if loops and #loops > 0 then
        local n = #loops
        loops[n] = (loops[n] - 1) & 255
        if loops[n] == 0 then loops[n] = nil
        else track.pos = track.pos - distance end
      end
    elseif command == 0x98 then
      track.pan = 1  -- left: the right output receives this voice at half level
    elseif command == 0x99 then
      track.pan = 2  -- right: the left output receives this voice at half level
    elseif command == 0x9A then
      track.pan = 3  -- center
    elseif command == 0x83 then
      -- Mode changes. The reference mixer still needs to be decoded.
    elseif command == 0x84 or command == 0x85 or command == 0x86 or
           command == 0x8C or command == 0xAB or command == 0xC8 or
           command == 0xFF then
      -- Commands without operands, their voice/effect state is pending.
    elseif command_lengths[command] then
      for _ = 1, command_lengths[command] do next_byte(track) end
    else
      error(string.format("unimplemented SCM3LT command %02X at +%X", command, track.pos - 1))
    end
  end
  error("SCM3LT command loop did not reach a note")
end

local function sequence_frame()
  tempo_fraction = tempo_fraction + tempo
  while tempo_fraction >= 128 do
    tempo_fraction = tempo_fraction - 128
    for _, track in ipairs(tracks) do tick_track(track) end
  end
end

local function frame()
  video_fraction = video_fraction + 16777216
  if video_fraction >= 280896 * RATE then
    video_fraction = video_fraction - 280896 * RATE
    sequence_frame()
  end
  local left, right = 0, 0
  for _, track in ipairs(tracks) do
    local sample = track.sample
    if track.note and sample then
      local index = math.floor(track.position)
      if index >= sample.length then
        if sample.flags == 2 then
          index = index % sample.length
          track.position = index + (track.position - math.floor(track.position))
        elseif sample.flags == 12 and sample.loop_start < sample.length then
          index = sample.loop_start + (index - sample.loop_start) %
            (sample.length - sample.loop_start)
          track.position = index + (track.position - math.floor(track.position))
        else
          track.sample = nil
          goto next_track
        end
      end
      local value
      if sample.delta then
        -- 4-bit delta stream: the driver keeps the running value in the low
        -- bits of the voice position, decodes one nibble per output sample and
        -- clamps it back into signed 8-bit range at each mixer block.
        local packed = u8(sample.data + index // 2)
        local nibble = index % 2 == 0 and (packed & 15) or (packed >> 4)
        if nibble >= 8 then nibble = nibble - 16 end
        value = track.delta + nibble * 4
        track.delta = value
        track.delta_count = track.delta_count + 1
        if track.delta_count % 64 == 0 then
          track.delta = math.max(-127, math.min(127, track.delta))
        end
      else
        value = u8(sample.data + index)
        if value >= 128 then value = value - 256 end
      end
      local level = value * (track.gain or 0) / 512
      if track.pan == 1 then
        left, right = left + level, right + level / 2
      elseif track.pan == 2 then
        left, right = left + level / 2, right + level
      else
        left, right = left + level, right + level
      end
      track.position = track.position + track.step
    end
    ::next_track::
  end
  card.write(SPACE, DAC_LEFT, math.max(0, math.min(255, math.floor(128 + left + 0.5))))
  card.write(SPACE, DAC_RIGHT, math.max(0, math.min(255, math.floor(128 + right + 0.5))))
end

local function fill(now)
  for _ = 1, BLOCK do frame() end
  deadline = deadline + BLOCK * 1000000000 / RATE
  card.after(math.max(1, math.floor(deadline - now + 0.5)), fill)
end

function on_reset(cold)
  bank = project.read("music_bank.bin")
  pitch_table = project.read("pitch_table.bin")
  gain_table = project.read("gain_table.bin")
  local attenuation_table = project.read("song_attenuation.bin")
  local song = tonumber((project.read("song.txt") or "0A"):match("%x+"), 16)
  assert(song and song >= 0 and song <= 0x37, "invalid song index")
  attenuation = assert(attenuation_table:byte(song + 1), "missing attenuation")
  assert(attenuation > 0, "song index is not in this GSF album")
  tracks = {}
  for i = 0, 15 do
    tracks[i + 1] = {pos = u32(0x10 + song * 96 + i * 4), remaining = 0,
                    volume = 31, gain = gain(31 - attenuation), pan = 3, position = 0,
                     delta = 0, delta_count = 0, note = nil, finished = false}
  end
  tempo, tempo_fraction, video_fraction = 128, 0, 0
  for _, dac in ipairs({DAC_LEFT, DAC_RIGHT}) do
    card.write(SPACE, dac + 1, 2)
    card.write(SPACE, dac, 128)
    card.write(SPACE, dac + 3, RATE & 255)
    card.write(SPACE, dac + 4, RATE >> 8)
    card.write(SPACE, dac + 1, 3)
  end
  deadline = card.time_ns()
  for _ = 1, 32 do
    card.write(SPACE, DAC_LEFT, 128)
    card.write(SPACE, DAC_RIGHT, 128)
  end
  deadline = deadline + BLOCK * 1000000000 / RATE
  card.after(math.max(1, math.floor(deadline - card.time_ns() + 0.5)), fill)
end
