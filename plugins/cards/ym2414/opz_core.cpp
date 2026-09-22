#include "opz_core.h"
#include <state.hpp>

#include "opz_tables.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>

namespace srz80::ym2414 {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr uint32_t kWaveformLength = 0x400; // one full sine period
constexpr uint32_t kEnvelopeQuiet = 0x380;

// Phase-step sentinel: recompute the step every sample (PM active or fix mode)
constexpr uint32_t kPhaseStepDynamic = 0;

// Generated once: the sine/attenuation ROM and the derived OPZ/LFO waveforms
struct Tables {
    uint16_t sin[256];
    uint16_t power[256];
    uint16_t waveform[8][kWaveformLength];
    int16_t lfo[4][256];

    Tables() {
        for (int i = 0; i < 256; ++i) {
            double s = std::sin((2.0 * i + 1.0) * kPi / 1024.0);
            double a = -std::log2(s) * 256.0;
            sin[i] = static_cast<uint16_t>(a < 4095.0 ? a : 4095.0);
        }
        // Exponential attenuation -> 13-bit linear volume mantissa (0..255/256
        // of an octave per step).
        for (int i = 0; i < 256; ++i)
            power[i] = static_cast<uint16_t>(std::lround(std::exp2(13.0 - i / 256.0)));

        auto abs_sin = [&](uint32_t input) -> uint32_t {
            if (input & 0x100)
                input = ~input;
            return sin[input & 0xff];
        };

        for (uint32_t i = 0; i < kWaveformLength; ++i)
            waveform[0][i] =
                static_cast<uint16_t>(abs_sin(i) | (static_cast<uint16_t>((i >> 9) & 1) << 15));
        uint16_t zeroval = waveform[0][0];
        for (uint32_t i = 0; i < kWaveformLength; ++i) {
            uint32_t mag = 2 * (waveform[0][i] & 0x7fff);
            if (mag > zeroval)
                mag = zeroval;
            waveform[1][i] = static_cast<uint16_t>(mag | (static_cast<uint16_t>((i >> 9) & 1) << 15));
        }
        for (uint32_t i = 0; i < kWaveformLength; ++i) {
            bool hi = (i >> 9) & 1;
            waveform[2][i] = hi ? zeroval : waveform[0][i];
            waveform[3][i] = hi ? zeroval : waveform[1][i];
            waveform[4][i] = hi ? zeroval : waveform[0][(i * 2) & 0x3ff];
            waveform[5][i] = hi ? zeroval : waveform[1][(i * 2) & 0x3ff];
            waveform[6][i] = hi ? zeroval : waveform[0][(i * 2) & 0x1ff];
            waveform[7][i] = hi ? zeroval : waveform[1][(i * 2) & 0x1ff];
        }

        // LFO waveforms: AM in the low byte, signed PM in the high byte
        for (uint32_t i = 0; i < 256; ++i) {
            uint8_t am = static_cast<uint8_t>(i ^ 0xff);
            int8_t pm = static_cast<int8_t>(i);
            lfo[0][i] = static_cast<int16_t>(am | (pm << 8));
            am = (i >> 7) ? 0 : 0xff;
            pm = static_cast<int8_t>(static_cast<uint8_t>(am ^ 0x80));
            lfo[1][i] = static_cast<int16_t>(am | (pm << 8));
            am = static_cast<uint8_t>((i >> 7) ? (i << 1) : ((i ^ 0xff) << 1));
            pm = static_cast<int8_t>((i >> 6) & 1 ? am : static_cast<uint8_t>(~am));
            lfo[2][i] = static_cast<int16_t>(am | (pm << 8));
            lfo[3][i] = 0; // noise, filled at runtime
        }
    }
};

const Tables &rom() {
    static const Tables instance;
    return instance;
}

inline uint32_t effective_rate(uint32_t rawrate, uint32_t ksr) {
    return rawrate == 0 ? 0 : std::min<uint32_t>(rawrate + ksr, 63);
}

inline uint32_t attenuation_to_volume(uint32_t input) {
    return rom().power[input & 0xff] >> (input >> 8);
}

inline int32_t detune_adjustment(uint32_t detune, uint32_t keycode) {
    int32_t result = tables::kDetuneAdjustment[keycode & 0x1f][detune & 3];
    return (detune & 4) ? -result : result;
}

inline uint32_t attenuation_increment(uint32_t rate, uint32_t index) {
    return (tables::kIncrementTable[rate & 0x3f] >> (4 * (index & 7))) & 0xf;
}

uint32_t opm_key_code_to_phase_step(uint32_t block_freq, int32_t delta) {
    uint32_t block = (block_freq >> 10) & 7;
    uint32_t adjusted_code = ((block_freq >> 6) & 0xf) - ((block_freq >> 8) & 3);
    int32_t eff_freq = static_cast<int32_t>((adjusted_code << 6) | (block_freq & 0x3f));
    eff_freq += delta;
    if (static_cast<uint32_t>(eff_freq) >= 768) {
        if (eff_freq < 0) {
            eff_freq += 768;
            if (block-- == 0)
                return tables::kPhaseStep[0] >> 7;
        } else {
            eff_freq -= 768;
            if (eff_freq >= 768) {
                ++block;
                eff_freq -= 768;
            }
            if (block++ >= 7)
                return tables::kPhaseStep[767];
        }
    }
    return tables::kPhaseStep[eff_freq] >> (block ^ 7);
}

inline int32_t roundtrip_fp(int32_t value) {
    if (value < -32768)
        return -32768;
    if (value > 32767)
        return 32767;
    return value;
}

inline int32_t add_clamped(int32_t left, int32_t right) {
    return std::clamp(left + right, -32768, 32767);
}

} // namespace

