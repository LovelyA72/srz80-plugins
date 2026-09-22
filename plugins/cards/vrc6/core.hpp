#ifndef SRZ80_PLUGINS_VRC6_CORE_HPP
#define SRZ80_PLUGINS_VRC6_CORE_HPP

#include <cstdint>

namespace vrc6 {

/* Konami VRC6 expansion audio: two pulse channels and one sawtooth channel,
   driven directly by the machine's processor clock.  The unit has no CPU
   coupling of its own - no interrupts, no bus reads, no DMA - so the owner can
   advance it purely as a function of time and read one DAC level per channel.

   Behaviour follows the published VRC6 audio description (NESdev wiki,
   "VRC6 audio"): a 12-bit period divider per channel, a 16-step duty
   generator for the pulses and a 14-clock accumulation ramp for the saw. */

/* The chip decodes three four-byte register blocks at CPU $9000, $A000 and
   $B000; the audio registers therefore span $9000..$B002 inclusive. */
constexpr uint64_t kRegisterSpan = 0x2003;
constexpr uint32_t kPulseCount = 2;
constexpr uint32_t kChannelCount = 3; /* pulse 1, pulse 2, saw */
/* Highest DAC level the mixer can see: 15 + 15 from the pulses plus 31 from
   the saw's top five accumulator bits. */
constexpr uint32_t kMaximumSum = 15 + 15 + 31;

class Core {
  public:
    Core() { power_on(); }

    /* Power-on state: every register cleared, every sequencer at rest and all
       channels silent.  Reset is identical for cold and warm resets because
       the chip has no reset-sensitive state of its own. */
    void power_on();

    /* True for any byte of the mapped register window. */
    static bool decodes(uint32_t offset) { return offset < kRegisterSpan; }
    /* Register writes.  Offsets are relative to the window start (the chip's
       $9000 block); offsets outside the window are ignored. */
    void write(uint32_t offset, uint8_t value);

    /* Advances the oscillators by one processor clock. */
    void clock();

    /* Current DAC level of a channel: 0..15 for the pulses, 0..31 for the
       saw's top five accumulator bits, and always 0 while the channel is
       disabled.  Side-effect free. */
    uint32_t channel_output(uint32_t channel) const;

    /* Latched register and sequencer state, for inspection only. */
    bool channel_enabled(uint32_t channel) const;
    bool pulse_mode(uint32_t pulse) const;
    uint32_t pulse_duty(uint32_t pulse) const;
    uint32_t pulse_volume(uint32_t pulse) const;
    uint32_t pulse_step(uint32_t pulse) const;
    uint32_t saw_rate() const;
    uint32_t saw_accumulator() const;
    uint32_t period(uint32_t channel) const;
    bool halted() const { return halt_; }
    /* Right shift applied to every period: 0, 4 (16x pitch) or 8 (256x). */
    uint32_t frequency_shift() const { return shift_; }

    /* Fixed-layout, endian-independent payload of latched registers and sequencer positions.
       state_size() mirrors save_state byte for byte, and a false return from
       load_state leaves the live unit untouched. */
    static constexpr uint64_t state_size() { return 37; }
    void save_state(uint8_t *buffer) const;
    bool load_state(const uint8_t *buffer, uint64_t size);

  private:
    struct Pulse {
        /* Latched register bytes, kept so a half-updated period pair keeps
           working across a state restore. */
        uint8_t control = 0;     /* MDDD VVVV */
        uint8_t period_low = 0;  /* low 8 bits of the period */
        uint8_t period_high = 0; /* E... FFFF */
        uint16_t period = 0;     /* composed 12-bit divider target */
        uint32_t divider = 0;    /* clocks accumulated since the last step */
        uint8_t step = 15;       /* duty position, counting 15 down to 0 */
        bool enabled = false;
    };
    struct Saw {
        uint8_t period_low = 0;
        uint8_t period_high = 0;
        uint8_t rate = 0;        /* ..AA AAAA, added to the accumulator */
        uint16_t period = 0;
        uint32_t divider = 0;
        uint8_t accumulator = 0; /* 8-bit ramp value; its top 5 bits are out */
        uint8_t reactions = 0;   /* additions done in the current ramp, 0..6 */
        uint8_t half = 0;        /* the accumulator only reacts every 2nd clock */
        bool enabled = false;
    };

    void write_pulse(Pulse &pulse, uint32_t reg, uint8_t value);
    void write_saw(uint32_t reg, uint8_t value);
    void write_control(uint8_t value);
    void clock_pulse(Pulse &pulse);
    void clock_saw();
    /* The period a channel's divider compares against, after the global
       frequency shift. */
    static uint32_t effective_period(uint16_t period, uint32_t shift);
    void refresh_period(Pulse &pulse);
    void refresh_period(Saw &saw);

    Pulse pulses_[kPulseCount];
    Saw saw_;
    uint32_t shift_ = 0; /* 0, 4 or 8 */
    bool halt_ = false;
};

} // namespace vrc6

#endif
