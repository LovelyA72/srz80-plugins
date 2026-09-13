// SRZ80 card plugin: Yamaha V9938 / V9958 VDP.
//
// This is the host-facing half of the port in v9938_core.cpp.  It owns the
// card ABI surface and everything the engine can drive:
//
//   four IO ports     -> v99x8_device::read()/write()
//   the raster        -> one self-rescheduling scheduled event per scanline
//   the IRQ signal    -> v99x8_device::irq_line()
//   a video surface   -> the core's RGBA8 framebuffer, served in chunks
//   save/load state   -> the core's field-wise snapshot plus card metadata
//
// See README.md for the build wiring and TODO-VDP.md for what is deferred.

#include <boundary.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>

#include "v9938_core.cpp"

namespace {

// ---------------------------------------------------------------------------
//  Configuration
// ---------------------------------------------------------------------------

// The V9938 runs from its own crystal, so its raster is a property of the card,
// not of whichever master clock happens to be running.  The card schedules its
// scanline event directly in simulated nanoseconds, which means the VDP still
// produces frames when no CPU clock is enabled at all -- useful while laying a
// project out.
constexpr uint64_t default_raster_clock_hz = 21'477'272;  // MSX V9938 X1
constexpr uint64_t min_raster_clock_hz = 1'000'000;
constexpr uint64_t max_raster_clock_hz = 50'000'000;
constexpr uint32_t port_count = 4;

struct Settings {
    int model = srz80::vdp::MODEL_V9938;
    uint32_t vram_size = 0x20000;
    uint64_t raster_clock_hz = default_raster_clock_hz;
    std::string io_space = "cpu0.io";
};

// Small hand-written JSON reader, matching the sibling cards that do not link a
// JSON library.  Unknown keys are rejected so a typo in a project file fails
// loudly instead of being ignored.
class Config {
  public:
    explicit Config(const SrhConfig *config) {
        if (srz80::sdk::has_field(config, &SrhConfig::config_json) && config->config_json)
            text_ = {config->config_json, static_cast<size_t>(config->config_json_size)};
        else
            text_ = "{}";
    }

    bool parse(Settings &settings) {
        if (!take('{'))
            return false;
        skip();
        if (take('}'))
            return done();
        while (true) {
            std::string key, text;
            bool quoted = false;
            if (!string(key) || !take(':') || !scalar(text, quoted))
                return false;
            if (key == "io_space") {
                if (!quoted || text.empty() || text.size() > 128)
                    return false;
                settings.io_space = text;
            } else if (key == "model") {
                uint64_t number = 0;
                if (quoted && text == "V9938")
                    number = srz80::vdp::MODEL_V9938;
                else if (quoted && text == "V9958")
                    number = srz80::vdp::MODEL_V9958;
                else if (!unsigned_value(text, quoted, number) || number > 1)
                    return false;
                settings.model = int(number);
            } else if (key == "vram_size") {
                uint64_t number = 0;
                if (!unsigned_value(text, quoted, number) || number < 0x20000 ||
                    number > 0x30000)
                    return false;
                settings.vram_size = uint32_t(number);
            } else if (key == "raster_clock_hz") {
                uint64_t number = 0;
                if (!unsigned_value(text, quoted, number) || number < min_raster_clock_hz ||
                    number > max_raster_clock_hz)
                    return false;
                settings.raster_clock_hz = number;
            } else {
                return false;
            }
            if (take('}'))
                return done();
            if (!take(','))
                return false;
        }
    }

  private:
    void skip() {
        while (position_ < text_.size() &&
               (text_[position_] == ' ' || text_[position_] == '\t' ||
                text_[position_] == '\n' || text_[position_] == '\r'))
            ++position_;
    }
    bool take(char c) {
        skip();
        if (position_ >= text_.size() || text_[position_] != c)
            return false;
        ++position_;
        return true;
    }
    bool string(std::string &out) {
        skip();
        if (position_ >= text_.size() || text_[position_++] != '"')
            return false;
        while (position_ < text_.size() && text_[position_] != '"') {
            if (text_[position_] == '\\')
                return false;
            out.push_back(text_[position_++]);
        }
        if (position_ >= text_.size())
            return false;
        ++position_;
        return true;
    }
    bool scalar(std::string &out, bool &quoted) {
        skip();
        if (position_ < text_.size() && text_[position_] == '"') {
            quoted = true;
            return string(out);
        }
        const auto first = position_;
        while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9')
            ++position_;
        if (first == position_)
            return false;
        out.assign(text_.substr(first, position_ - first));
        return true;
    }
    static bool unsigned_value(const std::string &text, bool quoted, uint64_t &out) {
        int base = 10;
        std::string_view digits = text;
        if (quoted && digits.size() > 2 && digits[0] == '0' &&
            (digits[1] == 'x' || digits[1] == 'X')) {
            base = 16;
            digits.remove_prefix(2);
        }
        if (digits.empty() || digits.size() > 16)
            return false;
        uint64_t value = 0;
        for (const char c : digits) {
            int digit = -1;
            if (c >= '0' && c <= '9')
                digit = c - '0';
            else if (base == 16 && c >= 'a' && c <= 'f')
                digit = 10 + (c - 'a');
            else if (base == 16 && c >= 'A' && c <= 'F')
                digit = 10 + (c - 'A');
            if (digit < 0 || digit >= base)
                return false;
            value = value * uint64_t(base) + uint64_t(digit);
        }
        out = value;
        return true;
    }
    bool done() {
        skip();
        return position_ == text_.size();
    }

