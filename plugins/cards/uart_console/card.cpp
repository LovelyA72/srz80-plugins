#include <boundary.hpp>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {
struct Json {
    const char *text;
    size_t size;
    Json(const char *t, size_t n) : text(t ? t : ""), size(t ? n : 0) {}
    void skip_ws(size_t &i) const {
        while (i < size && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r'))
            ++i;
    }
    bool match(size_t &i, char c) const {
        skip_ws(i);
        if (i < size && text[i] == c) {
            ++i;
            return true;
        }
        return false;
    }
    bool object(size_t &i) const {
        if (!match(i, '{'))
            return false;
        if (match(i, '}'))
            return true;
        while (true) {
            if (!string(i))
                return false;
            if (!match(i, ':'))
                return false;
            if (!value(i))
                return false;
            if (match(i, '}'))
                return true;
            if (!match(i, ','))
                return false;
        }
    }
    bool string(size_t &i) const {
        skip_ws(i);
        if (i >= size || text[i] != '"')
            return false;
        ++i;
        while (i < size && text[i] != '"') {
            if (text[i] == '\\')
                ++i;
            ++i;
        }
        if (i >= size)
            return false;
        ++i;
        return true;
    }
    bool value(size_t &i) const {
        skip_ws(i);
        if (i >= size)
            return false;
        char c = text[i];
        if (c == '"')
            return string(i);
        if (c == '{')
            return object(i);
        if (c == '[') {
            ++i;
            if (match(i, ']'))
                return true;
            while (true) {
                if (!value(i))
                    return false;
                if (match(i, ']'))
                    return true;
                if (!match(i, ','))
                    return false;
            }
        }
        if (c == 't' && i + 4 <= size && std::strncmp(text + i, "true", 4) == 0) {
            i += 4;
            return true;
        }
        if (c == 'f' && i + 5 <= size && std::strncmp(text + i, "false", 5) == 0) {
            i += 5;
            return true;
        }
        if (c == 'n' && i + 4 <= size && std::strncmp(text + i, "null", 4) == 0) {
            i += 4;
            return true;
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            ++i;
            while (i < size && ((text[i] >= '0' && text[i] <= '9') || text[i] == '.' || text[i] == 'e' ||
                                text[i] == 'E' || text[i] == '+' || text[i] == '-'))
                ++i;
            return true;
        }
        return false;
    }
};

struct Uart {
    const ShouryoHost *host;
    const SrhHostInputV1 *input_api;
    SrhHandle owner, space, mapping = 0, input = 0;
    uint64_t base = 0;
    size_t tx_capacity = 64 * 1024;
    std::vector<uint8_t> transcript;
    bool overflow = false;

    static uint64_t get_uint(const SrhConfig *config, const char *key, uint64_t fallback) {
        if (!srz80::sdk::has_field(config, &SrhConfig::config_json) || !config->config_json)
            return fallback;
        Json json(config->config_json, config->config_json_size);
        size_t i = 0;
        if (!json.object(i))
            return fallback;
        // The compact dump produced by the host loader is easy to scan for
        // "key":<number>.  Unknown keys are intentionally ignored.
        std::string needle = std::string("\"") + key + "\"";
        size_t pos = 0;
        while ((pos = std::string(config->config_json, config->config_json_size).find(needle, pos)) !=
               std::string::npos) {
            pos += needle.size();
            size_t j = pos;
            json.skip_ws(j);
            if (j < json.size && json.text[j] == ':') {
                ++j;
                json.skip_ws(j);
                if (j < json.size && json.text[j] >= '0' && json.text[j] <= '9') {
                    uint64_t value = 0;
                    while (j < json.size && json.text[j] >= '0' && json.text[j] <= '9') {
                        value = value * 10 + static_cast<uint64_t>(json.text[j] - '0');
                        ++j;
                    }
                    return value;
                }
            }
            pos = j;
        }
        return fallback;
    }
    static std::string get_string(const SrhConfig *config, const char *key,
                                  const std::string &fallback) {
        if (!srz80::sdk::has_field(config, &SrhConfig::config_json) || !config->config_json)
            return fallback;
        std::string src(config->config_json, config->config_json_size);
        std::string needle = std::string("\"") + key + "\"";
        size_t pos = src.find(needle);
        if (pos == std::string::npos)
            return fallback;
        pos += needle.size();
        while (pos < src.size() && (src[pos] == ' ' || src[pos] == '\t' || src[pos] == '\n'))
            ++pos;
        if (pos >= src.size() || src[pos] != ':')
            return fallback;
        ++pos;
        while (pos < src.size() && (src[pos] == ' ' || src[pos] == '\t' || src[pos] == '\n'))
            ++pos;
        if (pos >= src.size() || src[pos] != '"')
            return fallback;
        ++pos;
        std::string out;
        while (pos < src.size() && src[pos] != '"') {
            if (src[pos] == '\\' && pos + 1 < src.size()) {
                ++pos;
                if (src[pos] == 'n')
                    out += '\n';
                else if (src[pos] == 'r')
                    out += '\r';
                else if (src[pos] == 't')
                    out += '\t';
                else
                    out += src[pos];
                ++pos;
            } else
                out += src[pos++];
        }
        return out;
    }

