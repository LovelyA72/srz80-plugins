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
    line_=lines()-1; frame_=faults_=fault_address_=0;
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
    if (offset<4) return std::array<uint8_t,4>{'V','S','N',1}[offset];
    if (offset==6) return registers_[6] | (vblank_ ? 0x80 : 0);
    if (offset==7) return 0x1f;
    if (offset==0x10) return 0;
    if (offset==0x11) return 1;
    if (offset==0x12) return 240;
    if (offset==0x44) return registers_[5]==0 ? 0 : (registers_[5]>=3 ? 2 : 1);
    if (offset==0x54) return uint8_t(line_);
    if (offset==0x55) return uint8_t(line_>>8);
    if (offset>=0x56 && offset<=0x5d) return uint8_t(frame_ >> ((offset-0x56)*8));
    return registers_[offset];
}
void Core::write(unsigned offset, uint8_t value) {
    if (offset==4) registers_[offset]=value&0x7f;
    else if (offset==5) { if (value<=4) registers_[offset]=value; }
    else if (offset==6) registers_[offset] &= ~(value&7);
    else if (offset==0x18) registers_[offset]=value&15;
    else if (offset==0x38) registers_[offset]=value&3;
    else if (offset==0x39) registers_[offset]=value&3;
    else if ((offset>=0x14 && offset<=0x17) || (offset>=0x20 && offset<=0x37) ||
             (offset>=0x40 && offset<=0x43) || offset==0x45) registers_[offset]=value;
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
bool Core::store(uint64_t address, uint8_t value) {
    if (address>UINT32_MAX || !memory_.write(address,value)) { fault(address,true); return false; }
    return true;
}
void Core::blank_line() {
    const size_t begin=size_t(line_)*surface_width*4;
    for (size_t x=0; x<width; ++x) {
        const size_t p=begin+x*4;
        back_[p]=back_[p+1]=back_[p+2]=0;
        back_[p+3]=255;
    }
}
void Core::render_line() {
    if (!(registers_[4]&1)) { blank_line(); return; }
    // This gateway keeps fault/strict policy independent of rendering style.
    // Wide addresses survive layout arithmetic all the way to the core check.
    class Gateway final : public Memory {
    public:
        explicit Gateway(Core &core) : core_(core) {}
        bool read(uint64_t address, uint8_t &value) override { value=core_.fetch(address); return true; }
        bool write(uint64_t address, uint8_t value) override { return core_.store(address,value); }
    private:
        Core &core_;
    } gateway(*this);
    const auto snapshot=registers_;
    const VideoLine line{frame_,line_,width,region_,snapshot};
    rendering_=true;
    try {
        const auto effects=renderer_->render_scanline(line,gateway,
            std::span(back_).subspan(size_t(line_)*surface_width*4,width*4));
        if (effects.sprite_zero) registers_[6] |= 2;
        if (effects.sprite_overflow) registers_[6] |= 4;
    } catch (const AbortLine &) {
        blank_line();
    } catch (...) {
        rendering_=false;
        throw;
    }
    rendering_=false;
}
void Core::tick() {
    if (line_==lines()-1) { vblank_=false; registers_[6] &= ~6; }
    if (line_==241) vblank_=true;
    if (line_<height) render_line();
    ++line_;
    if (line_==height) std::swap(front_, back_); // publish the completed frame
    if (line_==lines()) { line_=0; ++frame_; renderer_->begin_frame(frame_); }
}
std::vector<uint8_t> Core::save() const {
    std::vector<uint8_t> out;
    out.reserve(164+2*frame_bytes);
    state::append(out,0x314e5356,4); // VSN1
    state::append(out,2,2);
    state::append(out,unsigned(region_),1); state::append(out,strict_,1);
    out.insert(out.end(),registers_.begin(),registers_.end());
    state::append(out,line_,2); state::append(out,vblank_,1); state::append(out,fault_write_,1);
    state::append(out,frame_,8); state::append(out,faults_,8); state::append(out,fault_address_,8);
    out.insert(out.end(),front_.begin(),front_.end());
    out.insert(out.end(),back_.begin(),back_.end());
    return out;
}
bool Core::load(std::span<const uint8_t> data) {
    constexpr size_t header=164;
    if (data.size()!=header+2*frame_bytes || state::get(data,0,4)!=0x314e5356 ||
        state::get(data,4,2)!=2 || data[6]!=unsigned(region_) || data[7]!=strict_ ||
        state::get(data,136,2)>=lines() || data[138]>1 || data[139]>1) return false;
    // Validate all stored register bytes, including reserved and masked bits,
    // before touching live state. Status is stored separately from live blank.
    Core validator(memory_,region_,strict_);
    for (unsigned i=0; i<128; ++i) validator.write(i,data[8+i]);
    validator.registers_[6]=data[14]&7;
    if (!std::equal(validator.registers_.begin(),validator.registers_.end(),data.begin()+8)) return false;
    // Every shipped output pixel is opaque; reject corrupt alpha/border data.
    const auto valid_frame=[&](size_t off) {
        for (size_t i=0; i<frame_bytes; i+=4) {
            if (data[off+i+3]!=255) return false;
            const size_t pixel=i/4;
            if ((pixel%surface_width>=width || pixel/surface_width>=height) &&
                (data[off+i] || data[off+i+1] || data[off+i+2])) return false;
        }
        return true;
    };
    if (!valid_frame(header) || !valid_frame(header+frame_bytes)) return false;
    registers_=validator.registers_;
    line_=uint32_t(state::get(data,136,2)); vblank_=data[138]; fault_write_=data[139];
    frame_=state::get(data,140,8); faults_=state::get(data,148,8); fault_address_=state::get(data,156,8);
    std::copy(data.begin()+header,data.begin()+header+frame_bytes,front_.begin());
    std::copy(data.begin()+header+frame_bytes,data.end(),back_.begin());
    renderer_->reset(); // Clears the frame-latched palette/OAM caches.
    return true;
}
} // namespace vsn
