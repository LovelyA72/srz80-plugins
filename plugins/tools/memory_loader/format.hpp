#pragma once

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace srz80::memory_loader {

struct Segment {
    uint64_t address;
    std::vector<uint8_t> bytes;
};

inline std::vector<uint8_t> parse_byte_text(std::string text) {
    for (char &character : text)
        if (character == ',' || character == ';')
            character = ' ';

    std::vector<uint8_t> result;
    size_t position = 0;
    while (position < text.size()) {
        while (position < text.size() && std::isspace(static_cast<unsigned char>(text[position])))
            ++position;
        if (position == text.size())
            break;
        auto end = text.find_first_of(" \t\r\n", position);
        if (end == std::string::npos)
            end = text.size();
        auto token = text.substr(position, end - position);
        if (token.starts_with("0x") || token.starts_with("0X"))
            token.erase(0, 2);
        if (token.empty() || token.size() % 2)
            throw std::invalid_argument("Expected complete hexadecimal bytes");
        for (size_t offset = 0; offset < token.size(); offset += 2) {
            unsigned value = 0;
            const auto *first = token.data() + offset;
            auto converted = std::from_chars(first, first + 2, value, 16);
            if (converted.ec != std::errc{} || converted.ptr != first + 2)
                throw std::invalid_argument("Invalid hexadecimal input");
            result.push_back(static_cast<uint8_t>(value));
        }
        position = end;
    }
    if (result.empty())
        throw std::invalid_argument("Enter at least one byte");
    return result;
}

inline std::string format_hex(const std::vector<uint8_t> &bytes) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        result.push_back(digits[byte >> 4u]);
        result.push_back(digits[byte & 0x0Fu]);
    }
    return result;
}

inline std::vector<Segment> parse_intel_hex(std::string_view text) {
    std::map<uint64_t, uint8_t> data;
    uint64_t base = 0;
    bool end_record_seen = false;
    size_t line_number = 0;

    auto error = [&](const std::string &message) -> void {
        throw std::invalid_argument("Intel HEX line " + std::to_string(line_number) + ": " + message);
    };
    auto hex_byte = [&](char high, char low) -> uint8_t {
        char pair[] = {high, low};
        unsigned value = 0;
        const auto converted = std::from_chars(pair, pair + 2, value, 16);
        if (converted.ec != std::errc{} || converted.ptr != pair + 2)
            error("invalid hexadecimal digit");
        return static_cast<uint8_t>(value);
    };

    size_t start = 0;
    while (start <= text.size()) {
        ++line_number;
        const auto newline = text.find('\n', start);
        const auto end = newline == std::string_view::npos ? text.size() : newline;
        auto line = text.substr(start, end - start);
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front())))
            line.remove_prefix(1);
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back())))
            line.remove_suffix(1);

        if (!line.empty()) {
            if (end_record_seen)
                error("records found after EOF");
            if (line.front() != ':' || line.size() < 11 || (line.size() - 1) % 2)
                error("malformed record");

            std::vector<uint8_t> record;
            record.reserve((line.size() - 1) / 2);
            for (size_t offset = 1; offset < line.size(); offset += 2)
                record.push_back(hex_byte(line[offset], line[offset + 1]));
            if (record.size() != static_cast<size_t>(record[0]) + 5)
                error("record length does not match byte count");
            unsigned checksum = 0;
            for (const auto byte : record)
                checksum += byte;
            if ((checksum & 0xFFu) != 0)
                error("checksum mismatch");

            const auto length = record[0];
            const auto address = static_cast<uint16_t>((record[1] << 8u) | record[2]);
            const auto type = record[3];
            const auto *payload = record.data() + 4;
            switch (type) {
            case 0x00: {
                if (length && base > UINT64_MAX - address)
                    error("address overflow");
                const auto absolute = base + address;
                if (length && absolute > UINT64_MAX - (length - 1u))
                    error("address range overflow");
                for (uint32_t offset = 0; offset < length; ++offset) {
                    const auto target = absolute + offset;
                    const auto [found, inserted] = data.emplace(target, payload[offset]);
                    if (!inserted && found->second != payload[offset])
                        error("overlapping records contain different data");
                }
                break;
            }
            case 0x01:
                if (length || address)
                    error("invalid EOF record");
                end_record_seen = true;
                break;
            case 0x02:
                if (length != 2 || address)
                    error("invalid extended-segment address record");
                base = (static_cast<uint64_t>((payload[0] << 8u) | payload[1])) << 4u;
                break;
            case 0x03:
                if (length != 4 || address)
                    error("invalid start-segment address record");
                break;
            case 0x04:
                if (length != 2 || address)
                    error("invalid extended-linear address record");
                base = static_cast<uint64_t>((payload[0] << 8u) | payload[1]) << 16u;
                break;
            case 0x05:
                if (length != 4 || address)
                    error("invalid start-linear address record");
                break;
            default:
                error("unsupported record type");
            }
        }

        if (newline == std::string_view::npos)
            break;
        start = newline + 1;
    }

    if (!end_record_seen)
        throw std::invalid_argument("Intel HEX file has no EOF record");
    if (data.empty())
        throw std::invalid_argument("Intel HEX file contains no data");

    std::vector<Segment> result;
    for (const auto &[address, byte] : data) {
        if (result.empty() || result.back().address + result.back().bytes.size() != address)
            result.push_back({address, {}});
        result.back().bytes.push_back(byte);
    }
    return result;
}

} // namespace srz80::memory_loader
