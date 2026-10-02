// SPDX-License-Identifier: MIT
#pragma once
#include "vsn_memory.hpp"
#include "vsn_renderer.hpp"
#include "vsn_registers.hpp"
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
// Simulated-time raster clock, independent of the host scheduler.
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
class Core {
public:
    // Modes 0-4 render at 256x240 and scale to the 512x480 surface.
    // Mode 5 renders at 512x480.
    static constexpr unsigned width=256, height=240;
    static constexpr unsigned hires_width=512, hires_height=480;
    static constexpr unsigned surface_width=hires_width, surface_height=hires_height;
    static constexpr size_t frame_bytes=surface_width*surface_height*4;
    // DMA transfers at most 16 bytes per scanline event.
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
    uint64_t published_frame() const { return published_frame_; }
    uint32_t published_line() const { return published_line_; }
    uint64_t fault_count() const { return faults_; }
    uint64_t fault_address() const { return fault_address_; }
    bool fault_was_write() const { return fault_write_; }
    uint32_t value(unsigned offset, unsigned bytes) const;
    // Vblank drives NMI. Raster and DMA causes drive IRQ.
    bool nmi_asserted() const { return registers_[8] & registers_[9] & 0x01; }
    bool irq_asserted() const { return registers_[8] & registers_[9] & 0x0e; }

    // Checked guest memory access for rendering and DMA.
    uint8_t fetch(uint64_t address);
    // Read a little-endian word. Fall back to byte reads when the transport
    // refuses the word, preserving byte-level fault handling.
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
    // Publish back_ to front_ after the visible area finishes.
    std::vector<uint8_t> front_, back_;
    uint32_t line_=0;
    uint64_t frame_=0, faults_=0, fault_address_=0;
    uint64_t published_frame_=0;
    uint32_t published_line_=0;
    bool vblank_=false, fault_write_=false, rendering_=false;
};
} // namespace vsn
