#include <boundary.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <string>

#include "SID.h"
#include "dsid.h"

namespace {
constexpr uint32_t kRegisterCount = 0x20;
constexpr uint32_t kUsedRegisterCount = 0x1D;
constexpr uint32_t kDefaultChipClockHz = 985248; // PAL C64 clock
constexpr uint32_t kDefaultSampleRate = 44100;
constexpr uint32_t kMinSampleRate = 8000;
constexpr uint32_t kMaxSampleRate = 192000;
constexpr uint32_t kMinChipClockHz = 100000;
constexpr uint32_t kMaxChipClockHz = 10000000;

enum class Backend : uint32_t { deflemask, resid };

Backend configured_backend(const ShouryoHost *host) {
    const void *extension = nullptr;
    if (!host || !host->query || host->query(host->context, "host.config.v1", &extension) != SRH_OK ||
        !extension)
        return Backend::resid;
    const auto config = static_cast<const SrhHostConfigV1 *>(extension);
    if (!srz80::sdk::valid(config) || !config->get_value)
        return Backend::resid;
    char value[32]{};
    if (config->get_value(config->context, "cards.sid.backend", value,
                          static_cast<uint32_t>(sizeof(value))) != SRH_OK)
        return Backend::resid;
    if (std::strcmp(value, "Deflemask") == 0)
        return Backend::deflemask;
    if (std::strcmp(value, "resid") == 0)
        return Backend::resid;
    return Backend::resid;
}

struct Settings {
    uint32_t chip_clock_hz = kDefaultChipClockHz;
    uint32_t sample_rate = kDefaultSampleRate;
    reSIDfp::ChipModel model = reSIDfp::MOS8580;
    std::string stream_name = "SID";
};

bool valid_sampling_parameters(uint32_t chip_clock_hz, uint32_t sample_rate) {
    // reSID's fixed-point resamplers require at least one SID clock per output
    // sample. Normal SID clocks are many times faster than the audio rate.
    return chip_clock_hz >= sample_rate;
}

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
            if (key == "chip_clock_hz") {
                uint64_t number = 0;
                if (!unsigned_value(value, quoted, number) || number < kMinChipClockHz ||
                    number > kMaxChipClockHz)
                    return false;
                settings.chip_clock_hz = static_cast<uint32_t>(number);
            } else if (key == "sample_rate") {
                uint64_t number = 0;
                if (!unsigned_value(value, quoted, number) || number < kMinSampleRate ||
                    number > kMaxSampleRate)
                    return false;
                settings.sample_rate = static_cast<uint32_t>(number);
            } else if (key == "model") {
                if (!quoted)
                    return false;
                if (value == "6581")
                    settings.model = reSIDfp::MOS6581;
                else if (value == "8580")
                    settings.model = reSIDfp::MOS8580;
                else
                    return false;
            } else if (key == "stream_name") {
                if (!quoted || value.empty() || value.size() > 256)
                    return false;
                settings.stream_name = value;
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

struct SidCard {
    const ShouryoHost *host = nullptr;
    const SrhHostConfigV1 *host_config = nullptr;
    SrhHandle owner = 0, space = 0, mapping = 0, stream = 0;
    uint64_t base = 0;
    uint32_t chip_clock_hz = kDefaultChipClockHz;
    uint32_t sample_rate = kDefaultSampleRate;
    int32_t priority = 0;
    reSIDfp::ChipModel model = reSIDfp::MOS8580;
    Backend backend = Backend::resid;
    std::string stream_name = "SID";
    std::unique_ptr<reSIDfp::SID> sid;
    SID_chip dsid{};
    std::array<uint8_t, kRegisterCount> registers{};
    std::deque<int16_t> pending_samples;
    uint64_t cycle_accum = 0;
    int16_t last_sample = 0;

    void configure_sampling() {
        if (backend == Backend::resid) {
            const auto accurate_frequency = std::min(20000.0, 0.45 * sample_rate);
            sid->setSamplingParameters(static_cast<double>(chip_clock_hz), reSIDfp::RESAMPLE,
                                       static_cast<double>(sample_rate), accurate_frequency);
        }
        pending_samples.clear();
        cycle_accum = 0;
        last_sample = 0;
    }

    void initialise_engine() {
        if (backend == Backend::resid) {
            sid = std::make_unique<reSIDfp::SID>();
            sid->setChipModel(model);
            configure_sampling();
        } else {
            sid.reset();
            std::memset(&dsid, 0, sizeof(dsid));
            dSID_init(&dsid, static_cast<double>(chip_clock_hz), static_cast<double>(sample_rate),
                      model == reSIDfp::MOS6581 ? 6581 : 8580, 1);
            dSID_setMuteMask(&dsid, 0x07);
            configure_sampling();
        }
    }

    void restore_registers() {
        for (uint32_t reg = 0; reg < kUsedRegisterCount; ++reg) {
            if (backend == Backend::resid)
                sid->write(static_cast<int>(reg), registers[reg]);
            else
                dSID_write(&dsid, static_cast<unsigned char>(reg), registers[reg]);
        }
    }

    void reset() {
        registers.fill(0);
        initialise_engine();
    }

    void change_engine() {
        initialise_engine();
        restore_registers();
    }

    SrhStatus read(uint64_t address, uint8_t *value) {
        if (!value || address < base || address - base >= kRegisterCount)
            return SRH_INVALID;
        const auto offset = static_cast<uint32_t>(address - base);
        *value = backend == Backend::resid ? sid->read(static_cast<int>(offset)) : registers[offset];
        return SRH_OK;
    }

    SrhStatus write(uint64_t address, uint8_t value) {
        if (address < base || address - base >= kRegisterCount)
            return SRH_INVALID;
        const auto offset = static_cast<uint32_t>(address - base);
        registers[offset] = value;
        if (offset < kUsedRegisterCount) {
            if (backend == Backend::resid)
                sid->write(static_cast<int>(offset), value);
            else
                dSID_write(&dsid, static_cast<unsigned char>(offset), value);
        }
        return SRH_OK;
    }
};

SrhStatus SRH_CALL backend_config_get(void *context, char *value, uint32_t capacity) {
    auto *card = static_cast<SidCard *>(context);
    if (!card || !card->host_config || !card->host_config->get_value || !value || !capacity)
        return SRH_INVALID;
    return card->host_config->get_value(card->host_config->context, "cards.sid.backend", value,
                                        capacity);
}

SrhStatus SRH_CALL backend_config_set(void *, const char *value) {
    if (!value || (std::strcmp(value, "Deflemask") != 0 && std::strcmp(value, "resid") != 0))
        return SRH_INVALID;
    return SRH_OK;
}

SrhStatus SRH_CALL read(void *context, uint64_t address, uint8_t *value) {
    return static_cast<SidCard *>(context)->read(address, value);
}
SrhStatus SRH_CALL write(void *context, uint64_t address, uint8_t value) {
    return static_cast<SidCard *>(context)->write(address, value);
}
SrhStatus SRH_CALL render(void *context, uint64_t, uint32_t frames, int16_t *interleaved) {
    if (!interleaved)
        return SRH_INVALID;
    auto &card = *static_cast<SidCard *>(context);
    for (uint32_t frame = 0; frame < frames; ++frame) {
        if (card.backend == Backend::resid) {
            std::array<short, 4> generated{};
            card.cycle_accum += card.chip_clock_hz;
            const auto cycles = static_cast<uint32_t>(card.cycle_accum / card.sample_rate);
            card.cycle_accum %= card.sample_rate;
            const auto produced = card.sid->clock(cycles, generated.data());
            for (int i = 0; i < produced; ++i)
                card.pending_samples.push_back(generated[static_cast<size_t>(i)]);
            if (!card.pending_samples.empty()) {
                card.last_sample = card.pending_samples.front();
                card.pending_samples.pop_front();
            }
        } else {
            const auto sample = dSID_render(&card.dsid) * 32767.0;
            card.last_sample = static_cast<int16_t>(std::clamp(
                sample, static_cast<double>(std::numeric_limits<int16_t>::min()),
                static_cast<double>(std::numeric_limits<int16_t>::max())));
        }
        interleaved[static_cast<size_t>(frame) * 2] = card.last_sample;
        interleaved[static_cast<size_t>(frame) * 2 + 1] = card.last_sample;
    }
    return SRH_OK;
}

SrhStatus SRH_CALL create(const ShouryoHost *host, SrhHandle owner, const SrhConfig *config,
                          void **result) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!srz80::sdk::valid(host) || !srz80::sdk::valid(config) || !result || !config->space ||
            config->size != kRegisterCount || config->base > UINT64_MAX - (kRegisterCount - 1))
            return SRH_INVALID;
        Settings settings;
        if (!Json(config).parse(settings))
            return SRH_INVALID;
        if (!valid_sampling_parameters(settings.chip_clock_hz, settings.sample_rate))
            return SRH_INVALID;
        const void *extension = nullptr;
        if (!host->query || host->query(host->context, "host.audio.v1", &extension) != SRH_OK ||
            !extension)
            return SRH_UNAVAILABLE;
        const auto audio = static_cast<const SrhHostAudioV1 *>(extension);
        if (!srz80::sdk::valid(audio) || !audio->register_source)
            return SRH_UNAVAILABLE;

