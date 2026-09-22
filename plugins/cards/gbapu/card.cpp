#include <state.hpp>
#include "gb_state.hpp"
#include <boundary.hpp>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
extern "C" {
#include "gb.h"
}
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace {
constexpr uint64_t gb_clock_rate = 0x400000; // 4.194304 MHz GB CPU clock
constexpr uint64_t gbapu_register_count = 0x30; // NR10..NR52 plus wave RAM
constexpr uint32_t gbapu_default_sample_rate = 44100;

struct Settings {
    uint32_t sample_rate = gbapu_default_sample_rate;
    std::string stream_name = "GB APU";
    GB_model_t model = GB_MODEL_DMG_B;
    GB_highpass_mode_t highpass_mode = GB_HIGHPASS_OFF;
};

class Json {
  public:
    explicit Json(const SrhConfig *config)
        : text_(config && srz80::sdk::has_field(config, &SrhConfig::config_json) && config->config_json
                    ? std::string(config->config_json, config->config_json_size)
                    : "{}") {}

    bool parse(Settings &settings) {
        if (!take('{'))
            return false;
        skip();
        if (take('}'))
            return done();
        while (true) {
            std::string key, value;
            bool quoted = false;
            if (!string(key) || !take(':') || !scalar(value, quoted))
                return false;
            if (key == "sample_rate") {
                uint64_t number = 0;
                if (!unsigned_value(value, quoted, number) || number < 8000 || number > 192000)
                    return false;
                settings.sample_rate = static_cast<uint32_t>(number);
            } else if (key == "stream_name") {
                if (!quoted || value.empty() || value.size() > 256)
                    return false;
                settings.stream_name = value;
            } else if (key == "model") {
                if (!quoted)
                    return false;
                if (value == "dmg")
                    settings.model = GB_MODEL_DMG_B;
                else if (value == "cgb")
                    settings.model = GB_MODEL_CGB_E;
                else if (value == "agb")
                    settings.model = GB_MODEL_AGB_NATIVE;
                else
                    return false;
            } else if (key == "highpass_mode") {
                if (!quoted)
                    return false;
                if (value == "off")
                    settings.highpass_mode = GB_HIGHPASS_OFF;
                else if (value == "accurate")
                    settings.highpass_mode = GB_HIGHPASS_ACCURATE;
                else if (value == "remove_dc")
                    settings.highpass_mode = GB_HIGHPASS_REMOVE_DC_OFFSET;
                else
                    return false;
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
        while (position_ < text_.size() && (text_[position_] == ' ' || text_[position_] == '\t' ||
                                            text_[position_] == '\r' || text_[position_] == '\n'))
            ++position_;
    }
    bool take(char expected) {
        skip();
        if (position_ >= text_.size() || text_[position_] != expected)
            return false;
        ++position_;
        return true;
    }
    bool string(std::string &out) {
        skip();
        if (position_ >= text_.size() || text_[position_++] != '"')
            return false;
        while (position_ < text_.size() && text_[position_] != '"') {
            if (text_[position_] == '\\' || static_cast<unsigned char>(text_[position_]) < 0x20)
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
        while (position_ < text_.size() && text_[position_] != ',' && text_[position_] != '}' &&
               text_[position_] != ' ' && text_[position_] != '\t' && text_[position_] != '\r' &&
               text_[position_] != '\n')
            ++position_;
        if (first == position_)
            return false;
        out.assign(text_, first, position_ - first);
        return true;
    }
    static bool unsigned_value(const std::string &value, bool quoted, uint64_t &out) {
        auto digits = value;
        int base = 10;
        if (quoted && digits.size() > 2 && digits[0] == '0' &&
            (digits[1] == 'x' || digits[1] == 'X')) {
            base = 16;
            digits.erase(0, 2);
        }
        if (digits.empty())
            return false;
        out = 0;
        for (char c : digits) {
            unsigned digit = c >= '0' && c <= '9' ? static_cast<unsigned>(c - '0')
                            : c >= 'a' && c <= 'f' ? static_cast<unsigned>(c - 'a' + 10)
                            : c >= 'A' && c <= 'F' ? static_cast<unsigned>(c - 'A' + 10)
                                                  : 99;
            if (digit >= static_cast<unsigned>(base) || out > (UINT64_MAX - digit) / base)
                return false;
            out = out * static_cast<unsigned>(base) + digit;
        }
        return true;
    }
    bool done() {
        skip();
        return position_ == text_.size();
    }
    std::string text_;
    size_t position_ = 0;
};

struct GbApu {
    const ShouryoHost *host = nullptr;
    SrhHandle owner = 0, space = 0, mapping = 0, stream = 0;
    uint64_t base = 0;
    uint32_t sample_rate = gbapu_default_sample_rate;
    int32_t priority = 0;
    GB_model_t model = GB_MODEL_DMG_B;
    GB_highpass_mode_t highpass_mode = GB_HIGHPASS_OFF;
    std::string stream_name = "GB APU";
    std::unique_ptr<GB_gameboy_t> gb;
    uint64_t cycle_accum = 0;

    void reset() {
        std::memset(gb.get(), 0, sizeof(GB_gameboy_t));
        gb->model = model;
        GB_apu_init(gb.get());
        GB_set_sample_rate(gb.get(), sample_rate);
        GB_set_highpass_filter_mode(gb.get(), highpass_mode);
        cycle_accum = 0;
    }

    SrhStatus read(uint64_t address, uint8_t *value) {
        if (!value || address < base || address - base >= gbapu_register_count)
            return SRH_INVALID;
        *value = GB_apu_read(gb.get(), static_cast<uint8_t>(0x10 + (address - base)));
        return SRH_OK;
    }

    SrhStatus write(uint64_t address, uint8_t value) {
        if (address < base || address - base >= gbapu_register_count)
            return SRH_INVALID;
        GB_apu_write(gb.get(), static_cast<uint8_t>(0x10 + (address - base)), value);
        return SRH_OK;
    }
};

SrhStatus SRH_CALL read(void *context, uint64_t address, uint8_t *value) {
    return static_cast<GbApu *>(context)->read(address, value);
}
SrhStatus SRH_CALL write(void *context, uint64_t address, uint8_t value) {
    return static_cast<GbApu *>(context)->write(address, value);
}
SrhStatus SRH_CALL render(void *context, uint64_t, uint32_t frames, int16_t *interleaved) {
    if (!interleaved)
        return SRH_INVALID;
    auto &apu = *static_cast<GbApu *>(context);
    for (uint32_t frame = 0; frame < frames; ++frame) {
        // Advance the APU by an exact average of GB_CLOCK_RATE / sample_rate
        // CPU cycles per output sample.  GB_advance_cycles runs only the APU
        // and timer state machines, never the GB CPU.
        apu.cycle_accum += gb_clock_rate;
        const uint8_t cycles = static_cast<uint8_t>(apu.cycle_accum / apu.sample_rate);
        apu.cycle_accum %= apu.sample_rate;
        GB_advance_cycles(apu.gb.get(), cycles);
        const auto sample = apu.gb->apu_output.final_sample;
        interleaved[static_cast<size_t>(frame) * 2] = static_cast<int16_t>(
            std::clamp(static_cast<int32_t>(sample.left), static_cast<int32_t>(INT16_MIN),
                       static_cast<int32_t>(INT16_MAX)));
        interleaved[static_cast<size_t>(frame) * 2 + 1] = static_cast<int16_t>(
            std::clamp(static_cast<int32_t>(sample.right), static_cast<int32_t>(INT16_MIN),
                       static_cast<int32_t>(INT16_MAX)));
    }
    return SRH_OK;
}
SrhStatus SRH_CALL create(const ShouryoHost *host, SrhHandle owner, const SrhConfig *config,
                          void **result) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!srz80::sdk::valid(host) || !srz80::sdk::valid(config) || !result || !config->space ||
            config->size != gbapu_register_count || config->base > UINT64_MAX - (gbapu_register_count - 1))
            return SRH_INVALID;
        Settings settings;
        if (!Json(config).parse(settings))
            return SRH_INVALID;
        const void *extension = nullptr;
        if (!host->query || host->query(host->context, "host.audio.v1", &extension) != SRH_OK ||
            !extension)
            return SRH_UNAVAILABLE;
        auto audio = static_cast<const SrhHostAudioV1 *>(extension);
        if (!srz80::sdk::valid(audio) || !audio->register_source)
            return SRH_UNAVAILABLE;
        auto apu = std::make_unique<GbApu>();
        apu->host = host;
        apu->owner = owner;
        apu->space = config->space;
        apu->base = config->base;
        apu->sample_rate = settings.sample_rate;
        apu->priority = config->priority;
        apu->model = settings.model;
        apu->highpass_mode = settings.highpass_mode;
        apu->stream_name = settings.stream_name;
        apu->gb = std::make_unique<GB_gameboy_t>();
        apu->reset();
        SrhMapping mapping{SRH_INIT(SrhMapping),
                           config->space,
                           config->base,
                           config->base + gbapu_register_count - 1,
                           config->priority,
                           apu.get(),
                           read,
                           write,
                           read};
        auto status = host->map(host->context, owner, &mapping, &apu->mapping);
        if (status != SRH_OK)
            return status;
        status = audio->register_source(audio->context, owner, settings.sample_rate, 2,
                                        SRH_AUDIO_S16_STEREO, settings.stream_name.c_str(), render,
                                        apu.get(), &apu->stream);
        if (status != SRH_OK)
            return status;
        *result = apu.release();
        return SRH_OK;
    });
}
void SRH_CALL destroy(void *context) {
    delete static_cast<GbApu *>(context);
}
SrhStatus SRH_CALL reset(void *context, uint32_t) {
    static_cast<GbApu *>(context)->reset();
    return SRH_OK;
}
uint32_t SRH_CALL count(void *) { return 4; }
SrhStatus SRH_CALL info(void *, uint32_t index, SrhProperty *out) {
    if (!srz80::sdk::valid(out) || index >= 4)
        return SRH_INVALID;
    static const char *names[] = {"base", "sample_rate", "model", "highpass_mode"};
    static const char *descriptions[] = {"First mapped APU register (NR10)", "Native output rate",
                                         "GB APU model (DMG, CGB, or AGB)", "Output high-pass filter"};
    static const uint32_t kinds[] = {SRH_UNSIGNED, SRH_UNSIGNED, SRH_ENUM, SRH_ENUM};
    static const uint32_t bits[] = {16, 32, 0, 0};
    static const uint32_t bases[] = {16, 10, 0, 0};
    static const char *enum_labels[] = {nullptr, nullptr, "dmg|cgb|agb", "off|accurate|remove_dc"};
    *out = {SRH_INIT(SrhProperty), names[index], "GB APU", descriptions[index], kinds[index],
            bits[index], bases[index], 1, enum_labels[index]};
    return SRH_OK;
}
SrhStatus SRH_CALL get(void *context, uint32_t index, SrhValue *out) {
    if (!srz80::sdk::valid(out) || index >= 4)
        return SRH_INVALID;
    auto &apu = *static_cast<GbApu *>(context);
    switch (index) {
    case 0:
        out->unsigned_value = apu.base;
        break;
    case 1:
        out->unsigned_value = apu.sample_rate;
        break;
    case 2:
        out->unsigned_value = apu.model == GB_MODEL_DMG_B ? 0 : apu.model == GB_MODEL_CGB_E ? 1 : 2;
        break;
    case 3:
        out->unsigned_value = static_cast<uint32_t>(apu.highpass_mode);
        break;
    }
    return SRH_OK;
}
SrhStatus SRH_CALL set(void *context, uint32_t index, const SrhValue *in) {
    if (!srz80::sdk::valid(in) || index >= 4)
        return SRH_INVALID;
    auto &apu = *static_cast<GbApu *>(context);
    switch (index) {
    case 0: {
        if (in->unsigned_value > UINT64_MAX - (gbapu_register_count - 1))
            return SRH_INVALID;
        if (in->unsigned_value == apu.base)
            return SRH_OK;
        SrhMapping mapping{SRH_INIT(SrhMapping),
                           apu.space,
                           in->unsigned_value,
                           in->unsigned_value + gbapu_register_count - 1,
                           apu.priority,
                           &apu,
                           read,
                           write,
                           read};
        SrhHandle new_mapping = 0;
        auto status = apu.host->map(apu.host->context, apu.owner, &mapping, &new_mapping);
        if (status != SRH_OK)
            return status;
        if (apu.host->unmap(apu.host->context, apu.mapping) != SRH_OK) {
            apu.host->unmap(apu.host->context, new_mapping);
            return SRH_INVALID;
        }
        apu.base = in->unsigned_value;
        apu.mapping = new_mapping;
        return SRH_OK;
    }
    case 1:
        if (in->unsigned_value < 8000 || in->unsigned_value > 192000 ||
            in->unsigned_value != apu.sample_rate)
            return SRH_INVALID;
        apu.sample_rate = static_cast<uint32_t>(in->unsigned_value);
        GB_set_sample_rate(apu.gb.get(), apu.sample_rate);
        return SRH_OK;
    case 2:
        if (in->unsigned_value > 2)
            return SRH_INVALID;
        apu.model = in->unsigned_value == 0 ? GB_MODEL_DMG_B
                   : in->unsigned_value == 1 ? GB_MODEL_CGB_E
                                             : GB_MODEL_AGB_NATIVE;
        apu.gb->model = apu.model;
        return SRH_OK;
    case 3:
        if (in->unsigned_value >= GB_HIGHPASS_MAX)
            return SRH_INVALID;
        apu.highpass_mode = static_cast<GB_highpass_mode_t>(in->unsigned_value);
        GB_set_highpass_filter_mode(apu.gb.get(), apu.highpass_mode);
        return SRH_OK;
    }
    return SRH_INVALID;
}
SrhStatus SRH_CALL save_payload(void *context, uint8_t *buffer, uint64_t *size) {
    auto &apu = *static_cast<GbApu *>(context);
    auto staged = *apu.gb;
    srz80::sdk::state::Writer writer;
    writer(apu.cycle_accum);
    archive_gb(writer, staged);
    return srz80::sdk::state::copy_payload(writer.bytes, buffer, size);
}
SrhStatus SRH_CALL load_payload(void *context, const uint8_t *buffer, uint64_t size) {
    auto &apu = *static_cast<GbApu *>(context);
    auto staged = *apu.gb;
    uint64_t accumulator = 0;
    srz80::sdk::state::Reader reader({buffer, static_cast<size_t>(size)});
    reader(accumulator);
    archive_gb(reader, staged);
    if (!reader.finished() || accumulator >= apu.sample_rate || staged.model != apu.model ||
        staged.apu_output.sample_rate != apu.sample_rate ||
        staged.apu_output.highpass_mode != apu.highpass_mode || !valid_gb_state(staged))
        return SRH_INVALID;
    *apu.gb = staged;
    apu.cycle_accum = accumulator;
    return SRH_OK;
}

const SrhCardDescriptor descriptor{SRH_INIT(SrhCardDescriptor), "Audio", "Game Boy APU",
                                   "Standalone Game Boy audio processing unit", 0xFF10, 0x30, 0,
                                   0, 0, 0,
                                   R"({"sample_rate":44100,"stream_name":"GB APU","model":"dmg","highpass_mode":"off"})",
                                   nullptr, nullptr};
using State = srz80::sdk::state::Callbacks<save_payload, load_payload, 1>;
const SrhPlugin api{SRH_INIT(SrhPlugin), "gbapu", create, destroy, reset, count, info, get, set,
                    State::save, State::load, &descriptor};
} // namespace

extern "C" SRH_EXPORT const SrhPlugin *SRH_CALL srz80_plugin_init(const ShouryoHost *host) {
    return srz80::sdk::valid(host) ? &api : nullptr;
}