    SrhStatus query_text(uint64_t offset, uint8_t *out, uint32_t *size, uint32_t *total) const {
        if (!out || !size || !total)
            return SRH_INVALID;
        *total = static_cast<uint32_t>(transcript.size());
        if (offset >= transcript.size()) {
            *size = 0;
            return SRH_OK;
        }
        auto available = transcript.size() - offset;
        auto n = std::min<uint64_t>(available, *size);
        std::memcpy(out, transcript.data() + offset, static_cast<size_t>(n));
        *size = static_cast<uint32_t>(n);
        return SRH_OK;
    }
    SrhStatus read_port(uint64_t address, uint8_t *value) {
        if (!value || address < base || address - base > 1)
            return SRH_INVALID;
        if (address == base) {
            uint64_t due = 0;
            auto status = input_api->input_due(input_api->context, input, &due);
            if (status != SRH_OK)
                return status;
            if (due) {
                uint64_t timestamp = 0;
                status = input_api->input_pop(input_api->context, input, &timestamp, value);
                if (status != SRH_OK)
                    return status;
            } else {
                *value = 0;
            }
            return SRH_OK;
        }
        *value = static_cast<uint8_t>((due_now() ? 1 : 0) | 2 | (overflow ? 4 : 0));
        return SRH_OK;
    }
    SrhStatus write_port(uint64_t address, uint8_t value) {
        if (address < base || address - base > 1)
            return SRH_INVALID;
        if (address == base) {
            if (transcript.size() < tx_capacity)
                transcript.push_back(value);
            else
                host->log(host->context, owner, "UART transcript full; byte dropped");
            return SRH_OK;
        }
        if (value & 1)
            overflow = false;
        if (value & 2)
            transcript.clear();
        return SRH_OK;
    }
    uint64_t due_now() const {
        uint64_t due = 0;
        if (input_api->input_due(input_api->context, input, &due) != SRH_OK)
            due = 0;
        return due;
    }
};

SrhStatus SRH_CALL read(void *p, uint64_t address, uint8_t *value) {
    return static_cast<Uart *>(p)->read_port(address, value);
}
SrhStatus SRH_CALL write(void *p, uint64_t address, uint8_t value) {
    return static_cast<Uart *>(p)->write_port(address, value);
}
SrhStatus SRH_CALL text_query(void *p, uint64_t offset, uint8_t *out, uint32_t *size,
                              uint32_t *total) {
    return srz80::sdk::guard([&] {
        return static_cast<Uart *>(p)->query_text(offset, out, size, total);
    });
}
SrhStatus SRH_CALL create(const ShouryoHost *host, SrhHandle owner, const SrhConfig *config,
                          void **result) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!srz80::sdk::valid(host) || !srz80::sdk::valid(config) || !result || !config->space)
            return SRH_INVALID;
        const void *extension = nullptr;
        if (host->query(host->context, "host.input.v1", &extension) != SRH_OK || !extension)
            return SRH_UNAVAILABLE;
        auto input = static_cast<const SrhHostInputV1 *>(extension);
        if (!srz80::sdk::valid(input))
            return SRH_UNAVAILABLE;
        extension = nullptr;
        if (host->query(host->context, "host.text.v1", &extension) != SRH_OK || !extension)
            return SRH_UNAVAILABLE;
        auto text = static_cast<const SrhHostTextV1 *>(extension);
        if (!srz80::sdk::valid(text))
            return SRH_UNAVAILABLE;
        auto uart = std::make_unique<Uart>();
        uart->host = host;
        uart->input_api = input;
        uart->owner = owner;
        uart->space = config->space;
        uart->base = Uart::get_uint(config, "base", 0x80);
        uart->tx_capacity = static_cast<size_t>(Uart::get_uint(config, "transcript_capacity", 64 * 1024));
        auto endpoint = Uart::get_string(config, "endpoint", "uart.rx");
        SrhHandle input_handle = 0;
        auto status = input->register_input(input->context, owner, endpoint.c_str(), 1024,
                                            &input_handle);
        if (status != SRH_OK)
            return status;
        uart->input = input_handle;
        SrhMapping mapping{SRH_INIT(SrhMapping),
                           config->space,
                           uart->base,
                           uart->base + 1,
                           config->priority,
                           uart.get(),
                           read,
                           write,
                           nullptr};
        SrhHandle mapping_handle = 0;
        status = host->map(host->context, owner, &mapping, &mapping_handle);
        if (status != SRH_OK)
            return status;
        uart->mapping = mapping_handle;
        status = text->register_text(text->context, owner, text_query, uart.get());
        if (status != SRH_OK)
            return status;
        *result = uart.release();
        return SRH_OK;
    });
}
void SRH_CALL destroy(void *p) {
    delete static_cast<Uart *>(p);
}
SrhStatus SRH_CALL reset(void *p, uint32_t cold) {
    auto &u = *static_cast<Uart *>(p);
    if (cold) {
        u.overflow = false;
        u.transcript.clear();
    }
    return SRH_OK;
}
uint32_t SRH_CALL count(void *) {
    return 4;
}
SrhStatus SRH_CALL info(void *, uint32_t index, SrhProperty *out) {
    if (!srz80::sdk::valid(out) || index >= 4)
        return SRH_INVALID;
    static const char *names[]{"base", "rx_pending", "rx_overflow", "tx_count"};
    static const char *groups[]{"UART", "UART", "UART", "UART"};
    static const char *descriptions[]{"Configured base port", "Bytes due in the host input queue",
                                      "Sticky RX overflow", "Bytes written to the terminal transcript"};
    uint32_t kinds[] = {SRH_UNSIGNED, SRH_UNSIGNED, SRH_BOOLEAN, SRH_UNSIGNED};
    uint32_t bits[] = {16, 32, 1, 32};
    uint32_t bases[] = {16, 10, 10, 10};
    *out = {SRH_INIT(SrhProperty), names[index], groups[index], descriptions[index], kinds[index],
            bits[index], bases[index], 0, nullptr};
    return SRH_OK;
}
SrhStatus SRH_CALL get(void *p, uint32_t index, SrhValue *out) {
    if (!srz80::sdk::valid(out) || index >= 4)
        return SRH_INVALID;
    auto &u = *static_cast<Uart *>(p);
    if (index == 0)
        out->unsigned_value = u.base;
    else if (index == 1)
        out->unsigned_value = u.due_now();
    else if (index == 2)
        out->unsigned_value = u.overflow;
    else
        out->unsigned_value = u.transcript.size();
    return SRH_OK;
}
SrhStatus SRH_CALL set(void *, uint32_t, const SrhValue *) {
    return SRH_INVALID;
}
SrhStatus SRH_CALL save_state(void *p, uint8_t *buffer, uint64_t *size) {
    if (!size)
        return SRH_INVALID;
    auto &u = *static_cast<Uart *>(p);
    uint64_t required = u.transcript.size() + 1;
    if (!buffer) {
        *size = required;
        return SRH_OK;
    }
    if (*size < required) {
        *size = required;
        return SRH_UNAVAILABLE;
    }
    if (!u.transcript.empty())
        std::memcpy(buffer, u.transcript.data(), u.transcript.size());
    buffer[u.transcript.size()] = u.overflow ? 1 : 0;
    *size = required;
    return SRH_OK;
}
SrhStatus SRH_CALL load_state(void *p, const uint8_t *buffer, uint64_t size) {
    auto &u = *static_cast<Uart *>(p);
    if (size == 0 || !buffer || size > u.tx_capacity + 1)
        return SRH_INVALID;
    u.transcript.assign(buffer, buffer + size - 1);
    u.overflow = buffer[size - 1] != 0;
    return SRH_OK;
}
const SrhCardDescriptor descriptor{SRH_INIT(SrhCardDescriptor), "I/O", "UART console",
                                   "Byte-oriented console endpoint", 0x80, 2, 0x80, 0, 0, 0,
                                   R"({"base":128,"endpoint":"uart0.rx","transcript_capacity":65536})",
                                   nullptr, "base"};
const SrhPlugin api{SRH_INIT(SrhPlugin), "uart_console", create, destroy, reset,
                    count,         info,          get,    set,           save_state, load_state,
                    &descriptor};
} // namespace
extern "C" SRH_EXPORT const SrhPlugin *SRH_CALL srz80_plugin_init(const ShouryoHost *host) {
    return srz80::sdk::valid(host) ? &api : nullptr;
}
