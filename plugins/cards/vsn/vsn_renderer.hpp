// SPDX-License-Identifier: MIT
#pragma once
#include "vsn_memory.hpp"
#include <cstdint>
#include <memory>
#include <span>

namespace vsn {
enum class Region : uint8_t { ntsc, pal, vga };
struct VideoLine {
    uint64_t frame;
    unsigned y, width;
    Region region;
    // Register snapshot valid only during this call. Backends decode it.
    std::span<const uint8_t,128> registers;
    uint32_t value(unsigned offset, unsigned bytes) const {
        if (bytes>4 || offset>=registers.size() || bytes>registers.size()-offset) return 0;
        uint32_t result=0;
        for (unsigned i=0; i<bytes; ++i) result |= uint32_t(registers[offset+i]) << (8*i);
        return result;
    }
};
struct VideoEffects { bool sprite_zero=false, sprite_overflow=false; };
class Renderer {
public:
    virtual ~Renderer() = default;
    virtual void reset() {}
    // rgba has width*4 bytes in RGBA order. All arguments are borrowed.
    virtual VideoEffects render_scanline(const VideoLine &, Memory &,
                                        std::span<uint8_t> rgba) = 0;
};
std::unique_ptr<Renderer> make_tile_renderer();
} // namespace vsn
