#include <json.hpp>

#include <frozen.h>

#include <charconv>
#include <climits>
#include <cstring>
#include <limits>

namespace srz80::sdk::json {
namespace {

struct WalkContext {
    Visit visit;
    void *context;
    const char *input_begin;
    const char *input_end;
    bool accepted = true;
    bool strict = true;
};

Type type_of(json_token_type type) {
    switch (type) {
    case JSON_TYPE_STRING: return Type::string;
    case JSON_TYPE_NUMBER: return Type::number;
    case JSON_TYPE_TRUE:
    case JSON_TYPE_FALSE: return Type::boolean;
    case JSON_TYPE_NULL: return Type::null;
    case JSON_TYPE_OBJECT_START: return Type::object_begin;
    case JSON_TYPE_OBJECT_END: return Type::object_end;
    case JSON_TYPE_ARRAY_START: return Type::array_begin;
    case JSON_TYPE_ARRAY_END: return Type::array_end;
    default: return Type::null;
    }
}

void visit_token(void *opaque, const char *name, size_t name_size, const char *,
                 const json_token *raw) {
    auto &state = *static_cast<WalkContext *>(opaque);
    if (!state.accepted || !raw) return;
    // Frozen deliberately accepts identifier-style object keys. The SDK contract is strict JSON.
    if (name && name >= state.input_begin && name < state.input_end &&
        (name == state.input_begin || name[-1] != '"')) {
        state.strict = false;
        return;
    }
    Token token{type_of(raw->type),
                name ? std::string_view(name, name_size) : std::string_view{},
                raw->ptr && raw->len >= 0 ? std::string_view(raw->ptr, size_t(raw->len))
                                          : std::string_view{},
                raw->type == JSON_TYPE_TRUE};
    state.accepted = state.visit(state.context, token);
}

bool continuation(unsigned char c) { return (c & 0xc0u) == 0x80u; }

bool append_utf8(uint32_t cp, std::string &out) {
    if (cp <= 0x7f) out.push_back(char(cp));
    else if (cp <= 0x7ff) {
        out.push_back(char(0xc0u | (cp >> 6)));
        out.push_back(char(0x80u | (cp & 0x3fu)));
    } else if (cp <= 0xffff) {
        if (cp >= 0xd800 && cp <= 0xdfff) return false;
        out.push_back(char(0xe0u | (cp >> 12)));
        out.push_back(char(0x80u | ((cp >> 6) & 0x3fu)));
        out.push_back(char(0x80u | (cp & 0x3fu)));
    } else if (cp <= 0x10ffff) {
        out.push_back(char(0xf0u | (cp >> 18)));
        out.push_back(char(0x80u | ((cp >> 12) & 0x3fu)));
        out.push_back(char(0x80u | ((cp >> 6) & 0x3fu)));
        out.push_back(char(0x80u | (cp & 0x3fu)));
    } else return false;
    return true;
}

bool hex4(std::string_view raw, size_t at, uint32_t &value) {
    if (raw.size() - at < 4) return false;
    value = 0;
    for (size_t i = 0; i < 4; ++i) {
        const char c = raw[at + i];
        const unsigned digit = c >= '0' && c <= '9' ? unsigned(c - '0')
                             : c >= 'a' && c <= 'f' ? unsigned(c - 'a' + 10)
                             : c >= 'A' && c <= 'F' ? unsigned(c - 'A' + 10) : 16u;
        if (digit == 16) return false;
        value = value * 16 + digit;
    }
    return true;
}

} // namespace

Result walk(std::string_view input, Visit visit, void *context, uint32_t max_depth) noexcept {
    if (!visit || input.size() > size_t(INT_MAX) || max_depth == 0 || max_depth > INT_MAX)
        return {input.size() > size_t(INT_MAX) ? Error::too_large : Error::invalid, 0};
    WalkContext state{visit, context, input.data(), input.data() + input.size()};
    frozen_args args{};
    args.callback = visit_token;
    args.callback_data = &state;
    args.limit = int(max_depth);
    const int consumed = json_walk_args(input.data(), int(input.size()), &args);
    if (!state.strict) return {Error::invalid, consumed > 0 ? size_t(consumed) : 0};
    if (!state.accepted) return {Error::rejected, consumed > 0 ? size_t(consumed) : 0};
    if (consumed == JSON_DEPTH_LIMIT) return {Error::too_deep, 0};
    if (consumed == JSON_STRING_INCOMPLETE) return {Error::incomplete, 0};
    if (consumed < 0 || size_t(consumed) != input.size())
        return {Error::invalid, consumed > 0 ? size_t(consumed) : 0};
    return {};
}

Result object(std::string_view input, Visit visit, void *context) noexcept {
    struct ObjectContext {
        Visit visit;
        void *context;
        bool opened = false;
        bool closed = false;
    } state{visit, context};
    const auto adapter = [](void *opaque, const Token &token) noexcept {
        auto &object = *static_cast<ObjectContext *>(opaque);
        if (token.type == Type::object_begin) {
            if (object.opened || object.closed || !token.name.empty()) return false;
            object.opened = true;
            return true;
        }
        if (token.type == Type::object_end) {
            if (!object.opened || object.closed) return false;
            object.closed = true;
            return true;
        }
        if (!object.opened || object.closed || token.name.empty() ||
            token.type == Type::array_begin || token.type == Type::array_end)
            return false;
        return object.visit(object.context, token);
    };
    if (!visit) return {Error::invalid, 0};
    const auto result = walk(input, adapter, &state);
    if (!result) return result;
    return state.opened && state.closed ? Result{} : Result{Error::invalid, 0};
}

Result unsigned_integer(std::string_view raw, uint64_t &value) noexcept {
    if (raw.empty()) return {Error::invalid, 0};
    uint64_t parsed = 0;
    const auto result = std::from_chars(raw.data(), raw.data() + raw.size(), parsed);
    if (result.ec == std::errc::result_out_of_range) return {Error::overflow, 0};
    if (result.ec != std::errc{} || result.ptr != raw.data() + raw.size()) return {Error::invalid, 0};
    value = parsed;
    return {};
}

Result signed_integer(std::string_view raw, int64_t &value) noexcept {
    if (raw.empty()) return {Error::invalid, 0};
    int64_t parsed = 0;
    const auto result = std::from_chars(raw.data(), raw.data() + raw.size(), parsed);
    if (result.ec == std::errc::result_out_of_range) return {Error::overflow, 0};
    if (result.ec != std::errc{} || result.ptr != raw.data() + raw.size()) return {Error::invalid, 0};
    value = parsed;
    return {};
}

Result unsigned_value(const Token &token, uint64_t &value) noexcept {
    if (token.type == Type::number) return unsigned_integer(token.value, value);
    if (token.type != Type::string) return {Error::invalid, 0};
    std::string decoded;
    const auto decoded_result = decode_string(token.value, decoded);
    if (!decoded_result || decoded.empty())
        return decoded_result ? Result{Error::invalid, 0} : decoded_result;
    int base = 10;
    std::string_view digits = decoded;
    if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) {
        base = 16;
        digits.remove_prefix(2);
    }
    if (digits.empty()) return {Error::invalid, 0};
    uint64_t parsed = 0;
    const auto converted = std::from_chars(digits.data(), digits.data() + digits.size(), parsed, base);
    if (converted.ec == std::errc::result_out_of_range) return {Error::overflow, 0};
    if (converted.ec != std::errc{} || converted.ptr != digits.data() + digits.size())
        return {Error::invalid, 0};
    value = parsed;
    return {};
}