namespace {

struct SavedOperator {
    uint32_t phase;
    uint16_t env_attenuation;
    uint8_t env_state;
    uint8_t key_state;
    uint8_t keyon_live;
    uint32_t ramp_counter;
    uint32_t actual_level;
    uint32_t subphase;
    int32_t last_output;
};
struct SavedChannel {
    uint32_t feedback[2];
    int32_t feedback_in;
    int32_t output[2];
};
struct SavedState {
    uint8_t address;
    uint8_t status;
    uint32_t busy_remaining;
    uint32_t lfo_counter[2];
    uint32_t noise_lfsr;
    uint32_t noise_counter;
    uint8_t noise_state;
    int32_t lfo_am[2];
    int16_t lfo_wave[4][256];
    uint32_t env_counter;
    uint32_t timer_a_counter;
    uint32_t timer_b_counter;
    uint8_t timer_a_running;
    uint8_t timer_b_running;
    uint8_t regs[Core::kRegisters];
    SavedOperator ops[Core::kOperators];
    SavedChannel chans[Core::kChannels];
};
template <class Archive> void archive_state(Archive &ar, SavedState &s) {
    ar.fields(s.address, s.status, s.busy_remaining, s.lfo_counter, s.noise_lfsr,
              s.noise_counter, s.noise_state, s.lfo_am, s.lfo_wave, s.env_counter,
              s.timer_a_counter, s.timer_b_counter, s.timer_a_running, s.timer_b_running, s.regs);
    for (auto &o : s.ops)
        ar.fields(o.phase, o.env_attenuation, o.env_state, o.key_state, o.keyon_live,
                  o.ramp_counter, o.actual_level, o.subphase, o.last_output);
    for (auto &c : s.chans) ar.fields(c.feedback, c.feedback_in, c.output);
}

}

Core::Core(uint32_t chip_clock_hz) : native_rate_(chip_clock_hz / kClockDivider) { reset(); }

void Core::reset() {
    std::fill(std::begin(reg_), std::end(reg_), 0);
    // Both outputs are enabled by default (matches the register reset value)
    for (uint32_t ch = 0; ch < kChannels; ++ch)
        reg_[0x30 + ch] = 0x01;

    for (uint32_t ch = 0; ch < kChannels; ++ch)
        for (uint32_t slot = 0; slot < kOperatorSlots; ++slot)
            op_[channel_operator(ch, slot)].ch_index = static_cast<uint8_t>(ch);

    for (auto &op : op_) {
        op.phase = 0;
        op.env_attenuation = 0x3ff;
        op.env_state = kRelease;
        op.key_state = 0;
        op.keyon_live = 0;
        op.ramp_counter = 0;
        op.actual_level = 0;
        op.subphase = 0;
        op.last_output = 0;
        op.cache = Cache{};
    }
    for (auto &ch : ch_) {
        ch.feedback[0] = ch.feedback[1] = 0;
        ch.feedback_in = 0;
        ch.output[0] = ch.output[1] = 0;
        ch.algorithm_index = 0;
        ch.am_sens = 0;
        ch.am_select = 0;
    }

    const Tables &t = rom();
    for (uint32_t w = 0; w < 3; ++w)
        for (uint32_t i = 0; i < 256; ++i)
            lfo_wave_[w][i] = t.lfo[w][i];
    for (uint32_t i = 0; i < 256; ++i)
        lfo_wave_[3][i] = 0;

    address_ = 0;
    status_ = 0;
    busy_remaining_ = 0;
    cache_dirty_ = true;
    lfo_active_ = true;
    lfo_counter_[0] = lfo_counter_[1] = 0;
    noise_lfsr_ = 1;
    noise_counter_ = 0;
    noise_state_ = 0;
    lfo_am_[0] = lfo_am_[1] = 0;
    env_counter_ = 0;
    timer_a_counter_ = timer_b_counter_ = 0;
    timer_a_running_ = timer_b_running_ = false;
    output_[0] = output_[1] = 0;
}

uint32_t Core::channel_operator(uint32_t channel, uint32_t slot) {
    // Channel operator order is carrier1, modulator1, carrier2, modulator2
    static constexpr uint32_t kOffsets[4] = {0, 16, 8, 24};
    return channel + kOffsets[slot & 3];
}

