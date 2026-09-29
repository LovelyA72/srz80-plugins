#include <state.hpp>
#include <boundary.hpp>

#include <nlohmann/json.hpp>
#include <srz80/signals.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

namespace {
using Json = nlohmann::json;

constexpr uint32_t kChannelCount = 4;
constexpr uint32_t kPortCount = 4;
constexpr int32_t kIrqAssertedMv = 1000;
constexpr int32_t kSignalHighMv = 700;
constexpr uint32_t kGlobalProperties = 3;
constexpr uint32_t kChannelProperties = 4;
constexpr uint32_t kPropertyCount = kGlobalProperties + kChannelCount * kChannelProperties;

struct Ctc;
struct ClkTrgContext {
    Ctc *ctc = nullptr;
    uint32_t channel = 0;
};

struct Channel {
    bool interrupt_enable = false;
    bool counter_mode = false;
    bool prescaler_256 = false;
    bool edge_rising = false;
    bool external_trigger = false;
    bool expect_time_constant = false;
    bool has_time_constant = false;
    bool running = false;
    bool waiting_trigger = false;
    bool interrupt_pending = false;

    uint8_t time_constant = 0;
    uint16_t down_counter = 0;
    uint16_t prescaler = 0;
    uint64_t zero_count = 0;

    uint16_t load_value() const { return time_constant ? time_constant : 256; }
    uint16_t prescaler_period() const { return prescaler_256 ? 256 : 16; }

    uint8_t control_word() const {
        uint8_t value = 0x01; // control word marker
        if (interrupt_enable)
            value |= 0x80;
        if (counter_mode)
            value |= 0x40;
        if (prescaler_256)
            value |= 0x20;
        if (edge_rising)
            value |= 0x10;
        if (external_trigger)
            value |= 0x08;
        if (expect_time_constant)
            value |= 0x04;
        return value;
    }
};

struct Settings {
    std::string irq = "IRQ";
    bool intack_enable = true;
    uint64_t intack_port = 0;
    std::array<std::string, kChannelCount> clk_trg_signals{};
};

struct Ctc {
    const ShouryoHost *host = nullptr;
    const SrhHostSignalsV1 *signals = nullptr;
    SrhHandle owner = 0;
    SrhHandle normal_mapping = 0;
    SrhHandle intack_mapping = 0;
    SrhHandle irq_signal = 0;
    uint64_t base = 0;
    uint64_t intack_port = 0;
    bool intack_enabled = true;

    std::array<Channel, kChannelCount> channels{};
    uint8_t vector = 0;

    std::array<bool, kChannelCount> clk_trg_level{};
    std::array<ClkTrgContext, kChannelCount> clk_trg_contexts{};

    std::string state_cache;

    bool any_pending() const {
        for (const auto &channel : channels) {
            if (channel.interrupt_pending)
                return true;
        }
        return false;
    }

    SrhStatus update_irq() {
        if (!irq_signal)
            return SRH_OK;
        if (any_pending()) {
            if (!host->signal_drive)
                return SRH_UNAVAILABLE;
            return host->signal_drive(host->context, owner, irq_signal, kIrqAssertedMv, 0);
        }
        if (signals && signals->release)
            return signals->release(signals->context, owner, irq_signal);
        if (host->signal_drive)
            return host->signal_drive(host->context, owner, irq_signal, 0, 0);
        return SRH_OK;
    }

    void set_interrupt_pending(uint32_t channel) {
        channels[channel].interrupt_pending = true;
        update_irq();
    }

    void decrement_counter(uint32_t channel) {
        auto &ch = channels[channel];
        if (ch.down_counter <= 1) {
            ch.down_counter = ch.load_value();
            ++ch.zero_count;
            if (ch.interrupt_enable)
                set_interrupt_pending(channel);
        } else {
            --ch.down_counter;
        }
    }

    void start_timer(uint32_t channel) {
        auto &ch = channels[channel];
        ch.down_counter = ch.load_value();
        ch.prescaler = static_cast<uint16_t>(ch.prescaler_period() - 1);
        ch.running = true;
        ch.waiting_trigger = false;
    }

