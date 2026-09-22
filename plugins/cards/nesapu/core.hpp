#ifndef SRZ80_PLUGINS_NESAPU_CORE_HPP
#define SRZ80_PLUGINS_NESAPU_CORE_HPP

#include <cstdint>

namespace nesapu {

/* Console timing.  The region fixes the frame sequencer and the noise and delta
   period tables, independently of the clock the unit is driven at. */
enum class Region : uint32_t { ntsc = 0, pal = 1, dendy = 2 };

constexpr uint32_t kChannelCount = 5;      /* two pulses, triangle, noise, delta */
constexpr uint32_t kRegisterWindow = 0x18; /* $4000..$4017 */
constexpr uint32_t kStatusOffset = 0x15;   /* $4015 */
constexpr uint32_t kFrameOffset = 0x17;    /* $4017 */

/* The unit keeps no clock of its own: the owner advances it one CPU cycle at a
   time and reads one output level per channel.  Everything inside - the frame
   sequencer, the envelope and sweep dividers, the channel timers and the delta
   transfer - derives from that single cycle stream, so their phases cannot
   drift apart. */
class NesApu {
  public:
    NesApu();

    /* Power-on state: every counter cleared, no channel producing output. */
    void power_on();
    void set_region(Region region);
    Region region() const { return region_; }

    /* Offset 0 is $4000.  Writes to $4014 and $4016 are accepted by the window
       and ignored, because those addresses belong to the console's other units
       and not to the audio unit. */
    static bool decodes(uint32_t offset) { return offset < kRegisterWindow; }
    /* Only $4015 is readable; every other register reads back as open bus, which
       the card reports as a zero for the bus to override. */
    static bool readable(uint32_t offset) { return offset == kStatusOffset; }
    void write(uint32_t offset, uint8_t value);
    uint8_t read(uint32_t offset);
    /* The value $4015 would report, without the flag-clearing side effect. */
    uint8_t peek(uint32_t offset) const;

    void clock();
    /* Advances an exact average of clock_hz/sample_rate CPU cycles.  The
       remainder is carried across calls, so no long-term drift is introduced
       and the rate does not depend on how the caller chunks its output. */
    void advance(uint32_t clock_hz, uint32_t sample_rate);
    /* The carried remainder, so the owner can make its own image exact. */
    uint64_t accumulator() const { return sample_accum_; }
    bool set_accumulator(uint64_t value, uint32_t sample_rate) {
        if (sample_rate == 0 || value >= sample_rate)
            return false;
        sample_accum_ = value;
        return true;
    }

    /* Raw DAC levels before mixing: 0..15 for the tonal channels and the 7-bit
       level counter for the delta channel. */
    uint32_t channel_output(uint32_t channel) const;

    uint8_t status() const { return status_; }
    bool frame_interrupt() const { return (status_ & 0x40u) != 0; }
    bool delta_interrupt() const { return (status_ & 0x80u) != 0; }
    /* True while the delta unit has bytes left to transfer. */
    bool delta_active() const { return delta_.remaining != 0; }
    /* CPU cycles the delta unit asked the machine for during the last clock(),
       which the console charges to the instruction that was executing. */
    uint32_t stall_cycles() const { return stall_cycles_; }

    /* The unit stores no sample bytes itself: it asks the machine for the byte
       at the console address it has latched, the way the console's own DMA would,
       so samples live in ordinary RAM or ROM cards.  A reader that reports
       failure leaves the unit untouched rather than latching a fabricated byte. */
    void set_sample_reader(bool (*reader)(void *context, uint16_t address, uint8_t *value),
                           void *context);

    /* Fixed-layout, endian-independent payload.  state_size() mirrors
       save_state byte for byte.  A false return from load_state leaves the live
       unit untouched. */
    static constexpr uint64_t state_size() {
        // Frame counter fields followed by one record per channel.
        return 8 + 2 + 2 * 24 + 12 + 19 + 23;
    }
    void save_state(uint8_t *buffer) const;
    bool load_state(const uint8_t *buffer, uint64_t size);