void Core::write_data(uint8_t data) {
    const uint32_t index = address_;
    if (index == 0x17 && (data & 0x80)) {
        reg_[0x188] = data;
    } else if (index == 0x19 && (data & 0x80)) {
        reg_[0x189] = data;
    } else if ((index & 0xe0) == 0x40 && (data & 0x80)) {
        reg_[0x100 + (index & 0x1f)] = data;
    } else if ((index & 0xe0) == 0xc0 && (data & 0x20)) {
        reg_[0x120 + (index & 0x1f)] = data;
    } else if (index < 0x100) {
        reg_[index] = data;
    }

    if (index == 0x14)
        handle_mode_write(data);
    if (index == 0x08)
        key_on(data & 7, (data >> 3) & 0xf);
    // Busy for 64 input clocks, i.e. exactly one native sample
    busy_remaining_ = 1;
    cache_dirty_ = true;
}

uint8_t Core::read_status() const {
    uint8_t result = status_;
    if (busy_remaining_ > 0)
        result |= 0x80;
    return result;
}

void Core::handle_mode_write(uint8_t data) {
    // Bits 4/5 reset the latched timer A/B overflow flags
    if (data & 0x10)
        status_ &= static_cast<uint8_t>(~0x01u);
    if (data & 0x20)
        status_ &= static_cast<uint8_t>(~0x02u);

    // The timers run while their "load" bits (0/1) are set and restart their
    // counters on a 0 -> 1 transition.  Bits 2/3 only gate the status/IRQ flag
    const bool run_a = (data & 0x01) != 0;
    const bool run_b = (data & 0x02) != 0;
    if (run_a && !timer_a_running_) {
        timer_a_running_ = true;
        timer_a_counter_ = 0;
    } else if (!run_a) {
        timer_a_running_ = false;
        timer_a_counter_ = 0;
    }
    if (run_b && !timer_b_running_) {
        timer_b_running_ = true;
        timer_b_counter_ = 0;
    } else if (!run_b) {
        timer_b_running_ = false;
        timer_b_counter_ = 0;
    }
}

void Core::key_on(uint32_t channel, uint32_t opmask) {
    if (channel >= kChannels)
        return;
    for (uint32_t slot = 0; slot < kOperatorSlots; ++slot) {
        Operator &o = op_[channel_operator(channel, slot)];
        o.keyon_live = static_cast<uint8_t>((o.keyon_live & ~(1u << kKeyOnNormal)) |
                                            (((opmask >> slot) & 1u) << kKeyOnNormal));
    }
}

void Core::cache_channel(uint32_t channel) {
    Channel &c = ch_[channel];
    c.algorithm_index = static_cast<uint8_t>(ch_algorithm(channel));
    c.am_sens = static_cast<uint8_t>(ch_lfo_am_sens(channel));
    c.am_select = static_cast<uint8_t>(ch_lfo_am_select(channel));
    c.pm_sens = static_cast<uint8_t>(ch_lfo_pm_sens(channel));
    c.pm_select = static_cast<uint8_t>(ch_lfo_pm_select(channel));
    const uint32_t feedback = ch_feedback(channel);
    c.feedback_shift = static_cast<uint8_t>(feedback == 0 ? 0 : 10 - feedback);
    c.output_mask = static_cast<uint8_t>(ch_output_0(channel) | (ch_output_1(channel) << 1));
}

void Core::cache_global() {
    global_.noise_period = noise_frequency() ^ 0x1f;
    const uint32_t rate0 = lfo_rate();
    const uint32_t rate1 = lfo2_rate();
    global_.lfo_increment[0] = rate0 == 0 ? 0 : (0x10u | (rate0 & 0xf)) << ((rate0 >> 4) & 0xf);
    global_.lfo_increment[1] = rate1 == 0 ? 0 : (0x10u | (rate1 & 0xf)) << ((rate1 >> 4) & 0xf);
    global_.lfo_waveform[0] = static_cast<uint8_t>(lfo_waveform());
    global_.lfo_waveform[1] = static_cast<uint8_t>(lfo2_waveform());
    global_.lfo_am_depth[0] = static_cast<uint8_t>(lfo_am_depth());
    global_.lfo_am_depth[1] = static_cast<uint8_t>(lfo2_am_depth());
    global_.lfo_pm_depth[0] = static_cast<uint8_t>(lfo_pm_depth());
    global_.lfo_pm_depth[1] = static_cast<uint8_t>(lfo2_pm_depth());
    const uint8_t reset = static_cast<uint8_t>(lfo_reset());
    global_.lfo_sync[0] = static_cast<uint8_t>(lfo_sync() | reset);
    global_.lfo_sync[1] = static_cast<uint8_t>(lfo2_sync() | reset);
    global_.timer_period[0] = 1024u - timer_a_value();
    global_.timer_period[1] = 16u * (256u - timer_b_value());
    global_.timer_status_enable[0] = static_cast<uint8_t>(byte(0x14, 2, 1));
    global_.timer_status_enable[1] = static_cast<uint8_t>(byte(0x14, 3, 1));
    global_.csm = static_cast<uint8_t>(csm());
}