    void load_time_constant(uint32_t channel, uint8_t value) {
        auto &ch = channels[channel];
        ch.time_constant = value;
        ch.has_time_constant = true;
        if (ch.counter_mode) {
            ch.down_counter = ch.load_value();
            ch.running = true;
            ch.waiting_trigger = false;
            return;
        }
        if (ch.external_trigger) {
            ch.down_counter = ch.load_value();
            ch.prescaler = static_cast<uint16_t>(ch.prescaler_period() - 1);
            ch.running = false;
            ch.waiting_trigger = true;
        } else {
            start_timer(channel);
        }
    }

    void write_control(uint32_t channel, uint8_t value) {
        auto &ch = channels[channel];
        const bool reset = (value & 0x02) != 0;
        ch.interrupt_enable = (value & 0x80) != 0;
        ch.counter_mode = (value & 0x40) != 0;
        ch.prescaler_256 = (value & 0x20) != 0;
        ch.edge_rising = (value & 0x10) != 0;
        ch.external_trigger = (value & 0x08) != 0;
        ch.expect_time_constant = (value & 0x04) != 0;

        if (reset) {
            ch.running = false;
            ch.waiting_trigger = false;
            ch.down_counter = 0;
            ch.prescaler = 0;
            ch.interrupt_pending = false;
        }
        if (!ch.interrupt_enable)
            ch.interrupt_pending = false;
        update_irq();
    }

    void write_channel(uint32_t channel, uint8_t value) {
        auto &ch = channels[channel];
        if (ch.expect_time_constant) {
            ch.expect_time_constant = false;
            load_time_constant(channel, value);
            return;
        }
        if ((value & 0x01) == 0) {
            if (channel == 0)
                vector = static_cast<uint8_t>(value & 0xF8);
            return;
        }
        write_control(channel, value);
    }

    uint8_t read_channel(uint32_t channel) const {
        return static_cast<uint8_t>(channels[channel].down_counter & 0xFF);
    }

    int highest_priority_pending() const {
        for (uint32_t channel = 0; channel < kChannelCount; ++channel) {
            if (channels[channel].interrupt_pending)
                return static_cast<int>(channel);
        }
        return -1;
    }

    void force_irq_edge() {
        if (!irq_signal)
            return;
        if (signals && signals->release)
            signals->release(signals->context, owner, irq_signal);
        if (host->signal_drive)
            host->signal_drive(host->context, owner, irq_signal, kIrqAssertedMv, 0);
    }

    uint8_t read_intack(bool consume) {
        const int channel = highest_priority_pending();
        if (channel < 0)
            return 0xFF;
        const uint8_t result =
            static_cast<uint8_t>((vector & 0xF8) | (static_cast<uint8_t>(channel) << 1));
        if (consume) {
            channels[static_cast<uint32_t>(channel)].interrupt_pending = false;
            // The Z80 card's IRQ input observes a rising edge. If another CTC
            // channel already has a pending request, pulse the line so the
            // next request is not hidden by the still-asserted level.
            if (any_pending())
                force_irq_edge();
            else
                update_irq();
        }
        return result;
    }

    void external_edge(uint32_t channel, bool rising) {
        auto &ch = channels[channel];
        if (ch.counter_mode) {
            if (ch.has_time_constant && ch.running && rising == ch.edge_rising)
                decrement_counter(channel);
            return;
        }
        if (ch.external_trigger && ch.waiting_trigger && rising == ch.edge_rising)
            start_timer(channel);
    }

    void tick() {
        for (uint32_t channel = 0; channel < kChannelCount; ++channel) {
            auto &ch = channels[channel];
            if (!ch.running || ch.counter_mode || ch.waiting_trigger)
                continue;
            if (ch.prescaler == 0) {
                ch.prescaler = static_cast<uint16_t>(ch.prescaler_period() - 1);
                decrement_counter(channel);
            } else {
                --ch.prescaler;
            }
        }
    }

    void reset() {
        channels = {};
        vector = 0;
        state_cache.clear();
        for (auto &level : clk_trg_level)
            level = false;
        update_irq();
    }

