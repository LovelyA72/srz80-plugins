#include "ay8913.hpp"

#include <algorithm>
#include <cstring>

namespace ay8913 {
namespace {

// ay8910_param from MAME (RL = 2000, Matthew Westcott measurements).
// r_up = 800000, r_down = 8000000, 16 DAC resistor levels.
constexpr double kRUp = 800000.0;
constexpr double kRDown = 8000000.0;
constexpr double kRLoad = 1000.0;
constexpr double kRes[16] = {
    15950, 15350, 15090, 14760, 14275, 13620, 12890, 11370,
    10600, 8590,  7190,  5985,  4820,  3945,  3017,  2345,
};

constexpr double kMaxOutput = 1.0;

inline unsigned bit(unsigned value, unsigned n) { return (value >> n) & 1u; }

} // namespace

core::core() { build_tables(); }

void core::build_tables() {
    // build_single_table(rl, par, normalize=1, tab, zero_is_off)
    // The AY uses the same parameter table for fixed volume and envelope;
    // the only difference is zero_is_off (1 for volume, 0 for envelope).
    auto build = [](double rl, double r_up, double r_down, const double *res, int count,
                    int zero_is_off, float *tab) {
        double temp[32], min = 10.0, max = 0.0;
        for (int j = 0; j < count; ++j) {
            double rt = 1.0 / r_down + 1.0 / rl;
            double rw = 1.0 / res[j];
            rt += 1.0 / res[j];
            if (!(zero_is_off && j == 0)) {
                rw += 1.0 / r_up;
                rt += 1.0 / r_up;
            }
            temp[j] = rw / rt;
            if (temp[j] < min)
                min = temp[j];
            if (temp[j] > max)
                max = temp[j];
        }
        for (int j = 0; j < count; ++j)
            tab[j] = static_cast<float>(kMaxOutput * (((temp[j] - min) / (max - min)) - 0.25) * 0.5);
    };
    build(kRLoad, kRUp, kRDown, kRes, 16, 1, vol_table_);
    build(kRLoad, kRUp, kRDown, kRes, 16, 0, env_table_);
}

void core::reset() {
    active_ = false;
    register_latch_ = 0;
    rng_ = 1;
    for (auto &t : tones_)
        t = tone_t{};
    envelope_ = envelope_t{};
    count_noise_ = 0;
    prescale_noise_ = 0;
    // MAME zeroes registers 0..13 (i < AY_PORTA).  Zero all 16 here so that
    // the inert port registers $0E/$0F are also deterministic after reset.
    for (unsigned i = 0; i < kRegisters; ++i)
        write_reg(i, 0);
}

void core::write_reg(unsigned r, unsigned v) {
    // Collapse the shared envelope-shape register bank (no-op for the 8913,
    // which has no expanded bank, but mirrors MAME).
    if ((r & 0x0f) == 0x0d)
        r &= 0x0f;

    regs_[r] = static_cast<uint8_t>(v);

    switch (r) {
    case 0x00:
    case 0x01:
        tones_[0].period = regs_[0x00] | ((regs_[0x01] & 0x0f) << 8);
        break;
    case 0x02:
    case 0x03:
        tones_[1].period = regs_[0x02] | ((regs_[0x03] & 0x0f) << 8);
        break;
    case 0x04:
    case 0x05:
        tones_[2].period = regs_[0x04] | ((regs_[0x05] & 0x0f) << 8);
        break;
    case 0x06: // noise period: applied live, no cached action
        break;
    case 0x07: // enable/mixer: read live from regs_[7]; no GPIO side effect on the 8913
        break;
    case 0x08:
        tones_[0].volume = regs_[0x08];
        break;
    case 0x09:
        tones_[1].volume = regs_[0x09];
        break;
    case 0x0a:
        tones_[2].volume = regs_[0x0a];
        break;
    case 0x0b:
    case 0x0c:
        envelope_.period = regs_[0x0b] | (regs_[0x0c] << 8);
        break;
    case 0x0d: {
        const uint8_t shape = regs_[0x0d];
        envelope_.attack = (shape & 0x04) ? kEnvStepMask : 0x00;
        if ((shape & 0x08) == 0) {
            envelope_.hold = 1;
            envelope_.alternate = envelope_.attack;
        } else {
            envelope_.hold = shape & 0x01;
            envelope_.alternate = shape & 0x02;
        }
        envelope_.step = kEnvStepMask;
        envelope_.holding = 0;
        envelope_.volume = static_cast<uint8_t>(envelope_.step ^ envelope_.attack);
        break;
    }
    case 0x0e:
    case 0x0f: // I/O ports: no pins on the 8913, stored only
        break;
    default: // reserved registers read as zero
        regs_[r] = 0;
        break;
    }
}

void core::set_register(unsigned r, uint8_t value) {
    write_reg(r & 0x0f, value);
}

void core::latch_address(uint8_t value) {
    // A7..A4 are a mask-programmed code; the common part is 0000.  A write
    // whose upper nibble does not match leaves the chip inactive and does not
    // update the latch (MAME: m_active = (data >> 4) == 0).
    if ((value >> 4) == 0) {
        active_ = true;
        register_latch_ = value & 0x0f;
    } else {
        active_ = false;
    }
}

void core::write_data(uint8_t value) {
    if (active_)
        write_reg(register_latch_, value);
}

uint8_t core::read_data() const {
    if (!active_)
        return 0xff; // high impedance
    return regs_[register_latch_ & 0x0f];
}

uint32_t core::tone_period(unsigned channel) const { return tones_[channel].period; }
bool core::tone_output(unsigned channel) const { return tones_[channel].output != 0; }
bool core::noise_output() const { return (rng_ & 1u) != 0; }
uint32_t core::rng_value() const { return rng_; }
uint8_t core::envelope_volume() const { return envelope_.volume; }

bool core::channel_enabled(unsigned channel) const {
    const unsigned tone_enable = bit(regs_[0x07], channel);
    const unsigned noise_enable = bit(regs_[0x07], 3 + channel);
    return ((tones_[channel].output | tone_enable) & (noise_output() | noise_enable)) != 0;
}

void core::step() {
    // One internal tick = input_clock / 8.
    for (auto &t : tones_) {
        const uint32_t period = std::max<uint32_t>(1, t.period);
        t.count += 1;
        while (static_cast<uint32_t>(t.count) >= period) {
            t.duty_cycle = (t.duty_cycle - 1) & 0x1f;
            t.output = t.duty_cycle & 1u;
            t.count -= static_cast<int32_t>(period);
        }
    }

    const unsigned noise_period = regs_[0x06] & 0x1f;
    if ((++count_noise_) >= static_cast<int32_t>(noise_period)) {
        count_noise_ = 0;
        prescale_noise_ ^= 1u;
        if (!prescale_noise_) {
            // 17-bit LFSR: input = bit0 ^ bit3.
            rng_ = (rng_ >> 1) | (((rng_ & 1u) ^ ((rng_ >> 3) & 1u)) << 16);
        }
    }

    if (envelope_.holding == 0) {
        const uint32_t period = envelope_.period * kEnvStepMultiplier;
        if (++envelope_.count >= static_cast<int32_t>(period)) {
            envelope_.count = 0;
            --envelope_.step;
            if (envelope_.step < 0) {
                if (envelope_.hold) {
                    if (envelope_.alternate)
                        envelope_.attack ^= kEnvStepMask;
                    envelope_.holding = 1;
                    envelope_.step = 0;
                } else {
                    if (envelope_.alternate && (envelope_.step & (kEnvStepMask + 1)))
                        envelope_.attack ^= kEnvStepMask;
                    envelope_.step &= kEnvStepMask;
                }
            }
        }
    }
    envelope_.volume = static_cast<uint8_t>(envelope_.step ^ envelope_.attack);
}

float core::channel_sample(unsigned channel) const {
    const tone_t &tone = tones_[channel];
    const bool enabled = channel_enabled(channel);
    // MAME indexes table[0] when the mix is 0 (the DC-offset "off" level),
    // rather than emitting a literal zero.
    if (bit(tone.volume, 4))
        return enabled ? env_table_[envelope_.volume & 0x0f] : env_table_[0];
    return enabled ? vol_table_[tone.volume & 0x0f] : vol_table_[0];
}

float core::sample() const {
    float mix = 0.0f;
    for (unsigned channel = 0; channel < kChannels; ++channel)
        mix += channel_sample(channel);
    return mix;
}

void core::save_state(uint8_t *dst) const {
    std::memset(dst, 0, serialized_size);
    std::memcpy(dst, regs_, kRegisters);
    dst[kRegisters] = register_latch_;
    dst[1 + kRegisters] = active_ ? 1 : 0;

    size_t off = 2 + kRegisters;
    auto put32 = [&](uint32_t v) {
        dst[off++] = static_cast<uint8_t>(v);
        dst[off++] = static_cast<uint8_t>(v >> 8);
        dst[off++] = static_cast<uint8_t>(v >> 16);
        dst[off++] = static_cast<uint8_t>(v >> 24);
    };

    put32(rng_);
    put32(static_cast<uint32_t>(count_noise_));
    dst[off++] = prescale_noise_;

    for (const auto &t : tones_) {
        put32(t.period);
        dst[off++] = t.volume;
        put32(static_cast<uint32_t>(t.count));
        dst[off++] = t.duty_cycle;
        dst[off++] = t.output;
    }

    put32(envelope_.period);
    put32(static_cast<uint32_t>(envelope_.count));
    dst[off++] = static_cast<uint8_t>(envelope_.step);
    dst[off++] = envelope_.volume;
    dst[off++] = envelope_.hold;
    dst[off++] = envelope_.alternate;
    dst[off++] = envelope_.attack;
    dst[off++] = envelope_.holding;
}

bool core::load_state(const uint8_t *src) {
    if (!src || src[kRegisters] > 15 || src[1 + kRegisters] > 1)
        return false;

    auto staged = *this;
    std::memcpy(staged.regs_, src, kRegisters);
    staged.register_latch_ = src[kRegisters];
    staged.active_ = src[1 + kRegisters] != 0;

    size_t off = 2 + kRegisters;
    auto get32 = [&]() -> uint32_t {
        uint32_t v = static_cast<uint32_t>(src[off]) | (static_cast<uint32_t>(src[off + 1]) << 8) |
                     (static_cast<uint32_t>(src[off + 2]) << 16) |
                     (static_cast<uint32_t>(src[off + 3]) << 24);
        off += 4;
        return v;
    };

    staged.rng_ = get32();
    staged.count_noise_ = static_cast<int32_t>(get32());
    staged.prescale_noise_ = src[off++];

    for (auto &t : staged.tones_) {
        t.period = get32();
        t.volume = src[off++];
        t.count = static_cast<int32_t>(get32());
        t.duty_cycle = src[off++];
        t.output = src[off++];
    }

    staged.envelope_.period = get32();
    staged.envelope_.count = static_cast<int32_t>(get32());
    staged.envelope_.step = static_cast<int8_t>(src[off++]);
    staged.envelope_.volume = src[off++];
    staged.envelope_.hold = src[off++];
    staged.envelope_.alternate = src[off++];
    staged.envelope_.attack = src[off++];
    staged.envelope_.holding = src[off++];
    if (staged.rng_ > 0x1ffff || staged.prescale_noise_ > 1 || staged.count_noise_ < 0 ||
        staged.count_noise_ > 31 || staged.envelope_.period > 0xffff ||
        staged.envelope_.count < 0 || staged.envelope_.count > 0x1ffff ||
        staged.envelope_.volume > 15 || staged.envelope_.step < 0 || staged.envelope_.step > 15 ||
        staged.envelope_.hold > 1 || staged.envelope_.holding > 1 ||
        (staged.envelope_.alternate != 0 && staged.envelope_.alternate != 2 && staged.envelope_.alternate != 15) ||
        (staged.envelope_.attack != 0 && staged.envelope_.attack != 15)) return false;
    for (const auto &tone : staged.tones_)
        if (tone.period > 0xfff || tone.count < 0 || tone.count > 0xfff ||
            tone.duty_cycle > 31 || tone.output > 1) return false;
    *this = staged;
    return true;
}

} // namespace ay8913