void Core::cache_operator(uint32_t channel, uint32_t op) {
    Operator &o = op_[op];
    Cache &cache = o.cache;
    cache.waveform = rom().waveform[op_waveform(op)];
    cache.am_enable = op_lfo_am_enable(op) ? 1 : 0;

    const uint32_t block_freq = cache.block_freq = ch_block_freq(channel);
    const uint32_t keycode = (block_freq >> 8) & 0x1f;
    cache.detune = detune_adjustment(op_detune(op), keycode);

    cache.multiple = op_multiple(op) << 4;
    if (cache.multiple == 0)
        cache.multiple = 0x08;
    cache.multiple |= op_fine(op);

    cache.fixed_mode = static_cast<uint8_t>(op_fix_mode(op));
    cache.fixed_range = static_cast<uint8_t>(op_fix_range(op));
    cache.fixed_frequency = static_cast<uint16_t>((op_fix_frequency(op) << 4) | op_fine(op));
    if (cache.fixed_frequency == 0)
        cache.fixed_frequency = 8;
    cache.detune2 = static_cast<uint8_t>(op_detune2(op));

    const Channel &ch = ch_[channel];
    const bool pm_active = global_.lfo_pm_depth[0] != 0 && (ch.pm_select || ch.pm_sens != 0);
    const bool pm2_active = global_.lfo_pm_depth[1] != 0 && (!ch.pm_select || ch.pm_sens != 0);
    if (!cache.fixed_mode && !pm_active && !pm2_active)
        cache.phase_step = compute_phase_step(channel, op, cache, 0);
    else
        cache.phase_step = kPhaseStepDynamic;

    cache.total_level = op_total_level(op) << 3;

    cache.eg_sustain = op_sustain_level(op);
    cache.eg_sustain |= (cache.eg_sustain + 1) & 0x10;
    cache.eg_sustain <<= 5;

    const uint32_t ksrval = keycode >> (op_ksr(op) ^ 3);
    cache.eg_rate[kAttack] = static_cast<uint8_t>(effective_rate(op_attack_rate(op) * 2, ksrval));
    cache.eg_rate[kDecay] = static_cast<uint8_t>(effective_rate(op_decay_rate(op) * 2, ksrval));
    cache.eg_rate[kSustain] = static_cast<uint8_t>(effective_rate(op_sustain_rate(op) * 2, ksrval));
    cache.eg_rate[kRelease] =
        static_cast<uint8_t>(effective_rate(op_release_rate(op) * 4 + 2, ksrval));
    cache.eg_rate[kReverb] = cache.eg_rate[kRelease];
    const uint32_t reverb = op_reverb_rate(op);
    if (reverb != 0)
        cache.eg_rate[kReverb] = static_cast<uint8_t>(std::min<uint32_t>(
            effective_rate(reverb * 4 + 2, ksrval), cache.eg_rate[kReverb]));

    cache.eg_shift = static_cast<uint8_t>(op_eg_shift(op));
    cache.tl_ramp = static_cast<uint8_t>(op_tl_ramp(op));
    cache.tl_ramp_period = static_cast<uint8_t>(ch_ramp_period(channel));
}

uint32_t Core::compute_phase_step(uint32_t channel, uint32_t op, const Cache &cache,
                                  int32_t lfo_raw_pm) {
    if (cache.fixed_mode) {
        // Fixed frequency mode: 8..255Hz in 1Hz steps, shifted by the range
        const uint32_t freq = static_cast<uint32_t>(cache.fixed_frequency) << cache.fixed_range;
        // Keep extra resolution so low pitches are not truncated to silence
        uint32_t substep = op_[op].subphase + 75u * 1024u * freq;
        op_[op].subphase = substep & 0xfff;
        return substep >> 12;
    }

    // Chord/octave detune (DT2), expressed as a phase displacement in 1/64ths
    static constexpr int16_t kDetune2Delta[4] = {0, (600 * 64 + 50) / 100, (781 * 64 + 50) / 100,
                                                 (950 * 64 + 50) / 100};
    int32_t delta = kDetune2Delta[cache.detune2];

    const Channel &ch = ch_[channel];
    const uint32_t pm_sensitivity = ch.pm_sens;
    if (pm_sensitivity != 0) {
        int32_t pm = static_cast<int8_t>(lfo_raw_pm >> (ch.pm_select ? 8 : 0));
        if (pm_sensitivity < 6)
            delta += pm >> (6 - pm_sensitivity);
        else
            delta += pm << (pm_sensitivity - 5);
    }

    uint32_t phase_step = opm_key_code_to_phase_step(cache.block_freq, delta);
    phase_step += static_cast<uint32_t>(cache.detune);
    return (phase_step * cache.multiple) >> 4;
}

