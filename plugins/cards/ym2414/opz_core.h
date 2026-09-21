// SRZ80's OPZ emulator
// This emulation core is by no mean accurate.
#pragma once

#include <cstdint>

namespace srz80::ym2414 {

class Core {
  public:
    static constexpr uint32_t kChannels = 8;
    static constexpr uint32_t kOperatorSlots = 4;
    static constexpr uint32_t kOperators = kChannels * kOperatorSlots;
    static constexpr uint32_t kRegisters = 0x190;

    // Native (FM DAC) sample rate = chip clock / (prescale * operators)
    static constexpr uint32_t kClockDivider = 64;

    explicit Core(uint32_t chip_clock_hz);

    void reset();

    // Register interface: 2 ports, address latch + data.
    void write_address(uint8_t value) { address_ = value; }
    void write_data(uint8_t value);
    uint8_t read_status() const;

    uint32_t native_rate() const { return native_rate_; }

    // Advance the whole chip by exactly one native sample.
    void clock();
    int16_t output_left() const { return output_[0]; }
    int16_t output_right() const { return output_[1]; }

    static uint64_t state_size();
    void save_state(uint8_t *buffer) const;
    // Returns false and leaves the device untouched on malformed input
    bool load_state(const uint8_t *buffer, uint64_t size);

    // Introspection for the host property view (never mutates device state)
    uint32_t operator_env(uint32_t op) const { return op_[op].env_attenuation; }
    uint32_t operator_state(uint32_t op) const { return op_[op].env_state; }
    int32_t operator_output(uint32_t op) const { return op_[op].last_output; }
    uint32_t operator_phase(uint32_t op) const { return (op_[op].phase >> 10) & 0x3ff; }
    uint32_t operator_phase_step(uint32_t op) const { return op_[op].cache.phase_step; }
    uint32_t operator_key(uint32_t op) const { return op_[op].key_state; }
    uint32_t channel_block_freq(uint32_t channel) const { return ch_block_freq(channel); }
    uint8_t register_value(uint32_t index) const { return index < kRegisters ? reg_[index] : 0; }

    // Global operator index for a channel's slot (0..3).
    static uint32_t channel_operator(uint32_t channel, uint32_t slot);

  private:
    enum EnvelopeState : uint8_t {
        kAttack = 0,
        kDecay,
        kSustain,
        kRelease,
        kReverb,
        kEnvelopeStates
    };

    enum KeyOnType : uint8_t { kKeyOnNormal = 0, kKeyOnCsm = 1 };

    struct Cache {
        const uint16_t *waveform = nullptr;
        uint32_t phase_step = 0; // 0 => recompute every sample (PM or fix mode)
        uint32_t total_level = 0;
        uint32_t block_freq = 0;
        int32_t detune = 0;
        uint32_t multiple = 0;
        uint16_t fixed_frequency = 0;
        uint8_t fixed_range = 0;
        uint8_t fixed_mode = 0;
        uint8_t detune2 = 0;
        uint8_t am_enable = 0; // 1 if LFO AM applies to this operator (cached)
        uint32_t eg_sustain = 0;
        uint8_t eg_rate[kEnvelopeStates] = {};
        uint8_t eg_shift = 0;
        uint8_t tl_ramp = 0;
        uint8_t tl_ramp_period = 0;
    };

    struct Operator {
        uint32_t phase = 0;
        uint16_t env_attenuation = 0x3ff;
        uint8_t env_state = kRelease;
        uint8_t key_state = 0;
        uint8_t keyon_live = 0;
        uint8_t ch_index = 0; // owning channel
        uint32_t ramp_counter = 0;
        uint32_t actual_level = 0;
        uint32_t subphase = 0; // extra fixed-frequency resolution
        int32_t last_output = 0;
        Cache cache;
    };

