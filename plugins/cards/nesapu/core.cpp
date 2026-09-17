#include "core.hpp"

/* A standalone Nintendo Entertainment System audio processing unit: two pulse
   channels with an envelope and a sweep unit each, a triangle channel with a
   linear counter, a noise channel, and a delta channel that reads its samples
   out of the machine's own address space.

   The unit follows the register behaviour published in the NESdev reference.
   Timing inside the unit is expressed in console CPU cycles; the APU cycle is
   half of one, so the frame sequencer and the envelope dividers step on every
   second clock while the channel timers step on every clock. */

namespace nesapu {
namespace {

constexpr uint8_t kStatusFrameIrq = 0x40;
constexpr uint8_t kStatusDeltaIrq = 0x80;
constexpr uint8_t kChannelBit[kChannelCount] = {0x01, 0x02, 0x04, 0x08, 0x10};

/* One entry per frame step: how many CPU cycles the step lasts.  The four-step
   sequence totals 29830 cycles and the five-step sequence 37282; the lengths
   are not all equal, which is why the step boundaries wander by a cycle or two
   relative to a plain division of the frame. */
struct FrameTiming {
    uint16_t step[5];
    uint8_t count;
};
constexpr FrameTiming kNtscFrame[2] = {{{7457, 7456, 7458, 7458, 0}, 4},
                                       {{7457, 7458, 7456, 7458, 7452}, 5}};
constexpr FrameTiming kPalFrame[2] = {{{8313, 8314, 8312, 8314, 0}, 4},
                                      {{8313, 8314, 8314, 8312, 8312}, 5}};

/* Indexed by the four bits written to $400E or $4010. */
constexpr uint16_t kNtscNoisePeriod[16] = {4,   8,   16,  32,  64,   96,
                                           128, 160, 202, 254, 380,  508,
                                           762, 1016, 2034, 4068};
constexpr uint16_t kPalNoisePeriod[16] = {4,   8,   14,  30,  60,   88,
                                          118, 148, 188, 236, 354,  472,
                                          708, 944, 1890, 3778};
constexpr uint16_t kNtscDeltaPeriod[16] = {428, 380, 340, 320, 286, 254, 226, 214,
                                           190, 160, 142, 128, 106, 84,  72,  54};
constexpr uint16_t kPalDeltaPeriod[16] = {398, 354, 316, 298, 276, 236, 210, 198,
                                          176, 148, 132, 118, 98,  78,  66,  50};

/* Programmed lengths, indexed by the five bits written to a length register.
   Entry one is the 254-cycle maximum. */
constexpr uint8_t kLengthTable[32] = {10,  254, 20, 2,  40, 4,  80,  6,  160, 8,  60,
                                      10,  14,  12, 26, 14, 12, 16,  24, 18,  48, 20,
                                      96,  22,  192, 24, 72, 26, 16, 28, 32,  30};

/* Duty patterns in the order $4000 bits 7-6 select them.  The last two entries
   are the complemented forms; the second pulse channel reads those two in the
   opposite order, which is a console quirk. */
constexpr uint8_t kDutyPattern[8] = {0x01, 0x03, 0x0F, 0xFC, 0x01, 0x0F, 0x03, 0xFC};

constexpr uint8_t kTrianglePattern[32] = {15, 14, 13, 12, 11, 10, 9,  8,  7,  6,  5,
                                          4,  3,  2,  1,  0,  0,  1,  2,  3,  4,  5,
                                          6,  7,  8,  9,  10, 11, 12, 13, 14, 15};

const FrameTiming &frame_timing(Region region, bool five_step) {
    if (region == Region::ntsc)
        return kNtscFrame[five_step ? 1 : 0];
    return kPalFrame[five_step ? 1 : 0];
}
const uint16_t *noise_periods(Region region) {
    return region == Region::ntsc ? kNtscNoisePeriod : kPalNoisePeriod;
}
const uint16_t *delta_periods(Region region) {
    return region == Region::ntsc ? kNtscDeltaPeriod : kPalDeltaPeriod;
}

/* State image: an eight-byte tag naming the unit, an eight-byte layout version,
   then a fixed field order.  Only the field order has to survive a version bump;
   every value is written byte-wise, low byte first, so the image does not depend
   on the host's word size or byte order. */
constexpr uint8_t kStateTag[8] = {'N', 'E', 'S', 'A', 'P', 'U', '2', 'A'};
constexpr uint64_t kStateVersion = 1;

void put16(uint8_t *out, uint16_t value) {
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8);
}
uint16_t get16(const uint8_t *in) {
    return static_cast<uint16_t>(in[0] | (static_cast<uint16_t>(in[1]) << 8));
}
void put64(uint8_t *out, uint64_t value) {
    for (uint32_t byte = 0; byte < 8; ++byte)
        out[byte] = static_cast<uint8_t>(value >> (byte * 8));
}

} // namespace