    std::string_view text_;
    size_t position_ = 0;
};

// ---------------------------------------------------------------------------
//  Card
// ---------------------------------------------------------------------------

// One V9938 scanline is HTOTAL * 2 = 1368 pixel clocks, and the engine clock
// runs at a quarter of the pixel clock, so the scheduler needs 342 MHz ticks
// per line.  Keeping the arithmetic in 64-bit integers avoids a floating point
// divide per scanline.
constexpr uint64_t line_ticks_numerator = 342ull * 1000ull * 1000ull * 1000ull;

class Card final : public srz80::vdp::v99x8_device {
  public:
    Card(const Settings &settings, const ShouryoHost *host, SrhHandle owner)
        : v99x8_device(settings.model, settings.vram_size),
          host_(host),
          owner_(owner),
          raster_clock_hz_(settings.raster_clock_hz),
          vram_size_(settings.vram_size),
          model_(settings.model) {}

    // ---- ABI lifecycle -----------------------------------------------------

    // Registers the port mapping and the video surface, then brings the chip
    // out of reset.  Both host calls can fail, so the status is propagated
    // rather than swallowed: a card that could not claim its ports must not
    // report a successful create().
    SrhStatus start(const SrhMapping &mapping, const SrhHostVideoV1 *video) {
        SrhStatus status = host_->map(host_->context, owner_, &mapping, &mapping_);
        if (status != SRH_OK)
            return status;
        // The surface is sized for the taller PAL frame so a PAL/NTSC switch
        // never has to re-register, matching the core's fixed-size buffer.
        status = video->register_video_ex(video->context, owner_, surface_width, surface_max_height,
                                SRH_VIDEO_RGBA8, query, this, SRH_VIDEO_ALLOW_SHADER, &surface_);
        if (status != SRH_OK)
            return status;
        status = video->set_video_timing(video->context, surface_, timing, this);
        if (status != SRH_OK) return status;
        device_start();
        clear_frame();
        arm_line();
        return SRH_OK;
    }

    void reset(bool cold) {
        if (cold)
            clear_vram();
        device_reset();
        frame_number_ = 0;
        // Register reset blanks the display even if no further raster event runs.
        clear_frame();
        // Re-arm from the restarted timeline rather than from the old due time.
        if (line_event_) {
            host_->cancel(host_->context, line_event_);
            line_event_ = 0;
        }
        arm_line();
    }

    // Optional: a rack with no IRQ-capable CPU has no such signal, and the
    // interrupt flags stay readable through status register 0.
    void connect_irq(SrhHandle signal) { irq_signal_ = signal; }

    // ---- ports -------------------------------------------------------------

    SrhStatus port_read(uint64_t address, uint8_t *value) {
        if (!value || address < base_ || address - base_ >= port_count)
            return SRH_INVALID;
        *value = read(uint32_t(address - base_));
        return SRH_OK;
    }

    SrhStatus port_write(uint64_t address, uint8_t value) {
        if (address < base_ || address - base_ >= port_count)
            return SRH_INVALID;
        write(uint32_t(address - base_), value);
        return SRH_OK;
    }

    void set_base(uint64_t base) { base_ = base; }

    // ---- video -------------------------------------------------------------

