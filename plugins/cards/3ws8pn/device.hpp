#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

// 3WS8PN (S3W2) fantasy wavetable sound chip.
//
// Ported from the reference implementation in s3w2_core (sound.hpp/sound.cpp).
//
// Register map (big endian):
//   0x000-0x7FF  wavetable RAM, 8 channels x 256 bytes
//   0x800-0x8FF  control registers, 8 channels x 32 bytes
//     +0x00-0x01 frequency in Hz
//     +0x02      waveform type: 0 wavetable, 1 PCM, 2 noise, 3 DMA PCM
//     +0x03      volume 0-255
//     +0x04      panpot, MSB left 4 bits, LSB right 4 bits
//     +0x05      modulation type (MSB 5 bits) | target channel (LSB 3 bits)
//     +0x06-0x07 modulation parameter 1
//     +0x08-0x09 modulation parameter 2
//     +0x0A      any access resets the phase (a write also reloads the LFSR)
//     +0x0B      targeting mode, bit 0: 0 absolute, 1 relative (3 bit signed)
//     +0x10-0x12 PCM start address (20 bit)
//     +0x13-0x15 PCM end address (20 bit)
//     +0x16-0x18 PCM loop address (20 bit)
//     +0x19      PCM control, bit 0 play/stop, bit 1 loop
//   0x900-0xFFF  unimplemented, reads as 0 and ignores writes
//
// Timing: the channel phase advances by frequency/192000 per native sample,
// exactly like the reference's frequency/SOUND_CLOCK. Four native samples are
// averaged into one 48 kHz output frame.

namespace srz80::threews8pn {

constexpr uint32_t kRegisterSize = 0x1000;
constexpr uint32_t kChannelCount = 8;
constexpr uint32_t kWavetableSize = 256;
constexpr uint32_t kPcmSize = 1u << 20;
constexpr uint32_t kSampleRate = 48'000;
constexpr uint32_t kSoundClock = 192'000; // 9.216 MHz / 48
constexpr uint32_t kSubsampleCount = kSoundClock / kSampleRate;
constexpr uint32_t kNoiseLfsrSeed = 0x12D4803Cu;

enum Waveform : uint8_t { kWavetable = 0, kPcm = 1, kNoise = 2, kDmaPcm = 3 };
enum Modulation : uint8_t {
    kModNone = 0,
    kModPhase = 1,
    kModRing = 2,
    kModHardSync = 3,
    kModWindow = 4,
};
// A noise channel reuses modulation type 1 as a custom LFSR tap mask.
constexpr uint8_t kModNoiseCustomTap = kModPhase;

struct Channel {
    std::array<uint8_t, kWavetableSize> wave{};
    uint16_t frequency = 0;
    uint8_t type = kWavetable;
    uint8_t volume = 0;
    uint8_t pan = 0xFF;
    uint8_t mod = kModNone;
    uint8_t target = 0;
    uint8_t target_mode = 0;
    uint16_t p1 = 0;
    uint16_t p2 = 0;
    uint32_t start = 0, end = 0, loop = 0;
    uint8_t control = 0;
    uint64_t old_phase = 0;
    uint32_t lfsr = kNoiseLfsrSeed;
    double phase = 0.0;
    int16_t last = 0;
    bool active = true;
    Channel() { wave.fill(0x80); }
};

struct Device {
    static constexpr uint32_t register_size = kRegisterSize;
    static constexpr uint32_t sample_rate = kSampleRate;

    std::array<Channel, kChannelCount> ch{};
    std::vector<uint8_t> pcm = std::vector<uint8_t>(kPcmSize, 0);

    void reset() {
        std::fill(pcm.begin(), pcm.end(), 0);
        for (auto &c : ch)
            c = Channel{};
    }

    uint8_t read(uint32_t address, bool side_effects) {
        if (address <= 0x7FF)
            return ch[(address >> 8) & (kChannelCount - 1)].wave[address & 0xFF];
        if (address > 0x8FF)
            return 0;
        auto &c = channel(address);
        switch (control_offset(address)) {
        case 0x00: return uint8_t(c.frequency >> 8);
        case 0x01: return uint8_t(c.frequency);
        case 0x02: return c.type;
        case 0x03: return c.volume;
        case 0x04: return c.pan;
        case 0x05: return uint8_t((c.mod << 3) | (c.target & 7));
        case 0x06: return uint8_t(c.p1 >> 8);
        case 0x07: return uint8_t(c.p1);
        case 0x08: return uint8_t(c.p2 >> 8);
        case 0x09: return uint8_t(c.p2);
        case 0x0A:
            if (side_effects)
                c.phase = 0.0;
            return 0;
        case 0x0B: return uint8_t(c.target_mode & 1);
        case 0x10: return pcm_byte(c.start, 2);
        case 0x11: return pcm_byte(c.start, 1);
        case 0x12: return pcm_byte(c.start, 0);
        case 0x13: return pcm_byte(c.end, 2);
        case 0x14: return pcm_byte(c.end, 1);
        case 0x15: return pcm_byte(c.end, 0);
        case 0x16: return pcm_byte(c.loop, 2);
        case 0x17: return pcm_byte(c.loop, 1);
        case 0x18: return pcm_byte(c.loop, 0);
        case 0x19: return c.control;
        default: return 0;
        }
    }

