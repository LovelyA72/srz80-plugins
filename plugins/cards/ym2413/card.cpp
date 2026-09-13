#include <boundary.hpp>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

extern "C" {
#include "emu2413.h"
}

namespace {
struct Settings {
    uint32_t chip_clock_hz = 3579545;
    uint32_t sample_rate = 44100;
    std::string stream_name = "YM2413";
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
            if (key == "chip_clock_hz") {
                uint64_t number = 0;
                if (!unsigned_value(value, quoted, number) || number < 1 || number > 10000000)
                    return false;
                settings.chip_clock_hz = static_cast<uint32_t>(number);
            } else if (key == "sample_rate") {
                uint64_t number = 0;
                if (!unsigned_value(value, quoted, number) || number < 8000 || number > 192000)
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

struct Ym2413 {
    const ShouryoHost *host = nullptr;
    SrhHandle owner = 0, space = 0, mapping = 0, stream = 0;
    uint64_t base = 0;
    uint32_t chip_clock_hz = 0, sample_rate = 0;
    uint8_t address = 0;
    uint8_t registers[0x40]{};
    OPLL *chip = nullptr;

    SrhStatus read(uint64_t address, uint8_t *value) const {
        if (!value || address < base || address - base >= 2)
            return SRH_INVALID;
        *value = 0;
        return SRH_OK;
    }
    SrhStatus write(uint64_t address, uint8_t value) {
        if (address < base || address - base >= 2)
            return SRH_INVALID;
        if (address == base) {
            this->address = value & 0x3F;
        } else {
            registers[this->address] = value;
            OPLL_writeReg(chip, this->address, value);
        }
        return SRH_OK;
    }
    void reset() {
        address = 0;
        std::fill(std::begin(registers), std::end(registers), 0);
        OPLL_reset(chip);
    }
};

constexpr uint32_t kConfigCount = 3;
constexpr uint32_t kRawStart = kConfigCount;
constexpr uint32_t kRawCount = 0x40; // 64 YM2413 registers
constexpr uint32_t kInterfaceStart = kRawStart + kRawCount;
constexpr uint32_t kInterfaceCount = 2; // Address latch, Data write
constexpr uint32_t kSlotStart = kInterfaceStart + kInterfaceCount;
constexpr uint32_t kSlotCount = 18;
constexpr uint32_t kSlotFieldCount = 6; // Env, Out, Key, Sus, Freq, State
constexpr uint32_t kPropertyCount = kSlotStart + kSlotCount * kSlotFieldCount;

SrhStatus SRH_CALL read(void *context, uint64_t address, uint8_t *value) {
    return static_cast<Ym2413 *>(context)->read(address, value);
}
SrhStatus SRH_CALL write(void *context, uint64_t address, uint8_t value) {
    return static_cast<Ym2413 *>(context)->write(address, value);
}
SrhStatus SRH_CALL render(void *context, uint64_t, uint32_t frames, int16_t *interleaved) {
    if (!interleaved)
        return SRH_INVALID;
    auto &ym = *static_cast<Ym2413 *>(context);
    for (uint32_t i = 0; i < frames; ++i) {
        int32_t stereo[2]{};
        OPLL_calcStereo(ym.chip, stereo);
        interleaved[static_cast<size_t>(i) * 2] = static_cast<int16_t>(
            std::clamp(stereo[0], static_cast<int32_t>(INT16_MIN), static_cast<int32_t>(INT16_MAX)));
        interleaved[static_cast<size_t>(i) * 2 + 1] = static_cast<int16_t>(
            std::clamp(stereo[1], static_cast<int32_t>(INT16_MIN), static_cast<int32_t>(INT16_MAX)));
    }
    return SRH_OK;
}
SrhStatus SRH_CALL create(const ShouryoHost *host, SrhHandle owner, const SrhConfig *config,
                          void **result) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!srz80::sdk::valid(host) || !srz80::sdk::valid(config) || !result || !config->space ||
            config->size != 2 || config->base == UINT64_MAX)
            return SRH_INVALID;
        Settings settings;
        if (!Json(config).parse(settings))
            return SRH_INVALID;
        const void *extension = nullptr;
        if (!host->query || host->query(host->context, "host.audio.v1", &extension) != SRH_OK ||
            !extension)
            return SRH_UNAVAILABLE;
        auto audio = static_cast<const SrhHostAudioV1 *>(extension);
        if (!srz80::sdk::valid(audio) || !audio->register_source || settings.sample_rate != audio->sample_rate)
            return settings.sample_rate == audio->sample_rate ? SRH_UNAVAILABLE : SRH_INVALID;
        auto ym = std::make_unique<Ym2413>();
        ym->host = host;
        ym->owner = owner;
        ym->space = config->space;
        ym->base = config->base;
        ym->chip_clock_hz = settings.chip_clock_hz;
        ym->sample_rate = settings.sample_rate;
        ym->chip = OPLL_new(settings.chip_clock_hz, settings.sample_rate);
        if (!ym->chip)
            return SRH_ERROR;
        SrhMapping mapping{SRH_INIT(SrhMapping), config->space, config->base, config->base + 1,
                           config->priority, ym.get(), read, write, read};
        auto status = host->map(host->context, owner, &mapping, &ym->mapping);
        if (status != SRH_OK)
            return status;
        status = audio->register_source(audio->context, owner, settings.sample_rate, 2,
                                        SRH_AUDIO_S16_STEREO, settings.stream_name.c_str(), render,
                                        ym.get(), &ym->stream);
        if (status != SRH_OK)
            return status;
        *result = ym.release();
        return SRH_OK;
    });
}
void SRH_CALL destroy(void *context) {
    auto *ym = static_cast<Ym2413 *>(context);
    if (ym) {
        OPLL_delete(ym->chip);
        delete ym;
    }
}
SrhStatus SRH_CALL reset(void *context, uint32_t) {
    static_cast<Ym2413 *>(context)->reset();
    return SRH_OK;
}
uint32_t SRH_CALL count(void *) { return kPropertyCount; }
SrhStatus SRH_CALL info(void *, uint32_t index, SrhProperty *out) {
    if (!srz80::sdk::valid(out) || index >= kPropertyCount)
        return SRH_INVALID;
    static const char *config_names[] = {"base", "chip_clock_hz", "sample_rate"};
    static const char *config_descriptions[] = {"First mapped I/O port", "YM2413 input clock",
                                                "Host output rate"};
    static const char *slot_names[] = {"Env", "Out", "Key", "Sus", "Freq", "State"};
    static const char *slot_descriptions[] = {"Envelope generator output",
                                              "Operator phase output",
                                              "Key-on flag",
                                              "Sustain/key-sus flag",
                                              "Block and f-number",
                                              "Envelope generator state"};
    static thread_local char name[128]{}, group[128]{}, description[256]{};
    const char *enum_text = nullptr;
    uint32_t kind = SRH_UNSIGNED;
    uint32_t bits = 8;
    uint32_t base = 10;
    uint32_t editable = 0;
    uint32_t ui_flags = 0;

    if (index < kConfigCount) {
        std::snprintf(name, sizeof(name), "%s", config_names[index]);
        std::snprintf(group, sizeof(group), "%s", "YM2413");
        std::snprintf(description, sizeof(description), "%s", config_descriptions[index]);
        bits = index == 0 ? 16u : 32u;
        base = index == 0 ? 16u : 10u;
    } else if (index < kInterfaceStart) {
        const uint32_t reg = index - kRawStart;
        std::snprintf(name, sizeof(name), "R%02X", reg);
        std::snprintf(group, sizeof(group), "%s", "Raw");
        std::snprintf(description, sizeof(description), "YM2413 register $%02X", reg);
        base = 16;
        editable = 1;
        ui_flags = SRH_PROPERTY_HIDE_UI | SRH_PROPERTY_LIVE_EDIT;
    } else if (index < kSlotStart) {
        if (index == kInterfaceStart) {
            std::snprintf(name, sizeof(name), "%s", "Address");
            std::snprintf(description, sizeof(description), "Current YM2413 register address latch");
            editable = 1;
        } else {
            std::snprintf(name, sizeof(name), "%s", "Data");
            std::snprintf(description, sizeof(description), "Write data to the latched YM2413 register");
            editable = 1;
        }
        std::snprintf(group, sizeof(group), "%s", "Interface");
        base = 16;
        ui_flags = SRH_PROPERTY_HIDE_UI | SRH_PROPERTY_LIVE_EDIT;
    } else {
        const uint32_t slot_index = (index - kSlotStart) / kSlotFieldCount;
        const uint32_t field = (index - kSlotStart) % kSlotFieldCount;
        std::snprintf(name, sizeof(name), "S%02u.%s", slot_index, slot_names[field]);
        std::snprintf(group, sizeof(group), "Slot %02u", slot_index);
        std::snprintf(description, sizeof(description), "%s", slot_descriptions[field]);
        if (field == 1) {
            kind = SRH_SIGNED;
            bits = 32;
        } else if (field == 2 || field == 3) {
            kind = SRH_BOOLEAN;
            bits = 1;
        } else if (field == 4) {
            kind = SRH_UNSIGNED;
            bits = 12;
        } else if (field == 5) {
            kind = SRH_ENUM;
            enum_text = "attack|decay|sustain|release|damp|unknown";
            bits = 3;
        } else {
            kind = SRH_UNSIGNED;
            bits = 8;
        }
        editable = 0;
        ui_flags = SRH_PROPERTY_HIDE_UI;
    }

    *out = {SRH_INIT(SrhProperty), name, group, description[0] ? description : nullptr, kind,
            bits, base, editable, enum_text, ui_flags};
    return SRH_OK;
}
SrhStatus SRH_CALL get(void *context, uint32_t index, SrhValue *out) {
    if (!srz80::sdk::valid(out) || index >= kPropertyCount)
        return SRH_INVALID;
    auto &ym = *static_cast<Ym2413 *>(context);
    out->unsigned_value = 0;
    out->signed_value = 0;
    if (index < kConfigCount) {
        out->unsigned_value = index == 0 ? ym.base : index == 1 ? ym.chip_clock_hz : ym.sample_rate;
        return SRH_OK;
    }
    if (index < kInterfaceStart) {
        out->unsigned_value = ym.registers[index - kRawStart];
        return SRH_OK;
    }
    if (index < kSlotStart) {
        out->unsigned_value = index == kInterfaceStart ? ym.address : ym.registers[ym.address];
        return SRH_OK;
    }
    const uint32_t slot_index = (index - kSlotStart) / kSlotFieldCount;
    const uint32_t field = (index - kSlotStart) % kSlotFieldCount;
    const auto &slot = ym.chip->slot[slot_index];
    switch (field) {
    case 0:
        out->unsigned_value = slot.eg_out;
        break;
    case 1:
        out->signed_value = slot.output[0];
        out->unsigned_value = static_cast<uint64_t>(static_cast<uint32_t>(slot.output[0]));
        break;
    case 2:
        out->unsigned_value = slot.key_flag != 0;
        break;
    case 3:
        out->unsigned_value = slot.sus_flag != 0;
        break;
    case 4:
        out->unsigned_value = slot.blk_fnum;
        break;
    case 5:
        out->unsigned_value = slot.eg_state;
        break;
    }
    return SRH_OK;
}
SrhStatus SRH_CALL set(void *context, uint32_t index, const SrhValue *in) {
    if (!srz80::sdk::valid(in) || index >= kPropertyCount)
        return SRH_INVALID;
    auto &ym = *static_cast<Ym2413 *>(context);
    if (index < kConfigCount)
        return SRH_INVALID;
    if (index < kInterfaceStart) {
        const uint32_t reg = index - kRawStart;
        if (in->unsigned_value > 0xFF)
            return SRH_INVALID;
        ym.registers[reg] = static_cast<uint8_t>(in->unsigned_value);
        OPLL_writeReg(ym.chip, reg, static_cast<uint8_t>(in->unsigned_value));
        return SRH_OK;
    }
    if (index < kSlotStart) {
        if (index == kInterfaceStart) {
            if (in->unsigned_value > 0x3F)
                return SRH_INVALID;
            ym.address = static_cast<uint8_t>(in->unsigned_value);
            return SRH_OK;
        }
        if (in->unsigned_value > 0xFF)
            return SRH_INVALID;
        const uint8_t reg = ym.address & 0x3F;
        ym.registers[reg] = static_cast<uint8_t>(in->unsigned_value);
        OPLL_writeReg(ym.chip, reg, static_cast<uint8_t>(in->unsigned_value));
        return SRH_OK;
    }
    return SRH_INVALID;
}
SrhStatus SRH_CALL save_state(void *context, uint8_t *buffer, uint64_t *size) {
    if (!size)
        return SRH_INVALID;
    constexpr uint64_t required = 1 + 0x40;
    if (!buffer) {
        *size = required;
        return SRH_OK;
    }
    if (*size < required) {
        *size = required;
        return SRH_UNAVAILABLE;
    }
    auto &ym = *static_cast<Ym2413 *>(context);
    buffer[0] = ym.address;
    std::memcpy(buffer + 1, ym.registers, 0x40);
    *size = required;
    return SRH_OK;
}
SrhStatus SRH_CALL load_state(void *context, const uint8_t *buffer, uint64_t size) {
    if (!buffer || size != 1 + 0x40)
        return SRH_INVALID;
    auto &ym = *static_cast<Ym2413 *>(context);
    ym.reset();
    for (uint32_t i = 0; i < 0x40; ++i) {
        ym.address = static_cast<uint8_t>(i);
        ym.registers[i] = buffer[i + 1];
        OPLL_writeReg(ym.chip, i, buffer[i + 1]);
    }
    ym.address = buffer[0] & 0x3F;
    return SRH_OK;
}

const SrhCardDescriptor descriptor{SRH_INIT(SrhCardDescriptor), "Audio", "YM2413",
                                   "Yamaha YM2413 FM synthesizer", 0xC0, 2, 0, 0, 0, 0,
                                   R"({"chip_clock_hz":3579545,"sample_rate":44100,"stream_name":"YM2413"})",
                                   nullptr, nullptr};
const SrhPlugin api{SRH_INIT(SrhPlugin), "ym2413", create, destroy, reset, count, info, get, set,
                    save_state, load_state, &descriptor};
} // namespace

extern "C" SRH_EXPORT const SrhPlugin *SRH_CALL srz80_plugin_init(const ShouryoHost *host) {
    return srz80::sdk::valid(host) ? &api : nullptr;
}
