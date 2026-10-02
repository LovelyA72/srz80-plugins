// SPDX-License-Identifier: MIT
#include "vsn_renderer.hpp"
#include "vsn_layout.hpp"
#include "vsn_registers.hpp"
#include "nes_palette.hpp"
#include <array>

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
    std::array<uint8_t,256> alpha{};
    uint64_t alpha_frame = ~0ull;
    uint32_t alpha_base = ~0u;
    uint8_t alpha_mode = 0xff;
    std::array<uint8_t,512> oam{};
    uint64_t palette_frame = ~0ull;
    uint32_t palette_base = ~0u;
    uint8_t palette_mode = 0xff, palette_nes_color = 0xff;
    uint64_t oam_frame = ~0ull;
    uint32_t oam_base = ~0u;
    uint8_t oam_mode = 0xff;
};

struct BackgroundRegisters {
    unsigned map, tiles, scroll_x, scroll_y, stride, columns, rows, tile_size;
    unsigned control, enable_mask, depth, depth_mask;
};

constexpr std::array<BackgroundRegisters,2> background_registers{{
    {reg::bg0_map,reg::bg0_tiles,reg::bg0_scroll_x,reg::bg0_scroll_y,
     reg::bg0_row_stride,reg::bg0_map_width,reg::bg0_map_height,reg::bg0_tile_size,
     reg::bg0_control,1,reg::bg0_control,2},
    {reg::map,reg::bg_tiles,reg::scroll_x,reg::scroll_y,
     reg::row_stride,reg::map_width,reg::map_height,reg::bg_tile_size,
     reg::control,2,reg::planar,1}
}};

struct BackgroundConfig {
    uint32_t map_base, tile_base;
    unsigned scroll_x, scroll_y, stride, columns, rows;
    layout::PackedGeometry geometry;
    uint8_t opacity;
    bool enabled, planar4;
};

struct BackgroundRow {
    BackgroundConfig config{};
    uint64_t descriptor_row=0;
    unsigned wrap_x=0, tile_row=0;
};

struct Pixel {
    uint8_t index=0, alpha=0;
};

struct SpritePixel {
    uint8_t index=0;
    bool behind=false;
};

Color blend(Color destination, const Color &source, unsigned alpha) {
    for (unsigned c=0; c<3; ++c)
        destination[c]=uint8_t((unsigned(source[c])*alpha +
                               unsigned(destination[c])*(255-alpha)+127)/255);
    return destination;
}