    void write(uint32_t address, uint8_t value) {
        if (address <= 0x7FF) {
            ch[(address >> 8) & (kChannelCount - 1)].wave[address & 0xFF] = value;
            return;
        }
        if (address > 0x8FF)
            return;
        auto &c = channel(address);
        switch (control_offset(address)) {
        case 0x00: c.frequency = uint16_t((c.frequency & 0x00FF) | (uint32_t(value) << 8)); break;
        case 0x01: c.frequency = uint16_t((c.frequency & 0xFF00) | value); break;
        case 0x02: c.type = value; break;
        case 0x03: c.volume = value; break;
        case 0x04: c.pan = value; break;
        case 0x05:
            c.mod = uint8_t(value >> 3);
            c.target = uint8_t(value & 7);
            break;
        case 0x06: c.p1 = uint16_t((c.p1 & 0x00FF) | (uint32_t(value) << 8)); break;
        case 0x07: c.p1 = uint16_t((c.p1 & 0xFF00) | value); break;
        case 0x08: c.p2 = uint16_t((c.p2 & 0x00FF) | (uint32_t(value) << 8)); break;
        case 0x09: c.p2 = uint16_t((c.p2 & 0xFF00) | value); break;
        case 0x0A:
            c.phase = 0.0;
            c.lfsr = kNoiseLfsrSeed;
            break;
        case 0x0B: c.target_mode = uint8_t(value & 1); break;
        // PCM addresses are 20 bit; the top nibble of the MSB byte is reserved.
        case 0x10: c.start = (c.start & 0x00FFFFu) | ((uint32_t(value) & 0x0F) << 16); break;
        case 0x11: c.start = (c.start & 0x0F00FFu) | (uint32_t(value) << 8); break;
        case 0x12: c.start = (c.start & 0x0FFF00u) | value; break;
        case 0x13: c.end = (c.end & 0x00FFFFu) | ((uint32_t(value) & 0x0F) << 16); break;
        case 0x14: c.end = (c.end & 0x0F00FFu) | (uint32_t(value) << 8); break;
        case 0x15: c.end = (c.end & 0x0FFF00u) | value; break;
        case 0x16: c.loop = (c.loop & 0x00FFFFu) | ((uint32_t(value) & 0x0F) << 16); break;
        case 0x17: c.loop = (c.loop & 0x0F00FFu) | (uint32_t(value) << 8); break;
        case 0x18: c.loop = (c.loop & 0x0FFF00u) | value; break;
        case 0x19:
            c.control = value;
            c.active = (value & 1) != 0;
            break;
        default: break;
        }
    }

    // One native sample of one channel; the volume is applied to the result.
    int16_t sample(uint32_t index) {
        auto &c = ch[index];
        if (!c.active)
            return 0;
        const uint32_t target = target_channel(index, c);
        int32_t value = 0;
        switch (c.type) {
        case kPcm: value = sample_pcm(c, target); break;
        case kNoise: value = sample_noise(c); break;
        // DMA PCM is not implemented by the reference implementation either.
        case kDmaPcm: value = 0; break;
        default: value = sample_wavetable(c, target); break;
        }
        c.last = int16_t(value);
        return int16_t(value * c.volume / 4);
    }

    void render(uint32_t frames, int16_t *out) {
        std::fill(out, out + 2 * frames, 0);
        for (uint32_t frame = 0; frame < frames; ++frame)
            for (uint32_t sub = 0; sub < kSubsampleCount; ++sub)
                for (uint32_t index = 0; index < kChannelCount; ++index) {
                    const int32_t s = sample(index);
                    const int32_t left = int32_t(ch[index].pan >> 4) * s / 15;
                    const int32_t right = int32_t(ch[index].pan & 0x0F) * s / 15;
                    out[2 * frame] = clamp16(int32_t(out[2 * frame]) + left);
                    out[2 * frame + 1] = clamp16(int32_t(out[2 * frame + 1]) + right);
                }
    }

