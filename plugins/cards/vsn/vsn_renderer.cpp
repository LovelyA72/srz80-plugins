// SPDX-License-Identifier: MIT
#include "vsn_renderer.hpp"
#include "vsn_layout.hpp"
#include "nes_palette.hpp"
#include <array>
#include <limits>

namespace vsn {
namespace {
using Color=std::array<uint8_t,4>;
// A line-local storage decoder. All video layout knowledge stays here/layout;
// transport and checked fault policy belong to the supplied Memory gateway.
class TileLine {
public:
    TileLine(const VideoLine &line, Memory &memory, std::span<uint8_t> output)
        : line_(line), memory_(memory), output_(output) {}
    VideoEffects render() {
        latch_palette();
        const auto control=r(4);
        const auto backdrop=palette_[r(5) == 0 ? 0 : r(0x45)];
        std::array<uint8_t,256> background{};
        for (unsigned x=0; x<line_.width; ++x) {
            if ((control & 2) && (r(5) != 0 || x>=8 || (control & 8)))
                background[x]=background_pixel(x, line_.y);
            put(x, background[x] ? palette_[background[x]] : backdrop);
        }
        if (control & 4) {
            // NES and planar4 keep the 256-byte OAM and planar decode; every
            // packed mode reads the 512-byte extended table.
            if (r(5)==0 || r(5)==2) sprites(background);
            else extended_sprites(background);
        }
        return effects_;
    }
private:
    uint8_t r(unsigned offset) const { return line_.registers[offset]; }
    uint32_t v(unsigned offset, unsigned bytes) const { return line_.value(offset,bytes); }
    uint8_t fetch(uint64_t address) {
        uint8_t value=0xff;
        memory_.read(address, value);
        return value;
    }
    uint16_t descriptor_at(uint64_t address) {
        return uint16_t(fetch(address)) | (uint16_t(fetch(address+1))<<8);
    }
    void put(unsigned x, const Color &color) {
        for (unsigned c=0; c<4; ++c) output_[x*4+c]=color[c];
    }
    Color nes_color(uint8_t index) const {
        unsigned emphasis=(r(0x18)>>1)&7;
        if (line_.region == Region::pal)
            emphasis=(emphasis&4)|((emphasis&1)<<1)|((emphasis&2)>>1);
        index &= (r(0x18)&1) ? 0x30 : 0x3f;
        const auto rgb=nes_rgb[index];
        Color color{uint8_t(rgb>>16),uint8_t(rgb>>8),uint8_t(rgb),255};
        // VSN digital emphasis: attenuate unselected channels by 3/4;
        // selecting all three attenuates all three, with integer truncation.
        for (unsigned channel=0; channel<3; ++channel)
            if (emphasis && (emphasis==7 || !(emphasis & (1u<<channel))))
                color[channel]=uint8_t(unsigned(color[channel])*3/4);
        return color;
    }
    void latch_palette() {
        const uint64_t base=v(0x40,4);
        if (r(5)==0) {
            for (unsigned i=0; i<32; ++i) {
                if (i>=16 && !(i&3)) palette_[i]=palette_[i-16];
                else palette_[i]=nes_color(fetch(base+i));
            }
        } else {
            // Modes 3/4 use linear RGB555; packed4 and planar4 use RGB444.
            const bool rgb555=(r(5)==3 || r(5)==4);
            for (unsigned i=0; i<256; ++i) {
                const uint16_t low=fetch(base+i*2);
                const uint16_t word=low | (uint16_t(fetch(base+i*2+1))<<8);
                if (rgb555) {
                    const auto expand=[](uint16_t v){ return uint8_t((v<<3)|(v>>2)); };
                    palette_[i]={expand((word>>10)&31),expand((word>>5)&31),expand(word&31),255};
                } else
                    palette_[i]={uint8_t(((word>>8)&15)*17),uint8_t(((word>>4)&15)*17),
                                 uint8_t((word&15)*17),255};
            }
        }
    }
    uint8_t pattern(uint64_t address, unsigned x) {
        const auto low=fetch(address);
        const auto high=fetch(address+8);
        return layout::planar2(low,high,x);
    }
    uint8_t background_pixel(unsigned x, unsigned y) {
        x+=v(0x14,2); y+=v(0x16,2);
        if (r(5)==1) {
            x %= unsigned(r(0x2a) ? r(0x2a) : 256)*8;
            y %= unsigned(r(0x2b) ? r(0x2b) : 256)*8;
            const auto address=layout::packed_map(v(0x20,4),v(0x28,2),x,y);
            const uint16_t descriptor=descriptor_at(address);
            const auto pixel=layout::nibble(fetch(layout::packed4(v(0x24,4),descriptor&4095,x&7,y&7)),x);
            return pixel ? uint8_t((descriptor>>12)*16+pixel) : 0;
        }
        if (r(5)==2) {
            // VT planar: same 16-bit descriptor map as packed4, tiles decoded as
            // 2bpp or 4bpp bitplanes over a linear base. Pixel zero is the
            // backdrop; otherwise palette index is bank*stride+pixel.
            x %= unsigned(r(0x2a) ? r(0x2a) : 256)*8;
            y %= unsigned(r(0x2b) ? r(0x2b) : 256)*8;
            const auto address=layout::packed_map(v(0x20,4),v(0x28,2),x,y);
            const uint16_t descriptor=descriptor_at(address);
            const unsigned tile=descriptor&4095, bank=descriptor>>12, lx=x&7, ly=y&7;
            if (r(0x39)&1) {
                const auto p0=fetch(layout::planar_tile(v(0x24,4),tile,0,ly,4));
                const auto p1=fetch(layout::planar_tile(v(0x24,4),tile,1,ly,4));
                const auto p2=fetch(layout::planar_tile(v(0x24,4),tile,2,ly,4));
                const auto p3=fetch(layout::planar_tile(v(0x24,4),tile,3,ly,4));
                const auto pixel=layout::planar4(p0,p1,p2,p3,lx);
                return pixel ? uint8_t(bank*16+pixel) : 0;
            }
            const auto p0=fetch(layout::planar_tile(v(0x24,4),tile,0,ly,2));
            const auto p1=fetch(layout::planar_tile(v(0x24,4),tile,1,ly,2));
            const auto pixel=layout::planar2(p0,p1,lx);
            return pixel ? uint8_t(bank*4+pixel) : 0;
        }
        if (r(5)==3) {
            // Packed 8bpp: one byte per pixel. The palette index is the pixel
            // value, so descriptor bank bits are ignored.
            x %= unsigned(r(0x2a) ? r(0x2a) : 256)*8;
            y %= unsigned(r(0x2b) ? r(0x2b) : 256)*8;
            const uint16_t descriptor=descriptor_at(layout::packed_map(v(0x20,4),v(0x28,2),x,y));
            return fetch(layout::packed8(v(0x24,4),descriptor&4095,x&7,y&7));
        }
        if (r(5)==4) {
            // Packed 16x16 8bpp: tile rows span 16 map columns and 16 pixel rows.
            x %= unsigned(r(0x2a) ? r(0x2a) : 256)*16;
            y %= unsigned(r(0x2b) ? r(0x2b) : 256)*16;
            const uint16_t descriptor=descriptor_at(layout::packed_map16(v(0x20,4),v(0x28,2),x,y));
            return fetch(layout::packed8_16x16(v(0x24,4),descriptor&4095,x&15,y&15));
        }
        x%=512; y%=480;
        const auto page=layout::nes_page(v(0x20,4),v(0x2c,2),v(0x2e,2),x,y);
        x%=256; y%=240;
        const auto tile=fetch(page+(y/8)*32+x/8);
        const auto attribute=fetch(layout::nes_attribute(page,x/8,y/8));
        const auto bank=(attribute>>layout::attribute_shift(x/8,y/8))&3;
        const auto pixel=pattern(layout::nes_pattern(v(0x24,4),r(0x38)&1,tile,y&7),x&7);
        return pixel ? uint8_t(bank*4+pixel) : 0;
    }
    void sprites(const std::array<uint8_t,256> &background) {
        std::array<uint8_t,256> oam{};
        for (unsigned i=0; i<oam.size(); ++i) oam[i]=fetch(uint64_t(v(0x30,4))+i);
        std::array<bool,256> occupied{};
        const unsigned height=(r(4)&64) ? 16 : 8;
        const bool planar=(r(5)==2);
        const bool sprite4bpp=planar && (r(0x39)&2);
        unsigned found=0;
        for (unsigned i=0; i<64; ++i) {
            const auto sprite=layout::nes_sprite(&oam[i*4]);
            if (line_.y<sprite.top || line_.y>=sprite.top+height) continue;
            ++found;
            if (found>8) {
                effects_.sprite_overflow=true;
                if (r(4)&32) continue;
            }
            unsigned row=line_.y-sprite.top;
            if (sprite.flip_y) row=height-1-row;
            unsigned tile=sprite.tile, table=(r(0x38)>>1)&1;
            if (height==16) { if (!planar) table=tile&1; tile=(tile&254)+row/8; row%=8; }
            // Latch this sprite's row of bitplanes before decoding its pixels.
            std::array<uint8_t,4> planes{};
            if (planar) {
                const unsigned count=sprite4bpp ? 4 : 2;
                for (unsigned p=0; p<count; ++p)
                    planes[p]=fetch(layout::planar_tile(v(0x34,4),tile,p,row,count));
            } else {
                const auto address=layout::nes_pattern(v(0x34,4),table,tile,row);
                planes[0]=fetch(address); planes[1]=fetch(address+8);
            }
            for (unsigned dx=0; dx<8 && sprite.x+dx<line_.width; ++dx) {
                const auto x=sprite.x+dx;
                if (x<8 && !(r(4)&16)) continue;
                const auto col=sprite.flip_x ? 7-dx : dx;
                const auto pixel=sprite4bpp
                    ? layout::planar4(planes[0],planes[1],planes[2],planes[3],col)
                    : layout::planar2(planes[0],planes[1],col);
                if (!pixel) continue;
                if (i==0 && background[x] && x!=255) effects_.sprite_zero=true;
                if (occupied[x]) continue;
                occupied[x]=true;
                const unsigned index=planar
                    ? unsigned(sprite.palette)*(sprite4bpp?16:4)+pixel
                    : 16+unsigned(sprite.palette)*4+pixel;
                if (!sprite.behind || !background[x]) put(x,palette_[index]);
            }
        }
    }
    // Extended 512-byte table: 32 records of 16 bytes with signed top-left
    // coordinates, so partial and fully offscreen sprites clip per pixel.
    // Record order is priority order and the lowest record wins pixel ties.
    void extended_sprites(const std::array<uint8_t,256> &background) {
        std::array<uint8_t,512> oam{};
        for (unsigned i=0; i<oam.size(); ++i) oam[i]=fetch(uint64_t(v(0x30,4))+i);
        std::array<bool,256> occupied{};
        const bool bpp8=(r(5)==3 || r(5)==4);
        const int line=int(line_.y);
        for (unsigned i=0; i<32; ++i) {
            const auto sprite=layout::ext_sprite(&oam[i*16]);
            if (!sprite.enable || sprite.size>2) continue;
            const unsigned height=sprite.size==0 ? 8 : 16;
            if (line<sprite.y || line>=sprite.y+int(height)) continue;
            unsigned row=unsigned(line-sprite.y);
            if (sprite.flip_y) row=height-1-row;
            // 8x16 pairs two consecutive 8x8 tiles; other sizes are one tile.
            unsigned tile=sprite.tile, tile_row=row;
            if (sprite.size==1) { tile=(tile&~1u)+row/8; tile_row=row%8; }
            const unsigned columns=sprite.size==2 ? 16 : 8;
            for (unsigned dx=0; dx<columns; ++dx) {
                const int x=sprite.x+int(dx);
                if (x<0 || x>=int(line_.width)) continue;
                const unsigned lx=sprite.flip_x ? columns-1-dx : dx;
                unsigned pixel;
                if (bpp8) {
                    const auto address=sprite.size==2
                        ? layout::packed8_16x16(v(0x34,4),tile,lx,tile_row)
                        : layout::packed8(v(0x34,4),tile,lx,tile_row);
                    pixel=fetch(address);
                } else {
                    const auto address=sprite.size==2
                        ? layout::packed4_16x16(v(0x34,4),tile,lx,tile_row)
                        : layout::packed4(v(0x34,4),tile,lx,tile_row);
                    pixel=layout::nibble(fetch(address),lx);
                }
                if (!pixel) continue;
                if (occupied[unsigned(x)]) continue;
                occupied[unsigned(x)]=true;
                // 8bpp pixels select the palette directly; 4bpp pixels add the
                // record palette as a 16-entry bank.
                const unsigned index=bpp8 ? pixel : unsigned(sprite.palette)*16+pixel;
                if (!sprite.behind || !background[unsigned(x)]) put(unsigned(x),palette_[index]);
            }
        }
    }
    const VideoLine &line_;
    Memory &memory_;
    std::span<uint8_t> output_;
    std::array<Color,256> palette_{};
    VideoEffects effects_{};
};
class TileRenderer final : public Renderer {
    VideoEffects render_scanline(const VideoLine &line, Memory &memory,
                                std::span<uint8_t> rgba) override {
        return TileLine(line,memory,rgba).render();
    }
};
} // namespace
std::unique_ptr<Renderer> make_tile_renderer() { return std::make_unique<TileRenderer>(); }
} // namespace vsn
