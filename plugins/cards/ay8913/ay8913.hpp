#pragma once
#include <cstdint>

// Self-contained AY-3-8913 programmable sound generator.
//
// Ported from MAME's ay8910.cpp sound core (BSD-3-Clause) and reduced to the
// AY-3-8913 variant:
//   * BC2 tied high internally -> the bus collapses to three states
//     (latch address / write data / read data), see the truth table in
//     ay8910.cpp.
//   * 0 I/O ports: registers $0E/$0F (PORTA/PORTB) are stored but inert.
//   * PSG_TYPE_AY: 16-step envelope (mask 0x0f), m_step = 2 (envelope paced
//     at half the tone rate), zero_is_off = 1, ay8910_param DAC curve,
//     legacy-normalized output.
//   * No AY8930 expanded mode, no extended envelope, no GPIO callbacks.
//
// The core steps on an internal tick equal to input_clock / 8.  The card
// drives step() from its audio render callback using a fractional
// accumulator, exactly like the seta_x1_010 card drives its own core.
namespace ay8913 {

class core {
public:
    static constexpr unsigned kRegisters = 16;
    static constexpr unsigned kChannels = 3;
    static constexpr uint64_t serialized_size = 75; // 1 magic + 74 state bytes

    core();
    void reset();

    // Bus interface (BC2 high; BDIR/BC1 as wired on the AY-3-8913).
    void latch_address(uint8_t value); // BDIR=1, BC1=1 : register select
    void write_data(uint8_t value);    // BDIR=1, BC1=0 : data write (gated by latch)
    uint8_t read_data() const;         // BDIR=0, BC1=1 : data read; 0xff when inactive
    // The address state itself is high-impedance (inactive); the card returns
    // 0xff when that address is read.

    // Direct register access for property editing and state replay.
    uint8_t register_value(unsigned r) const { return regs_[r & 0x0f]; }
    void set_register(unsigned r, uint8_t value); // bypasses the latch, writes reg directly

    uint8_t latch() const { return register_latch_; }
    bool active() const { return active_; }

    // Observables for the property inspector.
    uint32_t tone_period(unsigned channel) const;
    bool tone_output(unsigned channel) const;
    bool channel_enabled(unsigned channel) const;
    uint8_t envelope_volume() const;
    bool noise_output() const;
    uint32_t rng_value() const;

    // One input-clock/8 tick: advance tone counters, noise LFSR, envelope.
    void step();

    // Mono mix of the three channels as a float in a small bipolar range
    // (MAME legacy-normalized per-channel tables, summed).
    float channel_sample(unsigned channel) const;
    float sample() const;

    void save_state(uint8_t *dst) const;
    bool load_state(const uint8_t *src);

private:
    struct tone_t {
        uint32_t period = 0;
        uint8_t volume = 0;
        int32_t count = 0;
        uint8_t duty_cycle = 0;
        uint8_t output = 0;
    };

    struct envelope_t {
        uint32_t period = 0;
        int32_t count = 0;
        int8_t step = 0;
        uint8_t volume = 0;
        uint8_t hold = 0;
        uint8_t alternate = 0;
        uint8_t attack = 0;
        uint8_t holding = 0;
    };

    void build_tables();
    void write_reg(unsigned r, unsigned v);

    static constexpr uint8_t kMagic = 0xA3;
    static constexpr uint8_t kEnvStepMask = 0x0f; // AY 16-step envelope
    static constexpr int kEnvStepMultiplier = 2;  // AY m_step = 2

    uint8_t regs_[kRegisters]{};
    uint8_t register_latch_ = 0;
    bool active_ = false;

    tone_t tones_[kChannels];
    envelope_t envelope_;

    int32_t count_noise_ = 0;
    uint8_t prescale_noise_ = 0;
    uint32_t rng_ = 1;

    float vol_table_[16]{};
    float env_table_[16]{};
};

} // namespace ay8913