    std::string serialize() const {
        Json root;
        root["vector"] = vector;
        Json channels_json = Json::array();
        for (const auto &ch : channels) {
            channels_json.push_back(
                {{"interrupt_enable", ch.interrupt_enable},
                 {"counter_mode", ch.counter_mode},
                 {"prescaler_256", ch.prescaler_256},
                 {"edge_rising", ch.edge_rising},
                 {"external_trigger", ch.external_trigger},
                 {"expect_time_constant", ch.expect_time_constant},
                 {"has_time_constant", ch.has_time_constant},
                 {"running", ch.running},
                 {"waiting_trigger", ch.waiting_trigger},
                 {"interrupt_pending", ch.interrupt_pending},
                 {"time_constant", ch.time_constant},
                 {"down_counter", ch.down_counter},
                 {"prescaler", ch.prescaler},
                 {"zero_count", ch.zero_count}});
        }
        root["channels"] = std::move(channels_json);
        return root.dump();
    }

    static bool read_bool(const Json &object, const char *key, bool &out) {
        if (!object.contains(key) || !object[key].is_boolean())
            return false;
        out = object[key].get<bool>();
        return true;
    }

    static bool read_unsigned(const Json &object, const char *key, uint64_t max, uint64_t &out) {
        if (!object.contains(key) || !object[key].is_number_unsigned())
            return false;
        const uint64_t value = object[key].get<uint64_t>();
        if (value > max)
            return false;
        out = value;
        return true;
    }

