#include <state.hpp>
#include <boundary.hpp>
#include <nlohmann/json.hpp>
#include <srz80/providers.h>
#include <srz80/signals.h>

#include <array>
#include <cstring>
#include <deque>
#include <memory>
#include <string>

namespace {
using Json = nlohmann::json;

struct Event { uint8_t usage, flags; };

struct Keyboard {
    const ShouryoHost *host{};
    const SrhHostInputV1 *input_api{};
    const SrhHostSignalsV1 *signals{};
    const SrhHostProvidersV1 *providers{};
    SrhHandle owner{}, input{}, irq{};
    uint64_t base{};
    uint32_t rx_capacity = 64;
    // bit 0 IRQ enable, bit 1 RX enable. Bits 6/7 strobe clear FIFO/errors.
    uint8_t defaults = 2, control = 2, errors{}, key_index{}, last_flags{};
    bool have_packet_byte = false;
    uint8_t packet_usage{};
    std::array<uint8_t, 32> held{};
    std::deque<Event> rx;
    std::string endpoint, cache;

    bool enabled() const { return (control & 2) != 0; }
    bool pending() const { return enabled() && (control & 1) && !rx.empty(); }
    bool held_key(uint8_t usage) const { return (held[usage / 8] & (1u << (usage % 8))) != 0; }
    void set_held(uint8_t usage, bool down) {
        auto &bits = held[usage / 8];
        const uint8_t mask = uint8_t(1u << (usage % 8));
        if (down) bits |= mask;
        else bits &= uint8_t(~mask);
    }
    SrhStatus update_irq() {
        if (!irq) return SRH_OK;
        return pending() ? host->signal_drive(host->context, owner, irq, 1, 0)
                         : signals->release(signals->context, owner, irq);
    }
    void accept(uint8_t usage, uint8_t flags) {
        if (!enabled() || usage == 0) return;
        const bool down = (flags & 1) != 0;
        // A repeat does not change held state, but remains observable by a
        // text-oriented guest that wants host typematic behavior.
        if (!(flags & 2) && held_key(usage) == down) return;
        set_held(usage, down);
        if (rx.size() == rx_capacity) { errors |= 2; return; }
        rx.push_back({usage, uint8_t(flags & 3)});
    }
    SrhStatus receive() {
        uint64_t timestamp{};
        uint8_t byte{};
        while (input_api->input_pop(input_api->context, input, &timestamp, &byte) == SRH_OK) {
            (void)timestamp;
            if (!have_packet_byte) { packet_usage = byte; have_packet_byte = true; }
            else { accept(packet_usage, byte); have_packet_byte = false; }
        }
        return update_irq();
    }
    SrhStatus read(uint64_t address, uint8_t *value, bool peek) {
        if (!value || address < base || address - base > 0x3f) return SRH_INVALID;
        const auto offset = address - base;
        if (offset == 0) {
            *value = rx.empty() ? 0 : rx.front().usage;
            if (!peek && !rx.empty()) { last_flags = rx.front().flags; rx.pop_front(); return update_irq(); }
        } else if (offset == 1) *value = last_flags;
        else if (offset == 2) *value = uint8_t((rx.empty() ? 0 : 1) | (errors & 2) | (pending() ? 0x80 : 0));
        else if (offset == 3) *value = control;
        else if (offset == 4) *value = key_index;
        else if (offset == 5) *value = held_key(key_index) ? 1 : 0;
        else if (offset == 6) *value = 1; // keyboard controller protocol version
        else if (offset >= 0x20) *value = held[offset - 0x20];
        else *value = 0;
        return SRH_OK;
    }
    SrhStatus write(uint64_t address, uint8_t value) {
        if (address < base || address - base > 0x3f) return SRH_INVALID;
        switch (address - base) {
        case 2: errors &= uint8_t(~(value & 2)); break;
        case 3:
            control = value & 3;
            if (value & 0x40) { rx.clear(); have_packet_byte = false; }
            if (value & 0x80) errors = 0;
            return update_irq();
        case 4: key_index = value; break;
        default: break;
        }
        return SRH_OK;
    }
    std::string snapshot() const {
        return Json{{"schema", 1}, {"endpoint", endpoint}, {"rx_fill", rx.size()},
                    {"rx_capacity", rx_capacity}, {"overflow", bool(errors & 2)},
                    {"irq_enabled", bool(control & 1)}, {"irq_pending", pending()},
                    {"held_count", [&] { uint32_t n = 0; for (uint8_t b : held) for (; b; b &= uint8_t(b - 1)) ++n; return n; }()}}
            .dump();
    }
};

SrhStatus SRH_CALL snapshot(void *p, char *out, uint64_t *size) {
    return srz80::sdk::guard([&] {
        if (!size) return SRH_INVALID;
        auto &keyboard = *static_cast<Keyboard *>(p);
        if (!out) { keyboard.cache = keyboard.snapshot(); *size = keyboard.cache.size() + 1; return SRH_OK; }
        if (*size < keyboard.cache.size() + 1) { *size = keyboard.cache.size() + 1; return SRH_UNAVAILABLE; }
        std::memcpy(out, keyboard.cache.c_str(), keyboard.cache.size() + 1); *size = keyboard.cache.size() + 1;
        return SRH_OK;
    });
}
SrhStatus SRH_CALL read(void *p, uint64_t a, uint8_t *v) { return srz80::sdk::guard([&] { return static_cast<Keyboard *>(p)->read(a, v, false); }); }
SrhStatus SRH_CALL peek(void *p, uint64_t a, uint8_t *v) { return srz80::sdk::guard([&] { return static_cast<Keyboard *>(p)->read(a, v, true); }); }
SrhStatus SRH_CALL write(void *p, uint64_t a, uint8_t v) { return srz80::sdk::guard([&] { return static_cast<Keyboard *>(p)->write(a, v); }); }
SrhStatus SRH_CALL receive(void *p) { return srz80::sdk::guard([&] { return static_cast<Keyboard *>(p)->receive(); }); }

SrhStatus SRH_CALL create(const ShouryoHost *host, SrhHandle owner, const SrhConfig *config, void **out) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!srz80::sdk::valid(host) || !srz80::sdk::valid(config) || !out || !config->space || config->base > UINT64_MAX - 0x3f) return SRH_INVALID;
        auto keyboard = std::make_unique<Keyboard>(); keyboard->host = host; keyboard->owner = owner; keyboard->base = config->base;
        const void *extension{};
        if (host->query(host->context, "host.input.v1", &extension) != SRH_OK) return SRH_UNAVAILABLE;
        keyboard->input_api = static_cast<const SrhHostInputV1 *>(extension);
        if (!srz80::sdk::valid(keyboard->input_api) || !srz80::sdk::has_field(keyboard->input_api, &SrhHostInputV1::subscribe_due) || !keyboard->input_api->subscribe_due) return SRH_UNAVAILABLE;
        if (host->query(host->context, "host.providers.v1", &extension) != SRH_OK) return SRH_UNAVAILABLE;
        keyboard->providers = static_cast<const SrhHostProvidersV1 *>(extension);
        if (!srz80::sdk::valid(keyboard->providers)) return SRH_UNAVAILABLE;
        auto json = config->config_json ? Json::parse(config->config_json, config->config_json + config->config_json_size) : Json::object();
        if (!json.is_object()) return SRH_INVALID;
        auto capacity = json.value("rx_capacity", int64_t(64));
        if (capacity < 1 || capacity > 32768) return SRH_INVALID;
        keyboard->rx_capacity = static_cast<uint32_t>(capacity);
        keyboard->endpoint = json.value("endpoint", "keyboard" + std::to_string(owner) + ".events");
        if (keyboard->endpoint.empty() || keyboard->endpoint.size() > 127) return SRH_INVALID;
        keyboard->defaults = uint8_t(2 | (json.value("irq_enable", false) ? 1 : 0)); keyboard->control = keyboard->defaults;
        if (json.contains("irq")) {
            if (host->query(host->context, "host.signals.v1", &extension) != SRH_OK) return SRH_UNAVAILABLE;
            keyboard->signals = static_cast<const SrhHostSignalsV1 *>(extension);
            if (!srz80::sdk::valid(keyboard->signals) || !keyboard->signals->release || !json["irq"].is_string() ||
                host->signal_find(host->context, json["irq"].get_ref<const std::string &>().c_str(), &keyboard->irq) != SRH_OK) return SRH_NOT_FOUND;
        }
        SrhHandle subscription{}, mapping{};
        auto status = keyboard->input_api->register_input(keyboard->input_api->context, owner, keyboard->endpoint.c_str(), 65536, &keyboard->input);
        if (status != SRH_OK) return status;
        status = keyboard->input_api->subscribe_due(keyboard->input_api->context, owner, keyboard->input, receive, keyboard.get(), &subscription);
        if (status != SRH_OK) return status;
        SrhMapping map{SRH_INIT(SrhMapping), config->space, keyboard->base, keyboard->base + 0x3f, config->priority, keyboard.get(), read, write, peek, nullptr};
        if ((status = host->map(host->context, owner, &map, &mapping)) != SRH_OK) return status;
        SrhDataProviderV1 provider{SRH_INIT(SrhDataProviderV1), keyboard.get(), snapshot,
            [](void *, uint32_t, uint64_t, const char *, uint64_t) -> SrhStatus { return SRH_UNAVAILABLE; }, {}, {}, 0};
        std::strcpy(provider.name, "Keyboard"); std::strcpy(provider.protocol, "srz80.keyboard.v1");
        if ((status = keyboard->providers->register_provider(keyboard->providers->context, owner, &provider)) != SRH_OK) return status;
        *out = keyboard.release(); return SRH_OK;
    });
}
void SRH_CALL destroy(void *p) { delete static_cast<Keyboard *>(p); }
SrhStatus SRH_CALL reset(void *p, uint32_t) { return srz80::sdk::guard([&] { auto &k = *static_cast<Keyboard *>(p); k.rx.clear(); k.held.fill(0); k.have_packet_byte = false; k.errors = k.last_flags = 0; k.control = k.defaults; return k.update_irq(); }); }
SrhStatus SRH_CALL save_payload(void *p, uint8_t *out, uint64_t *size) {
    return srz80::sdk::guard([&] { if (!size) return SRH_INVALID; auto &k = *static_cast<Keyboard *>(p); auto text = Json{{"control", k.control}, {"errors", k.errors}, {"last_flags", k.last_flags}, {"held", k.held}, {"rx", Json::array()}}; for (const auto &e : k.rx) text["rx"].push_back({e.usage, e.flags}); const auto encoded = text.dump(); const auto capacity = *size; *size = encoded.size(); if (!out) return SRH_OK; if (capacity < encoded.size()) return SRH_UNAVAILABLE; std::memcpy(out, encoded.data(), encoded.size()); return SRH_OK; });
}
SrhStatus SRH_CALL load_payload(void *p, const uint8_t *data, uint64_t size) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!p || !data || !size || size > 4 * 1024 * 1024) return SRH_INVALID;
        auto &keyboard = *static_cast<Keyboard *>(p);
        const auto state = Json::parse(data, data + size, nullptr, false);
        if (state.is_discarded() || !state.is_object() || !state.contains("held") ||
            !state["held"].is_array() || state["held"].size() != keyboard.held.size() ||
            !state.contains("rx") || !state["rx"].is_array() || state["rx"].size() > keyboard.rx_capacity) return SRH_INVALID;
        const auto control = state.value("control", 256); const auto errors = state.value("errors", 256);
        const auto last_flags = state.value("last_flags", 256);
        if (control < 0 || control > 3 || errors < 0 || errors > 2 || last_flags < 0 || last_flags > 3) return SRH_INVALID;
        std::array<uint8_t, 32> held{};
        for (size_t i = 0; i < held.size(); ++i) {
            if (!state["held"][i].is_number_unsigned() || state["held"][i].get<uint64_t>() > 255) return SRH_INVALID;
            held[i] = state["held"][i].get<uint8_t>();
        }
        std::deque<Event> rx;
        for (const auto &entry : state["rx"]) {
            if (!entry.is_array() || entry.size() != 2 || !entry[0].is_number_unsigned() || !entry[1].is_number_unsigned() ||
                entry[0].get<uint64_t>() > 255 || entry[1].get<uint64_t>() > 3) return SRH_INVALID;
            rx.push_back({entry[0].get<uint8_t>(), entry[1].get<uint8_t>()});
        }
        keyboard.held = held; keyboard.rx.swap(rx); keyboard.control = static_cast<uint8_t>(control);
        keyboard.errors = static_cast<uint8_t>(errors); keyboard.last_flags = static_cast<uint8_t>(last_flags);
        keyboard.have_packet_byte = false;
        return keyboard.update_irq();
    });
}
uint32_t SRH_CALL count(void *) { return 5; }
SrhStatus SRH_CALL info(void *, uint32_t index, SrhProperty *out) {
    if (!srz80::sdk::valid(out) || index >= count(nullptr)) return SRH_INVALID;
    static const char *names[]{"base", "rx_fill", "overflow", "irq_pending", "held_keys"};
    *out = {SRH_INIT(SrhProperty), names[index], "Keyboard", "Keyboard controller state", index == 2 || index == 3 ? SRH_BOOLEAN : SRH_UNSIGNED, 32, index == 0 ? 16u : 10u, 0, nullptr, 0}; return SRH_OK;
}
SrhStatus SRH_CALL get(void *p, uint32_t index, SrhValue *out) { if (!srz80::sdk::valid(out) || index >= count(nullptr)) return SRH_INVALID; auto &k = *static_cast<Keyboard *>(p); if (index == 0) out->unsigned_value = k.base; else if (index == 1) out->unsigned_value = k.rx.size(); else if (index == 2) out->unsigned_value = (k.errors & 2) != 0; else if (index == 3) out->unsigned_value = k.pending(); else { uint32_t n = 0; for (uint8_t b : k.held) for (; b; b &= uint8_t(b - 1)) ++n; out->unsigned_value = n; } return SRH_OK; }
SrhStatus SRH_CALL set(void *, uint32_t, const SrhValue *) { return SRH_INVALID; }
const SrhCardDescriptor descriptor{SRH_INIT(SrhCardDescriptor), "I/O", "Keyboard controller", "HID key events, held-key bitmap, and optional level IRQ", 0xA0, 64, 0, 0, 0, 0, R"({"rx_capacity":64})", nullptr, nullptr, nullptr, 0};
using State = srz80::sdk::state::Callbacks<save_payload, load_payload, 1>;
const SrhPlugin api{SRH_INIT(SrhPlugin), "keyboard", create, destroy, reset, count, info, get, set, State::save, State::load, &descriptor, nullptr, nullptr};
}
extern "C" SRH_EXPORT const SrhPlugin *SRH_CALL srz80_plugin_init(const ShouryoHost *host) { return srz80::sdk::valid(host) ? &api : nullptr; }
