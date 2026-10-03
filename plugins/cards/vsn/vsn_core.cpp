// SPDX-License-Identifier: MIT
#include "vsn_core.hpp"
#include "state_bytes.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace vsn {
Core::Core(Memory &memory, Region region, bool strict, std::unique_ptr<Renderer> renderer)
    : memory_(memory), region_(region), strict_(strict), renderer_(std::move(renderer)),
      front_(frame_bytes), back_(frame_bytes) {
    if (!renderer_) throw std::invalid_argument("VSN requires a renderer");
    reset();
}
void Core::reset() {
    registers_.fill(0);
    registers_[4]=0x20;
    registers_[0x28]=64;
    registers_[0x2a]=32; registers_[0x2b]=30;
    registers_[0x2d]=4; registers_[0x2f]=8;
    registers_[reg::bg_alpha]=0x0f;
    registers_[reg::bg0_row_stride]=64;
    registers_[reg::bg0_map_width]=32;
    registers_[reg::bg0_map_height]=30;
    line_=lines()-1; frame_=faults_=fault_address_=0;
    published_frame_=published_line_=0;
    vblank_=fault_write_=rendering_=false;
    const auto clear=[](std::vector<uint8_t> &buffer) {
        std::fill(buffer.begin(),buffer.end(),0);
        for (size_t i=3; i<buffer.size(); i+=4) buffer[i]=255;
    };
    clear(front_); clear(back_);
    renderer_->reset();
}
uint32_t Core::value(unsigned offset, unsigned bytes) const {
    if (bytes>4 || offset>=128 || bytes>128-offset) return 0;
    uint32_t result=0;
    for (unsigned i=0; i<bytes; ++i) result |= uint32_t(read(offset+i)) << (i*8);
    return result;
}
uint8_t Core::read(unsigned offset) const {
    if (offset>=128) return 0;
    if (offset<4) return std::array<uint8_t,4>{'V','S','N',3}[offset];
    if (offset==6) return registers_[6] | (vblank_ ? 0x80 : 0);
    if (offset==7) return 0xff;
    if (offset==0x10 || offset==0x11) return uint8_t(logical_width() >> ((offset-0x10)*8));
    if (offset==0x12 || offset==0x13) return uint8_t(logical_height() >> ((offset-0x12)*8));
    if (offset==0x44) return registers_[5]==0 ? 0 : (registers_[5]>=3 ? 2 : 1);
    if (offset==0x54) return uint8_t(line_);
    if (offset==0x55) return uint8_t(line_>>8);
    if (offset>=0x56 && offset<=0x5d) return uint8_t(frame_ >> ((offset-0x56)*8));
    return registers_[offset];
}
void Core::write(unsigned offset, uint8_t value) {
    if (offset==4) registers_[offset]=value&0x7f;
    else if (offset==5) { if (value<=5) registers_[offset]=value; }
    else if (offset==6) registers_[offset] &= ~(value&7);
    else if (offset==8) registers_[offset] &= ~(value&0x0f);   // W1C acknowledge
    else if (offset==9) registers_[offset]=value&0x0f;          // cause enable mask
    else if (offset==0x18) registers_[offset]=value&15;
    else if (offset==reg::bg_tile_size || offset==reg::bg0_tile_size) {
        if (value<=2) registers_[offset]=value;
    }
    else if (offset==reg::bg0_control) registers_[offset]=value&3;
    else if (offset==reg::bg_alpha) registers_[offset]=value&15;
    else if (offset==reg::palette_alpha_control) registers_[offset]=value&1;
    else if (offset==0x38) registers_[offset]=value&3;
    else if (offset==0x39) registers_[offset]=value&3;
    else if (offset==0x6c) { registers_[offset]=value&0x0e; if (value&1) start_dma(); }
    else if (offset==0x6d) {}                                    // read-only status
    else if (offset>=0x60 && offset<=0x6b) {                     // live DMA src/dst/count
        if (!(registers_[0x6d]&1)) registers_[offset]=value;     // ignore while busy
    }
    else if ((offset>=0x14 && offset<=0x17) || (offset>=0x20 && offset<=0x37) ||
             (offset>=0x40 && offset<=0x43) || offset==0x45 ||
             (offset>=reg::palette_alpha && offset<reg::palette_alpha+4) ||
             (offset>=reg::bg0_map && offset<128) ||
             offset==0x50 || offset==0x51 || offset==0x6e) registers_[offset]=value;
}
void Core::fault(uint64_t address, bool writing) {
    registers_[6] |= 1;
    fault_address_=address; fault_write_=writing;
    if (faults_!=std::numeric_limits<uint64_t>::max()) ++faults_;
    if (strict_ && rendering_) throw AbortLine{};
}
uint8_t Core::fetch(uint64_t address) {
    uint8_t value=0xff;
    if (address>UINT32_MAX || !memory_.read(address,value)) {
        fault(address,false); return 0xff;
    }
    return value;
}
bool Core::fetch_word(uint64_t address, uint32_t &value) {
    if (address<=UINT32_MAX && memory_.read_word(address,value)) return true;
    // Fall back to byte reads when a word read fails. Each failed byte still
    // counts as a fault and can abort the line in strict mode.
    value=0;
    bool ok=true;
    for (unsigned i=0; i<4; ++i) {
        const uint64_t byte=address+i;
        uint8_t v=0xff;
        if (byte>UINT32_MAX || !memory_.read(byte,v)) {
            fault(byte,false);
            ok=false;
        }
        value|=uint32_t(v)<<(8*i);
    }
    return ok;
}
bool Core::store(uint64_t address, uint8_t value) {
    if (address>UINT32_MAX || !memory_.write(address,value)) { fault(address,true); return false; }
    return true;
}
uint32_t Core::reg32(unsigned offset) const {
    return uint32_t(registers_[offset]) | (uint32_t(registers_[offset+1])<<8) |
           (uint32_t(registers_[offset+2])<<16) | (uint32_t(registers_[offset+3])<<24);
}
void Core::set_reg32(unsigned offset, uint32_t value) {
    for (unsigned i=0; i<4; ++i) registers_[offset+i]=uint8_t(value>>(8*i));
}
void Core::start_dma() {
    if (registers_[0x6d]&1) return;  // ignore a start while a transfer is running
    registers_[0x6d]=1;              // busy. Clears sticky complete and fault
}
void Core::dma_fault() {
    registers_[0x6d]=4;              // fault, not busy
    registers_[8]|=8;                // pending DMA-fault interrupt
}
void Core::dma_chunk() {
    if (!(registers_[0x6d]&1)) return;
    const bool fill=(registers_[0x6c]&2)!=0;
    const bool hold_src=(registers_[0x6c]&4)!=0;
    const bool hold_dst=(registers_[0x6c]&8)!=0;
    uint32_t src=reg32(0x60), dst=reg32(0x64), count=reg32(0x68);
    for (unsigned i=0; i<dma_bytes_per_line && count; ++i) {
        uint8_t byte=registers_[0x6e];
        if (!fill) {
            uint8_t v=0xff;
            if (!memory_.read(src,v)) {
                set_reg32(0x60,src); set_reg32(0x64,dst); set_reg32(0x68,count);
                fault(src,false); dma_fault(); return;
            }
            byte=v;
        }
        if (!memory_.write(dst,byte)) {
            set_reg32(0x60,src); set_reg32(0x64,dst); set_reg32(0x68,count);
            fault(dst,true); dma_fault(); return;
        }
        if (!hold_src) ++src;
        if (!hold_dst) ++dst;
        --count;
    }
    set_reg32(0x60,src); set_reg32(0x64,dst); set_reg32(0x68,count);
    if (!count) { registers_[0x6d]=2; registers_[8]|=4; }  // complete
}
void Core::blank_rows() {
    // One raster line maps to two 512-wide surface rows in every mode.
    for (unsigned r=0; r<2; ++r) {
        const size_t begin=(size_t(line_)*2+r)*surface_width*4;
        for (size_t x=0; x<surface_width; ++x) {
            const size_t p=begin+x*4;
            back_[p]=back_[p+1]=back_[p+2]=0;
            back_[p+3]=255;
        }
    }
}
void Core::render_row(const std::array<uint8_t,128> &snapshot, Memory &gateway,
                      unsigned y, unsigned logical_width, std::span<uint8_t> rgba) {
    const VideoLine line{frame_,y,logical_width,region_,snapshot};
    const auto effects=renderer_->render_scanline(line,gateway,rgba);
    if (effects.sprite_zero) registers_[6] |= 2;
    if (effects.sprite_overflow) registers_[6] |= 4;
}
void Core::upscale_row(const uint8_t *source) {
    // Scale one 256-pixel row into two identical 512-pixel rows.
    for (unsigned r=0; r<2; ++r) {
        uint8_t *dest=back_.data()+(size_t(line_)*2+r)*surface_width*4;
        for (unsigned x=0; x<width; ++x) {
            const uint8_t *s=source+x*4;
            uint8_t *d=dest+x*8;
            for (unsigned c=0; c<4; ++c) d[c]=d[4+c]=s[c];
        }
    }
}
void Core::render_line() {
    if (!(registers_[4]&1)) { blank_rows(); return; }
    // Check the full address and apply the rendering fault policy here.
    class Gateway final : public Memory {
    public:
        explicit Gateway(Core &core) : core_(core) {}
        bool read(uint64_t address, uint8_t &value) override { value=core_.fetch(address); return true; }
        bool write(uint64_t address, uint8_t value) override { return core_.store(address,value); }
        bool read_word(uint64_t address, uint32_t &value) override { return core_.fetch_word(address,value); }
    private:
        Core &core_;
    } gateway(*this);
    const auto snapshot=registers_;
    rendering_=true;
    try {
        if (snapshot[5]==5) {
            // High-resolution mode renders two native 512-wide rows per line.
            for (unsigned r=0; r<2; ++r) {
                const unsigned y=line_*2+r;
                render_row(snapshot,gateway,y,surface_width,
                    std::span(back_).subspan(size_t(y)*surface_width*4,surface_width*4));
            }
        } else {
            // Scale one 256-pixel row into two surface rows.
            std::array<uint8_t,width*4> row{};
            render_row(snapshot,gateway,line_,width,std::span<uint8_t>(row));
            upscale_row(row.data());
        }
    } catch (const AbortLine &) {
        blank_rows();
    } catch (...) {
        rendering_=false;
        throw;
    }
    rendering_=false;
}
void Core::tick() {
    if (line_==lines()-1) { vblank_=false; registers_[6] &= ~6; }
    if (line_==241) { vblank_=true; registers_[8]|=1; }         // vblank NMI cause
    const uint32_t compare=reg32(0x50)&0xffff;
    if (compare<lines() && line_==compare) registers_[8]|=2;    // raster IRQ cause
    dma_chunk();                                                 // one chunk per line
    if (line_<height) render_line();
    ++line_;
    if (line_==height) {
        std::swap(front_, back_);
        published_frame_=frame_;
        published_line_=line_;
    }
    if (line_==lines()) { line_=0; ++frame_; }
}
std::vector<uint8_t> Core::save() const {
    std::vector<uint8_t> out;
    out.reserve(176+2*frame_bytes);
    state::append(out,0x314e5356,4); // VSN1
    state::append(out,7,2);
    state::append(out,unsigned(region_),1); state::append(out,strict_,1);
    out.insert(out.end(),registers_.begin(),registers_.end());
    state::append(out,line_,2); state::append(out,vblank_,1); state::append(out,fault_write_,1);
    state::append(out,frame_,8); state::append(out,faults_,8); state::append(out,fault_address_,8);
    state::append(out,published_frame_,8); state::append(out,published_line_,4);
    out.insert(out.end(),front_.begin(),front_.end());
    out.insert(out.end(),back_.begin(),back_.end());
    return out;
}
bool Core::load(std::span<const uint8_t> data) {
    constexpr size_t header=176;
    if (data.size()!=header+2*frame_bytes || state::get(data,0,4)!=0x314e5356 ||
        state::get(data,4,2)!=7 || data[6]!=unsigned(region_) || data[7]!=strict_ ||
        state::get(data,136,2)>=lines() || data[138]>1 || data[139]>1 ||
        state::get(data,164,8)>state::get(data,140,8) ||
        (state::get(data,172,4)!=0 && state::get(data,172,4)!=height)) return false;
    // Validate all stored register bytes, including reserved and masked bits,
    // before touching live state. Status is stored separately from live blank.
    Core validator(memory_,region_,strict_);
    for (unsigned i=0; i<128; ++i) validator.write(i,data[8+i]);
    validator.registers_[6]=data[14]&7;             // W1C STATUS causes
    validator.registers_[8]=data[16]&0x0f;          // W1C interrupt pending
    validator.registers_[0x6d]=data[8+0x6d]&0x07;   // read-only DMA status
    if (!std::equal(validator.registers_.begin(),validator.registers_.end(),data.begin()+8)) return false;
    // Every surface pixel must be opaque.
    const auto valid_frame=[&](size_t off) {
        for (size_t i=0; i<frame_bytes; i+=4)
            if (data[off+i+3]!=255) return false;
        return true;
    };
    if (!valid_frame(header) || !valid_frame(header+frame_bytes)) return false;
    registers_=validator.registers_;
    line_=uint32_t(state::get(data,136,2)); vblank_=data[138]; fault_write_=data[139];
    frame_=state::get(data,140,8); faults_=state::get(data,148,8); fault_address_=state::get(data,156,8);
    published_frame_=state::get(data,164,8); published_line_=uint32_t(state::get(data,172,4));
    std::copy(data.begin()+header,data.begin()+header+frame_bytes,front_.begin());
    std::copy(data.begin()+header+frame_bytes,data.end(),back_.begin());
    renderer_->reset(); // Clears the frame-latched palette/OAM caches.
    return true;
}
} // namespace vsn
