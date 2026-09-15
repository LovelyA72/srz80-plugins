#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace srz80::ymw258 {

struct VoiceInspection {
    uint32_t start = 0;
    uint32_t loop = 0;
    uint32_t length = 0;
    uint32_t envelope = 0;
    uint32_t total_level = 0;
    uint32_t output_peak = 0;
    uint8_t attack = 0;
    uint8_t decay1 = 0;
    uint8_t decay2 = 0;
    uint8_t decay_level = 0;
    uint8_t rate_correction = 0;
    uint8_t release = 0;
    uint8_t envelope_stage = 0;
    bool playing = false;
};

class Engine {
  public:
    static constexpr uint32_t voice_count = 28;
    static constexpr uint32_t register_count = 8;
    static constexpr uint32_t address_space_size = 1u << 22;

    explicit Engine(std::span<const uint8_t> rom, uint32_t chip_clock_hz = 9'878'400);

    void reset();
    void write(uint8_t encoded_slot, uint8_t reg, uint8_t value);
    std::array<int16_t, 2> generate();

    uint8_t reg(uint32_t voice, uint32_t index) const;
    VoiceInspection inspect_voice(uint32_t voice) const;
    std::vector<uint8_t> save_state() const;
    bool load_state(std::span<const uint8_t> state);

  private:
    enum class EnvelopeStage : uint8_t { off, attack, decay, sustain, release };

    struct Voice {
        std::array<uint8_t, register_count> regs{};
        uint32_t start = 0;
        uint32_t loop = 0;
        uint32_t length = 1;
        uint64_t phase = 0;
        uint32_t step = 0x10000;
        uint32_t pitch_lfo_phase = 0;
        uint32_t amplitude_lfo_phase = 0;
        int32_t previous_sample = 0;
        uint32_t envelope_volume = 0;
        uint32_t attack_step = 0;
        uint32_t decay1_step = 0;
        uint32_t decay2_step = 0;
        uint32_t release_step = 0;
        uint32_t total_level = 0;
        uint8_t format = 0;
        uint8_t attack = 0;
        uint8_t decay1 = 0;
        uint8_t decay2 = 0;
        uint8_t decay_level = 0;
        uint8_t rate_correction = 0;
        uint8_t release = 0;
        uint8_t target_level = 0;
        EnvelopeStage envelope = EnvelopeStage::off;
        bool playing = false;
        // UI-only rolling peak. It is intentionally excluded from execution
        // state so inspection cannot affect deterministic emulation.
        uint32_t output_peak = 0;
    };

    uint8_t read_rom(uint32_t address) const;
    int16_t sample_at(const Voice &voice, uint32_t position) const;
    void load_sample(Voice &voice);
    void update_pitch(Voice &voice);
    void key_on(Voice &voice);
    void calculate_envelope(Voice &voice);
    void advance_envelope(Voice &voice);
    void advance_total_level(Voice &voice);
    uint32_t effective_step(Voice &voice);
    uint32_t lfo_phase_step(uint8_t rate) const;
    static int decode_slot(uint8_t encoded);

    std::array<Voice, voice_count> voices_{};
    std::vector<uint8_t> rom_;
    std::array<uint32_t, 8> lfo_phase_steps_{};
};

} // namespace srz80::ymw258
