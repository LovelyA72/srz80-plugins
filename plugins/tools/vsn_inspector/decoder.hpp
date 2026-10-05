// SPDX-License-Identifier: MIT
#pragma once
#include "vsn_layout.hpp"
#include "nes_palette.hpp"
#include <array>
#include <cstdint>
#include <functional>

namespace vsn::inspect {
struct Sprite {
    int x=0, y=0;
    unsigned tile=0, bank=0, width=8, height=8;
    bool enabled=false, flip_x=false, flip_y=false, behind=false;
    std::array<uint8_t,16> raw{};
};
struct Cell {
    uint64_t address=0;
    unsigned tile=0, bank=0, raw=0;
};
struct Decoder {
    std::array<uint8_t,128> registers{};
    std::function<uint8_t(uint64_t)> read;
    bool pal=false;
    unsigned value(unsigned offset, unsigned bytes=1) const {
        unsigned result=0;
        for (unsigned i=0; i<bytes; ++i) result|=unsigned(registers[offset+i])<<(i*8);
        return result;
    }
    unsigned mode() const { return registers[5]; }
    unsigned word(uint64_t a) const { return read(a)|(unsigned(read(a+1))<<8); }
    bool oam() const { return mode()==0 || mode()==2; }
    unsigned sprite_count() const { return oam()?64:32; }
    unsigned depth(bool sprite, unsigned layer=1) const {
        if (mode()==0) return 2;
        if (mode()==1) return 4;
        if (mode()==2) return (sprite ? (value(0x39)&2) :
            (layer ? (value(0x39)&1) : (value(0x1a)&2))) ? 4:2;
        return 8;
    }
    layout::PackedGeometry geometry(unsigned layer) const {
        if (mode()==4) return {16,16};
        if (mode()==5) return layout::hires_geometry(value(layer?0x19:0x1b));
        return {8,8};
    }
    unsigned map_base(unsigned layer) const { return value(layer?0x20:0x70,4); }
    unsigned tile_base(bool sprite, unsigned layer=1) const {
        return value(sprite?0x34:(layer?0x24:0x74),4);
    }
    unsigned map_width(unsigned layer) const {
        if (mode()==0) return 64;
        const unsigned v=value(layer?0x2a:0x7a);
        return v?v:256;
    }
    unsigned map_height(unsigned layer) const {
        if (mode()==0) return 60;
        const unsigned v=value(layer?0x2b:0x7b);
        return v?v:256;
    }
    unsigned scroll(unsigned layer, bool y) const {
        return value(layer?(y?0x16:0x14):(y?0x7e:0x7c),2);
    }
    uint64_t pattern_address(bool sprite, unsigned layer, unsigned tile,
                             layout::PackedGeometry g) const {
        const uint64_t base=tile_base(sprite,layer);
        if (mode()==0) return base+((value(0x38)>>(sprite?1:0))&1)*4096ull+tile*16ull;
        return base+uint64_t(tile)*(mode()==2 ? depth(sprite,layer)*8 : g.bytes()*depth(sprite,layer)/8);
    }
    unsigned pixel(bool sprite, unsigned layer, unsigned tile,
                   layout::PackedGeometry g, unsigned x, unsigned y,
                   int nes_table=-1) const {
        uint64_t base=pattern_address(sprite,layer,tile,g);
        if (mode()==0 && nes_table>=0) base=uint64_t(tile_base(true))+nes_table*4096ull+tile*16ull;
        if (mode()==0 || mode()==2) {
            unsigned p=0;
            for (unsigned plane=0; plane<depth(sprite,layer); ++plane)
                p|=((read(base+plane*8+y)>>(7-x))&1)<<plane;
            return p;
        }
        if (depth(sprite,layer)==4) return layout::nibble(read(base+y*(g.width/2)+x/2),x);
        return read(base+y*g.width+x);
    }
    unsigned index(bool sprite, unsigned layer, unsigned bank, unsigned p) const {
        if (!p) return 0;
        if (mode()==0) return (sprite?16:0)+bank*4+p;
        return depth(sprite,layer)==8?p:bank*(1u<<depth(sprite,layer))+p;
    }
    uint32_t color(unsigned index, unsigned alpha=255) const {
        unsigned r,g,b;
        const uint64_t base=value(0x40,4);
        if (mode()==0) {
            index&=31;
            if (index>=16 && !(index&3)) index-=16;
            unsigned master=read(base+index)&63;
            if (value(0x18)&1) master&=0x30;
            const auto rgb=nes_rgb[master];
            r=rgb>>16; g=(rgb>>8)&255; b=rgb&255;
            unsigned emphasis=(value(0x18)>>1)&7;
            if (pal) emphasis=(emphasis&4)|((emphasis&1)<<1)|((emphasis&2)>>1);
            if (emphasis) {
                if (!(emphasis&1) || emphasis==7) r=r*3/4;
                if (!(emphasis&2) || emphasis==7) g=g*3/4;
                if (!(emphasis&4) || emphasis==7) b=b*3/4;
            }
        } else {
            const unsigned w=word(base+(index&255)*2);
            if (mode()<3) { r=((w>>8)&15)*17; g=((w>>4)&15)*17; b=(w&15)*17; }
            else {
                const auto expand=[](unsigned c){ return (c<<3)|(c>>2); };
                r=expand((w>>10)&31); g=expand((w>>5)&31); b=expand(w&31);
            }
        }
        return r|(g<<8)|(b<<16)|(alpha<<24);
    }
    Cell cell(unsigned layer, unsigned tx, unsigned ty) const {
        tx%=map_width(layer); ty%=map_height(layer);
        Cell c;
        if (mode()==0) {
            const auto page=layout::nes_page(map_base(layer),value(0x2c,2),value(0x2e,2),tx*8,ty*8);
            tx%=32; ty%=30;
            c.address=page+ty*32+tx;
            c.tile=c.raw=read(c.address);
            c.bank=(read(layout::nes_attribute(page,tx,ty))>>layout::attribute_shift(tx,ty))&3;
        } else {
            c.address=uint64_t(map_base(layer))+ty*uint64_t(value(layer?0x28:0x78,2))+tx*2;
            c.raw=word(c.address); c.tile=c.raw&4095; c.bank=c.raw>>12;
        }
        return c;
    }
    uint32_t background(unsigned layer, unsigned x, unsigned y) const {
        const auto g=geometry(layer);
        const auto c=cell(layer,x/g.width,y/g.height);
        const auto p=pixel(false,layer,c.tile,g,x%g.width,y%g.height);
        if (!p) return 0;
        const auto i=index(false,layer,c.bank,p);
        unsigned alpha=255;
        if (mode()!=0) {
            alpha=((value(0x1c)>>(layer*2))&3)*85;
            if (value(0x4a)&1) alpha=(alpha*read(uint64_t(value(0x46,4))+i)+127)/255;
        }
        return color(i,alpha);
    }
    Sprite sprite(unsigned n) const {
        Sprite s;
        const unsigned bytes=oam()?4:16;
        for (unsigned i=0; i<bytes; ++i) s.raw[i]=read(uint64_t(value(0x30,4))+n*bytes+i);
        if (oam()) {
            const auto o=layout::nes_sprite(s.raw.data());
            s.x=o.x; s.y=o.top; s.tile=o.tile; s.bank=o.palette;
            s.height=(value(4)&64)?16:8; s.enabled=o.top<240;
            s.flip_x=o.flip_x; s.flip_y=o.flip_y; s.behind=o.behind;
        } else {
            const auto o=layout::ext_sprite(s.raw.data());
            s.x=o.x; s.y=o.y; s.tile=o.tile; s.bank=o.palette;
            s.width=o.size==2?16:8; s.height=o.size==0?8:16;
            s.enabled=o.enable && o.size<=2;
            s.flip_x=o.flip_x; s.flip_y=o.flip_y; s.behind=o.behind;
        }
        return s;
    }
    uint32_t sprite_pixel(const Sprite &s, unsigned x, unsigned y) const {
        if (s.flip_x) x=s.width-1-x;
        if (s.flip_y) y=s.height-1-y;
        unsigned tile=s.tile;
        int table=-1;
        if (s.height==16 && s.width==8) {
            if (mode()==0) table=tile&1;
            tile=(tile&~1u)+y/8; y%=8;
        }
        const auto p=pixel(true,1,tile,{s.width,s.width},x,y,table);
        return p?color(index(true,1,s.bank,p)):0;
    }
};
}