    // Serves the whole allocated surface, including the rows a shorter display
    // mode never draws, so the reported geometry is stable for the UI and the
    // read offset arithmetic needs no mode awareness.
    SrhStatus render(uint64_t offset, uint8_t *out, uint32_t *size, uint32_t *total) const {
        if (!size || !total || (!out && *size))
            return SRH_INVALID;
        const uint32_t bytes = framebuffer_size();
        *total = bytes;
        if (offset >= bytes) {
            *size = 0;
            return SRH_OK;
        }
        const uint32_t count =
            uint32_t(std::min<uint64_t>(*size, uint64_t(bytes) - offset));
        if (count)
            std::memcpy(out, framebuffer() + offset, count);
        *size = count;
        return SRH_OK;
    }

    // ---- state -------------------------------------------------------------

    uint64_t state_size() const { return state_header_size + v99x8_device::state_size() + sizeof(frame_number_); }

    void save(uint8_t *buffer) const {
        if (!buffer)
            return;
        const uint32_t header[2] = {state_version, uint32_t(model_)};
        std::memcpy(buffer, header, sizeof(header));
        v99x8_device::save_state(buffer + state_header_size);
        std::memcpy(buffer + state_size() - sizeof(frame_number_), &frame_number_, sizeof(frame_number_));
    }

    bool load(const uint8_t *buffer, uint64_t size) {
        if (!buffer || size < state_header_size) return false;
        uint32_t header[2] = {0, 0};
        std::memcpy(header, buffer, sizeof(header));
        if (header[0] < 1 || header[0] > state_version || header[1] != uint32_t(model_))
            return false;
        const bool legacy = header[0] == 1;
        const uint64_t core_size = header[0] < 3 ? LEGACY_STATE_SIZE : v99x8_device::state_size();
        if (size != state_header_size + core_size + (legacy ? 0 : sizeof(frame_number_))) return false;
        if (!v99x8_device::load_state(buffer + state_header_size, core_size)) return false;
        frame_number_ = 0;
        if (!legacy)
            std::memcpy(&frame_number_, buffer + size - sizeof(frame_number_), sizeof(frame_number_));
        return true;
    }

    // ---- properties --------------------------------------------------------

    uint32_t property_count() const { return 6; }

    bool property_info(uint32_t index, SrhProperty *out) const {
        struct Info {
            const char *name;
            const char *group;
            const char *description;
            uint32_t kind, bits, base, editable, flags;
        };
        static const Info table[] = {
            {"model", "Chip", "V9938 or V9958", SRH_ENUM, 0, 0, 1, SRH_PROPERTY_PERSISTENT},
            {"vram", "Chip", "Installed VRAM in bytes", SRH_UNSIGNED, 32, 10, 0,
             SRH_PROPERTY_RUNTIME},
            {"raster_clock_hz", "Timing", "VDP crystal in Hz", SRH_UNSIGNED, 32, 10, 1, 0},
            {"pal", "Timing", "PAL/NTSC field rate", SRH_BOOLEAN, 0, 0, 0, SRH_PROPERTY_RUNTIME},
            {"mode", "Display", "Active screen mode", SRH_TEXT, 0, 0, 0,
             SRH_PROPERTY_RUNTIME},
            {"status", "Display", "Status register 0", SRH_UNSIGNED, 8, 16, 0,
             SRH_PROPERTY_RUNTIME},
        };
        if (!srz80::sdk::valid(out) || index >= property_count())
            return false;
        const Info &info = table[index];
        *out = {SRH_INIT(SrhProperty), info.name, info.group, info.description, info.kind,
                info.bits, info.base, info.editable, index == 0 ? "V9938|V9958" : nullptr, info.flags};
        return true;
    }

    bool property_get(uint32_t index, SrhValue *out) {
        if (!out || index >= property_count())
            return false;
        *out = SrhValue{SRH_INIT(SrhValue), 0, 0, {0}};
        switch (index) {
        case 0:
            out->unsigned_value = uint64_t(model_);
            return true;
        case 1:
            out->unsigned_value = vram_size_;
            return true;
        case 2:
            out->unsigned_value = raster_clock_hz_;
            return true;
        case 3:
            out->unsigned_value = palette_mode() ? 1 : 0;
            return true;
        case 4: {
            const char *name = mode_name();
            const size_t length = std::min<size_t>(std::strlen(name), sizeof(out->text) - 1);
            std::memcpy(out->text, name, length);
            out->text[length] = '\0';
            return true;
        }
        case 5:
            out->unsigned_value = status_registers()[0];
            return true;
        default:
            return false;
        }
    }