    struct Channel {
        uint32_t feedback[2] = {};
        int32_t feedback_in = 0;
        int32_t output[2] = {};
        uint8_t algorithm_index = 0;
        uint8_t am_sens = 0;    // LFO AM sensitivity (cached)
        uint8_t am_select = 0;  // LFO AM select 0/1 (cached)
        uint8_t pm_sens = 0;
        uint8_t pm_select = 0;
        uint8_t feedback_shift = 0; // zero means feedback disabled
        uint8_t output_mask = 0;    // bit 0 = left, bit 1 = right
    };

    struct GlobalCache {
        uint32_t noise_period = 0;
        uint32_t lfo_increment[2] = {};
        uint32_t timer_period[2] = {};
        uint8_t lfo_waveform[2] = {};
        uint8_t lfo_am_depth[2] = {};
        uint8_t lfo_pm_depth[2] = {};
        uint8_t lfo_sync[2] = {};
        uint8_t timer_status_enable[2] = {};
        uint8_t csm = 0;
    };

    // Raw register bit helpers (also used with a per-operator extra offset)
    uint32_t byte(uint32_t offset, uint32_t start, uint32_t count, uint32_t extra = 0) const {
        return (reg_[offset + extra] >> start) & ((1u << count) - 1u);
    }
    uint32_t word(uint32_t o1, uint32_t s1, uint32_t c1, uint32_t o2, uint32_t s2, uint32_t c2,
                  uint32_t extra = 0) const {
        return (byte(o1, s1, c1, extra) << c2) | byte(o2, s2, c2, extra);
    }

    // System registers
    uint32_t lfo_reset() const { return byte(0x09, 1, 1); }
    uint32_t noise_frequency() const { return byte(0x0f, 0, 5); }
    uint32_t timer_a_value() const { return word(0x10, 0, 8, 0x11, 0, 2); }
    uint32_t timer_b_value() const { return byte(0x12, 0, 8); }
    uint32_t csm() const { return byte(0x14, 7, 1); }
    uint32_t lfo2_pm_depth() const { return byte(0x188, 0, 7); }
    uint32_t lfo2_rate() const { return byte(0x16, 0, 8); }
    uint32_t lfo2_am_depth() const { return byte(0x17, 0, 7); }
    uint32_t lfo_rate() const { return byte(0x18, 0, 8); }
    uint32_t lfo_am_depth() const { return byte(0x19, 0, 7); }
    uint32_t lfo_pm_depth() const { return byte(0x189, 0, 7); }
    uint32_t lfo2_sync() const { return byte(0x1b, 5, 1); }
    uint32_t lfo_sync() const { return byte(0x1b, 4, 1); }
    uint32_t lfo2_waveform() const { return byte(0x1b, 2, 2); }
    uint32_t lfo_waveform() const { return byte(0x1b, 0, 2); }

    // Channel registers
    uint32_t ch_output_0(uint32_t ch) const {
        return byte(0x30, 0, 1, ch) | (byte(0x20, 7, 1, ch) ^ 1u);
    }
    uint32_t ch_output_1(uint32_t ch) const {
        return byte(0x20, 7, 1, ch) | byte(0x30, 0, 1, ch);
    }
    uint32_t ch_feedback(uint32_t ch) const { return byte(0x20, 3, 3, ch); }
    uint32_t ch_algorithm(uint32_t ch) const { return byte(0x20, 0, 3, ch); }
    uint32_t ch_block_freq(uint32_t ch) const {
        return word(0x28, 0, 7, 0x30, 2, 6, ch);
    }
    uint32_t ch_lfo_pm_sens(uint32_t ch) const { return byte(0x38, 4, 3, ch); }
    uint32_t ch_lfo_am_sens(uint32_t ch) const { return byte(0x38, 0, 2, ch); }
    uint32_t ch_lfo_pm_select(uint32_t ch) const { return byte(0x38, 7, 1, ch); }
    uint32_t ch_lfo_am_select(uint32_t ch) const { return byte(0x38, 2, 1, ch); }
    uint32_t ch_ramp_period(uint32_t ch) const { return byte(0x00, 0, 8, ch); }

