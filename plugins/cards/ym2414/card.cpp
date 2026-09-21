#include <boundary.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <memory>
#include <string>

#include "engine.h"

namespace {

constexpr uint32_t kMinChipClockHz = 1000000;
constexpr uint32_t kMaxChipClockHz = 50000000;
constexpr uint32_t kMinSampleRate = 8000;
constexpr uint32_t kMaxSampleRate = 192000;
constexpr uint32_t kPortCount = 2; // address latch + data

using srz80::ym2414::Backend;
using srz80::ym2414::Engine;

Backend configured_backend(const ShouryoHost *host) {
    const void *extension = nullptr;
    if (!host || !host->query || host->query(host->context, "host.config.v1", &extension) != SRH_OK ||
        !extension)
        return Backend::srz80;
    const auto config = static_cast<const SrhHostConfigV1 *>(extension);
    if (!srz80::sdk::valid(config) || !config->get_value)
        return Backend::srz80;
    char value[32]{};
    if (config->get_value(config->context, "cards.ym2414.backend", value,
                          static_cast<uint32_t>(sizeof(value))) != SRH_OK)
        return Backend::srz80;
    return std::strcmp(value, "ymfm") == 0 ? Backend::ymfm : Backend::srz80;
}

struct Settings {
    uint32_t chip_clock_hz = 3579545;
    uint32_t sample_rate = 44100;
    std::string stream_name = "YM2414";
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
            uint64_t number = 0;
            if (key == "chip_clock_hz") {
                if (!unsigned_value(value, quoted, number) || number < kMinChipClockHz ||
                    number > kMaxChipClockHz)
                    return false;
                settings.chip_clock_hz = static_cast<uint32_t>(number);
            } else if (key == "sample_rate") {
                if (!unsigned_value(value, quoted, number) || number < kMinSampleRate ||
                    number > kMaxSampleRate)
                    return false;
                settings.sample_rate = static_cast<uint32_t>(number);
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
        while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9')
            ++position_;
        if (first == position_)
            return false;
        out.assign(text_, first, position_ - first);
        return true;
    }
    static bool unsigned_value(const std::string &value, bool quoted, uint64_t &out) {
        auto digits = value;
        unsigned base = 10;
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
            if (digit >= base || out > (UINT64_MAX - digit) / base)
                return false;
            out = out * base + digit;
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

struct Ym2414 {
    const ShouryoHost *host = nullptr;
    const SrhHostConfigV1 *host_config = nullptr;
    SrhHandle owner = 0;
    SrhHandle space = 0;
    uint64_t base = 0;
    int32_t priority = 0;
    uint32_t chip_clock_hz = 0;
    uint32_t sample_rate = 0;
    Backend backend = Backend::srz80;
    uint8_t address = 0;
    uint8_t registers[Engine::kRegisters]{};
    std::unique_ptr<Engine> chip;
    uint64_t sample_phase = 0;

    void reset() {
        address = 0;
        std::fill(std::begin(registers), std::end(registers), 0);
        if (chip) {
            chip->reset();
            sample_phase = 0;
        }
    }

    void apply_register(uint8_t index, uint8_t value) {
        chip->write_address(index);
        chip->write_data(value);
    }

    SrhStatus read(uint64_t port, uint8_t *value) {
        if (!value || port < base || port - base >= kPortCount)
            return SRH_INVALID;
        *value = (port - base) == 0 ? 0 : chip->read_status();
        return SRH_OK;
    }

    SrhStatus write(uint64_t port, uint8_t value) {
        if (port < base || port - base >= kPortCount)
            return SRH_INVALID;
        if (port == base) {
            address = value;
            chip->write_address(value);
        } else {
            registers[address] = value;
            chip->write_address(address);
            chip->write_data(value);
        }
        return SRH_OK;
    }
};

constexpr uint32_t kConfigCount = 3;
constexpr uint32_t kRawStart = kConfigCount;
constexpr uint32_t kRawCount = Engine::kRegisters;
constexpr uint32_t kInterfaceStart = kRawStart + kRawCount;
constexpr uint32_t kInterfaceCount = 2; // Address latch, Data write
constexpr uint32_t kOperatorStart = kInterfaceStart + kInterfaceCount;
constexpr uint32_t kOperatorCount = Engine::kOperators;
constexpr uint32_t kOperatorFieldCount = 5; // Env, Out, Key, Phase, State
constexpr uint32_t kPropertyCount = kOperatorStart + kOperatorCount * kOperatorFieldCount;

SrhStatus SRH_CALL read(void *context, uint64_t port, uint8_t *value) {
    return static_cast<Ym2414 *>(context)->read(port, value);
}
SrhStatus SRH_CALL write(void *context, uint64_t port, uint8_t value) {
    return static_cast<Ym2414 *>(context)->write(port, value);
}

SrhStatus SRH_CALL backend_config_get(void *context, char *value, uint32_t capacity) {
    auto *card = static_cast<Ym2414 *>(context);
    if (!card || !card->host_config || !card->host_config->get_value || !value || !capacity)
        return SRH_INVALID;
    return card->host_config->get_value(card->host_config->context, "cards.ym2414.backend", value,
                                        capacity);
}

SrhStatus SRH_CALL backend_config_set(void *, const char *value) {
    if (!value || (std::strcmp(value, "srz80") != 0 && std::strcmp(value, "ymfm") != 0))
        return SRH_INVALID;
    return SRH_OK;
}

SrhStatus SRH_CALL render(void *context, uint64_t, uint32_t frames, int16_t *out) {
    if (!out)
        return SRH_INVALID;
    auto &card = *static_cast<Ym2414 *>(context);
    Engine &chip = *card.chip;
    const uint32_t native = chip.native_rate();
    if (native == 0)
        return SRH_ERROR;
    for (uint32_t frame = 0; frame < frames; ++frame) {
        chip.clock();
        card.sample_phase += card.sample_rate;
        while (card.sample_phase < native) {
            chip.clock();
            card.sample_phase += card.sample_rate;
        }
        card.sample_phase -= native;
        out[static_cast<size_t>(frame) * 2] = chip.output_left();
        out[static_cast<size_t>(frame) * 2 + 1] = chip.output_right();
    }
    return SRH_OK;
}

SrhStatus SRH_CALL create(const ShouryoHost *host, SrhHandle owner, const SrhConfig *config,
                          void **result) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!srz80::sdk::valid(host) || !srz80::sdk::valid(config) || !result || !config->space ||
            config->size != kPortCount || config->base == UINT64_MAX)
            return SRH_INVALID;
        if (!srz80::sdk::has_field(config, &SrhConfig::base) ||
            config->base > UINT64_MAX - (kPortCount - 1))
            return SRH_INVALID;
        Settings settings;
        if (!Json(config).parse(settings))
            return SRH_INVALID;

