// SRZ80 YM2414 (OPZ) engine selector.
//
// Two emulation cores are available for this card: the in-tree srz80 core
// (opz_core.*) and the vendored ymfm OPZ core (ymfm/, the same library
// the YMF262 card uses). This adapter presents a single surface to the card
// so the rest of the plugin is core-agnostic.
#pragma once

#include <cstdint>
#include <memory>

namespace srz80::ym2414 {

// Which emulation core drives the card. Chosen in the host settings and read
// once at create time ("used for new cards and newly opened projects").
enum class Backend : uint32_t { srz80, ymfm };

class Engine {
  public:
    static constexpr uint32_t kRegisters = 0x190;
    static constexpr uint32_t kOperators = 32;

    Engine(Backend backend, uint32_t chip_clock_hz);
    ~Engine();

    Engine(const Engine &) = delete;
    Engine &operator=(const Engine &) = delete;

    void reset();

    // Register interface: 2 ports, address latch + data.
    void write_address(uint8_t value);
    void write_data(uint8_t value);
    uint8_t read_status();

    // Native (FM DAC) sample rate = chip clock / (prescale * operators).
    uint32_t native_rate() const;

    // Advance the whole chip by exactly one native sample.
    void clock();
    int16_t output_left() const;
    int16_t output_right() const;

    // Introspection for the host property view (never mutates device state).
    uint32_t operator_env(uint32_t op);
    uint32_t operator_state(uint32_t op);
    int32_t operator_output(uint32_t op);
    uint32_t operator_phase(uint32_t op);
    uint32_t operator_key(uint32_t op);
    uint8_t register_value(uint32_t index) const;

    // Engine state persistence (register file plus DSP state), excluding the
    // card's own latch/resample header. The reported size is stable for a
    // given backend and instance.
    uint64_t state_size();
    void save_state(uint8_t *buffer);
    // Returns false and leaves the device untouched on malformed input.
    bool load_state(const uint8_t *buffer, uint64_t size);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace srz80::ym2414