NesApu::NesApu() { power_on(); }

void NesApu::power_on() {
    for (Pulse &pulse : pulses_) {
        pulse = Pulse{};
        /* A fresh console leaves the reload register one cycle long, which keeps
           the first writes from producing a click. */
        pulse.period = 1;
        pulse.countdown = 1;
        pulse.sweep.divider = 1;
        pulse.sweep.countdown = 1;
    }
    triangle_ = Triangle{};
    triangle_.period = 1;
    triangle_.countdown = 1;
    /* Starting the triangle at the top of its sequence avoids a step down to
       silence at power on. */
    triangle_.step = 7;
    noise_ = Noise{};
    noise_.period = noise_periods(region_)[0];
    noise_.countdown = noise_.period;
    delta_ = Delta{};
    delta_.period = delta_periods(region_)[0];
    delta_.countdown = delta_.period;

    frame_count_ = 0;
    /* The first clock advances into step one, so the sequence runs
       7457, 14914, 22371, 29829 CPU cycles exactly as the console does. */
    frame_step_ = 3;
    divider_ = 0;
    reset_delay_ = 0;
    five_step_ = false;
    irq_inhibit_ = false;
    length_clocked_ = false;
    status_ = 0;
    stall_cycles_ = 0;
    sample_accum_ = 0;
}

void NesApu::set_region(Region region) {
    region_ = region;
    noise_.period = noise_periods(region_)[noise_.rate];
    delta_.period = delta_periods(region_)[delta_.rate];
    /* Restart the frame so the new step lengths apply from the beginning, and
       name the step that precedes the first one so the first clock lands on
       step one. */
    frame_count_ = 0;
    frame_step_ = five_step_ ? 4 : 3;
    reset_delay_ = 0;
}

void NesApu::set_sample_reader(bool (*reader)(void *context, uint16_t address, uint8_t *value),
                               void *context) {
    reader_ = reader;
    reader_context_ = context;
}

NesApu::Length &NesApu::length_of(uint32_t channel) {
    switch (channel) {
    case 0:
        return pulses_[0].length;
    case 1:
        return pulses_[1].length;
    case 2:
        return triangle_.length;
    default:
        return noise_.length;
    }
}

const NesApu::Length &NesApu::length_of(uint32_t channel) const {
    switch (channel) {
    case 0:
        return pulses_[0].length;
    case 1:
        return pulses_[1].length;
    case 2:
        return triangle_.length;
    default:
        return noise_.length;
    }
}

void NesApu::write_pulse(Pulse &pulse, uint32_t index, uint8_t value) {
    /* $4000 / $4004: duty, length halt, constant volume, envelope divider. */
    pulse.duty = static_cast<uint8_t>(((value >> 6) & 0x03u) + (index == 1 ? 4u : 0u));
    pulse.length.halt = (value & 0x20u) != 0;
    pulse.envelope.loop = pulse.length.halt;
    pulse.envelope.constant = (value & 0x10u) != 0;
    pulse.envelope.divider = static_cast<uint8_t>(value & 0x0Fu);
}

void NesApu::load_pulse_length(Pulse &pulse, uint8_t value) {
    /* $4003 / $4007: timer high bits, length reload, envelope restart. */
    pulse.period = static_cast<uint16_t>((pulse.period & 0x0FFu) | ((value & 0x07u) << 8));
    /* A length reload is lost when it lands in the very cycle that clocked the
       length counters and the counter was already non-zero. */
    if (pulse.length.enabled && !(length_clocked_ && pulse.length.value != 0))
        pulse.length.value = kLengthTable[value >> 3];
    pulse.envelope.start = true;
    pulse.started = true;
    pulse.step = 0;
}