        const void *extension = nullptr;
        if (!host->query || host->query(host->context, "host.audio.v1", &extension) != SRH_OK ||
            !extension)
            return SRH_UNAVAILABLE;
        const auto audio = static_cast<const SrhHostAudioV1 *>(extension);
        if (!srz80::sdk::valid(audio) || !audio->register_source)
            return SRH_UNAVAILABLE;

        auto card = std::make_unique<Ym2414>();
        card->host = host;
        card->owner = owner;
        card->space = config->space;
        card->base = config->base;
        card->priority = config->priority;
        card->chip_clock_hz = settings.chip_clock_hz;
        card->sample_rate = settings.sample_rate;
        card->backend = configured_backend(host);
        card->chip = std::make_unique<Engine>(card->backend, settings.chip_clock_hz);
        card->reset();

        extension = nullptr;
        if (host->query(host->context, "host.config.v1", &extension) == SRH_OK && extension) {
            const auto host_config = static_cast<const SrhHostConfigV1 *>(extension);
            if (srz80::sdk::valid(host_config) && host_config->register_entry &&
                host_config->get_value) {
                card->host_config = host_config;
                const SrhConfigEntry entry{
                    SRH_INIT(SrhConfigEntry),
                    "Cards/YM2414",
                    "cards.ym2414.backend",
                    "Backend",
                    "YM2414 emulation engine. Used for new cards and newly opened projects",
                    Srh_CONFIG_ENUM,
                    "srz80|ymfm",
                    "srz80",
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
                           config->base + kPortCount - 1, config->priority, card.get(), read, write,
                           read, nullptr};
        SrhHandle mapping_handle = 0;
        auto status = host->map(host->context, owner, &mapping, &mapping_handle);
        if (status != SRH_OK)
            return status;

        SrhHandle stream = 0;
        status = audio->register_source(audio->context, owner, settings.sample_rate, 2,
                                        SRH_AUDIO_S16_STEREO, settings.stream_name.c_str(), render,
                                        card.get(), &stream);
        if (status != SRH_OK)
            return status;

        *result = card.release();
        return SRH_OK;
    });
}

void SRH_CALL destroy(void *context) { delete static_cast<Ym2414 *>(context); }

SrhStatus SRH_CALL reset(void *context, uint32_t) {
    static_cast<Ym2414 *>(context)->reset();
    return SRH_OK;
}

uint32_t SRH_CALL count(void *) { return kPropertyCount; }

SrhStatus SRH_CALL info(void *, uint32_t index, SrhProperty *out) {
    if (!srz80::sdk::valid(out) || index >= kPropertyCount)
        return SRH_INVALID;
    static const char *config_names[] = {"base", "chip_clock_hz", "sample_rate"};
    static const char *config_descriptions[] = {"First mapped I/O port", "YM2414 input clock",
                                                "Native output rate"};
    static const char *operator_names[] = {"Env", "Out", "Key", "Phase", "State"};
    static const char *operator_descriptions[] = {"Envelope generator attenuation",
                                                  "Operator output",
                                                  "Key-on flag",
                                                  "Phase accumulator",
                                                  "Envelope generator state"};
    static thread_local char name[128]{};
    static thread_local char group[128]{};
    static thread_local char description[256]{};
    const char *enum_text = nullptr;
    uint32_t kind = SRH_UNSIGNED;
    uint32_t bits = 8;
    uint32_t base = 10;
    uint32_t editable = 0;
    uint32_t ui_flags = 0;

    if (index < kConfigCount) {
        std::snprintf(name, sizeof(name), "%s", config_names[index]);
        std::snprintf(group, sizeof(group), "%s", "YM2414");
        std::snprintf(description, sizeof(description), "%s", config_descriptions[index]);
        bits = index == 0 ? 16u : 32u;
        base = index == 0 ? 16u : 10u;
        ui_flags = index >= 1 ? SRH_PROPERTY_PERSISTENT : SRH_PROPERTY_RUNTIME;
    } else if (index < kInterfaceStart) {
        const uint32_t reg = index - kRawStart;
        std::snprintf(name, sizeof(name), "R%03X", reg);
        std::snprintf(group, sizeof(group), "%s", "Raw");
        std::snprintf(description, sizeof(description), "YM2414 register $%03X", reg);
        base = 16;
        editable = 1;
        ui_flags = SRH_PROPERTY_HIDE_UI | SRH_PROPERTY_LIVE_EDIT | SRH_PROPERTY_RUNTIME;
    } else if (index < kOperatorStart) {
        if (index == kInterfaceStart) {
            std::snprintf(name, sizeof(name), "%s", "Address");
            std::snprintf(description, sizeof(description),
                          "Current YM2414 register address latch");
        } else {
            std::snprintf(name, sizeof(name), "%s", "Data");
            std::snprintf(description, sizeof(description),
                          "Write data to the latched YM2414 register");
        }
        std::snprintf(group, sizeof(group), "%s", "Interface");
        base = 16;
        editable = 1;
        ui_flags = SRH_PROPERTY_HIDE_UI | SRH_PROPERTY_LIVE_EDIT | SRH_PROPERTY_RUNTIME;
    } else {
        const uint32_t op = (index - kOperatorStart) / kOperatorFieldCount;
        const uint32_t field = (index - kOperatorStart) % kOperatorFieldCount;
        std::snprintf(name, sizeof(name), "O%02u.%s", op, operator_names[field]);
        std::snprintf(group, sizeof(group), "Operator %02u", op);
        std::snprintf(description, sizeof(description), "%s", operator_descriptions[field]);
        switch (field) {
        case 0:
            bits = 10;
            break;
        case 1:
            kind = SRH_SIGNED;
            bits = 32;
            break;
        case 2:
            kind = SRH_BOOLEAN;
            bits = 1;
            break;
        case 3:
            bits = 10;
            break;
        default:
            kind = SRH_ENUM;
            enum_text = "attack|decay|sustain|release|reverb";
            bits = 3;
            break;
        }
        ui_flags = SRH_PROPERTY_HIDE_UI;
    }

    *out = {SRH_INIT(SrhProperty), name, group, description[0] ? description : nullptr, kind, bits,
            base, editable, enum_text, ui_flags};
    return SRH_OK;
}

SrhStatus SRH_CALL get(void *context, uint32_t index, SrhValue *out) {
    if (!srz80::sdk::valid(out) || index >= kPropertyCount)
        return SRH_INVALID;
    auto &card = *static_cast<Ym2414 *>(context);
    Engine &chip = *card.chip;
    out->unsigned_value = 0;
    out->signed_value = 0;
    if (index == 0) {
        out->unsigned_value = card.base;
        return SRH_OK;
    }
    if (index == 1) {
        out->unsigned_value = card.chip_clock_hz;
        return SRH_OK;
    }
    if (index == 2) {
        out->unsigned_value = card.sample_rate;
        return SRH_OK;
    }
    if (index < kInterfaceStart) {
        out->unsigned_value = chip.register_value(index - kRawStart);
        return SRH_OK;
    }
    if (index < kOperatorStart) {
        out->unsigned_value =
            index == kInterfaceStart ? card.address : chip.register_value(card.address);
        return SRH_OK;
    }
    const uint32_t op = (index - kOperatorStart) / kOperatorFieldCount;
    const uint32_t field = (index - kOperatorStart) % kOperatorFieldCount;
    switch (field) {
    case 0:
        out->unsigned_value = chip.operator_env(op);
        break;
    case 1:
        out->signed_value = chip.operator_output(op);
        out->unsigned_value = static_cast<uint64_t>(static_cast<uint32_t>(chip.operator_output(op)));
        break;
    case 2:
        out->unsigned_value = chip.operator_key(op);
        break;
    case 3:
        out->unsigned_value = chip.operator_phase(op);
        break;
    default:
        out->unsigned_value = chip.operator_state(op);
        break;
    }
    return SRH_OK;
}

SrhStatus set_impl(void *context, uint32_t index, const SrhValue *in) {
    if (!srz80::sdk::valid(in) || index < kConfigCount || index >= kPropertyCount ||
        in->unsigned_value > 0xff)
        return SRH_INVALID;
    auto &card = *static_cast<Ym2414 *>(context);
    const uint8_t value = static_cast<uint8_t>(in->unsigned_value);
    if (index < kInterfaceStart) {
        const uint32_t reg = index - kRawStart;
        card.registers[reg] = value;
        card.apply_register(static_cast<uint8_t>(reg), value);
    } else if (index < kOperatorStart) {
        if (index == kInterfaceStart) {
            card.address = value;
            card.chip->write_address(value);
        } else {
            card.registers[card.address] = value;
            card.apply_register(card.address, value);
        }
    } else {
        return SRH_INVALID;
    }
    return SRH_OK;
}

SrhStatus SRH_CALL set(void *context, uint32_t index, const SrhValue *in) {
    return srz80::sdk::guard([&]() { return set_impl(context, index, in); });
}

SrhStatus SRH_CALL save_state(void *context, uint8_t *buffer, uint64_t *size) {
    if (!size)
        return SRH_INVALID;
    auto &card = *static_cast<Ym2414 *>(context);
    constexpr uint64_t kHeader = 2 + 8;
    const uint64_t required = kHeader + card.chip->state_size();
    if (!buffer) {
        *size = required;
        return SRH_OK;
    }
    if (*size < required) {
        *size = required;
        return SRH_UNAVAILABLE;
    }
    buffer[0] = static_cast<uint8_t>(card.backend);
    buffer[1] = card.address;
    for (uint32_t i = 0; i < 8; ++i)
        buffer[2 + i] = static_cast<uint8_t>(card.sample_phase >> (8 * i));
    card.chip->save_state(buffer + kHeader);
    *size = required;
    return SRH_OK;
}

SrhStatus SRH_CALL load_state(void *context, const uint8_t *buffer, uint64_t size) {
    constexpr uint64_t kHeader = 2 + 8;
    if (!buffer || size < kHeader)
        return SRH_INVALID;
    auto &card = *static_cast<Ym2414 *>(context);
    // A save produced by a different engine cannot be interpreted safely.
    if (buffer[0] != static_cast<uint8_t>(card.backend) ||
        size != kHeader + card.chip->state_size())
        return SRH_INVALID;
    if (!card.chip->load_state(buffer + kHeader, card.chip->state_size()))
        return SRH_INVALID;
    card.address = buffer[1];
    card.sample_phase = 0;
    for (uint32_t i = 0; i < 8; ++i)
        card.sample_phase |= static_cast<uint64_t>(buffer[2 + i]) << (8 * i);
    for (uint32_t i = 0; i < Engine::kRegisters; ++i)
        card.registers[i] = card.chip->register_value(i);
    return SRH_OK;
}

const SrhCardDescriptor descriptor{SRH_INIT(SrhCardDescriptor), "Audio", "YM2414",
                                   "Yamaha YM2414 (OPZ) FM synthesizer", 0, 2, 0, 0, 0, 0,
                                   R"({"chip_clock_hz":3579545,"sample_rate":44100,"stream_name":"YM2414"})",
                                   nullptr, nullptr, nullptr, 0};
const SrhPlugin api{SRH_INIT(SrhPlugin), "ym2414", create, destroy, reset, count, info, get, set,
                    save_state, load_state, &descriptor, nullptr, nullptr};
}

extern "C" SRH_EXPORT const SrhPlugin *SRH_CALL srz80_plugin_init(const ShouryoHost *host) {
    return srz80::sdk::valid(host) ? &api : nullptr;
}