    bool deserialize(const uint8_t *data, uint64_t size) {
        if (!data || size == 0)
            return false;
        const Json root = Json::parse(data, data + size, nullptr, false);
        if (root.is_discarded() || !root.is_object())
            return false;
        if (!root.contains("vector") || !root["vector"].is_number_unsigned() ||
            !root.contains("channels") || !root["channels"].is_array() ||
            root["channels"].size() != kChannelCount)
            return false;

        const uint64_t vector_value = root["vector"].get<uint64_t>();
        if (vector_value > 0xFF)
            return false;

        std::array<Channel, kChannelCount> next_channels{};
        for (uint32_t channel = 0; channel < kChannelCount; ++channel) {
            const Json &entry = root["channels"][channel];
            if (!entry.is_object())
                return false;
            Channel ch;
            uint64_t temporary = 0;
            if (!read_bool(entry, "interrupt_enable", ch.interrupt_enable) ||
                !read_bool(entry, "counter_mode", ch.counter_mode) ||
                !read_bool(entry, "prescaler_256", ch.prescaler_256) ||
                !read_bool(entry, "edge_rising", ch.edge_rising) ||
                !read_bool(entry, "external_trigger", ch.external_trigger) ||
                !read_bool(entry, "expect_time_constant", ch.expect_time_constant) ||
                !read_bool(entry, "has_time_constant", ch.has_time_constant) ||
                !read_bool(entry, "running", ch.running) ||
                !read_bool(entry, "waiting_trigger", ch.waiting_trigger) ||
                !read_bool(entry, "interrupt_pending", ch.interrupt_pending))
                return false;
            if (!read_unsigned(entry, "time_constant", 0xFF, temporary))
                return false;
            ch.time_constant = static_cast<uint8_t>(temporary);
            if (!read_unsigned(entry, "down_counter", 256, temporary))
                return false;
            ch.down_counter = static_cast<uint16_t>(temporary);
            if (!read_unsigned(entry, "prescaler", 0xFF, temporary))
                return false;
            ch.prescaler = static_cast<uint16_t>(temporary);
            if (!read_unsigned(entry, "zero_count", UINT64_MAX, temporary))
                return false;
            ch.zero_count = temporary;
            next_channels[channel] = ch;
        }

        channels = next_channels;
        vector = static_cast<uint8_t>(vector_value);
        return true;
    }
};

bool parse_settings(const SrhConfig *config, Settings &settings, std::string &error) {
    if (!srz80::sdk::has_field(config, &SrhConfig::config_json) || !config->config_json)
        return true;
    const Json root = Json::parse(config->config_json, config->config_json + config->config_json_size,
                                  nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        error = "config must be a JSON object";
        return false;
    }
    for (auto it = root.begin(); it != root.end(); ++it) {
        const std::string key = it.key();
        if (key != "irq" && key != "intack_enable" && key != "intack_port" &&
            key != "clk_trg_signals") {
            error = "unknown config key: " + key;
            return false;
        }
    }
    if (root.contains("irq")) {
        if (!root["irq"].is_string()) {
            error = "irq must be a string";
            return false;
        }
        settings.irq = root["irq"].get<std::string>();
        if (settings.irq.size() > 127) {
            error = "irq signal name is too long";
            return false;
        }
    }
    if (root.contains("intack_enable")) {
        if (!root["intack_enable"].is_boolean()) {
            error = "intack_enable must be a boolean";
            return false;
        }
        settings.intack_enable = root["intack_enable"].get<bool>();
    }
    if (root.contains("intack_port")) {
        if (!root["intack_port"].is_number_unsigned()) {
            error = "intack_port must be an unsigned integer";
            return false;
        }
        settings.intack_port = root["intack_port"].get<uint64_t>();
        if (settings.intack_port > 0xFFFF) {
            error = "intack_port must be in the 16-bit I/O range";
            return false;
        }
    }
    if (root.contains("clk_trg_signals")) {
        const Json &signals = root["clk_trg_signals"];
        if (!signals.is_array() || signals.size() != kChannelCount) {
            error = "clk_trg_signals must be an array of four signal names";
            return false;
        }
        for (uint32_t channel = 0; channel < kChannelCount; ++channel) {
            if (!signals[channel].is_string()) {
                error = "clk_trg_signals entries must be strings";
                return false;
            }
            settings.clk_trg_signals[channel] = signals[channel].get<std::string>();
            if (settings.clk_trg_signals[channel].size() > 127) {
                error = "clk_trg signal name is too long";
                return false;
            }
        }
    }
    return true;
}

void set_error_message(const SrhConfig *config, const std::string &message) {
    if (!config || !srz80::sdk::has_field(config, &SrhConfig::error_message) ||
        !config->error_message || config->error_message_capacity == 0)
        return;
    const size_t count = std::min<size_t>(message.size(), config->error_message_capacity - 1);
    if (count)
        std::memcpy(config->error_message, message.data(), count);
    config->error_message[count] = '\0';
}

SrhStatus SRH_CALL normal_read(void *context, uint64_t address, uint8_t *value) {
    return srz80::sdk::guard([&] {
        if (!value)
            return SRH_INVALID;
        auto &ctc = *static_cast<Ctc *>(context);
        if (address < ctc.base || address - ctc.base >= kPortCount)
            return SRH_INVALID;
        *value = ctc.read_channel(static_cast<uint32_t>(address - ctc.base));
        return SRH_OK;
    });
}

SrhStatus SRH_CALL normal_peek(void *context, uint64_t address, uint8_t *value) {
    return normal_read(context, address, value);
}

SrhStatus SRH_CALL normal_write(void *context, uint64_t address, uint8_t value) {
    return srz80::sdk::guard([&] {
        auto &ctc = *static_cast<Ctc *>(context);
        if (address < ctc.base || address - ctc.base >= kPortCount)
            return SRH_INVALID;
        ctc.write_channel(static_cast<uint32_t>(address - ctc.base), value);
        return SRH_OK;
    });
}

SrhStatus SRH_CALL intack_read(void *context, uint64_t address, uint8_t *value) {
    return srz80::sdk::guard([&] {
        if (!value)
            return SRH_INVALID;
        auto &ctc = *static_cast<Ctc *>(context);
        if (!ctc.intack_enabled || address != ctc.intack_port)
            return SRH_INVALID;
        *value = ctc.read_intack(true);
        return SRH_OK;
    });
}

SrhStatus SRH_CALL intack_peek(void *context, uint64_t address, uint8_t *value) {
    return srz80::sdk::guard([&] {
        if (!value)
            return SRH_INVALID;
        auto &ctc = *static_cast<Ctc *>(context);
        if (!ctc.intack_enabled || address != ctc.intack_port)
            return SRH_INVALID;
        *value = ctc.read_intack(false);
        return SRH_OK;
    });
}

SrhStatus SRH_CALL intack_write(void *, uint64_t, uint8_t) {
    // The vector port is a read-only INTACK alias. Ignore stray writes rather
    // than reporting a bus error.
    return SRH_OK;
}

SrhStatus SRH_CALL tick_callback(void *context) {
    return srz80::sdk::guard([&] {
        static_cast<Ctc *>(context)->tick();
        return SRH_OK;
    });
}

SrhStatus SRH_CALL clk_trg_callback(void *context, int32_t millivolts) {
    return srz80::sdk::guard([&] {
        auto &trg = *static_cast<ClkTrgContext *>(context);
        if (!trg.ctc)
            return SRH_INVALID;
        auto &ctc = *trg.ctc;
        const bool high = millivolts > kSignalHighMv;
        if (high != ctc.clk_trg_level[trg.channel])
            ctc.external_edge(trg.channel, high);
        ctc.clk_trg_level[trg.channel] = high;
        return SRH_OK;
    });
}

SrhStatus SRH_CALL create(const ShouryoHost *host, SrhHandle owner, const SrhConfig *config,
                          void **result) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!result)
            return SRH_INVALID;
        *result = nullptr;
        if (!srz80::sdk::valid(host) || !srz80::sdk::valid(config) || !config->space ||
            !host->map || !host->subscribe_clock || config->size != kPortCount ||
            config->base > UINT64_MAX - (kPortCount - 1))
            return SRH_INVALID;
        if (config->clock > 2) {
            set_error_message(config, "clock index must be 0, 1, or 2");
            return SRH_INVALID;
        }

        Settings settings;
        std::string error;
        if (!parse_settings(config, settings, error)) {
            set_error_message(config, error);
            return SRH_INVALID;
        }
        if (settings.intack_enable && settings.intack_port >= config->base &&
            settings.intack_port <= config->base + (kPortCount - 1)) {
            set_error_message(config, "intack_port overlaps the four CTC ports");
            return SRH_INVALID;
        }

        auto ctc = std::make_unique<Ctc>();
        ctc->host = host;
        ctc->owner = owner;
        ctc->base = config->base;
        ctc->intack_port = settings.intack_port;
        ctc->intack_enabled = settings.intack_enable;

        if (!settings.irq.empty()) {
            if (!srz80::sdk::has_field(host, &ShouryoHost::query) || !host->query) {
                set_error_message(config, "host query service is unavailable");
                return SRH_UNAVAILABLE;
            }
            const void *extension = nullptr;
            if (host->query(host->context, "host.signals.v1", &extension) != SRH_OK ||
                !extension) {
                set_error_message(config, "host.signals.v1 is unavailable");
                return SRH_UNAVAILABLE;
            }
            ctc->signals = static_cast<const SrhHostSignalsV1 *>(extension);
            if (!srz80::sdk::valid(ctc->signals) || !ctc->signals->release) {
                set_error_message(config, "host.signals.v1 is invalid");
                return SRH_UNAVAILABLE;
            }
            if (!host->signal_find ||
                host->signal_find(host->context, settings.irq.c_str(), &ctc->irq_signal) !=
                    SRH_OK ||
                !ctc->irq_signal) {
                set_error_message(config, "IRQ signal not found: " + settings.irq);
                return SRH_NOT_FOUND;
            }
        }

        SrhMapping normal_mapping{SRH_INIT(SrhMapping),
                                  config->space,
                                  config->base,
                                  config->base + (kPortCount - 1),
                                  config->priority,
                                  ctc.get(),
                                  normal_read,
                                  normal_write,
                                  normal_peek,
                                  nullptr};
        SrhStatus status = host->map(host->context, owner, &normal_mapping, &ctc->normal_mapping);
        if (status != SRH_OK)
            return status;

        if (ctc->intack_enabled) {
            SrhMapping intack_mapping{SRH_INIT(SrhMapping),
                                      config->space,
                                      ctc->intack_port,
                                      ctc->intack_port,
                                      config->priority,
                                      ctc.get(),
                                      intack_read,
                                      intack_write,
                                      intack_peek,
                                      nullptr};
            status = host->map(host->context, owner, &intack_mapping, &ctc->intack_mapping);
            if (status != SRH_OK)
                return status;
        }

        SrhHandle clock_subscription = 0;
        status = host->subscribe_clock(host->context, owner, config->clock, tick_callback,
                                       ctc.get(), &clock_subscription);
        if (status != SRH_OK)
            return status;

        ctc->reset();
        for (uint32_t channel = 0; channel < kChannelCount; ++channel) {
            const std::string &signal_name = settings.clk_trg_signals[channel];
            if (signal_name.empty())
                continue;
            if (!host->signal_find || !host->signal_subscribe) {
                set_error_message(config, "signal services are unavailable");
                return SRH_UNAVAILABLE;
            }
            SrhHandle signal = 0;
            status = host->signal_find(host->context, signal_name.c_str(), &signal);
            if (status != SRH_OK || !signal) {
                set_error_message(config, "CLK/TRG signal not found: " + signal_name);
                return SRH_NOT_FOUND;
            }
            ctc->clk_trg_contexts[channel].ctc = ctc.get();
            ctc->clk_trg_contexts[channel].channel = channel;
            SrhHandle subscription = 0;
            status = host->signal_subscribe(host->context, owner, signal, clk_trg_callback,
                                            &ctc->clk_trg_contexts[channel], &subscription);
            if (status != SRH_OK)
                return status;
            int32_t level = 0;
            if (host->signal_read && host->signal_read(host->context, signal, &level) == SRH_OK)
                ctc->clk_trg_level[channel] = level > kSignalHighMv;
        }

        *result = ctc.release();
        return SRH_OK;
    });
}