void Core::start_attack(uint32_t op, bool is_restart) {
    Operator &o = op_[op];
    if (o.env_state == kAttack)
        return;
    o.env_state = kAttack;
    if (!is_restart)
        o.phase = 0;
    if (o.cache.eg_rate[kAttack] >= 62)
        o.env_attenuation = 0;
}

void Core::start_release(uint32_t op) {
    Operator &o = op_[op];
    if (o.env_state >= kRelease)
        return;
    o.env_state = kRelease;
}

void Core::clock_keystate(uint32_t op, uint32_t keystate) {
    Operator &o = op_[op];
    if ((keystate ^ o.key_state) == 0)
        return;
    o.key_state = static_cast<uint8_t>(keystate);
    if (keystate)
        start_attack(op, false);
    else
        start_release(op);
}

void Core::prepare_operator(uint32_t op) {
    Operator &o = op_[op];
    clock_keystate(op, o.keyon_live != 0 ? 1u : 0u);
    if ((o.keyon_live & (1u << kKeyOnCsm)) != 0 && (o.keyon_live & (1u << kKeyOnNormal)) == 0)
        clock_keystate(op, 0);
    o.keyon_live &= static_cast<uint8_t>(~(1u << kKeyOnCsm));
}

void Core::clock_envelope(uint32_t op, uint32_t env_counter) {
    Operator &o = op_[op];
    // Fully closed and in the terminal state: no transition out of kReverb
    // exists and the attenuation is already saturated, so the work below is a
    // guaranteed no-op. Skip the rate/counter arithmetic entirely
    if (o.env_state == kReverb && o.env_attenuation == 0x3ff)
        return;

    if (o.env_state == kAttack && o.env_attenuation == 0)
        o.env_state = kDecay;
    if (o.env_state == kDecay && o.env_attenuation >= o.cache.eg_sustain)
        o.env_state = kSustain;

    const uint32_t rate = o.cache.eg_rate[o.env_state];
    const uint32_t rate_shift = rate >> 2;
    const uint32_t counter = env_counter << rate_shift;
    if ((counter & 0x7ff) != 0)
        return;

    const uint32_t relevant = (counter >> (rate_shift <= 11 ? 11 : rate_shift)) & 7;
    const uint32_t increment = attenuation_increment(rate, relevant);

    if (o.env_state == kAttack) {
        // Attack is a decreasing exponential: ~attenuation in modular arithmetic.
        if (rate < 62) {
            const uint32_t inverse = ~static_cast<uint32_t>(o.env_attenuation);
            o.env_attenuation =
                static_cast<uint16_t>(o.env_attenuation + ((inverse * increment) >> 4));
        }
    } else {
        o.env_attenuation = static_cast<uint16_t>(o.env_attenuation + increment);
        if (o.env_attenuation >= 0x400)
            o.env_attenuation = 0x3ff;
        if (o.env_state == kRelease && o.env_attenuation >= 0xc0)
            o.env_state = kReverb;
    }
}

uint32_t Core::envelope_attenuation(uint32_t op, uint32_t am_offset) const {
    const Operator &o = op_[op];
    uint32_t result = o.env_attenuation >> o.cache.eg_shift;
    if (o.cache.am_enable)
        result += am_offset;
    result += o.actual_level;
    return std::min<uint32_t>(result, 0x3ff);
}

int32_t Core::compute_volume(uint32_t op, uint32_t phase, uint32_t am_offset) const {
    const Operator &o = op_[op];
    if (o.env_attenuation > kEnvelopeQuiet && o.cache.eg_shift == 0)
        return 0;
    const uint32_t sin_attenuation = o.cache.waveform[phase & (kWaveformLength - 1)];
    const uint32_t env_attenuation = envelope_attenuation(op, am_offset) << 2;
    const int32_t result = static_cast<int32_t>(
        attenuation_to_volume((sin_attenuation & 0x7fff) + env_attenuation));
    return (sin_attenuation & 0x8000) ? -result : result;
}

void Core::clock_level_ramp(uint32_t op) {
    Operator &o = op_[op];
    if (++o.ramp_counter > o.cache.tl_ramp_period) {
        if (o.actual_level != o.cache.total_level) {
            if (o.actual_level > o.cache.total_level) {
                --o.actual_level;
                if (o.actual_level < o.cache.total_level)
                    o.actual_level = o.cache.total_level;
            } else {
                ++o.actual_level;
                if (o.actual_level > o.cache.total_level)
                    o.actual_level = o.cache.total_level;
            }
        }
        o.ramp_counter = 0;
    }
}

void Core::clock_phase(uint32_t op, int32_t lfo_raw_pm) {
    Operator &o = op_[op];
    uint32_t step = o.cache.phase_step;
    if (step == kPhaseStepDynamic)
        step = compute_phase_step(o.ch_index, op, o.cache, lfo_raw_pm);
    o.phase += step;
}

