#include <state.hpp>
#include <boundary.hpp>
#include <srz80/providers.h>
#include <srz80/signals.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <cstring>
#include <deque>
#include <memory>

namespace {
using Json = nlohmann::json;
std::atomic<uint64_t> epochs{1};
struct Event { uint64_t sequence, time; uint8_t byte; };
struct Midi {
    const ShouryoHost *host{};
    const SrhHostInputV1 *input_api{};
    const SrhHostSignalsV1 *signals{};
    const SrhHostProvidersV1 *providers{};
    SrhHandle owner{}, input{}, irq{};
    uint64_t base{}, epoch = epochs.fetch_add(1), sequence{};
    uint32_t rx_capacity = 256, tx_capacity = 4096;
    uint8_t defaults = 6, control = 6, errors{};
    std::string endpoint, cache;
    std::deque<std::pair<uint64_t, uint8_t>> rx;
    std::deque<Event> tx;
    bool pending() const { return (control & 3) == 3 && !rx.empty(); }
    SrhStatus update_irq() {
        return pending() ? host->signal_drive(host->context, owner, irq, 1, 0)
                         : signals->release(signals->context, owner, irq);
    }
    SrhStatus receive() {
        uint64_t time; uint8_t byte;
        while (input_api->input_pop(input_api->context, input, &time, &byte) == SRH_OK) {
            if (!(control & 2)) continue;
            if (rx.size() == rx_capacity) errors |= 4;
            else rx.emplace_back(time, byte);
        }
        return update_irq();
    }
    SrhStatus read(uint64_t address, uint8_t *value, bool peek) {
        if (!value || address < base || address - base > 3) return SRH_INVALID;
        switch (address - base) {
        case 0:
            *value = rx.empty() ? 0 : rx.front().second;
            if (!peek && !rx.empty()) { rx.pop_front(); return update_irq(); }
            break;
        case 1: *value = (rx.empty() ? 0 : 1) | ((control & 4) ? 2 : 0) | errors | (pending() ? 128 : 0); break;
        case 2: *value = control; break;
        case 3: *value = 1; break;
        }
        return SRH_OK;
    }
    SrhStatus write(uint64_t address, uint8_t value) {
        if (address < base || address - base > 3) return SRH_INVALID;
        switch (address - base) {
        case 0: {
            if (!(control & 4)) { errors |= 8; break; }
            if (sequence == UINT64_MAX) { epoch = epochs.fetch_add(1); sequence = 0; tx.clear(); }
            uint64_t now{};
            auto status = providers->simulation_time_ns(providers->context, &now);
            if (status != SRH_OK) return status;
            tx.push_back({sequence++, now, value});
            if (tx.size() > tx_capacity) { tx.pop_front(); errors |= 8; }
            break;
        }
        case 1: errors &= ~(value & 12); break;
        case 2:
            control = value & 7;
            if (value & 64) rx.clear();
            if (value & 128) tx.clear();
            return update_irq();
        }
        return SRH_OK;
    }
    void new_epoch() { epoch = epochs.fetch_add(1); tx.clear(); }
    std::string snapshot() const {
        // Build the flat event array directly. Constructing thousands of JSON
        // object maps at each publication dominates debug-build simulation cost.
        std::string events = "[";
        events.reserve(tx.size() * 72);
        constexpr char hex[] = "0123456789ABCDEF";
        for (const auto &e : tx) {
            if (events.size() > 1) events += ',';
            events += "{\"sequence\":" + std::to_string(e.sequence);
            events += ",\"time_ns\":" + std::to_string(e.time) + ",\"bytes_hex\":\"";
            events += hex[e.byte >> 4]; events += hex[e.byte & 15];
            events += "\"}";
        }
        events += ']';
        auto diagnostics = Json{{"schema", 1}, {"endpoint", endpoint}, {"epoch", epoch},
            {"oldest_sequence", tx.empty() ? sequence : tx.front().sequence}, {"next_sequence", sequence},
            {"rx_fill", rx.size()}, {"rx_capacity", rx_capacity}, {"tx_capacity", tx_capacity},
            {"rx_overrun", bool(errors & 4)}, {"tx_overrun", bool(errors & 8)},
            {"irq_enabled", bool(control & 1)}, {"irq_pending", pending()}}.dump();
        diagnostics.pop_back();
        return diagnostics + ",\"events\":" + events + "}";
    }
};
SrhStatus SRH_CALL snapshot(void *p, char *out, uint64_t *size) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!size) return SRH_INVALID;
        auto &m = *static_cast<Midi *>(p);
        if (!out) { m.cache = m.snapshot(); *size = m.cache.size() + 1; return SRH_OK; }
        if (*size < m.cache.size() + 1) { *size = m.cache.size() + 1; return SRH_UNAVAILABLE; }
        std::memcpy(out, m.cache.c_str(), m.cache.size() + 1);
        *size = m.cache.size() + 1;
        return SRH_OK;
    });
}
SrhStatus SRH_CALL read(void *p, uint64_t a, uint8_t *v) {
    return srz80::sdk::guard([&] { return static_cast<Midi *>(p)->read(a, v, false); });
}
SrhStatus SRH_CALL peek(void *p, uint64_t a, uint8_t *v) {
    return srz80::sdk::guard([&] { return static_cast<Midi *>(p)->read(a, v, true); });
}
SrhStatus SRH_CALL write(void *p, uint64_t a, uint8_t v) {
    return srz80::sdk::guard([&] { return static_cast<Midi *>(p)->write(a, v); });
}
SrhStatus SRH_CALL receive(void *p) {
    return srz80::sdk::guard([&] { return static_cast<Midi *>(p)->receive(); });
}
SrhStatus SRH_CALL create(const ShouryoHost *host, SrhHandle owner, const SrhConfig *config, void **out) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!srz80::sdk::valid(host) || !srz80::sdk::valid(config) || !out || !config->space || config->base > UINT64_MAX - 3)
            return SRH_INVALID;
        auto m = std::make_unique<Midi>();
        m->host = host; m->owner = owner; m->base = config->base;
        const void *ext{};
        if (host->query(host->context, "host.input.v1", &ext) != SRH_OK) return SRH_UNAVAILABLE;
        m->input_api = static_cast<const SrhHostInputV1 *>(ext);
        if (!srz80::sdk::valid(m->input_api) || !srz80::sdk::has_field(m->input_api, &SrhHostInputV1::subscribe_due) || !m->input_api->subscribe_due) return SRH_UNAVAILABLE;
        if (host->query(host->context, "host.signals.v1", &ext) != SRH_OK) return SRH_UNAVAILABLE;
        m->signals = static_cast<const SrhHostSignalsV1 *>(ext);
        if (!srz80::sdk::valid(m->signals) || !m->signals->release) return SRH_UNAVAILABLE;
        if (host->query(host->context, "host.providers.v1", &ext) != SRH_OK) return SRH_UNAVAILABLE;
        m->providers = static_cast<const SrhHostProvidersV1 *>(ext);
        if (!srz80::sdk::valid(m->providers)) return SRH_UNAVAILABLE;
        auto j = config->config_json ? Json::parse(config->config_json, config->config_json + config->config_json_size) : Json::object();
        if (!j.is_object()) return SRH_INVALID;
        auto capacity = [&](const char *name, uint32_t fallback, uint32_t max) {
            auto n = j.value(name, int64_t(fallback));
            if (n < 1 || n > max) throw std::invalid_argument(name);
            return static_cast<uint32_t>(n);
        };
        m->rx_capacity = capacity("rx_capacity", 256, 65536);
        m->tx_capacity = capacity("tx_capacity", 4096, 8192);
        m->endpoint = j.value("endpoint", "midi" + std::to_string(owner) + ".rx");
        if (m->endpoint.empty() || m->endpoint.size() > 127) return SRH_INVALID;
        m->defaults = (j.value("irq_enable", false) ? 1 : 0) | (j.value("rx_enable", true) ? 2 : 0) | (j.value("tx_enable", true) ? 4 : 0);
        m->control = m->defaults;
        const auto irq = j.value("irq", std::string("IRQ"));
        if (host->signal_find(host->context, irq.c_str(), &m->irq) != SRH_OK) {
            host->log(host->context, owner, ("MIDI: missing IRQ signal " + irq).c_str());
            return SRH_NOT_FOUND;
        }
        auto status = m->input_api->register_input(m->input_api->context, owner, m->endpoint.c_str(), 65536, &m->input);
        if (status != SRH_OK) return status;
        SrhHandle subscription{}, mapping{};
        status = m->input_api->subscribe_due(m->input_api->context, owner, m->input, receive, m.get(), &subscription);
        if (status != SRH_OK) return status;
        SrhMapping map{SRH_INIT(SrhMapping), config->space, m->base, m->base + 3, config->priority, m.get(), read, write, peek, nullptr};
        status = host->map(host->context, owner, &map, &mapping);
        if (status != SRH_OK) return status;
        SrhDataProviderV1 provider{};
        provider.abi_version = SRH_ABI; provider.struct_size = sizeof(provider);
        provider.context = m.get(); provider.snapshot = snapshot;
        provider.command = [](void *, uint32_t, uint64_t, const char *, uint64_t) -> SrhStatus { return SRH_UNAVAILABLE; };
        std::strcpy(provider.name, "MIDI"); std::strcpy(provider.protocol, "srz80.midi.v1");
        status = m->providers->register_provider(m->providers->context, owner, &provider);
        if (status != SRH_OK) return status;
        *out = m.release(); return SRH_OK;
    });
}
void SRH_CALL destroy(void *p) { delete static_cast<Midi *>(p); }
SrhStatus SRH_CALL reset(void *p, uint32_t) {
    return srz80::sdk::guard([&] {
        auto &m = *static_cast<Midi *>(p);
        m.rx.clear(); m.new_epoch(); m.errors = 0; m.control = m.defaults;
        return m.update_irq();
    });
}
SrhStatus SRH_CALL save_payload(void *p, uint8_t *out, uint64_t *size) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!size) return SRH_INVALID;
        auto &m = *static_cast<Midi *>(p);
        auto text = Json{{"control", m.control}, {"errors", m.errors}, {"sequence", m.sequence}, {"rx", m.rx}}.dump();
        auto capacity = *size; *size = text.size();
        if (!out) return SRH_OK;
        if (capacity < text.size()) return SRH_UNAVAILABLE;
        std::memcpy(out, text.data(), text.size()); return SRH_OK;
    });
}
SrhStatus SRH_CALL load_payload(void *p, const uint8_t *data, uint64_t size) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        auto &m = *static_cast<Midi *>(p);
        if (!data || !size || size > 4 * 1024 * 1024) return SRH_INVALID;
        auto j = Json::parse(data, data + size);
        auto control = j.at("control").get<int>(); auto errors = j.at("errors").get<int>();
        if (control < 0 || control > 7 || errors < 0 || (errors & ~12) ||
            !j.at("sequence").is_number_unsigned() || !j.at("rx").is_array() || j.at("rx").size() > m.rx_capacity) return SRH_INVALID;
        std::deque<std::pair<uint64_t, uint8_t>> rx;
        for (const auto &entry : j.at("rx")) {
            if (!entry.is_array() || entry.size() != 2 || !entry[0].is_number_unsigned() ||
                !entry[1].is_number_unsigned() || entry[1].get<uint64_t>() > 255) return SRH_INVALID;
            auto time = entry[0].get<uint64_t>();
            if (!rx.empty() && time < rx.back().first) return SRH_INVALID;
            rx.emplace_back(time, entry[1].get<uint8_t>());
        }
        auto sequence = j.at("sequence").get<uint64_t>();
        m.rx.swap(rx); m.control = control; m.errors = errors; m.sequence = sequence;
        m.new_epoch(); return m.update_irq();
    });
}
uint32_t SRH_CALL count(void *) { return 4; }
SrhStatus SRH_CALL info(void *, uint32_t index, SrhProperty *out) {
    if (!srz80::sdk::valid(out) || index >= 4) return SRH_INVALID;
    static const char *names[]{"base", "rx_fill", "tx_retained", "irq_pending"};
    static const char *descriptions[]{"Register base address", "Received bytes ready for the CPU",
        "Bytes retained for output consumers", "RX level interrupt asserted"};
    *out = {SRH_INIT(SrhProperty), names[index], "MIDI", descriptions[index],
        static_cast<uint32_t>(index == 3 ? SRH_BOOLEAN : SRH_UNSIGNED), 64, index == 0 ? 16u : 10u,
        0, nullptr, static_cast<uint32_t>(SRH_PROPERTY_RUNTIME)};
    return SRH_OK;
}
SrhStatus SRH_CALL get(void *p, uint32_t index, SrhValue *out) {
    if (!srz80::sdk::valid(out) || index >= 4) return SRH_INVALID;
    const auto &m = *static_cast<Midi *>(p);
    const uint64_t values[]{m.base, m.rx.size(), m.tx.size(), m.pending()};
    out->unsigned_value = values[index];
    return SRH_OK;
}
SrhStatus SRH_CALL set(void *, uint32_t, const SrhValue *) { return SRH_INVALID; }
const SrhCardDescriptor descriptor{SRH_INIT(SrhCardDescriptor), "I/O", "MIDI 1.0", "Byte-transparent MIDI port with a CPU-independent level IRQ",
    0x90, 4, 0, 0, 0, 0, R"({"irq":"IRQ","rx_capacity":256,"tx_capacity":4096})", nullptr, nullptr};
using State = srz80::sdk::state::Callbacks<save_payload, load_payload, 1>;
const SrhPlugin api{SRH_INIT(SrhPlugin), "midi", create, destroy, reset, count, info, get, set, State::save, State::load, &descriptor, nullptr, nullptr};
}
extern "C" SRH_EXPORT const SrhPlugin *SRH_CALL srz80_plugin_init(const ShouryoHost *host) {
    return srz80::sdk::valid(host) ? &api : nullptr;
}