void NesApu::write(uint32_t offset, uint8_t value) {
    if (!decodes(offset))
        return;
    switch (offset) {
    case 0x00:
    case 0x04:
        write_pulse(pulses_[offset >> 2], offset >> 2, value);
        break;
    case 0x01:
    case 0x05: {
        Pulse &pulse = pulses_[offset >> 2];
        pulse.sweep.enabled = (value & 0x80u) != 0;
        pulse.sweep.divider = static_cast<uint8_t>((value >> 4) & 0x07u);
        pulse.sweep.negate = (value & 0x08u) != 0;
        pulse.sweep.shift = static_cast<uint8_t>(value & 0x07u);
        pulse.sweep.reload = true;
        break;
    }
    case 0x02:
    case 0x06: {
        Pulse &pulse = pulses_[offset >> 2];
        pulse.period = static_cast<uint16_t>((pulse.period & 0x700u) | value);
        break;
    }
    case 0x03:
    case 0x07:
        load_pulse_length(pulses_[offset >> 2], value);
        break;
    case 0x08:
        /* $4008: the control flag doubles as the length halt flag, and the low
           seven bits are the linear counter reload value. */
        triangle_.control = (value & 0x80u) != 0;
        triangle_.length.halt = (value & 0x80u) != 0;
        triangle_.reload = static_cast<uint8_t>(value & 0x7Fu);
        break;
    case 0x0A:
        triangle_.period = static_cast<uint16_t>((triangle_.period & 0x700u) | value);
        break;
    case 0x0B:
        triangle_.period = static_cast<uint16_t>((triangle_.period & 0x0FFu) | ((value & 0x07u) << 8));
        if (triangle_.length.enabled && !(length_clocked_ && triangle_.length.value != 0))
            triangle_.length.value = kLengthTable[value >> 3];
        /* Writing the length register also sets the linear counter, which starts
           the channel without waiting for the next quarter frame. */
        triangle_.linear = triangle_.reload;
        break;
    case 0x0C:
        noise_.length.halt = (value & 0x20u) != 0;
        noise_.envelope.loop = noise_.length.halt;
        noise_.envelope.constant = (value & 0x10u) != 0;
        noise_.envelope.divider = static_cast<uint8_t>(value & 0x0Fu);
        break;
    case 0x0E:
        noise_.mode = (value & 0x80u) != 0;
        noise_.rate = static_cast<uint8_t>(value & 0x0Fu);
        noise_.period = noise_periods(region_)[noise_.rate];
        break;
    case 0x0F:
        if (noise_.length.enabled && !(length_clocked_ && noise_.length.value != 0))
            noise_.length.value = kLengthTable[value >> 3];
        noise_.envelope.start = true;
        break;
    case 0x10:
        delta_.rate = static_cast<uint8_t>(value & 0x0Fu);
        delta_.period = delta_periods(region_)[delta_.rate];
        delta_.irq_enabled = (value & 0x80u) != 0;
        if (!delta_.irq_enabled)
            status_ = static_cast<uint8_t>(status_ & ~kStatusDeltaIrq);
        delta_.loop = (value & 0x40u) != 0;
        break;
    case 0x11:
        /* The level counter is seven bits wide, so the top bit is discarded. */
        delta_.output = static_cast<uint8_t>(value & 0x7Fu);
        break;
    case 0x12:
        delta_.start = static_cast<uint16_t>(0xC000u | (static_cast<uint32_t>(value) << 6));
        break;
    case 0x13:
        delta_.length = static_cast<uint16_t>((static_cast<uint32_t>(value) << 4) | 1u);
        break;
    case 0x15:
        for (uint32_t channel = 0; channel < 4; ++channel) {
            Length &length = length_of(channel);
            const bool enabled = (value & kChannelBit[channel]) != 0;
            /* Enabling a silent counter loads entry zero; disabling always
               silences the channel and loses its count. */
            if (enabled && !length.enabled && length.value == 0)
                length.value = kLengthTable[0];
            length.enabled = enabled;
            if (!enabled)
                length.value = 0;
        }
        delta_.enabled = (value & kChannelBit[4]) != 0;
        if (!delta_.enabled) {
            delta_.remaining = 0;
            delta_.stopped = true;
        } else if (delta_.stopped) {
            /* Starting a sample when the unit was idle sets the address and the
               byte count from the programmed sample and begins fetching. */
            delta_.address = delta_.start;
            delta_.remaining = delta_.length;
            delta_.stopped = false;
        }
        status_ = static_cast<uint8_t>(status_ & ~kStatusFrameIrq);
        break;
    case 0x17:
        irq_inhibit_ = (value & 0x40u) != 0;
        five_step_ = (value & 0x80u) != 0;
        if (irq_inhibit_)
            status_ = static_cast<uint8_t>(status_ & ~kStatusFrameIrq);
        /* The sequence restarts a few cycles after the write rather than inside
           it, so a write landing in either half of an APU cycle behaves alike.
           The restart also emits a quarter and a half frame clock when the
           mode bit is set, which naming the preceding step achieves. */
        reset_delay_ = 4;
        break;
    default:
        /* $4014 is the sprite DMA port and $4016 the controller port; the unit
           does not answer them even though they sit inside this window. */
        break;
    }
}

