// SPDX-License-Identifier: MIT
#pragma once
#include "vsn_memory.hpp"
#include "vsn_renderer.hpp"
#include <array>
#include <cstdint>
#include <span>
#include <memory>
#include <vector>

namespace vsn {
struct RasterProfile { uint64_t clock; uint32_t periods, lines; };
constexpr RasterProfile profile(Region region) {
    if (region == Region::pal) return {26601712, 1705, 312};
    if (region == Region::vga) return {25175000, 800, 525};
    return {21477272, 1364, 262};
}
// Pure simulated-time arithmetic, usable without a scheduler or SRZ80.
struct RasterClock {
    uint64_t hz, numerator, remainder = 0;
    RasterClock(Region region, uint64_t frequency)
        : hz(frequency), numerator(uint64_t(profile(region).periods) * 1000000000) {}
    uint64_t next_delay() {
        const auto accumulated = numerator + remainder;
        remainder = accumulated % hz;
        return accumulated / hz;
    }
};
namespace reg {
constexpr unsigned control=0x04, mode=0x05, status=0x06, pending=0x08, enable=0x09,
    scroll_x=0x14, scroll_y=0x16, nes_color=0x18, map=0x20, bg_tiles=0x24,
    row_stride=0x28, map_width=0x2a, map_height=0x2b, page_x=0x2c, page_y=0x2e,
    sprites=0x30, sprite_tiles=0x34, nes_pattern=0x38, planar=0x39,
    palette=0x40, backdrop=0x45, raster=0x50,
    dma_src=0x60, dma_dst=0x64, dma_count=0x68, dma_cmd=0x6c, dma_status=0x6d, dma_fill=0x6e;
}

class Core {
public:
    // Low-resolution logical viewport shared by modes 0-4. The host surface is
    // always the high-resolution 512x480 frame; low-res modes are nearest-
    // neighbor 2x upscaled by the core, high-res mode renders natively.
    static constexpr unsigned width=256, height=240;
    static constexpr unsigned hires_width=512, hires_height=480;
    static constexpr unsigned surface_width=hires_width, surface_height=hires_height;
    static constexpr size_t frame_bytes=surface_width*surface_height*4;
    // DMA advances a bounded number of bytes per scanline event (one chunk per
    // scheduler boundary); the whole transfer is never performed in an MMIO write.
    static constexpr unsigned dma_bytes_per_line=16;
    using Color = std::array<uint8_t, 4>;
    explicit Core(Memory &memory, Region region=Region::ntsc, bool strict=false,
                  std::unique_ptr<Renderer> renderer = make_tile_renderer());
    void reset();
    uint8_t read(unsigned offset) const;
    void write(unsigned offset, uint8_t value);
    void tick();
    std::span<const uint8_t> pixels() const { return front_; }
    uint32_t line() const { return line_; }
    uint32_t lines() const { return profile(region_).lines; }
    uint64_t frame() const { return frame_; }
    uint64_t fault_count() const { return faults_; }
    uint64_t fault_address() const { return fault_address_; }
    bool fault_was_write() const { return fault_write_; }
    uint32_t value(unsigned offset, unsigned bytes) const;
    // Physical interrupt line levels derived from pending & enable. Vblank is
    // the only NMI cause; raster, DMA-complete and DMA-fault drive IRQ.
    bool nmi_asserted() const { return registers_[8] & registers_[9] & 0x01; }
    bool irq_asserted() const { return registers_[8] & registers_[9] & 0x0e; }

    // Shared checked memory gateway, including DMA writes. Rendering
    // calls only fetch; no storage organization is embedded in the adapter.
    uint8_t fetch(uint64_t address);
    // Reads four contiguous little-endian bytes, preferring the memory's word
    // transport and falling back to per-byte fetch() so fault accounting (and
    // the strict AbortLine path) stays identical to the byte path.
    bool fetch_word(uint64_t address, uint32_t &value);
    bool store(uint64_t address, uint8_t value);
    std::vector<uint8_t> save() const;
    bool load(std::span<const uint8_t> state);

private:
    struct AbortLine {};
    void fault(uint64_t address, bool writing);
    void render_line();
    void render_row(const std::array<uint8_t,128> &snapshot, Memory &gateway,
                    unsigned y, unsigned logical_width, std::span<uint8_t> rgba);
    void upscale_row(const uint8_t *source);
    void blank_rows();
    unsigned logical_width() const { return registers_[5]==5 ? hires_width : width; }
    unsigned logical_height() const { return registers_[5]==5 ? hires_height : height; }
    uint32_t reg32(unsigned offset) const;
    void set_reg32(unsigned offset, uint32_t value);
    void start_dma();
    void dma_chunk();
    void dma_fault();

    Memory &memory_;
    Region region_;
    bool strict_;
    std::array<uint8_t,128> registers_{};
    std::unique_ptr<Renderer> renderer_;
    // Double-buffered scanout: the raster renders into back_ and publishes the
    // completed frame into front_ at the end of the visible area, so a video
    // query always copies one complete frame instead of a mid-render image.
    std::vector<uint8_t> front_, back_;
    uint32_t line_=0;
    uint64_t frame_=0, faults_=0, fault_address_=0;
    bool vblank_=false, fault_write_=false, rendering_=false;
};
} // namespace vsn
