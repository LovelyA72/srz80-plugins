#include "core.hpp"

namespace vrc6 {
namespace {


/* The sawtooth ramp is seven accumulator reactions long: the rate is added
   six times and the seventh reaction clears the accumulator.  The accumulator
   only reacts on every second ramp clock, so a full ramp is 14 clocks. */
constexpr uint32_t kSawAdditions = 6;

void put_u8(uint8_t *&at, uint8_t value) { *at++ = value; }
void put_u16(uint8_t *&at, uint32_t value) {
    *at++ = static_cast<uint8_t>(value);
    *at++ = static_cast<uint8_t>(value >> 8);
}
void put_u32(uint8_t *&at, uint32_t value) {
    *at++ = static_cast<uint8_t>(value);
    *at++ = static_cast<uint8_t>(value >> 8);
    *at++ = static_cast<uint8_t>(value >> 16);
    *at++ = static_cast<uint8_t>(value >> 24);
}
uint8_t get_u8(const uint8_t *&at) { return *at++; }
uint32_t get_u16(const uint8_t *&at) {
    const uint32_t value = at[0] | (static_cast<uint32_t>(at[1]) << 8);
    at += 2;
    return value;
}
uint32_t get_u32(const uint8_t *&at) {
    const uint32_t value = at[0] | (static_cast<uint32_t>(at[1]) << 8) |
                           (static_cast<uint32_t>(at[2]) << 16) |
                           (static_cast<uint32_t>(at[3]) << 24);
    at += 4;
    return value;
}

} // namespace

uint32_t Core::effective_period(uint16_t period, uint32_t shift) { return period >> shift; }

void Core::power_on() {
    for (auto &pulse : pulses_)
        pulse = Pulse{};
    saw_ = Saw{};
    shift_ = 0;
    halt_ = false;
}

void Core::write(uint32_t offset, uint8_t value) {
    if (!decodes(offset))
        return;
    /* Only A0/A1 select the register inside a block and A12/A13 select the
       block, so each block mirrors its registers every four bytes. */
    const uint32_t block = (offset >> 12) & 3u;
    const uint32_t reg = offset & 3u;
    if (block == 0 && reg == 3) {
        write_control(value);
        return;
    }
    if (block == 0 || block == 1) {
        if (reg < 3)
            write_pulse(pulses_[block], reg, value);
        return;
    }
    if (block == 2 && reg < 3)
        write_saw(reg, value);
    /* $B003 belongs to the mapper's mirroring control, not to the audio unit. */
}

void Core::write_pulse(Pulse &pulse, uint32_t reg, uint8_t value) {
    switch (reg) {
    case 0: /* MDDD VVVV: mode, duty, volume */
        pulse.control = value;
        break;
    case 1: /* low 8 bits of the period */
        pulse.period_low = value;
        refresh_period(pulse);
        break;
    case 2: { /* E... FFFF: enable and high 4 bits of the period */
        pulse.period_high = value;
        refresh_period(pulse);
        if (!pulse.enabled) {
            /* A channel restarted by the enable bit begins its duty cycle at
               the first step. */
            pulse.step = 15;
        }
        pulse.enabled = (value & 0x80u) != 0;
        break;
    }
    default:
        break;
    }
}

void Core::write_saw(uint32_t reg, uint8_t value) {
    switch (reg) {
    case 0: /* ..AA AAAA: accumulator rate */
        saw_.rate = value & 0x3Fu;
        break;
    case 1:
        saw_.period_low = value;
        refresh_period(saw_);
        break;
    case 2: {
        saw_.period_high = value;
        refresh_period(saw_);
        if (!saw_.enabled) {
            /* Restarting through the enable bit clears the ramp and its
               reaction position. */
            saw_.accumulator = 0;
            saw_.reactions = 0;
            saw_.half = 0;
        }
        saw_.enabled = (value & 0x80u) != 0;
        break;
    }
    default:
        break;
    }
}

void Core::write_control(uint8_t value) {
    halt_ = (value & 0x01u) != 0;
    /* The 256x flag wins over the 16x flag; both raise pitch by shifting the
       periods right. */
    shift_ = (value & 0x04u) ? 8u : (value & 0x02u) ? 4u : 0u;
    for (auto &pulse : pulses_)
        refresh_period(pulse);
    refresh_period(saw_);
}

void Core::refresh_period(Pulse &pulse) {
    pulse.period = static_cast<uint16_t>(((pulse.period_high & 0x0Fu) << 8) | pulse.period_low);
    /* A shrunken period must not leave the divider with a backlog, or the
       channel would burst out several steps at once. */
    const uint32_t target = effective_period(pulse.period, shift_);
    if (pulse.divider > target)
        pulse.divider = target;
}

void Core::refresh_period(Saw &saw) {
    saw.period = static_cast<uint16_t>(((saw.period_high & 0x0Fu) << 8) | saw.period_low);
    const uint32_t target = effective_period(saw.period, shift_);
    if (saw.divider > target)
        saw.divider = target;
}

void Core::clock() {
    if (halt_)
        return; /* frozen: the outputs keep their current levels */
    for (auto &pulse : pulses_)
        clock_pulse(pulse);
    clock_saw();
}

void Core::clock_pulse(Pulse &pulse) {
    if (!pulse.enabled)
        return; /* a disabled channel is held in place */
    if (++pulse.divider > effective_period(pulse.period, shift_)) {
        pulse.divider = 0;
        pulse.step = (pulse.step - 1u) & 0x0Fu;
    }
}

void Core::clock_saw() {
    if (!saw_.enabled)
        return;
    if (++saw_.divider > effective_period(saw_.period, shift_)) {
        saw_.divider = 0;
        saw_.half ^= 1u;
        if (saw_.half)
            return; /* odd ramp clocks leave the accumulator alone */
        if (saw_.reactions == kSawAdditions) {
            saw_.accumulator = 0;
            saw_.reactions = 0;
        } else {
            saw_.accumulator = static_cast<uint8_t>(saw_.accumulator + saw_.rate);
            ++saw_.reactions;
        }
    }
}

uint32_t Core::channel_output(uint32_t channel) const {
    if (channel < kPulseCount) {
        const Pulse &pulse = pulses_[channel];
        if (!pulse.enabled)
            return 0;
        /* The duty generator emits the volume for the steps at or below the
           duty value - the last (duty + 1) of its 16 down-counting steps.
           Mode bypasses the duty generator entirely. */
        const bool mode = (pulse.control & 0x80u) != 0;
        const uint32_t duty = (pulse.control >> 4) & 7u;
        return (mode || pulse.step <= duty) ? (pulse.control & 0x0Fu) : 0;
    }
    if (!saw_.enabled)
        return 0;
    return saw_.accumulator >> 3;
}

bool Core::channel_enabled(uint32_t channel) const {
    return channel < kPulseCount ? pulses_[channel].enabled : saw_.enabled;
}
bool Core::pulse_mode(uint32_t pulse) const { return (pulses_[pulse].control & 0x80u) != 0; }
uint32_t Core::pulse_duty(uint32_t pulse) const { return (pulses_[pulse].control >> 4) & 7u; }
uint32_t Core::pulse_volume(uint32_t pulse) const { return pulses_[pulse].control & 0x0Fu; }
uint32_t Core::pulse_step(uint32_t pulse) const { return pulses_[pulse].step; }
uint32_t Core::saw_rate() const { return saw_.rate; }
uint32_t Core::saw_accumulator() const { return saw_.accumulator; }

uint32_t Core::period(uint32_t channel) const {
    return channel < kPulseCount ? pulses_[channel].period : saw_.period;
}

void Core::save_state(uint8_t *buffer) const {
    uint8_t *at = buffer;
    put_u8(at, halt_ ? 1u : 0u);
    put_u8(at, static_cast<uint8_t>(shift_));
    for (const auto &pulse : pulses_) {
        put_u8(at, pulse.control);
        put_u8(at, pulse.period_low);
        put_u8(at, pulse.period_high);
        put_u16(at, pulse.period);
        put_u32(at, pulse.divider);
        put_u8(at, pulse.step);
        put_u8(at, pulse.enabled ? 1u : 0u);
    }
    put_u8(at, saw_.period_low);
    put_u8(at, saw_.period_high);
    put_u8(at, saw_.rate);
    put_u16(at, saw_.period);
    put_u32(at, saw_.divider);
    put_u8(at, saw_.accumulator);
    put_u8(at, saw_.reactions);
    put_u8(at, saw_.half);
    put_u8(at, saw_.enabled ? 1u : 0u);
}

bool Core::load_state(const uint8_t *buffer, uint64_t size) {
    if (!buffer || size != state_size())
        return false;
    const uint8_t *at = buffer;

    /* Everything is decoded and validated into a fresh unit first; the live
       unit is only replaced once the whole image is known to be sound. */
    Core loaded;
    const uint8_t halt = get_u8(at);
    const uint8_t shift = get_u8(at);
    if (halt > 1 || (shift != 0 && shift != 4 && shift != 8))
        return false;
    loaded.halt_ = halt != 0;
    loaded.shift_ = shift;

    for (auto &pulse : loaded.pulses_) {
        pulse.control = get_u8(at);
        pulse.period_low = get_u8(at);
        pulse.period_high = get_u8(at);
        pulse.period = static_cast<uint16_t>(get_u16(at));
        pulse.divider = get_u32(at);
        pulse.step = get_u8(at);
        const uint8_t enabled = get_u8(at);
        if (pulse.period > 0x0FFFu || pulse.period != (((pulse.period_high & 0x0Fu) << 8) |
                                                       pulse.period_low) ||
            pulse.step > 15u || enabled > 1u ||
            pulse.divider > effective_period(pulse.period, loaded.shift_))
            return false;
        pulse.enabled = enabled != 0;
    }

    loaded.saw_.period_low = get_u8(at);
    loaded.saw_.period_high = get_u8(at);
    loaded.saw_.rate = get_u8(at);
    loaded.saw_.period = static_cast<uint16_t>(get_u16(at));
    loaded.saw_.divider = get_u32(at);
    loaded.saw_.accumulator = get_u8(at);
    loaded.saw_.reactions = get_u8(at);
    loaded.saw_.half = get_u8(at);
    const uint8_t enabled = get_u8(at);
    if (loaded.saw_.rate > 0x3Fu || loaded.saw_.period > 0x0FFFu ||
        loaded.saw_.period !=
            (((loaded.saw_.period_high & 0x0Fu) << 8) | loaded.saw_.period_low) ||
        loaded.saw_.reactions > kSawAdditions || loaded.saw_.half > 1u || enabled > 1u ||
        loaded.saw_.divider > effective_period(loaded.saw_.period, loaded.shift_))
        return false;
    loaded.saw_.enabled = enabled != 0;

    *this = loaded;
    return true;
}

} // namespace vrc6