int32_t Core::clock_noise_and_lfo() {
    for (int rep = 0; rep < 2; ++rep) {
        noise_lfsr_ <<= 1;
        noise_lfsr_ |= ((noise_lfsr_ >> 17) ^ (noise_lfsr_ >> 14) ^ 1u) & 1u;
        if (noise_counter_++ >= global_.noise_period) {
            noise_counter_ = 0;
            noise_state_ = static_cast<uint8_t>((noise_lfsr_ >> 17) & 1u);
        }
    }

    lfo_counter_[0] += global_.lfo_increment[0];
    lfo_counter_[1] += global_.lfo_increment[1];
    if (global_.lfo_sync[0])
        lfo_counter_[0] = 0;
    if (global_.lfo_sync[1])
        lfo_counter_[1] = 0;

    const uint32_t lfo0 = (lfo_counter_[0] >> 22) & 0xff;
    const uint32_t lfo1 = (lfo_counter_[1] >> 22) & 0xff;

    // Latch the running noise value one step ahead so waveform 3 is stable per
    // LFO clock
    const uint32_t lfo_noise = (noise_lfsr_ >> 17) & 0xff;
    lfo_wave_[3][(lfo0 + 1) & 0xff] = static_cast<int16_t>(lfo_noise | (lfo_noise << 8));
    lfo_wave_[3][(lfo1 + 1) & 0xff] = static_cast<int16_t>(lfo_noise | (lfo_noise << 8));

    const int32_t ampm0 = lfo_wave_[global_.lfo_waveform[0]][lfo0];
    const int32_t ampm1 = lfo_wave_[global_.lfo_waveform[1]][lfo1];

    lfo_am_[0] = ((ampm0 & 0xff) * global_.lfo_am_depth[0]) >> 7;
    lfo_am_[1] = ((ampm1 & 0xff) * global_.lfo_am_depth[1]) >> 7;

    const int32_t pm0 = ((ampm0 >> 8) * global_.lfo_pm_depth[0]) >> 7;
    const int32_t pm1 = ((ampm1 >> 8) * global_.lfo_pm_depth[1]) >> 7;
    return (pm0 & 0xff) | (pm1 << 8);
}

uint32_t Core::lfo_am_offset(uint32_t channel) const {
    const Channel &c = ch_[channel];
    if (c.am_sens == 0)
        return 0;
    const int32_t am = lfo_am_[c.am_select ? 1 : 0];
    return static_cast<uint32_t>(am << (c.am_sens - 1));
}

void Core::output_channel(uint32_t channel, int32_t *out) {
    Channel &c = ch_[channel];
    const uint32_t o0 = channel_operator(channel, 0);
    const uint32_t o1 = channel_operator(channel, 1);
    const uint32_t o2 = channel_operator(channel, 2);
    const uint32_t o3 = channel_operator(channel, 3);

    const uint32_t am_offset = lfo_am_offset(channel);
    int32_t opmod = 0;
    if (c.feedback_shift != 0)
        opmod = (c.feedback[0] + c.feedback[1]) >> c.feedback_shift;

    const int32_t op0 = op_[o0].last_output =
        compute_volume(o0, (op_[o0].phase >> 10) + opmod, am_offset);
    c.feedback_in = op0;

    int32_t op1;
    int32_t op2;
    int32_t result;
    switch (c.algorithm_index) {
    case 0: // O1 -> O2 -> O3 -> O4
        op1 = op_[o1].last_output = compute_volume(o1, (op_[o1].phase >> 10) + (op0 >> 1), am_offset);
        op2 = op_[o2].last_output = compute_volume(o2, (op_[o2].phase >> 10) + (op1 >> 1), am_offset);
        result = op_[o3].last_output = compute_volume(o3, (op_[o3].phase >> 10) + (op2 >> 1), am_offset);
        break;
    case 1: // (O1 + O2) -> O3 -> O4
        op1 = op_[o1].last_output = compute_volume(o1, op_[o1].phase >> 10, am_offset);
        op2 = op_[o2].last_output = compute_volume(o2, (op_[o2].phase >> 10) + ((op0 + op1) >> 1), am_offset);
        result = op_[o3].last_output = compute_volume(o3, (op_[o3].phase >> 10) + (op2 >> 1), am_offset);
        break;
    case 2: // (O1 + (O2 -> O3)) -> O4
        op1 = op_[o1].last_output = compute_volume(o1, op_[o1].phase >> 10, am_offset);
        op2 = op_[o2].last_output = compute_volume(o2, (op_[o2].phase >> 10) + (op1 >> 1), am_offset);
        result = op_[o3].last_output = compute_volume(o3, (op_[o3].phase >> 10) + ((op0 + op2) >> 1), am_offset);
        break;
    case 3: // ((O1 -> O2) + O3) -> O4
        op1 = op_[o1].last_output = compute_volume(o1, (op_[o1].phase >> 10) + (op0 >> 1), am_offset);
        op2 = op_[o2].last_output = compute_volume(o2, op_[o2].phase >> 10, am_offset);
        result = op_[o3].last_output = compute_volume(o3, (op_[o3].phase >> 10) + ((op1 + op2) >> 1), am_offset);
        break;
    case 4: // (O1 -> O2) + (O3 -> O4)
        op1 = op_[o1].last_output = compute_volume(o1, (op_[o1].phase >> 10) + (op0 >> 1), am_offset);
        op2 = op_[o2].last_output = compute_volume(o2, op_[o2].phase >> 10, am_offset);
        result = op_[o3].last_output = compute_volume(o3, (op_[o3].phase >> 10) + (op2 >> 1), am_offset);
        result = add_clamped(result, op1);
        break;
    case 5: // O1 -> (O2, O3, O4)
        op1 = op_[o1].last_output = compute_volume(o1, (op_[o1].phase >> 10) + (op0 >> 1), am_offset);
        op2 = op_[o2].last_output = compute_volume(o2, (op_[o2].phase >> 10) + (op0 >> 1), am_offset);
        result = op_[o3].last_output = compute_volume(o3, (op_[o3].phase >> 10) + (op0 >> 1), am_offset);
        result = add_clamped(add_clamped(result, op1), op2);
        break;
    case 6: // (O1 -> O2) + O3 + O4
        op1 = op_[o1].last_output = compute_volume(o1, (op_[o1].phase >> 10) + (op0 >> 1), am_offset);
        op2 = op_[o2].last_output = compute_volume(o2, op_[o2].phase >> 10, am_offset);
        result = op_[o3].last_output = compute_volume(o3, op_[o3].phase >> 10, am_offset);
        result = add_clamped(add_clamped(result, op1), op2);
        break;
    default: // O1 + O2 + O3 + O4
        op1 = op_[o1].last_output = compute_volume(o1, op_[o1].phase >> 10, am_offset);
        op2 = op_[o2].last_output = compute_volume(o2, op_[o2].phase >> 10, am_offset);
        result = op_[o3].last_output = compute_volume(o3, op_[o3].phase >> 10, am_offset);
        result = add_clamped(add_clamped(add_clamped(result, op0), op1), op2);
        break;
    }

    if (c.output_mask & 1)
        out[0] += result;
    if (c.output_mask & 2)
        out[1] += result;
}

