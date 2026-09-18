// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>

namespace vsn {
// The only connection between the rendering device and guest storage. No host
// handles, CPU bus semantics, private VRAM, or ABI objects leak into the core.
// Implementations report unmapped/denied accesses as false; ownership stays
// with the caller. Core checks its 32-bit address limit before invoking these.
class Memory {
public:
    virtual ~Memory() = default;
    virtual bool read(uint64_t address, uint8_t &value) = 0;
    virtual bool write(uint64_t address, uint8_t value) = 0;
};
} // namespace vsn