uint8_t NesApu::read(uint32_t offset) {
    const uint8_t value = peek(offset);
    if (offset == kStatusOffset)
        status_ = static_cast<uint8_t>(status_ & ~kStatusFrameIrq);
    return value;
}

uint8_t NesApu::peek(uint32_t offset) const {
    if (offset != kStatusOffset)
        return 0;
    uint8_t value = status_;
    for (uint32_t channel = 0; channel < 4; ++channel)
        if (length_of(channel).value != 0)
            value = static_cast<uint8_t>(value | kChannelBit[channel]);
    if (delta_.remaining != 0)
        value = static_cast<uint8_t>(value | kChannelBit[4]);
    return value;
}

void NesApu::clock() {
    stall_cycles_ = 0;

    if (reset_delay_ != 0 && --reset_delay_ == 0) {
        /* The timer restart names the step that precedes the first one, so the
           first clock after the restart lands on step one. */
        frame_count_ = 0;
        frame_step_ = five_step_ ? 4 : 3;
    }

    /* The APU cycle is half a CPU cycle, so the frame sequencer and the three
       envelope dividers advance on every second clock.  The step lengths are
       published in CPU cycles, so each step lasts half as many APU cycles. */
    divider_ ^= 1u;
    if (divider_ == 0) {
        const uint16_t step_cycles =
            static_cast<uint16_t>(frame_timing(region_, five_step_).step[frame_step_] >> 1);
        if (++frame_count_ >= step_cycles) {
            frame_count_ = 0;
            clock_frame_sequencer();
        }
        clock_envelope(pulses_[0].envelope);
        clock_envelope(pulses_[1].envelope);
        clock_envelope(noise_.envelope);
    }

    /* A programmed period of zero is a one-cycle period, so the counter runs
       down from the programmed value and clocks on reaching zero. */
    clock_pulse(pulses_[0]);
    clock_pulse(pulses_[1]);

    /* The triangle advances its sequence only while the length counter and the
       linear counter are both non-zero; otherwise it holds its position, which
       is what keeps the channel silent instead of restarting it. */
    if (triangle_.countdown == 0) {
        triangle_.countdown = triangle_.period;
        if (triangle_.length.value != 0 && triangle_.linear != 0)
            triangle_.step = static_cast<uint8_t>((triangle_.step + 1) & 0x1Fu);
        refresh_triangle();
    } else {
        --triangle_.countdown;
    }

    if (noise_.countdown == 0) {
        /* The register is fifteen bits wide and the feedback lands on bit
           fourteen, so the shift discards bit fourteen rather than bit fifteen
           of the stored value.  The short sequence takes its second tap from bit
           six instead of bit one. */
        const uint16_t tap = noise_.mode ? static_cast<uint16_t>(noise_.shift >> 1)
                                         : static_cast<uint16_t>(noise_.shift >> 6);
        const uint16_t feedback = static_cast<uint16_t>((noise_.shift ^ tap) & 1u);
        noise_.shift = static_cast<uint16_t>(((noise_.shift >> 1) | (feedback << 14)) & 0x7FFFu);
        noise_.countdown = noise_.period;
        refresh_noise();
    } else {
        --noise_.countdown;
    }

    if (delta_.countdown == 0) {
        delta_.countdown = delta_.period;
        if (!delta_.silent) {
            if ((delta_.shift & 1u) != 0) {
                if (delta_.output < 126)
                    delta_.output = static_cast<uint8_t>(delta_.output + 2u);
            } else if (delta_.output > 1) {
                delta_.output = static_cast<uint8_t>(delta_.output - 2u);
            }
        }
        delta_.shift = static_cast<uint8_t>(delta_.shift >> 1);
        /* The unit can only leave silence at the end of an eight-bit cycle. */
        if (--delta_.bits == 0) {
            delta_.bits = 8;
            if (delta_.buffered) {
                delta_.shift = delta_.buffer;
                delta_.buffered = false;
                delta_.silent = false;
            } else {
                delta_.silent = true;
            }
        }
    } else {
        --delta_.countdown;
    }

    transfer_delta();
}

void NesApu::advance(uint32_t clock_hz, uint32_t sample_rate) {
    if (sample_rate == 0)
        return;
    sample_accum_ += clock_hz;
    uint64_t cycles = sample_accum_ / sample_rate;
    sample_accum_ %= sample_rate;
    while (cycles-- != 0)
        clock();
}