void Core::clock() {
    if (busy_remaining_ > 0)
        --busy_remaining_;

    if (cache_dirty_) {
        cache_global();
        for (uint32_t ch = 0; ch < kChannels; ++ch)
            cache_channel(ch);
        lfo_active_ = global_.lfo_increment[0] != 0 || global_.lfo_increment[1] != 0 ||
                      global_.lfo_am_depth[0] != 0 || global_.lfo_am_depth[1] != 0 ||
                      global_.lfo_pm_depth[0] != 0 || global_.lfo_pm_depth[1] != 0;
        for (uint32_t op = 0; op < kOperators; ++op) {
            cache_operator(op_[op].ch_index, op);
            prepare_operator(op);
            if (!op_[op].cache.tl_ramp)
                op_[op].actual_level = op_[op].cache.total_level;
        }
        cache_dirty_ = false;
        if (!lfo_active_)
            lfo_am_[0] = lfo_am_[1] = 0;
    }

    ++env_counter_;
    if ((env_counter_ & 3) == 3)
        env_counter_ += 1;

    const int32_t lfo_pm = lfo_active_ ? clock_noise_and_lfo() : 0;

    for (uint32_t op = 0; op < kOperators; ++op)
        if (op_[op].cache.tl_ramp)
            clock_level_ramp(op);

    if ((env_counter_ & 3) == 0)
        for (uint32_t op = 0; op < kOperators; ++op)
            clock_envelope(op, env_counter_ >> 2);

    for (uint32_t op = 0; op < kOperators; ++op)
        clock_phase(op, lfo_pm);

    if (timer_a_running_ && ++timer_a_counter_ >= global_.timer_period[0]) {
        timer_a_counter_ = 0;
        if (global_.timer_status_enable[0])
            status_ |= 0x01;
        if (global_.csm) {
            for (uint32_t ch = 0; ch < kChannels; ++ch)
                for (uint32_t slot = 0; slot < kOperatorSlots; ++slot)
                    op_[channel_operator(ch, slot)].keyon_live |= (1u << kKeyOnCsm);
            cache_dirty_ = true; // re-run prepare() so CSM re-triggers the channels
        }
    }
    if (timer_b_running_ && ++timer_b_counter_ >= global_.timer_period[1]) {
        timer_b_counter_ = 0;
        if (global_.timer_status_enable[1])
            status_ |= 0x02;
    }

    for (uint32_t ch = 0; ch < kChannels; ++ch) {
        ch_[ch].feedback[0] = ch_[ch].feedback[1];
        ch_[ch].feedback[1] = ch_[ch].feedback_in;
    }

    int32_t out[2] = {0, 0};
    for (uint32_t ch = 0; ch < kChannels; ++ch)
        output_channel(ch, out);
    output_[0] = static_cast<int16_t>(roundtrip_fp(out[0]));
    output_[1] = static_cast<int16_t>(roundtrip_fp(out[1]));
}

