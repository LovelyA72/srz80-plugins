// VGM commands are timed in 44,100 Hz samples. Only the first YM2151 is audible.
const SPACE = "vgm.io";
const OPZ = 0x80;
const SAMPLE_RATE = 44100;
const FILE = "test.vgm";

let data, end, version, cursor, loopAt, samples, startNs;
let pan, fraction, levels;

function check(condition, message) {
  if (!condition) throw new Error(message);
}

function byte(pos) {
  check(pos >= 0 && pos < end, `truncated VGM at 0x${pos.toString(16)}`);
  return data[pos];
}

function word(pos) {
  return byte(pos) | (byte(pos + 1) << 8);
}

function dword(pos) {
  return (word(pos) | (word(pos + 2) << 16)) >>> 0;
}

function command(pos) {
  const op = byte(pos);
  let size = 1, wait = 0;
  if (op === 0x66) return {size, wait, end: true};
  if (op === 0x61) { size = 3; wait = word(pos + 1); }
  else if (op === 0x62 || op === 0x63) wait = op === 0x62 ? 735 : 882;
  else if (op >= 0x70 && op <= 0x7f) wait = (op & 15) + 1;
  else if (op >= 0x80 && op <= 0x8f) wait = op & 15;
  else if (op === 0x67) {
    check(byte(pos + 1) === 0x66, `invalid data block at 0x${pos.toString(16)}`);
    size = 7 + (dword(pos + 3) & 0x7fffffff);
  } else if (op === 0x68) {
    check(byte(pos + 1) === 0x66, `invalid PCM write at 0x${pos.toString(16)}`);
    size = 12;
  } else if (op === 0x90 || op === 0x91 || op === 0x95) size = 5;
  else if (op === 0x92) size = 6;
  else if (op === 0x93) size = 11;
  else if (op === 0x94) size = 2;
  else if (op === 0x00) size = 1;
  else if ((op >= 0x30 && op <= 0x3f) || op === 0x4f || op === 0x50) size = 2;
  else if (op >= 0x40 && op <= 0x4e) size = version < 0x160 ? 2 : 3;
  else if (op >= 0x51 && op <= 0x5f) size = 3;
  else if (op >= 0xa0 && op <= 0xbf) size = 3;
  else if (op >= 0xc0 && op <= 0xdf) size = 4;
  else if (op >= 0xe0) size = 5;
  else throw new Error(`unsupported VGM command 0x${op.toString(16)} at 0x${pos.toString(16)}`);
  check(pos + size <= end, `truncated command at 0x${pos.toString(16)}`);
  return {size, wait, end: false};
}

function validate() {
  check(data.length >= 0x40 && String.fromCharCode(...data.slice(0, 4)) === "Vgm ",
        "expected an uncompressed VGM file");
  end = data.length;
  const eof = 4 + dword(0x04);
  check(eof <= end && eof >= 0x40, "invalid VGM EOF offset");
  end = eof;
  version = dword(0x08);
  const chipClock = dword(0x30) & 0x3fffffff;
  check(chipClock > 0, "VGM has no YM2151 clock");
  const first = version < 0x150 || dword(0x34) === 0 ? 0x40 : 0x34 + dword(0x34);
  loopAt = dword(0x1c) === 0 ? null : 0x1c + dword(0x1c);
  check(first < end && (loopAt === null || loopAt >= first && loopAt < end),
        "invalid VGM data or loop offset");
  let pos = first, total = 0, loopSamples = null, writes = 0;
  while (pos < end) {
    if (pos === loopAt) loopSamples = total;
    const item = command(pos);
    if (data[pos] === 0x54) writes++;
    if (item.end) {
      check(writes > 0, "VGM has no YM2151 register writes");
      check(loopAt === null || loopSamples !== null && loopSamples < total,
            "loop offset is not a command boundary or has no wait");
      check(dword(0x18) === 0 || dword(0x18) === total, "VGM sample count mismatch");
      card.log(`YM2151 VGM: ${writes} writes, ${(total / SAMPLE_RATE).toFixed(2)} s, ${chipClock} Hz`);
      if (chipClock !== 4000000)
        card.log(`Set the YM2414 chip_clock_hz to ${chipClock} for the recorded pitch`);
      return first;
    }
    total += item.wait;
    pos += item.size;
  }
  throw new Error("VGM end command missing");
}

function opzWrite(reg, value) {
  card.write(SPACE, OPZ, reg);
  card.write(SPACE, OPZ + 1, value);
}

function opmWrite(reg, value) {
  if (reg >= 0x20 && reg <= 0x27) {
    const ch = reg & 7;
    const old = pan[ch];
    pan[ch] = value >>> 6;
    // OPZ uses 0x20.7 for right and 0x30.0 to force both outputs on.
    opzWrite(reg, (value & 0x3f) | ((pan[ch] & 2) ? 0x80 : 0));
    opzWrite(0x30 + ch, (fraction[ch] & 0xfe) | (pan[ch] === 3 ? 1 : 0));
    // Both card backends route these pan bits to at least one output.
    // Attenuate all operators for OPM's both-off setting.
    if ((old === 0) !== (pan[ch] === 0)) {
      for (let op = 0; op < 4; op++) {
        const index = ch + op * 8;
        opzWrite(0x60 + index, pan[ch] === 0 ? 0x7f : levels[index]);
      }
    }
  } else if (reg >= 0x30 && reg <= 0x37) {
    const ch = reg & 7;
    fraction[ch] = value;
    opzWrite(reg, (value & 0xfe) | (pan[ch] === 3 ? 1 : 0));
  } else if (reg >= 0x60 && reg <= 0x7f) {
    const index = reg - 0x60;
    levels[index] = value;
    opzWrite(reg, pan[index & 7] === 0 ? 0x7f : value);
  } else opzWrite(reg, value);
}

function play() {
  while (true) {
    const due = startNs + Math.round(samples * 1e9 / SAMPLE_RATE);
    const delay = due - card.time_ns();
    if (delay > 0) { card.after(delay, play); return; }
    const op = data[cursor];
    const item = command(cursor);
    if (item.end) {
      if (loopAt === null) { card.log("VGM playback complete"); return; }
      cursor = loopAt;
    } else {
      if (op === 0x54) {
        opmWrite(data[cursor + 1], data[cursor + 2]);
      }
      samples += item.wait;
      cursor += item.size;
    }
  }
}

export function on_reset() {
  data = project.read(FILE);
  cursor = validate();
  pan = new Uint8Array(8);
  fraction = new Uint8Array(8);
  levels = new Uint8Array(32);
  samples = 0;
  startNs = card.time_ns();
  play();
}