void NesApu::transfer_delta() {
    if (reader_ == nullptr || delta_.stopped || !delta_.enabled)
        return;
    if (delta_.remaining == 0 || delta_.buffered)
        return;
    uint8_t byte = 0;
    if (!reader_(reader_context_, delta_.address, &byte))
        return;
    delta_.buffer = byte;
    delta_.buffered = true;
    /* The console steals four cycles from the instruction that was executing
       when the refill happens. */
    stall_cycles_ += 4;
    delta_.address = static_cast<uint16_t>(delta_.address + 1u);
    if (--delta_.remaining == 0) {
        if (delta_.loop) {
            delta_.address = delta_.start;
            delta_.remaining = delta_.length;
        } else if (delta_.irq_enabled) {
            status_ = static_cast<uint8_t>(status_ | kStatusDeltaIrq);
        }
    }
}

void NesApu::clock_frame_sequencer() {
    const FrameTiming &timing = frame_timing(region_, five_step_);
    length_clocked_ = false;
    /* The frame counter counts APU cycles, so each step of the table lasts half
       as many of them as it does CPU cycles.  Step one is the step that follows
       a restart, and it clocks the envelopes and the linear counter.  In the
       four-step sequence the half-frame clocks land on steps two and four and
       the interrupt is raised at the end of step four; in the five-step sequence
       they land on steps two and five and no interrupt is raised. */
    const bool half_frame = five_step_ ? (frame_step_ == 0 || frame_step_ == 2 || frame_step_ == 4)
                                       : (frame_step_ == 0 || frame_step_ == 2);
    const bool quarter_frame = five_step_ ? frame_step_ != 3 : true;
    if (half_frame) {
        for (uint32_t channel = 0; channel < 4; ++channel)
            clock_length(length_of(channel));
        clock_sweep(pulses_[0]);
        clock_sweep(pulses_[1]);
        length_clocked_ = true;
    }
    if (quarter_frame)
        clock_quarter_frame();
    /* The interrupt flag is raised once per four-step sequence and never in the
       five-step sequence. */
    if (!five_step_ && frame_step_ == 2 && !irq_inhibit_)
        status_ = static_cast<uint8_t>(status_ | kStatusFrameIrq);
    frame_step_ = static_cast<uint8_t>(frame_step_ + 1u);
    if (frame_step_ >= timing.count)
        frame_step_ = 0;
}

void NesApu::clock_quarter_frame() {
    pulses_[0].envelope.start = true;
    pulses_[1].envelope.start = true;
    noise_.envelope.start = true;
    /* The reload flag wins over the decrement, and the control flag keeps the
       counter loaded; the control flag is only cleared when the length halt
       flag is clear. */
    if (triangle_.control)
        triangle_.linear = triangle_.reload;
    else if (triangle_.linear != 0)
        triangle_.linear = static_cast<uint8_t>(triangle_.linear - 1u);
    if (!triangle_.length.halt)
        triangle_.control = false;
}

void NesApu::clock_envelope(Envelope &envelope) {
    if (envelope.start) {
        envelope.start = false;
        envelope.decay = 15;
        envelope.countdown = envelope.divider;
    } else if (envelope.countdown != 0) {
        --envelope.countdown;
    } else {
        /* The divider has just counted out its V + 1 periods, so the decay level
           steps down once every V + 1 quarter frames. */
        if (!envelope.constant) {
            if (envelope.decay != 0)
                envelope.decay = static_cast<uint8_t>(envelope.decay - 1u);
            else if (envelope.loop)
                envelope.decay = 15;
        }
        envelope.countdown = envelope.divider;
    }
    /* The output follows the constant volume flag, so it is recomputed on every
       clock rather than only when the decay level changes.  The constant setting
       can be changed without restarting the envelope, and the channel has to be
       audible as soon as the flag is set. */
    envelope.volume = envelope.constant ? envelope.divider : envelope.decay;
}

void NesApu::clock_length(Length &length) {
    if (!length.halt && length.value != 0)
        length.value = static_cast<uint8_t>(length.value - 1u);
}

void NesApu::clock_pulse(Pulse &pulse) {
    if (pulse.countdown == 0) {
        pulse.countdown = pulse.period;
        pulse.step = static_cast<uint8_t>((pulse.step + 1u) & 0x07u);
        refresh_pulse(pulse);
    } else {
        --pulse.countdown;
    }
    /* The length counter silences the channel without stopping the sequencer,
       so a channel that is still counting can come back. */
    if (!pulse.started || pulse.length.value == 0)
        pulse.output = 0;
}