    bool property_set(uint32_t index, const SrhValue *in) {
        if (!in || index >= property_count())
            return false;
        switch (index) {
        case 0:
            if (in->unsigned_value > 1)
                return false;
            if (model_ != int(in->unsigned_value)) {
                model_ = int(in->unsigned_value);
                select_model(model_);
                reset(true);
            }
            return true;
        case 2: {
            const uint64_t hz = in->unsigned_value;
            if (hz < min_raster_clock_hz || hz > max_raster_clock_hz)
                return false;
            raster_clock_hz_ = hz;
            // The next scheduled line already has its due time, so the new rate
            // takes effect from the following line.  Re-arming here would need
            // a fresh baseline, which reset() supplies.
            return true;
        }
        default:
            return false;
        }
    }

  protected:
    // The core's only polymorphism point left to the card: the base
    // palette_init() builds the V9958 YJK table when the card is model 1.
    void irq_line(uint8_t state) override {
        if (!irq_signal_ || state == irq_level_)
            return;
        irq_level_ = state;
        // Rack signal levels are millivolts; the Z80 considers values above
        // 700 mV asserted.  A logical 1 here never reached its IRQ input.
        host_->signal_drive(host_->context, owner_, irq_signal_, state ? 1000 : 0, 0);
    }

  private:
    static constexpr uint32_t state_version = 3;
    static constexpr uint64_t state_header_size = sizeof(uint32_t) * 2;

    // The raster.  One event per scanline, rescheduled from inside its own
    // callback.  The delay is a 64-bit integer division of the current crystal
    // into a line's worth of nanoseconds, so the rate is exact in the
    // aggregate and never reads a wall clock.
    void arm_line() {
        const uint64_t delay = line_ticks_numerator / raster_clock_hz_;
        line_event_ = 0;
        host_->schedule(host_->context, owner_, delay ? delay : 1, line_event_callback, this,
                        &line_event_);
    }

    static SrhStatus SRH_CALL line_event_callback(void *context) {
        auto &card = *static_cast<Card *>(context);
        card.line_tick();
        if (card.scanout_line() == 0) ++card.frame_number_;
        card.arm_line();
        return SRH_OK;
    }

    static SrhStatus SRH_CALL timing(void *context, SrhVideoTiming *out) {
        if (!srz80::sdk::valid(out)) return SRH_INVALID;
        const auto &card = *static_cast<Card *>(context);
        *out = {SRH_INIT(SrhVideoTiming), card.frame_number_, card.scanout_line(), card.scanout_lines()};
        return SRH_OK;
    }

    static SrhStatus SRH_CALL query(void *context, uint64_t offset, uint8_t *out,
                                    uint32_t *size, uint32_t *total) {
        return srz80::sdk::guard(
            [&] { return static_cast<const Card *>(context)->render(offset, out, size, total); });
    }

    const ShouryoHost *host_ = nullptr;
    SrhHandle owner_ = 0;
    SrhHandle mapping_ = 0;
    SrhHandle surface_ = 0;
    SrhHandle line_event_ = 0;
    SrhHandle irq_signal_ = 0;
    uint64_t frame_number_ = 0;
    uint64_t base_ = 0;
    uint64_t raster_clock_hz_ = default_raster_clock_hz;
    uint32_t vram_size_ = 0;
    int model_ = srz80::vdp::MODEL_V9938;
    uint8_t irq_level_ = 0;
};

// ---------------------------------------------------------------------------
//  ABI shims
// ---------------------------------------------------------------------------

SrhStatus SRH_CALL read(void *context, uint64_t address, uint8_t *value) {
    return srz80::sdk::guard(
        [&] { return static_cast<Card *>(context)->port_read(address, value); });
}

SrhStatus SRH_CALL write(void *context, uint64_t address, uint8_t value) {
    return srz80::sdk::guard(
        [&] { return static_cast<Card *>(context)->port_write(address, value); });
}

SrhStatus SRH_CALL create(const ShouryoHost *host, SrhHandle owner, const SrhConfig *config,
                          void **result) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!srz80::sdk::valid(host) || !srz80::sdk::valid(config) || !result || !config->space ||
            !host->map || !host->schedule || !host->cancel || !host->signal_drive ||
            !host->query || config->size != port_count)
            return SRH_INVALID;
        Settings settings;
        if (!Config(config).parse(settings))
            return SRH_INVALID;
        const void *extension = nullptr;
        if (host->query(host->context, "host.video.v1", &extension) != SRH_OK || !extension)
            return SRH_UNAVAILABLE;
        const auto *video = static_cast<const SrhHostVideoV1 *>(extension);
        if (!srz80::sdk::valid(video) ||
            !srz80::sdk::has_field(video, &SrhHostVideoV1::set_video_timing) ||
            !video->register_video_ex || !video->set_video_timing)
            return SRH_UNAVAILABLE;

        auto card = std::make_unique<Card>(settings, host, owner);
        card->set_base(config->base);
        SrhMapping mapping{SRH_INIT(SrhMapping), config->space, config->base,
                           config->base + port_count - 1, config->priority, card.get(), read,
                           write, read, nullptr};
        auto status = card->start(mapping, video);
        if (status != SRH_OK)
            return status;

        // The IRQ line is optional.
        SrhHandle irq = 0;
        if (host->signal_find && host->signal_find(host->context, "IRQ", &irq) == SRH_OK && irq)
            card->connect_irq(irq);
        *result = card.release();
        return SRH_OK;
    });
}