        auto card = std::make_unique<SidCard>();
        card->host = host;
        card->owner = owner;
        card->space = config->space;
        card->base = config->base;
        card->chip_clock_hz = settings.chip_clock_hz;
        card->sample_rate = settings.sample_rate;
        card->priority = config->priority;
        card->model = settings.model;
        card->backend = configured_backend(host);
        card->stream_name = settings.stream_name;
        card->reset();

        extension = nullptr;
        if (host->query(host->context, "host.config.v1", &extension) == SRH_OK && extension) {
            const auto host_config = static_cast<const SrhHostConfigV1 *>(extension);
            if (srz80::sdk::valid(host_config) && host_config->register_entry &&
                host_config->get_value) {
                card->host_config = host_config;
                const SrhConfigEntry entry{
                    SRH_INIT(SrhConfigEntry),
                    "Cards/SID",
                    "cards.sid.backend",
                    "Backend",
                    "SID emulation engine. Used for new cards and newly opened projects",
                    Srh_CONFIG_ENUM,
                    "Deflemask|resid",
                    "resid",
                    card.get(),
                    owner,
                    backend_config_get,
                    backend_config_set};
                const auto config_status =
                    host_config->register_entry(host_config->context, &entry);
                if (config_status != SRH_OK)
                    return config_status;
            }
        }

