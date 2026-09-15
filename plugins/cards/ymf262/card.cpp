#include <boundary.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "ymfm/ymfm_opl.h"
extern "C" {
#include "opl3.h"
}

namespace {
constexpr uint32_t kReferenceClockHz = 14318180;
constexpr uint32_t kRegisterCount = 0x200;
constexpr uint32_t kUsedPortCount = 4;
constexpr uint32_t kMinChipClockHz = 1000000;
constexpr uint32_t kMaxChipClockHz = 50000000;
constexpr uint32_t kMinSampleRate = 8000;
constexpr uint32_t kMaxSampleRate = 192000;

enum class Backend : uint32_t { nuked, ymfm };

Backend configured_backend(const ShouryoHost *host) {
    const void *extension = nullptr;
    if (!host || !host->query || host->query(host->context, "host.config.v1", &extension) != SRH_OK ||
        !extension)
        return Backend::nuked;
    const auto config = static_cast<const SrhHostConfigV1 *>(extension);
    if (!srz80::sdk::valid(config) || !config->get_value)
        return Backend::nuked;
    char value[32]{};
    if (config->get_value(config->context, "cards.ymf262.backend", value,
                          static_cast<uint32_t>(sizeof(value))) != SRH_OK)
        return Backend::nuked;
    return std::strcmp(value, "ymfm") == 0 ? Backend::ymfm : Backend::nuked;
}

struct Settings {
    uint32_t chip_clock_hz = kReferenceClockHz;
    uint32_t sample_rate = 44100;
    std::string stream_name = "YMF262";
};

class Json {
  public:
    explicit Json(const SrhConfig *c)
        : text_(c && srz80::sdk::has_field(c, &SrhConfig::config_json) && c->config_json
                    ? std::string(c->config_json, c->config_json_size)
                    : "{}") {}