void SRH_CALL destroy(void *context) { delete static_cast<Card *>(context); }

SrhStatus SRH_CALL reset(void *context, uint32_t cold) {
    return srz80::sdk::guard([&] {
        static_cast<Card *>(context)->reset(cold != 0);
        return SRH_OK;
    });
}

uint32_t SRH_CALL property_count(void *context) {
    return static_cast<Card *>(context)->property_count();
}

SrhStatus SRH_CALL property_info(void *context, uint32_t index, SrhProperty *out) {
    return static_cast<Card *>(context)->property_info(index, out) ? SRH_OK : SRH_INVALID;
}

SrhStatus SRH_CALL property_get(void *context, uint32_t index, SrhValue *out) {
    return srz80::sdk::guard(
        [&] { return static_cast<Card *>(context)->property_get(index, out) ? SRH_OK : SRH_INVALID; });
}

SrhStatus SRH_CALL property_set(void *context, uint32_t index, const SrhValue *in) {
    return srz80::sdk::guard(
        [&] { return static_cast<Card *>(context)->property_set(index, in) ? SRH_OK : SRH_INVALID; });
}

SrhStatus SRH_CALL save_state(void *context, uint8_t *buffer, uint64_t *size) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!size)
            return SRH_INVALID;
        auto &card = *static_cast<Card *>(context);
        const uint64_t required = card.state_size();
        if (!buffer) {
            *size = required;
            return SRH_OK;
        }
        if (*size < required) {
            *size = required;
            return SRH_UNAVAILABLE;
        }
        card.save(buffer);
        *size = required;
        return SRH_OK;
    });
}

SrhStatus SRH_CALL load_state(void *context, const uint8_t *buffer, uint64_t size) {
    return srz80::sdk::guard([&] {
        return static_cast<Card *>(context)->load(buffer, size) ? SRH_OK : SRH_INVALID;
    });
}

// The VDP keeps no project state of its own: its configuration lives in the
// card record and the engine owns VRAM persistence through save_state.  A
// project chunk is declared anyway so the ABI shape is stable for phase 2,
// which adds the VRAM image slots.
SrhStatus SRH_CALL save_project_data(void *, uint8_t *buffer, uint64_t *size) {
    if (!size)
        return SRH_INVALID;
    if (!buffer) {
        *size = 0;
        return SRH_OK;
    }
    *size = 0;
    return SRH_OK;
}

SrhStatus SRH_CALL load_project_data(void *, const uint8_t *, uint64_t size) {
    return size == 0 ? SRH_OK : SRH_INVALID;
}

const SrhCardDescriptor descriptor{
    SRH_INIT(SrhCardDescriptor),
    "Video",
    "V9938 VDP",
    "Yamaha V9938 / V9958 video display processor",
    0x98,           // default IO base: the MSX VDP port block
    port_count,     // 0x98-0x9B
    0,              // no reset vector
    0,              // default priority
    0,              // no clock subscription; the raster is card-scheduled
    SRH_CARD_REQUIRES_IO_SPACE,
    R"({"io_space":"cpu0.io","model":0,"vram_size":131072,"raster_clock_hz":21477272})",
    "io_space",
    nullptr,
    nullptr,
    0,
};

const SrhPlugin api{SRH_INIT(SrhPlugin),
                    "vdp",
                    create,
                    destroy,
                    reset,
                    property_count,
                    property_info,
                    property_get,
                    property_set,
                    save_state,
                    load_state,
                    &descriptor,
                    save_project_data,
                    load_project_data};

} // namespace

extern "C" SRH_EXPORT const SrhPlugin *SRH_CALL srz80_plugin_init(const ShouryoHost *host) {
    return srz80::sdk::valid(host) ? &api : nullptr;
}