int32_t NesApu::sweep_target(const Pulse &pulse) const {
    if (pulse.sweep.shift == 0)
        return static_cast<int32_t>(pulse.period);
    const int32_t change = static_cast<int32_t>(pulse.period >> pulse.sweep.shift);
    if (!pulse.sweep.negate)
        return static_cast<int32_t>(pulse.period) + change;
    /* The two pulse channels have their adders' carry inputs wired differently:
       the first subtracts one more than the second. */
    const int32_t offset = pulse.duty >= 4 ? 0 : 1;
    const int32_t target = static_cast<int32_t>(pulse.period) - change - offset;
    return target < 0 ? 0 : target;
}

bool NesApu::sweep_silent(const Pulse &pulse) const {
    if (pulse.period < 8)
        return true;
    /* Muting follows the target period whatever the enable flag and the divider
       are doing, so a large period mutes the channel even with the sweep off. */
    return sweep_target(pulse) > 0x7FF;
}

void NesApu::refresh_pulse(Pulse &pulse) {
    if (pulse.length.value == 0 || sweep_silent(pulse)) {
        pulse.output = 0;
        return;
    }
    const uint8_t pattern = kDutyPattern[pulse.duty & 0x07u];
    pulse.output = ((pattern >> pulse.step) & 1u) != 0 ? pulse.envelope.volume : 0u;
}

void NesApu::clock_sweep(Pulse &pulse) {
    /* The divider keeps counting while the reload flag is set, which is what
       delays the first period adjustment by one half frame after a write. */
    if (pulse.sweep.countdown == 0) {
        if (pulse.sweep.enabled && pulse.sweep.shift != 0 && !sweep_silent(pulse))
            pulse.period = static_cast<uint16_t>(sweep_target(pulse));
    }
    if (pulse.sweep.countdown == 0 || pulse.sweep.reload) {
        pulse.sweep.countdown = pulse.sweep.divider;
        pulse.sweep.reload = false;
    } else {
        --pulse.sweep.countdown;
    }
    refresh_pulse(pulse);
}

void NesApu::refresh_triangle() {
    triangle_.output = (triangle_.length.value != 0 && triangle_.linear != 0)
                           ? kTrianglePattern[triangle_.step & 0x1Fu]
                           : 0u;
}

void NesApu::refresh_noise() {
    noise_.output = (noise_.length.value != 0 && (noise_.shift & 1u) == 0) ? noise_.envelope.volume
                                                                          : 0u;
}

uint32_t NesApu::channel_output(uint32_t channel) const {
    switch (channel) {
    case 0:
        return pulses_[0].output;
    case 1:
        return pulses_[1].output;
    case 2:
        return triangle_.output;
    case 3:
        return noise_.output;
    case 4:
        return delta_.output;
    default:
        return 0;
    }
}