  private:
    static constexpr uint32_t kChannelMask = kChannelCount - 1;

    static Channel &channel_of(std::array<Channel, kChannelCount> &channels, uint32_t address) {
        return channels[((address - 0x800) >> 5) & kChannelMask];
    }
    static uint32_t control_offset(uint32_t address) { return (address - 0x800) & 0x1F; }
    Channel &channel(uint32_t address) { return channel_of(ch, address); }

    static uint8_t pcm_byte(uint32_t address, uint32_t shift) {
        return uint8_t((address >> (shift * 8)) & 0xFF);
    }
    static int16_t clamp16(int32_t value) {
        return int16_t(std::clamp(value, -32768, 32767));
    }
    // Phase in wavetable steps (256 per period), saturated against overflow.
    static uint64_t phase_steps(double phase) {
        if (!(phase > 0.0))
            return 0;
        const double steps = phase * 256.0;
        return steps < 1.8446744073709552e19 ? uint64_t(steps) : UINT64_MAX;
    }

    // Absolute target channel: bit 0 of the targeting mode selects a signed
    // 3-bit relative offset, otherwise the target field is an absolute index.
    static uint32_t target_channel(uint32_t carrier, const Channel &c) {
        if ((c.target_mode & 1) == 0)
            return c.target & kChannelMask;
        int32_t offset = int32_t(c.target & 7);
        if (offset >= 4)
            offset -= 8;
        int32_t index = int32_t(carrier) + offset;
        if (index < 0)
            index += int32_t(kChannelCount);
        else if (index >= int32_t(kChannelCount))
            index -= int32_t(kChannelCount);
        return uint32_t(index) & kChannelMask;
    }

    static int16_t phase_offset(const Channel &c, int16_t modulator) {
        return int16_t((int32_t(c.p1) * int32_t(modulator)) >> 12);
    }

    int32_t sample_wavetable(Channel &c, uint32_t target) {
        c.phase += double(c.frequency) / double(kSoundClock);
        uint32_t index = uint32_t(phase_steps(c.phase)) & 0xFF;
        const int16_t modulator = ch[target].last;
        if (c.mod == kModPhase)
            index = uint32_t(int32_t(index) + int32_t(phase_offset(c, modulator))) & 0xFF;
        else if (c.mod == kModHardSync && ch[target].phase < 1.0) {
            c.phase = 0.0;
            index = 0;
        }
        uint8_t value = c.wave[index];
        if (c.mod == kModRing) {
            const int32_t mod = ((int32_t(value) - 128) * int32_t(modulator) * int32_t(c.p1)) / 65536 + 128;
            value = uint8_t(std::clamp(mod, 0, 255));
        } else if (c.mod == kModWindow && modulator < 0) {
            value = 128;
        }
        return int32_t(value) - 128;
    }

    int32_t sample_pcm(Channel &c, uint32_t target) {
        uint64_t phase = phase_steps(c.phase);
        if (c.mod == kModPhase)
            phase = uint64_t(int64_t(phase) + int64_t(phase_offset(c, ch[target].last)));
        const uint32_t address = (uint32_t(phase) + c.start) & (kPcmSize - 1);
        int32_t value = int32_t(pcm[address]) - 128;
        if (address >= c.end) {
            if (c.control & 2)
                c.phase = double(c.loop - c.start) / 256.0;
            else {
                c.phase = double(c.end - c.start) / 256.0;
                value = 0;
            }
        } else {
            c.phase += double(c.frequency) / double(kSoundClock) / 8.0;
        }
        return value;
    }

    int32_t sample_noise(Channel &c) {
        c.phase += double(c.frequency) / double(kSoundClock);
        const uint64_t phase = phase_steps(c.phase);
        // The LFSR is clocked once every eight wavetable steps, which is what
        // makes the frequency register audible in noise mode.
        if (c.old_phase / 8 != phase / 8) {
            c.old_phase = phase;
            uint32_t bit = 0;
            if (c.mod == kModNoiseCustomTap) {
                const uint32_t taps = (uint32_t(c.p1) << 16) | c.p2; // 23-bit tap mask
                for (uint32_t i = 0; i < 23; ++i)
                    if (taps & (1u << i))
                        bit ^= (c.lfsr >> i) & 1u;
            } else {
                bit = ((c.lfsr >> 0) ^ (c.lfsr >> 1)) & 1u;
            }
            c.lfsr = (c.lfsr >> 1) | (bit << 22);
        }
        return (c.lfsr & 1u) ? 127 : -128;
    }
};

} // namespace srz80::threews8pn