    // Operator registers
    uint32_t op_detune(uint32_t op) const { return byte(0x40, 4, 3, op); }
    uint32_t op_multiple(uint32_t op) const { return byte(0x40, 0, 4, op); }
    uint32_t op_fix_range(uint32_t op) const { return byte(0x40, 4, 3, op); }
    uint32_t op_fix_frequency(uint32_t op) const { return byte(0x40, 0, 4, op); }
    uint32_t op_waveform(uint32_t op) const { return byte(0x100, 4, 3, op); }
    uint32_t op_fine(uint32_t op) const { return byte(0x100, 0, 4, op); }
    uint32_t op_total_level(uint32_t op) const { return byte(0x60, 0, 7, op); }
    uint32_t op_tl_ramp(uint32_t op) const { return byte(0x60, 7, 1, op); }
    uint32_t op_ksr(uint32_t op) const { return byte(0x80, 6, 2, op); }
    uint32_t op_fix_mode(uint32_t op) const { return byte(0x80, 5, 1, op); }
    uint32_t op_attack_rate(uint32_t op) const { return byte(0x80, 0, 5, op); }
    uint32_t op_lfo_am_enable(uint32_t op) const { return byte(0xa0, 7, 1, op); }
    uint32_t op_decay_rate(uint32_t op) const { return byte(0xa0, 0, 5, op); }
    uint32_t op_detune2(uint32_t op) const { return byte(0xc0, 6, 2, op); }
    uint32_t op_sustain_rate(uint32_t op) const { return byte(0xc0, 0, 5, op); }
    uint32_t op_eg_shift(uint32_t op) const { return byte(0x120, 6, 2, op); }
    uint32_t op_reverb_rate(uint32_t op) const { return byte(0x120, 0, 3, op); }
    uint32_t op_sustain_level(uint32_t op) const { return byte(0xe0, 4, 4, op); }
    uint32_t op_release_rate(uint32_t op) const { return byte(0xe0, 0, 4, op); }

    // Model helpers
    void cache_channel(uint32_t channel);
    void cache_operator(uint32_t channel, uint32_t op);
    void cache_global();
    uint32_t compute_phase_step(uint32_t channel, uint32_t op, const Cache &cache,
                                int32_t lfo_raw_pm);
    void prepare_operator(uint32_t op);
    void clock_keystate(uint32_t op, uint32_t keystate);
    void start_attack(uint32_t op, bool is_restart);
    void start_release(uint32_t op);
    void clock_level_ramp(uint32_t op);
    void clock_phase(uint32_t op, int32_t lfo_raw_pm);
    void clock_envelope(uint32_t op, uint32_t env_counter);
    uint32_t envelope_attenuation(uint32_t op, uint32_t am_offset) const;
    int32_t compute_volume(uint32_t op, uint32_t phase, uint32_t am_offset) const;

    int32_t clock_noise_and_lfo();
    uint32_t lfo_am_offset(uint32_t channel) const;

    void output_channel(uint32_t channel, int32_t *out);
    void key_on(uint32_t channel, uint32_t opmask);
    void handle_mode_write(uint8_t data);

    uint8_t reg_[kRegisters] = {};
    uint8_t address_ = 0;
    uint8_t status_ = 0;
    uint32_t busy_remaining_ = 0;
    bool cache_dirty_ = true; // re-derive operator caches on the next clock
    bool lfo_active_ = true;  // false when the LFO/noise block is fully inert

    uint32_t lfo_counter_[2] = {};
    uint32_t noise_lfsr_ = 1;
    uint32_t noise_counter_ = 0;
    uint8_t noise_state_ = 0;
    int32_t lfo_am_[2] = {};
    int16_t lfo_wave_[4][256] = {};

    uint32_t env_counter_ = 0;

    uint32_t timer_a_counter_ = 0;
    uint32_t timer_b_counter_ = 0;
    bool timer_a_running_ = false;
    bool timer_b_running_ = false;

    Operator op_[kOperators];
    Channel ch_[kChannels];
    GlobalCache global_;

    uint32_t native_rate_ = 0;
    int16_t output_[2] = {};
};

}