void NesApu::save_state(uint8_t *buffer) const {
    uint8_t *out = buffer;
    for (uint32_t byte = 0; byte < 8; ++byte)
        out[byte] = kStateTag[byte];
    out += 8;
    put64(out, kStateVersion);
    out += 8;
    *out++ = static_cast<uint8_t>(region_);
    *out++ = frame_step_;
    *out++ = divider_;
    *out++ = reset_delay_;
    *out++ = five_step_ ? 1u : 0u;
    *out++ = length_clocked_ ? 1u : 0u;
    *out++ = irq_inhibit_ ? 1u : 0u;
    *out++ = status_;
    put16(out, frame_count_);
    out += 2;

    for (const Pulse &pulse : pulses_) {
        put16(out, pulse.countdown);
        out += 2;
        put16(out, pulse.period);
        out += 2;
        *out++ = pulse.duty;
        *out++ = pulse.step;
        *out++ = pulse.output;
        *out++ = pulse.started ? 1u : 0u;
        *out++ = pulse.length.halt ? 1u : 0u;
        *out++ = pulse.length.enabled ? 1u : 0u;
        *out++ = pulse.length.value;
        *out++ = pulse.envelope.start ? 1u : 0u;
        *out++ = pulse.envelope.loop ? 1u : 0u;
        *out++ = pulse.envelope.constant ? 1u : 0u;
        *out++ = pulse.envelope.divider;
        *out++ = pulse.envelope.countdown;
        *out++ = pulse.envelope.decay;
        *out++ = pulse.envelope.volume;
        *out++ = pulse.sweep.enabled ? 1u : 0u;
        *out++ = pulse.sweep.negate ? 1u : 0u;
        *out++ = pulse.sweep.reload ? 1u : 0u;
        *out++ = pulse.sweep.divider;
        *out++ = pulse.sweep.countdown;
        *out++ = pulse.sweep.shift;
    }

    put16(out, triangle_.countdown);
    out += 2;
    put16(out, triangle_.period);
    out += 2;
    *out++ = triangle_.step;
    *out++ = triangle_.output;
    *out++ = triangle_.linear;
    *out++ = triangle_.reload;
    *out++ = triangle_.control ? 1u : 0u;
    *out++ = triangle_.length.halt ? 1u : 0u;
    *out++ = triangle_.length.enabled ? 1u : 0u;
    *out++ = triangle_.length.value;

    put16(out, noise_.countdown);
    out += 2;
    put16(out, noise_.period);
    out += 2;
    put16(out, noise_.shift);
    out += 2;
    *out++ = noise_.rate;
    *out++ = noise_.mode ? 1u : 0u;
    *out++ = noise_.output;
    *out++ = noise_.length.halt ? 1u : 0u;
    *out++ = noise_.length.enabled ? 1u : 0u;
    *out++ = noise_.length.value;
    *out++ = noise_.envelope.start ? 1u : 0u;
    *out++ = noise_.envelope.loop ? 1u : 0u;
    *out++ = noise_.envelope.constant ? 1u : 0u;
    *out++ = noise_.envelope.divider;
    *out++ = noise_.envelope.countdown;
    *out++ = noise_.envelope.decay;
    *out++ = noise_.envelope.volume;

    put16(out, delta_.countdown);
    out += 2;
    put16(out, delta_.period);
    out += 2;
    put16(out, delta_.address);
    out += 2;
    put16(out, delta_.start);
    out += 2;
    put16(out, delta_.remaining);
    out += 2;
    put16(out, delta_.length);
    out += 2;
    *out++ = delta_.rate;
    *out++ = delta_.output;
    *out++ = delta_.shift;
    *out++ = delta_.bits;
    *out++ = delta_.buffer;
    *out++ = delta_.buffered ? 1u : 0u;
    *out++ = delta_.silent ? 1u : 0u;
    *out++ = delta_.loop ? 1u : 0u;
    *out++ = delta_.irq_enabled ? 1u : 0u;
    *out++ = delta_.enabled ? 1u : 0u;
    *out++ = delta_.stopped ? 1u : 0u;
}

