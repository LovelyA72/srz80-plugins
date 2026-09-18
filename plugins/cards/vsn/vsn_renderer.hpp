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
    // Snapshot valid for this call only. Backends decode their own register
    // formats; they never depend on Core, a scheduler, or an SRZ80 header.
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
    // Allows a future frame-based/3D backend to prepare its internal frame.
    // Raster and output ownership still belong to Core. No guest fetches here;
    // fetches occur through the checked Memory provided to render_scanline.
    virtual void begin_frame(uint64_t) {}
    // rgba is exactly width*4 bytes in R,G,B,A order, with opaque alpha.
    // Inputs/output are borrowed only for the duration of this call.
    virtual VideoEffects render_scanline(const VideoLine &, Memory &,
                                        std::span<uint8_t> rgba) = 0;
};
std::unique_ptr<Renderer> make_tile_renderer();
} // namespace vsn