        SrhMapping mapping{SRH_INIT(SrhMapping), config->space, config->base,
                           config->base + kRegisterCount - 1, config->priority, card.get(), read, write,
                           read};
        auto status = host->map(host->context, owner, &mapping, &card->mapping);
        if (status != SRH_OK)
            return status;
        status = audio->register_source(audio->context, owner, settings.sample_rate, 2,
                                        SRH_AUDIO_S16_STEREO, settings.stream_name.c_str(), render,
                                        card.get(), &card->stream);
        if (status != SRH_OK)
            return status;
        *result = card.release();
        return SRH_OK;
    });
}

void SRH_CALL destroy(void *context) { delete static_cast<SidCard *>(context); }

SrhStatus SRH_CALL reset(void *context, uint32_t) {
    static_cast<SidCard *>(context)->reset();
    return SRH_OK;
}

constexpr uint32_t kConfigCount = 4;
constexpr uint32_t kPropertyCount = kConfigCount + kRegisterCount;

uint32_t SRH_CALL count(void *) { return kPropertyCount; }

SrhStatus SRH_CALL info(void *, uint32_t index, SrhProperty *out) {
    if (!srz80::sdk::valid(out) || index >= kPropertyCount)
        return SRH_INVALID;
    static const char *config_names[] = {"base", "chip_clock_hz", "sample_rate", "model"};
    static const char *config_descriptions[] = {"First mapped SID register", "SID input clock",
                                                "Native output rate", "SID chip model"};
    if (index < kConfigCount) {
        const bool model = index == 3;
        *out = {SRH_INIT(SrhProperty), config_names[index], "SID", config_descriptions[index],
                static_cast<uint32_t>(model ? SRH_ENUM : SRH_UNSIGNED), model ? 0u : index == 0 ? 16u : 32u,
                model ? 0u : index == 0 ? 16u : 10u, 1, model ? "6581|8580" : nullptr,
                static_cast<uint32_t>(index >= 1 ? SRH_PROPERTY_PERSISTENT : SRH_PROPERTY_RUNTIME)};
        return SRH_OK;
    }
    const auto reg = index - kConfigCount;
    static thread_local char name[8];
    static thread_local char description[64];
    const auto register_number = static_cast<unsigned char>(reg);
    std::snprintf(name, sizeof(name), "R%02X", register_number);
    std::snprintf(description, sizeof(description), "SID register $%02X", register_number);
    *out = {SRH_INIT(SrhProperty), name, "Registers", description, SRH_UNSIGNED, 8, 16, 1,
            nullptr, SRH_PROPERTY_HIDE_UI | SRH_PROPERTY_LIVE_EDIT | SRH_PROPERTY_RUNTIME};
    return SRH_OK;
}

SrhStatus SRH_CALL get(void *context, uint32_t index, SrhValue *out) {
    if (!srz80::sdk::valid(out) || index >= kPropertyCount)
        return SRH_INVALID;
    auto &card = *static_cast<SidCard *>(context);
    if (index == 0)
        out->unsigned_value = card.base;
    else if (index == 1)
        out->unsigned_value = card.chip_clock_hz;
    else if (index == 2)
        out->unsigned_value = card.sample_rate;
    else if (index == 3)
        out->unsigned_value = card.model == reSIDfp::MOS6581 ? 0 : 1;
    else
        out->unsigned_value = card.registers[index - kConfigCount];
    return SRH_OK;
}