void SRH_CALL destroy(void *context) { delete static_cast<Ctc *>(context); }

SrhStatus SRH_CALL reset(void *context, uint32_t cold) {
    return srz80::sdk::guard([&] {
        (void)cold;
        static_cast<Ctc *>(context)->reset();
        return SRH_OK;
    });
}

uint32_t SRH_CALL property_count(void *) { return kPropertyCount; }

SrhStatus SRH_CALL property_info(void *, uint32_t index, SrhProperty *out) {
    if (!srz80::sdk::valid(out) || index >= kPropertyCount)
        return SRH_INVALID;
    static const char *const names[kPropertyCount] = {
        "base",
        "vector",
        "irq_pending",
        "ch0_control",
        "ch0_down_counter",
        "ch0_pending",
        "ch0_clk_trg",
        "ch1_control",
        "ch1_down_counter",
        "ch1_pending",
        "ch1_clk_trg",
        "ch2_control",
        "ch2_down_counter",
        "ch2_pending",
        "ch2_clk_trg",
        "ch3_control",
        "ch3_down_counter",
        "ch3_pending",
        "ch3_clk_trg"};
    static const char *const global_descriptions[] = {
        "Configured CTC I/O base", "Interrupt vector base",
        "Any channel has a pending timer interrupt"};
    const bool global = index < kGlobalProperties;
    const char *description = "";
    uint32_t kind = SRH_UNSIGNED;
    uint32_t bits = 64;
    uint32_t base = 10;
    uint32_t editable = 0;
    uint32_t flags = SRH_PROPERTY_RUNTIME;
    if (global) {
        description = global_descriptions[index];
    } else {
        const uint32_t field = (index - kGlobalProperties) % kChannelProperties;
        switch (field) {
        case 0:
            description = "Channel control word reconstructed from internal state";
            bits = 8;
            base = 16;
            break;
        case 1:
            description = "Current down-counter value (256 is read as 0)";
            bits = 16;
            break;
        case 2:
            description = "Channel interrupt request is pending";
            kind = SRH_BOOLEAN;
            bits = 1;
            break;
        case 3:
            description = "Write 1 for a rising CLK/TRG edge, 0 for a falling edge";
            kind = SRH_BOOLEAN;
            bits = 1;
            editable = 1;
            flags |= SRH_PROPERTY_LIVE_EDIT;
            break;
        default:
            return SRH_INVALID;
        }
    }
    if (global) {
        if (index == 0) {
            bits = 16;
            base = 16;
        } else if (index == 1) {
            bits = 8;
            base = 16;
        } else {
            kind = SRH_BOOLEAN;
            bits = 1;
        }
    }
    *out = {SRH_INIT(SrhProperty), names[index], "CTC", description, kind, bits, base, editable,
            nullptr, flags};
    return SRH_OK;
}