Result decode_string(std::string_view raw, std::string &value) noexcept {
    std::string decoded;
    try { decoded.reserve(raw.size()); } catch (...) { return {Error::too_large, 0}; }
    for (size_t i = 0; i < raw.size();) {
        const auto c = static_cast<unsigned char>(raw[i++]);
        if (c == '\\') {
            if (i == raw.size()) return {Error::incomplete, i};
            const char escape = raw[i++];
            if (escape == '"' || escape == '\\' || escape == '/') decoded.push_back(escape);
            else if (escape == 'b') decoded.push_back('\b');
            else if (escape == 'f') decoded.push_back('\f');
            else if (escape == 'n') decoded.push_back('\n');
            else if (escape == 'r') decoded.push_back('\r');
            else if (escape == 't') decoded.push_back('\t');
            else if (escape == 'u') {
                uint32_t cp = 0;
                if (!hex4(raw, i, cp)) return {Error::invalid, i};
                i += 4;
                if (cp >= 0xd800 && cp <= 0xdbff) {
                    if (raw.size() - i < 6 || raw[i] != '\\' || raw[i + 1] != 'u')
                        return {Error::invalid_utf8, i};
                    uint32_t low = 0;
                    if (!hex4(raw, i + 2, low) || low < 0xdc00 || low > 0xdfff)
                        return {Error::invalid_utf8, i};
                    i += 6;
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
                } else if (cp >= 0xdc00 && cp <= 0xdfff) return {Error::invalid_utf8, i};
                if (!append_utf8(cp, decoded)) return {Error::invalid_utf8, i};
            } else return {Error::invalid, i};
            continue;
        }
        if (c < 0x20) return {Error::invalid, i - 1};
        if (c < 0x80) { decoded.push_back(char(c)); continue; }
        const size_t extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc2 ? 1 : 99;
        if (extra == 99 || raw.size() - i < extra) return {Error::invalid_utf8, i - 1};
        uint32_t cp = c & (extra == 1 ? 0x1fu : extra == 2 ? 0x0fu : 0x07u);
        for (size_t n = 0; n < extra; ++n) {
            const auto next = static_cast<unsigned char>(raw[i++]);
            if (!continuation(next)) return {Error::invalid_utf8, i - 1};
            cp = (cp << 6) | (next & 0x3fu);
        }
        if ((extra == 2 && cp < 0x800) || (extra == 3 && cp < 0x10000) ||
            cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
            return {Error::invalid_utf8, i - extra - 1};
        decoded.append(raw.substr(i - extra - 1, extra + 1));
    }
    value = std::move(decoded);
    return {};
}

Writer::Writer(char *buffer, size_t capacity) noexcept : buffer_(buffer), capacity_(capacity) {}

bool Writer::append(const char *data, size_t size) noexcept {
    if (!ok_ || required_ > std::numeric_limits<size_t>::max() - size) return ok_ = false;
    const size_t available = written_ < capacity_ ? capacity_ - written_ : 0;
    const size_t copy = size < available ? size : available;
    if (copy && buffer_) std::memcpy(buffer_ + written_, data, copy);
    written_ += copy;
    required_ += size;
    return true;
}

bool Writer::before_value() noexcept {
    if (!ok_) return false;
    if (!depth_) {
        if (root_written_) return ok_ = false;
        root_written_ = true;
        return true;
    }
    auto &level = levels_[depth_ - 1];
    if (level.kind == Kind::object) {
        if (!level.wants_value) return ok_ = false;
        level.wants_value = false;
    } else {
        if (!level.first) append(",", 1);
        level.first = false;
    }
    return ok_;
}

bool Writer::begin_object() noexcept {
    if (!before_value() || depth_ == 32 || !append("{", 1)) return ok_ = false;
    levels_[depth_++] = {Kind::object, true, false};
    return true;
}
bool Writer::end_object() noexcept {
    if (!ok_ || !depth_ || levels_[depth_ - 1].kind != Kind::object || levels_[depth_ - 1].wants_value)
        return ok_ = false;
    --depth_;
    return append("}", 1);
}
bool Writer::begin_array() noexcept {
    if (!before_value() || depth_ == 32 || !append("[", 1)) return ok_ = false;
    levels_[depth_++] = {Kind::array, true, false};
    return true;
}
bool Writer::end_array() noexcept {
    if (!ok_ || !depth_ || levels_[depth_ - 1].kind != Kind::array) return ok_ = false;
    --depth_;
    return append("]", 1);
}
bool Writer::quoted(std::string_view value) noexcept {
    if (!append("\"", 1)) return false;
    for (unsigned char c : value) {
        switch (c) {
        case '"': if (!append("\\\"", 2)) return false; break;
        case '\\': if (!append("\\\\", 2)) return false; break;
        case '\b': if (!append("\\b", 2)) return false; break;
        case '\f': if (!append("\\f", 2)) return false; break;
        case '\n': if (!append("\\n", 2)) return false; break;
        case '\r': if (!append("\\r", 2)) return false; break;
        case '\t': if (!append("\\t", 2)) return false; break;
        default:
            if (c < 0x20) {
                constexpr char hex[] = "0123456789abcdef";
                const char escaped[] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 15]};
                if (!append(escaped, sizeof escaped)) return false;
            } else if (!append(reinterpret_cast<const char *>(&c), 1)) return false;
        }
    }
    return append("\"", 1);
}
bool Writer::key(std::string_view value) noexcept {
    if (!ok_ || !depth_) return ok_ = false;
    auto &level = levels_[depth_ - 1];
    if (level.kind != Kind::object || level.wants_value) return ok_ = false;
    if (!level.first) append(",", 1);
    level.first = false;
    if (!quoted(value) || !append(":", 1)) return false;
    level.wants_value = true;
    return true;
}
bool Writer::string(std::string_view value) noexcept { return before_value() && quoted(value); }
bool Writer::unsigned_integer(uint64_t value) noexcept {
    if (!before_value()) return false;
    char text[32]; const auto result = std::to_chars(text, text + sizeof text, value);
    return result.ec == std::errc{} && append(text, size_t(result.ptr - text));
}
bool Writer::signed_integer(int64_t value) noexcept {
    if (!before_value()) return false;
    char text[32]; const auto result = std::to_chars(text, text + sizeof text, value);
    return result.ec == std::errc{} && append(text, size_t(result.ptr - text));
}
bool Writer::boolean(bool value) noexcept { return before_value() && append(value ? "true" : "false", value ? 4 : 5); }
bool Writer::null() noexcept { return before_value() && append("null", 4); }
bool Writer::finish() noexcept { return ok_ && root_written_ && depth_ == 0; }

} // namespace srz80::sdk::json