  private:
    struct Envelope {
        bool start = false;
        bool loop = false;
        bool constant = false;
        uint8_t divider = 0; /* programmed period V, counted out as V + 1 */
        uint8_t countdown = 0;
        uint8_t decay = 0;
        uint8_t volume = 0;
    };
    struct Sweep {
        bool enabled = false;
        bool negate = false;
        bool reload = false;
        uint8_t divider = 0; /* programmed period P, counted out as P + 1 */
        uint8_t countdown = 0;
        uint8_t shift = 0;
    };
    struct Length {
        bool halt = false;
        bool enabled = false;
        uint8_t value = 0; /* 0..254; the longest programmed length is 254 */
    };
    struct Pulse {
        uint16_t countdown = 0;
        uint16_t period = 0; /* programmed timer, counted out as period + 1 */
        uint8_t duty = 0;
        uint8_t step = 0;
        uint8_t output = 0;
        bool started = false;
        Length length;
        Envelope envelope;
        Sweep sweep;
    };
    struct Triangle {
        uint16_t countdown = 0;
        uint16_t period = 0;
        uint8_t step = 0;
        uint8_t output = 0;
        uint8_t linear = 0;
        uint8_t reload = 0;
        bool control = false;
        Length length;
    };
    struct Noise {
        uint16_t countdown = 0;
        uint16_t period = 0;
        uint16_t shift = 1;
        uint8_t rate = 0;
        uint8_t output = 0;
        bool mode = false;
        Length length;
        Envelope envelope;
    };
    struct Delta {
        uint16_t countdown = 0;
        uint16_t period = 0;
        uint16_t address = 0xC000;
        uint16_t start = 0xC000;
        uint16_t remaining = 1;
        uint16_t length = 1;
        uint8_t rate = 0;
        uint8_t output = 0; /* 7-bit level counter, always even */
        uint8_t shift = 0;
        uint8_t bits = 8;
        uint8_t buffer = 0;
        bool buffered = false;
        bool silent = true;
        bool loop = false;
        bool irq_enabled = false;
        bool enabled = false;
        /* A $4015 write stops the transfer, but the remaining count stays
           visible through $4015 until the refill path observes the stop. */
        bool stopped = false;
    };

    int32_t sweep_target(const Pulse &pulse) const;
    bool sweep_silent(const Pulse &pulse) const;
    void clock_frame_sequencer();
    void clock_quarter_frame();
    void clock_envelope(Envelope &envelope);
    void clock_length(Length &length);
    void clock_pulse(Pulse &pulse);
    void clock_sweep(Pulse &pulse);
    void refresh_pulse(Pulse &pulse);
    void refresh_triangle();
    void refresh_noise();
    void transfer_delta();
    void write_pulse(Pulse &pulse, uint32_t index, uint8_t value);
    void load_pulse_length(Pulse &pulse, uint8_t value);
    Length &length_of(uint32_t channel);
    const Length &length_of(uint32_t channel) const;

    Pulse pulses_[2];
    Triangle triangle_;
    Noise noise_;
    Delta delta_;

    Region region_ = Region::ntsc;
    /* Position inside the current frame step, in CPU cycles. */
    uint16_t frame_count_ = 0;
    uint8_t frame_step_ = 0;
    uint8_t divider_ = 0; /* which CPU cycle of the current APU cycle */
    uint8_t reset_delay_ = 0;
    bool five_step_ = false;
    bool irq_inhibit_ = false;
    /* Set only for the cycles in which a length counter was clocked, so a
       length reload landing in that very cycle can be dropped. */
    bool length_clocked_ = false;
    uint8_t status_ = 0;

    bool (*reader_)(void *, uint16_t, uint8_t *) = nullptr;
    void *reader_context_ = nullptr;
    uint32_t stall_cycles_ = 0;
    uint64_t sample_accum_ = 0;
};

} // namespace nesapu

#endif