SrhStatus set_impl(void *context, uint32_t index, const SrhValue *in) {
    if (!srz80::sdk::valid(in) || index >= kPropertyCount)
        return SRH_INVALID;
    auto &card = *static_cast<SidCard *>(context);
    if (index == 0) {
        if (in->unsigned_value > UINT64_MAX - (kRegisterCount - 1) ||
            in->unsigned_value == card.base)
            return in->unsigned_value == card.base ? SRH_OK : SRH_INVALID;
        SrhMapping mapping{SRH_INIT(SrhMapping), card.space, in->unsigned_value,
                           in->unsigned_value + kRegisterCount - 1, card.priority, &card, read, write,
                           read};
        SrhHandle new_mapping = 0;
        auto status = card.host->map(card.host->context, card.owner, &mapping, &new_mapping);
        if (status != SRH_OK)
            return status;
        if (card.host->unmap(card.host->context, card.mapping) != SRH_OK) {
            card.host->unmap(card.host->context, new_mapping);
            return SRH_INVALID;
        }
        card.base = in->unsigned_value;
        card.mapping = new_mapping;
        return SRH_OK;
    }
    if (index == 1) {
        if (in->unsigned_value < kMinChipClockHz || in->unsigned_value > kMaxChipClockHz ||
            in->unsigned_value < card.sample_rate)
            return SRH_INVALID;
        card.chip_clock_hz = static_cast<uint32_t>(in->unsigned_value);
        card.change_engine();
        return SRH_OK;
    }
    if (index == 2) {
        if (in->unsigned_value < kMinSampleRate || in->unsigned_value > kMaxSampleRate ||
            in->unsigned_value != card.sample_rate || in->unsigned_value > card.chip_clock_hz)
            return SRH_INVALID;
        card.sample_rate = static_cast<uint32_t>(in->unsigned_value);
        card.change_engine();
        return SRH_OK;
    }
    if (index == 3) {
        if (in->unsigned_value > 1)
            return SRH_INVALID;
        card.model = in->unsigned_value == 0 ? reSIDfp::MOS6581 : reSIDfp::MOS8580;
        card.change_engine();
        return SRH_OK;
    }
    const auto reg = index - kConfigCount;
    if (in->unsigned_value > 0xFF)
        return SRH_INVALID;
    card.registers[reg] = static_cast<uint8_t>(in->unsigned_value);
    if (reg < kUsedRegisterCount) {
        if (card.backend == Backend::resid)
            card.sid->write(static_cast<int>(reg), card.registers[reg]);
        else
            dSID_write(&card.dsid, static_cast<unsigned char>(reg), card.registers[reg]);
    }
    return SRH_OK;
}

SrhStatus SRH_CALL set(void *context, uint32_t index, const SrhValue *in) {
    return srz80::sdk::guard([&]() { return set_impl(context, index, in); });
}

SrhStatus SRH_CALL save_state(void *context, uint8_t *buffer, uint64_t *size) {
    if (!size)
        return SRH_INVALID;
    constexpr uint64_t required = kRegisterCount + sizeof(uint64_t);
    if (!buffer) {
        *size = required;
        return SRH_OK;
    }
    if (*size < required) {
        *size = required;
        return SRH_UNAVAILABLE;
    }
    auto &card = *static_cast<SidCard *>(context);
    std::memcpy(buffer, card.registers.data(), kRegisterCount);
    std::memcpy(buffer + kRegisterCount, &card.cycle_accum, sizeof(card.cycle_accum));
    *size = required;
    return SRH_OK;
}

SrhStatus SRH_CALL load_state(void *context, const uint8_t *buffer, uint64_t size) {
    constexpr uint64_t required = kRegisterCount + sizeof(uint64_t);
    if (!buffer || size != required)
        return SRH_INVALID;
    auto &card = *static_cast<SidCard *>(context);
    card.reset();
    std::memcpy(card.registers.data(), buffer, kRegisterCount);
    std::memcpy(&card.cycle_accum, buffer + kRegisterCount, sizeof(card.cycle_accum));
    card.restore_registers();
    return SRH_OK;
}

const SrhCardDescriptor descriptor{SRH_INIT(SrhCardDescriptor), "Audio", "SID",
                                   "MOS 6581/8580 three-voice synthesizer", 0xD400, 0x20, 0, 0, 0,
                                   0,
                                   R"({"chip_clock_hz":985248,"sample_rate":44100,"model":"8580","stream_name":"SID"})",
                                   nullptr, nullptr};
const SrhPlugin api{SRH_INIT(SrhPlugin), "sid", create, destroy, reset, count, info, get, set,
                    save_state, load_state, &descriptor};
} // namespace

extern "C" SRH_EXPORT const SrhPlugin *SRH_CALL srz80_plugin_init(const ShouryoHost *host) {
    return srz80::sdk::valid(host) ? &api : nullptr;
}
