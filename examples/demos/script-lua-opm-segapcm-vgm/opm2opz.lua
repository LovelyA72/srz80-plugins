local OPMConverter = {}
OPMConverter.__index = OPMConverter

function OPMConverter.new(options)
  assert(type(options) == "table", "opm2opz needs an options table")
  for _, name in ipairs({"write", "time_ns", "after"}) do
    assert(type(options[name]) == "function", "opm2opz needs a " .. name .. " callback")
  end
  local rate = options.sample_rate or 44100
  assert(type(rate) == "number" and rate > 0 and rate < math.huge,
    "opm2opz sample_rate must be positive and finite")
  local self = setmetatable({emit = options.write, time_ns = options.time_ns,
    after = options.after, key_off_ns = math.ceil(2 * 1000000000 / rate), epoch = 0}, OPMConverter)
  self:reset()
  return self
end

function OPMConverter:reset()
  self.epoch = self.epoch + 1
  self.pan, self.fraction, self.levels, self.keys = {}, {}, {}, {}
  for ch = 0, 7 do
    self.pan[ch], self.fraction[ch] = 0, 0
    self.keys[ch] = {mask = 0, off = {}, generation = 0}
  end
  for index = 0, 31 do self.levels[index] = 0 end
end

function OPMConverter:key_write(value)
  local ch = value & 7
  local key = self.keys[ch]
  local mask = value & 0x78
  local now, delay = self.time_ns(), 0
  key.generation = key.generation + 1
  local generation, epoch = key.generation, self.epoch
  for op = 0, 3 do
    local bit = 1 << (op + 3)
    if mask & bit == 0 then
      key.off[op] = now
    elseif key.mask & bit == 0 and key.off[op] then
      delay = math.max(delay, key.off[op] + self.key_off_ns - now)
    end
  end
  if delay > 0 then
    -- Release operators now, even if another operator's key-on needs to wait
    local held = key.mask & mask
    if held ~= key.mask then
      self.emit(0x08, (value & 0x87) | held)
      key.mask = held
    end
    self.after(delay, function()
      if self.epoch == epoch and key.generation == generation then
        self.emit(0x08, value)
        key.mask = mask
      end
    end)
  else
    self.emit(0x08, value)
    key.mask = mask
  end
end

function OPMConverter:write(reg, value)
  assert(math.type(reg) == "integer" and reg >= 0 and reg <= 0xFF,
    "opm2opz register must be a byte")
  assert(math.type(value) == "integer" and value >= 0 and value <= 0xFF,
    "opm2opz value must be a byte")
  if reg == 0x08 then
    self:key_write(value)
  elseif reg >= 0x20 and reg <= 0x27 then
    local ch = reg & 7
    local old = self.pan[ch]
    self.pan[ch] = value >> 6
    self.emit(reg, (value & 0x3F) | ((self.pan[ch] & 2) ~= 0 and 0x80 or 0))
    self.emit(0x30 + ch, (self.fraction[ch] & 0xFE) | (self.pan[ch] == 3 and 1 or 0))
    if (old == 0) ~= (self.pan[ch] == 0) then
      for op = 0, 3 do
        local index = ch + op * 8
        self.emit(0x60 + index, self.pan[ch] == 0 and 0x7F or self.levels[index])
      end
    end
  elseif reg >= 0x30 and reg <= 0x37 then
    local ch = reg & 7
    self.fraction[ch] = value
    self.emit(reg, (value & 0xFE) | (self.pan[ch] == 3 and 1 or 0))
  elseif reg >= 0x60 and reg <= 0x7F then
    local index = reg - 0x60
    self.levels[index] = value
    self.emit(reg, self.pan[index & 7] == 0 and 0x7F or value)
  else
    self.emit(reg, value)
  end
end

return OPMConverter