SrhStatus SRH_CALL property_get(void *context, uint32_t index, SrhValue *out) {
    return srz80::sdk::guard([&] {
        if (!srz80::sdk::valid(out))
            return SRH_INVALID;
        auto &ctc = *static_cast<Ctc *>(context);
        *out = {SRH_INIT(SrhValue), 0, 0, {}};
        if (index == 0) {
            out->unsigned_value = ctc.base;
            return SRH_OK;
        }
        if (index == 1) {
            out->unsigned_value = ctc.vector;
            return SRH_OK;
        }
        if (index == 2) {
            out->unsigned_value = ctc.any_pending();
            return SRH_OK;
        }
        if (index >= kPropertyCount)
            return SRH_INVALID;
        const uint32_t channel = (index - kGlobalProperties) / kChannelProperties;
        const uint32_t field = (index - kGlobalProperties) % kChannelProperties;
        const Channel &ch = ctc.channels[channel];
        switch (field) {
        case 0:
            out->unsigned_value = ch.control_word();
            break;
        case 1:
            out->unsigned_value = ch.down_counter;
            break;
        case 2:
            out->unsigned_value = ch.interrupt_pending;
            break;
        case 3:
            out->unsigned_value = 0;
            break;
        default:
            return SRH_INVALID;
        }
        return SRH_OK;
    });
}