    bool parse(Settings &s) {
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
                s.chip_clock_hz = static_cast<uint32_t>(number);
            } else if (key == "sample_rate") {
                if (!unsigned_value(value, quoted, number) || number < kMinSampleRate ||
                    number > kMaxSampleRate)
                    return false;
                s.sample_rate = static_cast<uint32_t>(number);
            } else if (key == "stream_name") {
                if (!quoted || value.empty() || value.size() > 256)
                    return false;
                s.stream_name = value;
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

struct YmfmInterface final : ymfm::ymfm_interface {};

struct Ymf262 {
    const ShouryoHost *host = nullptr;
    const SrhHostConfigV1 *host_config = nullptr;
    SrhHandle owner = 0;
    uint64_t base = 0;
    int32_t priority = 0;
    uint32_t chip_clock_hz = 0;
    uint32_t sample_rate = 0;
    Backend backend = Backend::nuked;
    uint8_t address[2]{};
    uint8_t registers[kRegisterCount]{};
    opl3_chip nuked_chip{};
    YmfmInterface ymfm_interface;
    std::unique_ptr<ymfm::ymf262> ymfm_chip;
    uint32_t ymfm_native_rate = 0;
    uint64_t ymfm_sample_phase = 0;

    uint32_t emulator_rate() const {
        return static_cast<uint32_t>((static_cast<uint64_t>(sample_rate) * kReferenceClockHz +
                                       chip_clock_hz / 2) /
                                      chip_clock_hz);
    }

    void initialise_engine() {
        ymfm_sample_phase = 0;
        if (backend == Backend::ymfm) {
            ymfm_chip = std::make_unique<ymfm::ymf262>(ymfm_interface);
            ymfm_chip->reset();
            ymfm_native_rate = ymfm_chip->sample_rate(chip_clock_hz);
        } else {
            ymfm_chip.reset();
            ymfm_native_rate = 0;
            OPL3_Reset(&nuked_chip, emulator_rate());
        }
    }

    void write_engine_register(uint16_t reg, uint8_t value) {
        if (backend == Backend::nuked) {
            OPL3_WriteReg(&nuked_chip, reg, value);
        } else if (reg & 0x100u) {
            ymfm_chip->write(2, static_cast<uint8_t>(reg));
            ymfm_chip->write(3, value);
        } else {
            ymfm_chip->write(0, static_cast<uint8_t>(reg));
            ymfm_chip->write(1, value);
        }
    }

    void restore_registers() {
        if (backend == Backend::ymfm)
            write_engine_register(0x105, registers[0x105]);
        for (uint16_t reg = 0; reg < kRegisterCount; ++reg)
            if (backend == Backend::nuked || reg != 0x105)
                write_engine_register(reg, registers[reg]);
        if (backend == Backend::ymfm) {
            ymfm_chip->write(0, address[0]);
            ymfm_chip->write(2, address[1]);
        }
    }

    void reset() {
        std::fill(std::begin(address), std::end(address), 0);
        std::fill(std::begin(registers), std::end(registers), 0);
        initialise_engine();
    }

    void change_engine() {
        initialise_engine();
        restore_registers();
    }

    SrhStatus read(uint64_t port, uint8_t *value) const {
        if (!value || port < base || port - base >= kUsedPortCount)
            return SRH_INVALID;
        *value = backend == Backend::ymfm ? ymfm_chip->read(static_cast<uint32_t>(port - base)) : 0;
        return SRH_OK;
    }

    SrhStatus write(uint64_t port, uint8_t value) {
        if (port < base || port - base >= kUsedPortCount)
            return SRH_INVALID;
        const auto offset = static_cast<uint32_t>(port - base);
        const auto bank = offset >> 1;
        if ((offset & 1u) == 0) {
            address[bank] = value;
            if (backend == Backend::ymfm)
                ymfm_chip->write(offset, value);
        } else {
            const auto reg = static_cast<uint16_t>((bank << 8) | address[bank]);
            registers[reg] = value;
            if (backend == Backend::nuked)
                OPL3_WriteReg(&nuked_chip, reg, value);
            else
                ymfm_chip->write(offset, value);
        }
        return SRH_OK;
    }
};

constexpr uint32_t kConfigCount = 3;
constexpr uint32_t kRawStart = kConfigCount;
constexpr uint32_t kInterfaceStart = kRawStart + kRegisterCount;
constexpr uint32_t kPropertyCount = kInterfaceStart + kUsedPortCount;

SrhStatus SRH_CALL read(void *c, uint64_t address, uint8_t *value) {
    return static_cast<Ymf262 *>(c)->read(address, value);
}

SrhStatus SRH_CALL write(void *c, uint64_t address, uint8_t value) {
    return static_cast<Ymf262 *>(c)->write(address, value);
}

SrhStatus SRH_CALL render(void *c, uint64_t, uint32_t frames, int16_t *out) {
    if (!out)
        return SRH_INVALID;
    auto &card = *static_cast<Ymf262 *>(c);
    for (uint32_t frame = 0; frame < frames; ++frame) {
        if (card.backend == Backend::nuked) {
            OPL3_GenerateResampled(&card.nuked_chip, out + static_cast<size_t>(frame) * 2);
            continue;
        }

        ymfm::ymf262::output_data generated{};
        do {
            card.ymfm_chip->generate(&generated, 1);
            card.ymfm_sample_phase += card.sample_rate;
        } while (card.ymfm_sample_phase < card.ymfm_native_rate);
        card.ymfm_sample_phase -= card.ymfm_native_rate;
        while (card.ymfm_sample_phase >= card.ymfm_native_rate) {
            card.ymfm_chip->generate(&generated, 1);
            card.ymfm_sample_phase -= card.ymfm_native_rate;
        }
        out[static_cast<size_t>(frame) * 2] = static_cast<int16_t>(std::clamp(
            generated.data[0], static_cast<int32_t>(INT16_MIN), static_cast<int32_t>(INT16_MAX)));
        out[static_cast<size_t>(frame) * 2 + 1] = static_cast<int16_t>(std::clamp(
            generated.data[1], static_cast<int32_t>(INT16_MIN), static_cast<int32_t>(INT16_MAX)));
    }
    return SRH_OK;
}

SrhStatus SRH_CALL backend_config_get(void *context, char *value, uint32_t capacity) {
    auto *card = static_cast<Ymf262 *>(context);
    if (!card || !card->host_config || !card->host_config->get_value || !value || !capacity)
        return SRH_INVALID;
    return card->host_config->get_value(card->host_config->context, "cards.ymf262.backend", value,
                                        capacity);
}

SrhStatus SRH_CALL backend_config_set(void *, const char *value) {
    if (!value || (std::strcmp(value, "Nuked-OPL3") != 0 && std::strcmp(value, "Nuked") != 0 &&
                   std::strcmp(value, "ymfm") != 0))
        return SRH_INVALID;
    return SRH_OK;
}

SrhStatus SRH_CALL create(const ShouryoHost *host, SrhHandle owner, const SrhConfig *config,
                          void **result) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!srz80::sdk::valid(host) || !srz80::sdk::valid(config) || !result || !config->space ||
            config->size != kUsedPortCount || config->base > UINT64_MAX - (kUsedPortCount - 1))
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
            return SRH_INVALID;