bool NesApu::load_state(const uint8_t *buffer, uint64_t size) {
    if (buffer == nullptr || size != state_size())
        return false;
    const uint8_t *in = buffer;
    for (uint32_t byte = 0; byte < 8; ++byte)
        if (in[byte] != kStateTag[byte])
            return false;
    in += 8;
    uint64_t version = 0;
    for (uint32_t byte = 0; byte < 8; ++byte)
        version |= static_cast<uint64_t>(in[byte]) << (byte * 8);
    in += 8;
    if (version != kStateVersion)
        return false;

    /* The image is decoded into a staged unit first, so an image that turns out
       to be malformed is rejected without disturbing anything that is live. */
    NesApu staged;
    staged.region_ = static_cast<Region>(*in++);
    staged.frame_step_ = *in++;
    staged.divider_ = *in++;
    staged.reset_delay_ = *in++;
    staged.five_step_ = *in++ != 0;
    staged.length_clocked_ = *in++ != 0;
    staged.irq_inhibit_ = *in++ != 0;
    staged.status_ = *in++;
    staged.frame_count_ = get16(in);
    in += 2;
    if (staged.region_ != Region::ntsc && staged.region_ != Region::pal &&
        staged.region_ != Region::dendy)
        return false;
    if (staged.divider_ > 1 || staged.reset_delay_ > 4 || (staged.status_ & 0x3Fu) != 0)
        return false;
    if (staged.frame_step_ >= frame_timing(staged.region_, staged.five_step_).count ||
        staged.frame_count_ >= frame_timing(staged.region_, staged.five_step_).step[staged.frame_step_])
        return false;

    for (Pulse &pulse : staged.pulses_) {
        pulse.countdown = get16(in);
        in += 2;
        pulse.period = get16(in);
        in += 2;
        pulse.duty = *in++;
        pulse.step = *in++;
        pulse.output = *in++;
        pulse.started = *in++ != 0;
        pulse.length.halt = *in++ != 0;
        pulse.length.enabled = *in++ != 0;
        pulse.length.value = *in++;
        pulse.envelope.start = *in++ != 0;
        pulse.envelope.loop = *in++ != 0;
        pulse.envelope.constant = *in++ != 0;
        pulse.envelope.divider = *in++;
        pulse.envelope.countdown = *in++;
        pulse.envelope.decay = *in++;
        pulse.envelope.volume = *in++;
        pulse.sweep.enabled = *in++ != 0;
        pulse.sweep.negate = *in++ != 0;
        pulse.sweep.reload = *in++ != 0;
        pulse.sweep.divider = *in++;
        pulse.sweep.countdown = *in++;
        pulse.sweep.shift = *in++;
        if (pulse.duty > 7 || pulse.step > 7 || pulse.output > 15 || pulse.envelope.decay > 15 ||
            pulse.envelope.volume > 15 || pulse.envelope.divider > 15 ||
            pulse.envelope.countdown > 15 || pulse.sweep.divider > 7 ||
            pulse.sweep.countdown > 7 || pulse.sweep.shift > 7 || pulse.period > 0x7FF ||
            pulse.countdown > 0x7FF)
            return false;
    }

    staged.triangle_.countdown = get16(in);
    in += 2;
    staged.triangle_.period = get16(in);
    in += 2;
    staged.triangle_.step = *in++;
    staged.triangle_.output = *in++;
    staged.triangle_.linear = *in++;
    staged.triangle_.reload = *in++;
    staged.triangle_.control = *in++ != 0;
    staged.triangle_.length.halt = *in++ != 0;
    staged.triangle_.length.enabled = *in++ != 0;
    staged.triangle_.length.value = *in++;
    if (staged.triangle_.step > 31 || staged.triangle_.output > 15 ||
        staged.triangle_.reload > 127 || staged.triangle_.linear > 127 ||
        staged.triangle_.period > 0x7FF || staged.triangle_.countdown > 0x7FF)
        return false;

    staged.noise_.countdown = get16(in);
    in += 2;
    staged.noise_.period = get16(in);
    in += 2;
    staged.noise_.shift = get16(in);
    in += 2;
    staged.noise_.rate = *in++;
    staged.noise_.mode = *in++ != 0;
    staged.noise_.output = *in++;
    staged.noise_.length.halt = *in++ != 0;
    staged.noise_.length.enabled = *in++ != 0;
    staged.noise_.length.value = *in++;
    staged.noise_.envelope.start = *in++ != 0;
    staged.noise_.envelope.loop = *in++ != 0;
    staged.noise_.envelope.constant = *in++ != 0;
    staged.noise_.envelope.divider = *in++;
    staged.noise_.envelope.countdown = *in++;
    staged.noise_.envelope.decay = *in++;
    staged.noise_.envelope.volume = *in++;
    if (staged.noise_.rate > 15 || staged.noise_.shift == 0 || staged.noise_.output > 15 ||
        (staged.noise_.shift >> 15) != 0 || staged.noise_.envelope.divider > 15 ||
        staged.noise_.envelope.countdown > 15 || staged.noise_.envelope.decay > 15 ||
        staged.noise_.envelope.volume > 15 || staged.noise_.period > 0x7FF ||
        staged.noise_.countdown > 0x7FF)
        return false;

    staged.delta_.countdown = get16(in);
    in += 2;
    staged.delta_.period = get16(in);
    in += 2;
    staged.delta_.address = get16(in);
    in += 2;
    staged.delta_.start = get16(in);
    in += 2;
    staged.delta_.remaining = get16(in);
    in += 2;
    staged.delta_.length = get16(in);
    in += 2;
    staged.delta_.rate = *in++;
    staged.delta_.output = *in++;
    staged.delta_.shift = *in++;
    staged.delta_.bits = *in++;
    staged.delta_.buffer = *in++;
    staged.delta_.buffered = *in++ != 0;
    staged.delta_.silent = *in++ != 0;
    staged.delta_.loop = *in++ != 0;
    staged.delta_.irq_enabled = *in++ != 0;
    staged.delta_.enabled = *in++ != 0;
    staged.delta_.stopped = *in++ != 0;
    if (staged.delta_.rate > 15 || (staged.delta_.output & 0x80u) != 0 || staged.delta_.bits == 0 ||
        staged.delta_.bits > 8 || staged.delta_.period > 0x7FF || staged.delta_.countdown > 0x7FF)
        return false;

    pulses_[0] = staged.pulses_[0];
    pulses_[1] = staged.pulses_[1];
    triangle_ = staged.triangle_;
    noise_ = staged.noise_;
    delta_ = staged.delta_;
    region_ = staged.region_;
    frame_count_ = staged.frame_count_;
    frame_step_ = staged.frame_step_;
    divider_ = staged.divider_;
    reset_delay_ = staged.reset_delay_;
    five_step_ = staged.five_step_;
    length_clocked_ = staged.length_clocked_;
    irq_inhibit_ = staged.irq_inhibit_;
    status_ = staged.status_;
    stall_cycles_ = 0;
    return true;
}

} // namespace nesapu
