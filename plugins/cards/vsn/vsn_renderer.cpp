// SPDX-License-Identifier: MIT
#include "vsn_renderer.hpp"
#include "vsn_layout.hpp"
#include "nes_palette.hpp"
#include <array>
#include <limits>

namespace vsn {
namespace {
using Color=std::array<uint8_t,4>;

// Cache guest bytes for one scanline. Full address tags prevent collisions
// from returning the wrong byte.
class ByteCache {
public:
    explicit ByteCache(Memory &memory) : memory_(memory) { tags_.fill(~0ull); word_tags_.fill(~0ull); }
    uint8_t fetch(uint64_t address) {
        const uint32_t slot = uint32_t((address * 0x9e3779b97f4a7c15ull) >> (64 - kBits));
        if (tags_[slot] == address) return values_[slot];
        uint8_t value = 0xff;
        memory_.read(address, value);
        tags_[slot] = address;
        values_[slot] = value;
        return value;
    }
    // Read one aligned word within the tile row.
    uint8_t fetch_word(uint64_t address, unsigned byte) {
        const uint32_t slot = uint32_t((address * 0x9e3779b97f4a7c15ull) >> (64 - kBits));
        if (word_tags_[slot] == address) return uint8_t(word_values_[slot] >> (byte * 8));
        uint32_t word = 0xffffffff;
        memory_.read_word(address, word);
        word_tags_[slot] = address;
        word_values_[slot] = word;
        return uint8_t(word >> (byte * 8));
    }
private:
    static constexpr unsigned kBits = 10;
    Memory &memory_;
    std::array<uint64_t, 1u << kBits> tags_;
    std::array<uint8_t, 1u << kBits> values_;
    std::array<uint64_t, 1u << kBits> word_tags_;
    std::array<uint32_t, 1u << kBits> word_values_;
};

// Fetch palette and sprite data on first use each frame. Source or format
// changes invalidate the cached data.
struct FrameLatch {
    std::array<Color,256> palette{};
    std::array<uint8_t,512> oam{};
    uint64_t palette_frame = ~0ull;
    uint32_t palette_base = ~0u;
    uint8_t palette_mode = 0xff, palette_nes_color = 0xff;
    uint64_t oam_frame = ~0ull;
    uint32_t oam_base = ~0u;
    uint8_t oam_mode = 0xff;
};

// Decode one scanline from guest memory.
class TileLine {
public:
    TileLine(const VideoLine &line, Memory &memory, std::span<uint8_t> output,
             FrameLatch &latch)
        : line_(line), memory_(memory), output_(output), latch_(latch), cache_(memory) {}
    VideoEffects render() {
        latch_palette();
        prepare_row();
        const auto control=r(4);
        const auto backdrop=latch_.palette[r(5) == 0 ? 0 : r(0x45)];
        std::array<uint8_t,512> background{};
        for (unsigned x=0; x<line_.width; ++x) {
            if ((control & 2) && (r(5) != 0 || x>=8 || (control & 8)))
                background[x]=background_pixel(x);
            put(x, background[x] ? latch_.palette[background[x]] : backdrop);
        }
        if (control & 4) {
            // NES and planar4 keep the 256-byte OAM and planar decode. Every
            // packed mode reads the 512-byte extended table.
            if (r(5)==0 || r(5)==2) sprites(background);
            else extended_sprites(background);
        }
        return effects_;
    }
private:
    uint8_t r(unsigned offset) const { return line_.registers[offset]; }
    uint32_t v(unsigned offset, unsigned bytes) const { return line_.value(offset,bytes); }
    uint8_t fetch_direct(uint64_t address) {
        uint8_t value=0xff;
        memory_.read(address, value);
        return value;
    }
    uint16_t descriptor_at(uint64_t address) {
        return uint16_t(cache_.fetch(address)) | (uint16_t(cache_.fetch(address+1))<<8);
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
        // VSN digital emphasis attenuates unselected channels by 3/4.
        // Selecting all three attenuates all channels, with integer truncation.
        for (unsigned channel=0; channel<3; ++channel)
            if (emphasis && (emphasis==7 || !(emphasis & (1u<<channel))))
                color[channel]=uint8_t(unsigned(color[channel])*3/4);
        return color;
    }
    void latch_palette() {
        const uint32_t base=v(0x40,4);
        const uint8_t mode=r(5), nes=r(0x18);
        if (latch_.palette_mode==mode && latch_.palette_base==base &&
            latch_.palette_nes_color==nes && latch_.palette_frame==line_.frame) return;
        latch_.palette_mode=mode; latch_.palette_base=base;
        latch_.palette_nes_color=nes; latch_.palette_frame=line_.frame;
        auto &palette=latch_.palette;
        if (mode==0) {
            for (unsigned i=0; i<32; ++i) {
                if (i>=16 && !(i&3)) palette[i]=palette[i-16];
                else palette[i]=nes_color(fetch_direct(base+i));
            }
        } else {
            // Modes 3/4/5 use linear RGB555. Packed4 and planar4 use RGB444.
            const bool rgb555=(mode==3 || mode==4 || mode==5);
            for (unsigned i=0; i<256; ++i) {
                const uint16_t low=fetch_direct(base+i*2);
                const uint16_t word=low | (uint16_t(fetch_direct(base+i*2+1))<<8);
                if (rgb555) {
                    const auto expand=[](uint16_t v){ return uint8_t((v<<3)|(v>>2)); };
                    palette[i]={expand((word>>10)&31),expand((word>>5)&31),expand(word&31),255};
                } else
                    palette[i]={uint8_t(((word>>8)&15)*17),uint8_t(((word>>4)&15)*17),
                                uint8_t((word&15)*17),255};
            }
        }
    }
    void latch_oam() {
        const uint32_t base=v(0x30,4);
        const uint8_t mode=r(5);
        if (latch_.oam_mode==mode && latch_.oam_base==base && latch_.oam_frame==line_.frame) return;
        latch_.oam_mode=mode; latch_.oam_base=base; latch_.oam_frame=line_.frame;
        const unsigned size=(mode==0 || mode==2) ? 256 : 512;
        for (unsigned i=0; i<size; ++i) latch_.oam[i]=fetch_direct(uint64_t(base)+i);
    }
    uint8_t pattern(uint64_t address, unsigned x) {
        const auto low=cache_.fetch(address);
        const auto high=cache_.fetch(address+8);
        return layout::planar2(low,high,x);
    }
    // Calculate shared row addresses and offsets once per scanline.
    void prepare_row() {
        const uint8_t mode=r(5);
        scroll_x_=uint16_t(v(0x14,2));
        map_base_=v(0x20,4);
        tile_base_=v(0x24,4);
        const uint32_t scrolled_y=uint32_t(line_.y)+v(0x16,2);
        const unsigned map_w=r(0x2a) ? r(0x2a) : 256;
        const unsigned map_h=r(0x2b) ? r(0x2b) : 256;
        if (mode==4 || mode==5) {
            stride_=uint16_t(v(0x28,2));
            wrap_x_=map_w*16;
            y_wrapped_=scrolled_y%(map_h*16);
            desc_row_=uint64_t(y_wrapped_/16)*stride_;
            tile_row_=y_wrapped_&15;
        } else if (mode>=1 && mode<=3) {
            stride_=uint16_t(v(0x28,2));
            wrap_x_=map_w*8;
            y_wrapped_=scrolled_y%(map_h*8);
            desc_row_=uint64_t(y_wrapped_/8)*stride_;
            tile_row_=y_wrapped_&7;
        } else { // mode 0 (NES)
            stride_x_=uint16_t(v(0x2c,2));
            stride_y_=uint16_t(v(0x2e,2));
            nes_page_y_=uint64_t((scrolled_y%480)/240)*stride_y_;
            nes_row_y_=scrolled_y%240;
            tile_row_=nes_row_y_&7;
        }
    }
    uint8_t background_pixel(unsigned x) {
        x+=scroll_x_;
        if (r(5)==1) {
            x %= wrap_x_;
            const uint16_t descriptor=descriptor_at(map_base_+desc_row_+uint64_t(x/8)*2);
            const unsigned lx=x&7;
            const auto row=cache_.fetch_word(tile_base_+uint64_t(descriptor&4095)*32+uint64_t(tile_row_)*4,lx/2);
            const auto pixel=layout::nibble(row,lx);
            return pixel ? uint8_t((descriptor>>12)*16+pixel) : 0;
        }
        if (r(5)==2) {
            // VT planar: same 16-bit descriptor map as packed4, tiles decoded as
            // 2bpp or 4bpp bitplanes over a linear base. Pixel zero is the
            // backdrop. Otherwise palette index is bank*stride+pixel.
            x %= wrap_x_;
            const uint16_t descriptor=descriptor_at(map_base_+desc_row_+uint64_t(x/8)*2);
            const unsigned tile=descriptor&4095, bank=descriptor>>12, lx=x&7;
            if (r(0x39)&1) {
                const auto p0=cache_.fetch(layout::planar_tile(tile_base_,tile,0,tile_row_,4));
                const auto p1=cache_.fetch(layout::planar_tile(tile_base_,tile,1,tile_row_,4));
                const auto p2=cache_.fetch(layout::planar_tile(tile_base_,tile,2,tile_row_,4));
                const auto p3=cache_.fetch(layout::planar_tile(tile_base_,tile,3,tile_row_,4));
                const auto pixel=layout::planar4(p0,p1,p2,p3,lx);
                return pixel ? uint8_t(bank*16+pixel) : 0;
            }
            const auto p0=cache_.fetch(layout::planar_tile(tile_base_,tile,0,tile_row_,2));
            const auto p1=cache_.fetch(layout::planar_tile(tile_base_,tile,1,tile_row_,2));
            const auto pixel=layout::planar2(p0,p1,lx);
            return pixel ? uint8_t(bank*4+pixel) : 0;
        }
        if (r(5)==3) {
            // Packed 8bpp: one byte per pixel. The palette index is the pixel
            // value, so descriptor bank bits are ignored.
            x %= wrap_x_;
            const uint16_t descriptor=descriptor_at(map_base_+desc_row_+uint64_t(x/8)*2);
            const unsigned lx=x&7;
            return cache_.fetch_word(tile_base_+uint64_t(descriptor&4095)*64+uint64_t(tile_row_)*8+(lx&~3u),lx&3);
        }
        if (r(5)==4 || r(5)==5) {
            // Modes 4 and 5 share 16x16 8bpp tiles. Mode 5 uses 512x480.
            x %= wrap_x_;
            const uint16_t descriptor=descriptor_at(map_base_+desc_row_+uint64_t(x/16)*2);
            const unsigned lx=x&15;
            return cache_.fetch_word(tile_base_+uint64_t(descriptor&4095)*256+uint64_t(tile_row_)*16+(lx&~3u),lx&3);
        }
        x%=512;
        const auto page=map_base_+uint64_t(x/256)*stride_x_+nes_page_y_;
        x%=256;
        const auto tile=cache_.fetch(page+(nes_row_y_/8)*32+x/8);
        const auto attribute=cache_.fetch(layout::nes_attribute(page,x/8,nes_row_y_/8));
        const auto bank=(attribute>>layout::attribute_shift(x/8,nes_row_y_/8))&3;
        const auto pixel=pattern(layout::nes_pattern(tile_base_,r(0x38)&1,tile,tile_row_),x&7);
        return pixel ? uint8_t(bank*4+pixel) : 0;
    }
    void sprites(const std::array<uint8_t,512> &background) {
        latch_oam();
        std::array<bool,512> occupied{};
        const unsigned height=(r(4)&64) ? 16 : 8;
        const bool planar=(r(5)==2);
        const bool sprite4bpp=planar && (r(0x39)&2);
        unsigned found=0;
        for (unsigned i=0; i<64; ++i) {
            const auto sprite=layout::nes_sprite(&latch_.oam[i*4]);
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
                    planes[p]=cache_.fetch(layout::planar_tile(v(0x34,4),tile,p,row,count));
            } else {
                const auto address=layout::nes_pattern(v(0x34,4),table,tile,row);
                planes[0]=cache_.fetch(address); planes[1]=cache_.fetch(address+8);
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
                if (!sprite.behind || !background[x]) put(x,latch_.palette[index]);
            }
        }
    }
    // Extended sprites use 32 records. Earlier records win pixel ties.
    void extended_sprites(const std::array<uint8_t,512> &background) {
        latch_oam();
        std::array<bool,512> occupied{};
        const bool bpp8=(r(5)==3 || r(5)==4 || r(5)==5);
        const int line=int(line_.y);
        for (unsigned i=0; i<32; ++i) {
            const auto sprite=layout::ext_sprite(&latch_.oam[i*16]);
            if (!sprite.enable || sprite.size>2) continue;
            const unsigned height=sprite.size==0 ? 8 : 16;
            if (line<sprite.y || line>=sprite.y+int(height)) continue;
            unsigned row=unsigned(line-sprite.y);
            if (sprite.flip_y) row=height-1-row;
            // 8x16 pairs two consecutive 8x8 tiles. Other sizes are one tile.
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
                    pixel=cache_.fetch(address);
                } else {
                    const auto address=sprite.size==2
                        ? layout::packed4_16x16(v(0x34,4),tile,lx,tile_row)
                        : layout::packed4(v(0x34,4),tile,lx,tile_row);
                    pixel=layout::nibble(cache_.fetch(address),lx);
                }
                if (!pixel) continue;
                if (occupied[unsigned(x)]) continue;
                occupied[unsigned(x)]=true;
                // 8bpp pixels select the palette directly. 4bpp pixels add the
                // record palette as a 16-entry bank.
                const unsigned index=bpp8 ? pixel : unsigned(sprite.palette)*16+pixel;
                if (!sprite.behind || !background[unsigned(x)]) put(unsigned(x),latch_.palette[index]);
            }
        }
    }
    const VideoLine &line_;
    Memory &memory_;
    std::span<uint8_t> output_;
    FrameLatch &latch_;
    ByteCache cache_;
    // Per-scanline row invariants, filled once by prepare_row().
    uint16_t scroll_x_=0;
    uint64_t map_base_=0;
    uint32_t tile_base_=0;
    uint16_t stride_=0, stride_x_=0, stride_y_=0;
    uint32_t wrap_x_=0, y_wrapped_=0, tile_row_=0, nes_row_y_=0;
    uint64_t desc_row_=0, nes_page_y_=0;
    VideoEffects effects_{};
};
class TileRenderer final : public Renderer {
public:
    void reset() override { latch_=FrameLatch{}; }
    VideoEffects render_scanline(const VideoLine &line, Memory &memory,
                                 std::span<uint8_t> rgba) override {
        return TileLine(line,memory,rgba,latch_).render();
    }
private:
    FrameLatch latch_{};
};
} // namespace
std::unique_ptr<Renderer> make_tile_renderer() { return std::make_unique<TileRenderer>(); }
} // namespace vsn
