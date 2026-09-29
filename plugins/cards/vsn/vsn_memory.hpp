// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>

namespace vsn {
// Guest memory interface. The caller owns storage. Return false for denied
// or unmapped accesses. Core checks the 32-bit address limit.
class Memory {
public:
    virtual ~Memory() = default;
    virtual bool read(uint64_t address, uint8_t &value) = 0;
    virtual bool write(uint64_t address, uint8_t value) = 0;
    // Return false if a word read cannot preserve byte-read behavior.
    // Callers then read each byte separately.
    virtual bool read_word(uint64_t, uint32_t &) { return false; }
};
} // namespace vsn