// Decode one scanline from guest memory.
class TileLine {
public:
    TileLine(const VideoLine &line, Memory &memory, std::span<uint8_t> output,
             FrameLatch &latch)
        : line_(line), memory_(memory), output_(output), latch_(latch), cache_(memory) {}
    VideoEffects render() {
        latch_palette();
        const bool nes=r(reg::mode)==0;
        std::array<BackgroundRow,2> rows{};
        rows[1]=prepare_row(background_config(1));
        if (!nes) {
            rows[0]=prepare_row(background_config(0));
            if (rows[0].config.enabled || rows[1].config.enabled) latch_alpha();
        }
        std::array<std::array<Pixel,512>,2> backgrounds{};
        for (unsigned layer=0; layer<2; ++layer) {
            const auto &row=rows[layer];
            if (!row.config.enabled) continue;
            for (unsigned x=0; x<line_.width; ++x) {
                if (nes && x<8 && !(r(reg::control)&8)) continue;
                const auto index=nes ? nes_background_pixel(x) : background_pixel(row,x);
                if (!index) continue;
                const unsigned alpha=(!nes && (r(reg::palette_alpha_control)&1))
                    ? latch_.alpha[index] : 255;
                backgrounds[layer][x]={index,uint8_t((alpha*row.config.opacity+127)/255)};
            }
        }
        std::array<SpritePixel,512> selected{};
        if (r(reg::control)&4) {
            if (nes || r(reg::mode)==2) sprites(backgrounds[1],selected);
            else extended_sprites(selected);
        }
        const auto backdrop=latch_.palette[nes ? 0 : r(reg::backdrop)];
        for (unsigned x=0; x<line_.width; ++x) {
            Color color=backdrop;
            const auto sprite=selected[x];
            if (sprite.index && sprite.behind) color=latch_.palette[sprite.index];
            for (const auto &background : backgrounds) {
                const auto pixel=background[x];
                if (pixel.alpha) color=blend(color,latch_.palette[pixel.index],pixel.alpha);
            }
            if (sprite.index && !sprite.behind) color=latch_.palette[sprite.index];
            put(x,color);
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
                const uint16_t low=fetch_direct(uint64_t(base)+i*2);
                const uint16_t word=low | (uint16_t(fetch_direct(uint64_t(base)+i*2+1))<<8);
                if (rgb555) {
                    const auto expand=[](uint16_t v){ return uint8_t((v<<3)|(v>>2)); };
                    palette[i]={expand((word>>10)&31),expand((word>>5)&31),expand(word&31),255};
                } else
                    palette[i]={uint8_t(((word>>8)&15)*17),uint8_t(((word>>4)&15)*17),
                                uint8_t((word&15)*17),255};
            }
        }
    }
    void latch_alpha() {
        if (!(r(reg::palette_alpha_control)&1)) return;
        const uint32_t base=v(reg::palette_alpha,4);
        if (latch_.alpha_base==base && latch_.alpha_frame==line_.frame &&
            latch_.alpha_mode==r(reg::mode)) return;
        for (unsigned i=0; i<256; ++i) latch_.alpha[i]=fetch_direct(uint64_t(base)+i);
        latch_.alpha_base=base;
        latch_.alpha_frame=line_.frame;
        latch_.alpha_mode=r(reg::mode);
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
    BackgroundConfig background_config(unsigned layer) const {
        const auto &offsets=background_registers[layer];
        const unsigned mode=r(reg::mode);
        const unsigned selector=r(offsets.tile_size);
        const auto geometry=mode==5 ? layout::hires_geometry(selector) :
            mode==4 ? layout::PackedGeometry{16,16} : layout::PackedGeometry{8,8};
        const unsigned columns=r(offsets.columns);
        const unsigned rows=r(offsets.rows);
        const uint8_t opacity=mode==0 ? 255 : uint8_t(((r(reg::bg_alpha)>>(layer*2))&3)*85);
        const bool enabled=(r(offsets.control)&offsets.enable_mask) && opacity!=0 &&
                           (layer==1 || mode!=0);
        return {v(offsets.map,4),v(offsets.tiles,4),
                v(offsets.scroll_x,2),v(offsets.scroll_y,2),v(offsets.stride,2),
                columns ? columns : 256, rows ? rows : 256, geometry, opacity, enabled,
                bool(r(offsets.depth)&offsets.depth_mask)};
    }
    BackgroundRow prepare_row(const BackgroundConfig &config) const {
        const unsigned y=(line_.y+config.scroll_y)%(config.rows*config.geometry.height);
        return {config,layout::packed_map(0,uint16_t(config.stride),0,y,config.geometry),
                config.columns*config.geometry.width,y%config.geometry.height};
    }
    uint8_t background_pixel(const BackgroundRow &row, unsigned x) {
        const auto &config=row.config;
        x=(x+config.scroll_x)%row.wrap_x;
        const uint16_t descriptor=descriptor_at(uint64_t(config.map_base)+row.descriptor_row+
                                               uint64_t(x/config.geometry.width)*2);
        const unsigned tile=descriptor&4095, bank=descriptor>>12;
        const unsigned lx=x%config.geometry.width;
        if (r(reg::mode)==1) {
            const auto address=layout::packed4(config.tile_base,tile,0,row.tile_row);
            const auto pixel=layout::nibble(cache_.fetch_word(address,lx/2),lx);
            return pixel ? uint8_t(bank*16+pixel) : 0;
        }
        if (r(reg::mode)==2) {
            const unsigned planes=config.planar4 ? 4 : 2;
            unsigned pixel=0;
            for (unsigned plane=0; plane<planes; ++plane) {
                const auto bits=cache_.fetch(layout::planar_tile(config.tile_base,tile,plane,
                                                               row.tile_row,planes));
                pixel|=((bits>>(7-lx))&1)<<plane;
            }
            return pixel ? uint8_t(bank*(1u<<planes)+pixel) : 0;
        }
        return cache_.fetch_word(layout::packed8(config.tile_base,tile,lx&~3u,
                                                 row.tile_row,config.geometry),lx&3);
    }
    uint8_t nes_background_pixel(unsigned x) {
        x=(x+v(reg::scroll_x,2))%512;
        const unsigned y=(line_.y+v(reg::scroll_y,2))%480;
        const auto page=layout::nes_page(v(reg::map,4),uint16_t(v(reg::page_x,2)),
                                        uint16_t(v(reg::page_y,2)),x,y);
        x%=256;
        const unsigned tile_y=(y%240)/8, tile_x=x/8;
        const auto tile=cache_.fetch(page+tile_y*32+tile_x);
        const auto attribute=cache_.fetch(layout::nes_attribute(page,tile_x,tile_y));
        const auto bank=(attribute>>layout::attribute_shift(tile_x,tile_y))&3;
        const auto pixel=pattern(layout::nes_pattern(v(reg::bg_tiles,4),r(reg::nes_pattern)&1,
                                                     tile,y%8),x&7);
        return pixel ? uint8_t(bank*4+pixel) : 0;
    }
    void sprites(const std::array<Pixel,512> &background, std::array<SpritePixel,512> &selected) {
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
                if (i==0 && background[x].alpha && x!=255) effects_.sprite_zero=true;
                if (occupied[x]) continue;
                occupied[x]=true;
                const unsigned index=planar
                    ? unsigned(sprite.palette)*(sprite4bpp?16:4)+pixel
                    : 16+unsigned(sprite.palette)*4+pixel;
                selected[x]={uint8_t(index),sprite.behind};
            }
        }
    }
    // Extended sprites use 32 records. Earlier records win pixel ties.
    void extended_sprites(std::array<SpritePixel,512> &selected) {
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
                selected[unsigned(x)]={uint8_t(index),sprite.behind};
            }
        }
    }
    const VideoLine &line_;
    Memory &memory_;
    std::span<uint8_t> output_;
    FrameLatch &latch_;
    ByteCache cache_;
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