        auto card = std::make_unique<Ymf262>();
        card->host = host;
        card->owner = owner;
        card->base = config->base;
        card->priority = config->priority;
        card->chip_clock_hz = settings.chip_clock_hz;
        card->sample_rate = settings.sample_rate;
        card->backend = configured_backend(host);
        card->reset();

        extension = nullptr;
        if (host->query(host->context, "host.config.v1", &extension) == SRH_OK && extension) {
            const auto host_config = static_cast<const SrhHostConfigV1 *>(extension);
            if (srz80::sdk::valid(host_config) && host_config->register_entry && host_config->get_value) {
                card->host_config = host_config;
                const SrhConfigEntry entry{
                    SRH_INIT(SrhConfigEntry),
                    "Cards/YMF262",
                    "cards.ymf262.backend",
                    "Backend",
                    "YMF262 emulation engine. Used for new cards and newly opened projects",
                    Srh_CONFIG_ENUM,
                    "Nuked-OPL3|ymfm",
                    "Nuked-OPL3",
                    card.get(),
                    owner,
                    backend_config_get,
                    backend_config_set};
                const auto config_status = host_config->register_entry(host_config->context, &entry);
                if (config_status != SRH_OK)
                    return config_status;
            }
        }

        SrhMapping mapping{SRH_INIT(SrhMapping), config->space, config->base,
                           config->base + kUsedPortCount - 1, config->priority, card.get(), read, write,
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

void SRH_CALL destroy(void *c) { delete static_cast<Ymf262 *>(c); }

SrhStatus SRH_CALL reset(void *c, uint32_t) {
    static_cast<Ymf262 *>(c)->reset();
    return SRH_OK;
}

uint32_t SRH_CALL count(void *) { return kPropertyCount; }

SrhStatus SRH_CALL info(void *, uint32_t index, SrhProperty *out) {
    if (!srz80::sdk::valid(out) || index >= kPropertyCount)
        return SRH_INVALID;
    static const char *names[] = {"base", "chip_clock_hz", "sample_rate"};
    static const char *descriptions[] = {"First mapped I/O port", "YMF262 input clock", "Native output rate"};
    if (index < kConfigCount) {
        *out = {SRH_INIT(SrhProperty), names[index], "YMF262", descriptions[index], SRH_UNSIGNED,
                index == 0 ? 16u : 32u, index == 0 ? 16u : 10u, 0, nullptr,
                static_cast<uint32_t>(index >= 1 ? SRH_PROPERTY_PERSISTENT : SRH_PROPERTY_RUNTIME)};
        return SRH_OK;
    }
    if (index < kInterfaceStart) {
        const auto reg = index - kRawStart;
        static thread_local char name[8];
        static thread_local char description[64];
        std::snprintf(name, sizeof(name), "R%03X", reg);
        std::snprintf(description, sizeof(description), "YMF262 register $%03X", reg);
        *out = {SRH_INIT(SrhProperty), name, reg < 0x100 ? "Bank 0" : "Bank 1", description,
                SRH_UNSIGNED, 8, 16, 1, nullptr,
                SRH_PROPERTY_HIDE_UI | SRH_PROPERTY_LIVE_EDIT | SRH_PROPERTY_RUNTIME};
        return SRH_OK;
    }
    const auto item = index - kInterfaceStart;
    const auto bank = item >> 1;
    const bool data = (item & 1u) != 0;
    static thread_local char name[32];
    static thread_local char description[128];
    std::snprintf(name, sizeof(name), "Bank %u %s", bank, data ? "Data" : "Address");
    std::snprintf(description, sizeof(description), "%s", data ? "Write data to the bank address latch"
                                                                  : "Current bank address latch");
    *out = {SRH_INIT(SrhProperty), name, "Interface", description, SRH_UNSIGNED, 8, 16, 1, nullptr,
            SRH_PROPERTY_HIDE_UI | SRH_PROPERTY_LIVE_EDIT | SRH_PROPERTY_RUNTIME};
    return SRH_OK;
}

SrhStatus SRH_CALL get(void *c, uint32_t index, SrhValue *out) {
    if (!srz80::sdk::valid(out) || index >= kPropertyCount)
        return SRH_INVALID;
    auto &card = *static_cast<Ymf262 *>(c);
    out->unsigned_value = 0;
    out->signed_value = 0;
    if (index == 0)
        out->unsigned_value = card.base;
    else if (index == 1)
        out->unsigned_value = card.chip_clock_hz;
    else if (index == 2)
        out->unsigned_value = card.sample_rate;
    else if (index < kInterfaceStart)
        out->unsigned_value = card.registers[index - kRawStart];
    else {
        const auto item = index - kInterfaceStart;
        const auto bank = item >> 1;
        out->unsigned_value = (item & 1u) == 0
                                  ? card.address[bank]
                                  : card.registers[(bank << 8) | card.address[bank]];
    }
    return SRH_OK;
}

SrhStatus set_impl(void *c, uint32_t index, const SrhValue *in) {
    if (!srz80::sdk::valid(in) || index < kConfigCount || index >= kPropertyCount ||
        in->unsigned_value > 0xff)
        return SRH_INVALID;
    auto &card = *static_cast<Ymf262 *>(c);
    const auto value = static_cast<uint8_t>(in->unsigned_value);
    if (index < kInterfaceStart) {
        const auto reg = static_cast<uint16_t>(index - kRawStart);
        card.registers[reg] = value;
        card.write_engine_register(reg, value);
    } else {
        const auto item = index - kInterfaceStart;
        const auto bank = item >> 1;
        if ((item & 1u) == 0) {
            card.address[bank] = value;
            if (card.backend == Backend::ymfm)
                card.ymfm_chip->write(bank == 0 ? 0 : 2, value);
        } else {
            const auto reg = static_cast<uint16_t>((bank << 8) | card.address[bank]);
            card.registers[reg] = value;
            card.write_engine_register(reg, value);
        }
    }
    return SRH_OK;
}

SrhStatus SRH_CALL set(void *c, uint32_t index, const SrhValue *in) {
    return srz80::sdk::guard([&]() { return set_impl(c, index, in); });
}

SrhStatus SRH_CALL save_state(void *c, uint8_t *buffer, uint64_t *size) {
    if (!size)
        return SRH_INVALID;
    constexpr uint64_t required = 2 + kRegisterCount;
    if (!buffer) {
        *size = required;
        return SRH_OK;
    }
    if (*size < required) {
        *size = required;
        return SRH_UNAVAILABLE;
    }
    const auto &card = *static_cast<Ymf262 *>(c);
    std::memcpy(buffer, card.address, 2);
    std::memcpy(buffer + 2, card.registers, kRegisterCount);
    *size = required;
    return SRH_OK;
}

SrhStatus SRH_CALL load_state(void *c, const uint8_t *buffer, uint64_t size) {
    constexpr uint64_t required = 2 + kRegisterCount;
    if (!buffer || size != required)
        return SRH_INVALID;
    auto &card = *static_cast<Ymf262 *>(c);
    card.reset();
    std::memcpy(card.address, buffer, 2);
    std::memcpy(card.registers, buffer + 2, kRegisterCount);
    card.restore_registers();
    return SRH_OK;
}

const SrhCardDescriptor descriptor{SRH_INIT(SrhCardDescriptor), "Audio", "YMF262",
                                   "Yamaha YMF262 (OPL3) FM synthesizer", 0x388, 4, 0, 0, 0, 0,
                                   R"({"chip_clock_hz":14318180,"sample_rate":44100,"stream_name":"YMF262"})",
                                   nullptr, nullptr};
const SrhPlugin api{SRH_INIT(SrhPlugin), "ymf262", create, destroy, reset, count, info, get, set,
                    save_state, load_state, &descriptor, nullptr, nullptr};
} // namespace

extern "C" SRH_EXPORT const SrhPlugin *SRH_CALL srz80_plugin_init(const ShouryoHost *host) {
    return srz80::sdk::valid(host) ? &api : nullptr;
}