uint64_t Core::state_size() {
    static const uint64_t size = [] {
        SavedState state{};
        srz80::sdk::state::Writer writer;
        archive_state(writer, state);
        return writer.bytes.size();
    }();
    return size;
}

void Core::save_state(uint8_t *buffer) const {
    SavedState state{};
    state.address = address_;
    state.status = status_;
    state.busy_remaining = busy_remaining_;
    state.lfo_counter[0] = lfo_counter_[0];
    state.lfo_counter[1] = lfo_counter_[1];
    state.noise_lfsr = noise_lfsr_;
    state.noise_counter = noise_counter_;
    state.noise_state = noise_state_;
    state.lfo_am[0] = lfo_am_[0];
    state.lfo_am[1] = lfo_am_[1];
    std::memcpy(state.lfo_wave, lfo_wave_, sizeof(state.lfo_wave));
    state.env_counter = env_counter_;
    state.timer_a_counter = timer_a_counter_;
    state.timer_b_counter = timer_b_counter_;
    state.timer_a_running = timer_a_running_ ? 1 : 0;
    state.timer_b_running = timer_b_running_ ? 1 : 0;
    std::memcpy(state.regs, reg_, sizeof(state.regs));
    for (uint32_t i = 0; i < kOperators; ++i) {
        const Operator &o = op_[i];
        SavedOperator &s = state.ops[i];
        s.phase = o.phase;
        s.env_attenuation = o.env_attenuation;
        s.env_state = o.env_state;
        s.key_state = o.key_state;
        s.keyon_live = o.keyon_live;
        s.ramp_counter = o.ramp_counter;
        s.actual_level = o.actual_level;
        s.subphase = o.subphase;
        s.last_output = o.last_output;
    }
    for (uint32_t i = 0; i < kChannels; ++i) {
        const Channel &c = ch_[i];
        SavedChannel &s = state.chans[i];
        s.feedback[0] = c.feedback[0];
        s.feedback[1] = c.feedback[1];
        s.feedback_in = c.feedback_in;
        s.output[0] = c.output[0];
        s.output[1] = c.output[1];
    }
    srz80::sdk::state::Writer writer;
    archive_state(writer, state);
    std::memcpy(buffer, writer.bytes.data(), writer.bytes.size());
}

bool Core::load_state(const uint8_t *buffer, uint64_t size) {
    if (!buffer || size != state_size())
        return false;
    SavedState state{};
    srz80::sdk::state::Reader reader({buffer, static_cast<size_t>(size)});
    archive_state(reader, state);
    if (!reader.finished() || state.timer_a_running > 1 || state.timer_b_running > 1 || state.noise_state > 1)
        return false;
    for (const auto &o : state.ops)
        if (o.env_state > kReverb || o.key_state > 1 || o.keyon_live > 3 || o.env_attenuation > 0x3ff)
            return false;

    reset(); // restores derived state (channel/operator wiring, LFO waveforms)

    address_ = state.address;
    status_ = state.status;
    busy_remaining_ = state.busy_remaining;
    lfo_counter_[0] = state.lfo_counter[0];
    lfo_counter_[1] = state.lfo_counter[1];
    noise_lfsr_ = state.noise_lfsr;
    noise_counter_ = state.noise_counter;
    noise_state_ = state.noise_state;
    lfo_am_[0] = state.lfo_am[0];
    lfo_am_[1] = state.lfo_am[1];
    std::memcpy(lfo_wave_, state.lfo_wave, sizeof(lfo_wave_));
    env_counter_ = state.env_counter;
    timer_a_counter_ = state.timer_a_counter;
    timer_b_counter_ = state.timer_b_counter;
    timer_a_running_ = state.timer_a_running != 0;
    timer_b_running_ = state.timer_b_running != 0;
    std::memcpy(reg_, state.regs, sizeof(reg_));
    for (uint32_t i = 0; i < kOperators; ++i) {
        Operator &o = op_[i];
        const SavedOperator &s = state.ops[i];
        o.phase = s.phase;
        o.env_attenuation = s.env_attenuation;
        o.env_state = s.env_state;
        o.key_state = s.key_state;
        o.keyon_live = s.keyon_live;
        o.ramp_counter = s.ramp_counter;
        o.actual_level = s.actual_level;
        o.subphase = s.subphase;
        o.last_output = s.last_output;
    }
    for (uint32_t i = 0; i < kChannels; ++i) {
        Channel &c = ch_[i];
        const SavedChannel &s = state.chans[i];
        c.feedback[0] = s.feedback[0];
        c.feedback[1] = s.feedback[1];
        c.feedback_in = s.feedback_in;
        c.output[0] = s.output[0];
        c.output[1] = s.output[1];
    }
    return true;
}

}
