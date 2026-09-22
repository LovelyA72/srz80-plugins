#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace srz80::sdk::json {

enum class Type : uint8_t {
    string,
    number,
    boolean,
    null,
    object_begin,
    object_end,
    array_begin,
    array_end,
};

struct Token {
    Type type;
    std::string_view name;  // Raw, escaped object key; empty for array elements.
    std::string_view value; // Raw token contents; strings exclude their quotes.
    bool boolean = false;
};

using Visit = bool (*)(void *context, const Token &token) noexcept;

enum class Error : uint8_t {
    none,
    invalid,
    incomplete,
    too_deep,
    too_large,
    rejected,
    overflow,
    invalid_utf8,
};

struct Result {
    Error error = Error::none;
    size_t offset = 0;
    explicit operator bool() const { return error == Error::none; }
};

// Walks exactly one complete JSON value. Input is bounded and need not be NUL terminated.
// The callback receives borrowed views that remain valid only as long as input does.
Result walk(std::string_view input, Visit visit, void *context, uint32_t max_depth = 32) noexcept;
// Walks a root object containing scalar values only. Nested objects and arrays are rejected.
Result object(std::string_view input, Visit visit, void *context) noexcept;

Result unsigned_integer(std::string_view raw, uint64_t &value) noexcept;
Result signed_integer(std::string_view raw, int64_t &value) noexcept;
// Accepts either a JSON unsigned integer or a quoted decimal/0x-prefixed integer.
Result unsigned_value(const Token &token, uint64_t &value) noexcept;
Result decode_string(std::string_view raw, std::string &value) noexcept;

class Writer {
  public:
    Writer(char *buffer, size_t capacity) noexcept;

    bool begin_object() noexcept;
    bool end_object() noexcept;
    bool begin_array() noexcept;
    bool end_array() noexcept;
    bool key(std::string_view value) noexcept;
    bool string(std::string_view value) noexcept;
    bool unsigned_integer(uint64_t value) noexcept;
    bool signed_integer(int64_t value) noexcept;
    bool boolean(bool value) noexcept;
    bool null() noexcept;

    bool finish() noexcept;
    bool ok() const noexcept { return ok_; }
    bool fits() const noexcept { return buffer_ && required_ <= capacity_; }
    size_t size() const noexcept { return required_; }

  private:
    enum class Kind : uint8_t { object, array };
    struct Level { Kind kind; bool first; bool wants_value; };

    bool before_value() noexcept;
    bool append(const char *data, size_t size) noexcept;
    bool quoted(std::string_view value) noexcept;

    char *buffer_;
    size_t capacity_;
    size_t written_ = 0;
    size_t required_ = 0;
    Level levels_[32]{};
    uint8_t depth_ = 0;
    bool root_written_ = false;
    bool ok_ = true;
};

} // namespace srz80::sdk::json
