// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <span>
#include <vector>
namespace vsn::state {
inline void append(std::vector<uint8_t> &out, uint64_t value, unsigned bytes) {
    for (unsigned i=0; i<bytes; ++i) out.push_back(uint8_t(value >> (8*i)));
}
inline uint64_t get(std::span<const uint8_t> data, size_t offset, unsigned bytes) {
    uint64_t value=0;
    for (unsigned i=0; i<bytes; ++i) value |= uint64_t(data[offset+i]) << (8*i);
    return value;
}
} // namespace vsn::state