SrhStatus SRH_CALL property_set(void *context, uint32_t index, const SrhValue *in) {
    return srz80::sdk::guard([&] {
        if (!srz80::sdk::valid(in) || index < kGlobalProperties || index >= kPropertyCount)
            return SRH_INVALID;
        auto &ctc = *static_cast<Ctc *>(context);
        const uint32_t channel = (index - kGlobalProperties) / kChannelProperties;
        const uint32_t field = (index - kGlobalProperties) % kChannelProperties;
        if (field != 3)
            return SRH_INVALID;
        if (in->unsigned_value > 1)
            return SRH_INVALID;
        ctc.external_edge(channel, in->unsigned_value != 0);
        return SRH_OK;
    });
}

SrhStatus SRH_CALL save_payload(void *context, uint8_t *buffer, uint64_t *size) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!size)
            return SRH_INVALID;
        auto &ctc = *static_cast<Ctc *>(context);
        const std::string payload = ctc.serialize();
        if (!buffer) {
            ctc.state_cache = payload;
            *size = ctc.state_cache.size();
            return SRH_OK;
        }
        if (*size < payload.size()) {
            *size = payload.size();
            return SRH_UNAVAILABLE;
        }
        if (!payload.empty())
            std::memcpy(buffer, payload.data(), payload.size());
        *size = payload.size();
        return SRH_OK;
    });
}

SrhStatus SRH_CALL load_payload(void *context, const uint8_t *buffer, uint64_t size) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!buffer || size == 0)
            return SRH_INVALID;
        auto &ctc = *static_cast<Ctc *>(context);
        if (!ctc.deserialize(buffer, size))
            return SRH_INVALID;
        return ctc.update_irq();
    });
}

const SrhCardDescriptor descriptor{SRH_INIT(SrhCardDescriptor),
                                   "I/O",
                                   "Z80 CTC",
                                   "Zilog Z80 CTC counter/timer with four channels and "
                                   "external system clock input",
                                   0x80,
                                   kPortCount,
                                   0,
                                   0,
                                   0,
                                   SRH_CARD_SHOW_CLOCK,
                                   R"({"irq":"IRQ","intack_enable":true,"intack_port":0})",
                                   nullptr,
                                   nullptr,
                                   nullptr,
                                   0};
using State = srz80::sdk::state::Callbacks<save_payload, load_payload, 1>;
const SrhPlugin api{SRH_INIT(SrhPlugin),
                    "ctc",
                    create,
                    destroy,
                    reset,
                    property_count,
                    property_info,
                    property_get,
                    property_set,
                    State::save,
                    State::load,
                    &descriptor,
                    nullptr,
                    nullptr};

} // namespace

extern "C" SRH_EXPORT const SrhPlugin *SRH_CALL srz80_plugin_init(const ShouryoHost *host) {
    return srz80::sdk::valid(host) ? &api : nullptr;
}
