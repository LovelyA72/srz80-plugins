#pragma once

#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace srz80::midi {

inline std::filesystem::path path_from_utf8(std::string_view text) {
#ifdef _WIN32
    if (text.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
        throw std::runtime_error("MIDI path is too long");
    const auto *bytes = text.data();
    const auto length = static_cast<int>(text.size());
    const int wide_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, length,
                                                nullptr, 0);
    if (!wide_length)
        throw std::runtime_error("MIDI path is not valid UTF-8");
    std::wstring wide(static_cast<size_t>(wide_length), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, length, wide.data(), wide_length))
        throw std::runtime_error("Could not convert MIDI path to Windows Unicode");
    return std::filesystem::path(std::move(wide));
#else
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
#endif
}

} // namespace srz80::midi
