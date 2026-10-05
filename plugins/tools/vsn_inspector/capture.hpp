// SPDX-License-Identifier: MIT
#pragma once
#include "decoder.hpp"
#include <srz80/tool.h>
#include <algorithm>
#include <map>
#include <unordered_map>
#include <vector>

namespace vsn::inspect {
struct View {
    bool tiles=true,sprites=true,layers=true;
    int tile_source=1,tile_page=0,selected_sprite=0,layer=1,layer_view=0,page_x=0,page_y=0;
    unsigned cell_x=0,cell_y=0;
    bool operator==(const View &) const = default;
};
class RangePlan {
    std::map<std::pair<SrhHandle,uint64_t>,uint64_t> ranges_;
public:
    void add(SrhHandle space,uint64_t address,uint32_t size) {
        if (!size || address>UINT64_MAX-size) return;
        uint64_t end=address+size;
        auto next=ranges_.lower_bound({space,address});
        if (next!=ranges_.begin()) {
            auto previous=std::prev(next);
            if (previous->first.first==space && previous->second>=address) {
                address=previous->first.second;end=std::max(end,previous->second);
                next=ranges_.erase(previous);
            }
        }
        while (next!=ranges_.end() && next->first.first==space && next->first.second<=end) {
            end=std::max(end,next->second);next=ranges_.erase(next);
        }
        ranges_[{space,address}]=end;
    }
    std::vector<SrhToolMemoryRange> take() const {
        std::vector<SrhToolMemoryRange> out;
        for (const auto &[key,end]:ranges_) {
            for (uint64_t address=key.second;address<end;) {
                const auto size=uint32_t(std::min<uint64_t>(end-address,SRH_TOOL_MEMORY_MAX_BYTES));
                out.push_back({SRH_INIT(SrhToolMemoryRange),key.first,address,size});address+=size;
            }
        }
        return out;
    }
};
class MemorySnapshot {
    struct Page { std::array<uint8_t,256> bytes{}; std::array<bool,256> valid{}; };
    std::unordered_map<uint64_t,Page> pages_;
public:
    unsigned failed=0;
    uint8_t read(uint64_t address) const {
        const auto page=pages_.find(address>>8);
        return page==pages_.end()?0:page->second.bytes[address&255];
    }
    void put(uint64_t address,uint8_t byte,SrhStatus status) {
        auto &page=pages_[address>>8];
        const auto offset=address&255;
        if (!page.valid[offset] && status!=SRH_OK) ++failed;
        page.valid[offset]=true;page.bytes[offset]=status==SRH_OK?byte:0;
    }
};
struct CaptureSource {
    SrhHandle owner=0,memory=0,io=0;
    uint64_t base=0,generation=0;
    bool pal=false;
    uint64_t memory_maximum=UINT32_MAX;
};
class Capture {
    const SrhToolHostV1 *host_;
    void *client_;
    enum class Stage { idle,registers,maps,patterns,verify } stage_=Stage::idle;
    CaptureSource source_{};
    View view_{};
    Decoder decoder_;
    MemorySnapshot staging_;
    SrhHandle request_=0;
    std::vector<SrhToolMemoryRange> ranges_,batch_;
    size_t cursor_=0;
    std::array<uint8_t,128> verification_{};
    void rejected(SrhStatus status) {
        const char *reason="unknown error";
        switch (status) {
        case SRH_ERROR: reason="host error";break;
        case SRH_INVALID: reason="invalid range or request";break;
        case SRH_NOT_FOUND: reason="request no longer available";break;
        case SRH_UNAVAILABLE: reason="host unavailable";break;
        case SRH_CONFLICT: reason="project changed";break;
        }
        const char *stage=stage_==Stage::registers?"registers":stage_==Stage::maps?"map data":
            stage_==Stage::patterns?"patterns":"register verification";
        error=std::string("Memory capture rejected: ")+stage+", "+reason;
        cancel();
    }
    void graphics(RangePlan &plan,uint64_t address,uint32_t size) {
        if (!size) return;
        const uint64_t maximum=std::min<uint64_t>(source_.memory_maximum,UINT32_MAX);
        if (address>maximum) { ++staging_.failed;return; }
        if (size-1>maximum-address) { ++staging_.failed;size=uint32_t(maximum-address+1); }
        const uint64_t end=address+size;
        if (source_.memory==source_.io && address<source_.base+128 && end>source_.base) {
            ++staging_.failed;
            if (address<source_.base) plan.add(source_.memory,address,uint32_t(source_.base-address));
            if (end>source_.base+128) plan.add(source_.memory,source_.base+128,uint32_t(end-source_.base-128));
        } else plan.add(source_.memory,address,size);
    }
    template<class Cell> void cells(Cell visit) {
        const unsigned layer=decoder_.mode()==0?1:unsigned(view_.layer);
        const auto g=decoder_.geometry(layer);
        const unsigned width=decoder_.mode()==5?512:256,height=decoder_.mode()==5?480:240;
        const unsigned mw=decoder_.map_width(layer)*g.width,mh=decoder_.map_height(layer)*g.height;
        const unsigned ox=view_.layer_view?unsigned(view_.page_x)*width:decoder_.scroll(layer,false);
        const unsigned oy=view_.layer_view?unsigned(view_.page_y)*height:decoder_.scroll(layer,true);
        for (unsigned y=0;y<height+(oy%g.height?g.height:0);y+=g.height)
            for (unsigned x=0;x<width+(ox%g.width?g.width:0);x+=g.width) {
            if (view_.layer_view && (ox+x>=mw || oy+y>=mh)) continue;
            visit(layer,((ox+x)%mw)/g.width,((oy+y)%mh)/g.height);
        }
        visit(layer,view_.cell_x%decoder_.map_width(layer),view_.cell_y%decoder_.map_height(layer));
    }
    void maps() {
        RangePlan plan;
        graphics(plan,decoder_.value(0x40,4),decoder_.mode()==0?32:512);
        if (decoder_.mode()!=0 && (decoder_.value(0x4a)&1)) graphics(plan,decoder_.value(0x46,4),256);
        if (view_.sprites || (view_.tiles && view_.tile_source==2))
            graphics(plan,decoder_.value(0x30,4),decoder_.oam()?256:512);
        if (view_.layers) cells([&](unsigned layer,unsigned tx,unsigned ty) {
            if (decoder_.mode()==0) {
                const auto page=layout::nes_page(decoder_.map_base(layer),decoder_.value(0x2c,2),decoder_.value(0x2e,2),tx*8,ty*8);
                graphics(plan,page+(ty%30)*32+tx%32,1);
                graphics(plan,layout::nes_attribute(page,tx%32,ty%30),1);
            } else graphics(plan,uint64_t(decoder_.map_base(layer))+ty*uint64_t(decoder_.value(layer?0x28:0x78,2))+tx*2,2);
        });
        ranges_=plan.take();cursor_=0;stage_=Stage::maps;
    }
    void patterns() {
        RangePlan plan;
        const auto pattern=[&](bool sprite,unsigned layer,unsigned tile,layout::PackedGeometry geometry) {
            graphics(plan,decoder_.pattern_address(sprite,layer,tile,geometry),decoder_.mode()==0?16:
                decoder_.mode()==2?decoder_.depth(sprite,layer)*8:geometry.bytes()*decoder_.depth(sprite,layer)/8);
        };
        if (view_.tiles) {
            const bool sprite=view_.tile_source==2;
            const unsigned layer=decoder_.mode()==0?1:(view_.tile_source==0?0:1);
            auto geometry=sprite?layout::PackedGeometry{8,8}:decoder_.geometry(layer);
            if (sprite && !decoder_.oam()) { const auto s=decoder_.sprite(unsigned(view_.selected_sprite)%decoder_.sprite_count());geometry={s.width,s.width}; }
            for (unsigned i=0;i<64;++i) pattern(sprite,layer,unsigned(view_.tile_page)*64+i,geometry);
        }
        if (view_.layers) cells([&](unsigned layer,unsigned x,unsigned y) {
            pattern(false,layer,decoder_.cell(layer,x,y).tile,decoder_.geometry(layer));
        });
        if (view_.sprites) for (unsigned i=0;i<decoder_.sprite_count();++i) {
            const auto s=decoder_.sprite(i);
            if (!s.enabled && int(i)!=view_.selected_sprite) continue;
            if (s.width==8 && s.height==16) {
                if (decoder_.mode()==0) graphics(plan,uint64_t(decoder_.tile_base(true))+(s.tile&1)*4096ull+(s.tile&~1u)*16ull,32);
                else { pattern(true,1,s.tile&~1u,{8,8});pattern(true,1,(s.tile&~1u)+1,{8,8}); }
            } else pattern(true,1,s.tile,{s.width,s.width});
        }
        ranges_=plan.take();cursor_=0;stage_=Stage::patterns;
    }
    bool configuration_matches() const {
        // Scrolling and opacity can move during a live capture
        // Restart only if the storage layout or palette interpretation changes
        for (unsigned offset=0;offset<4;++offset) if (decoder_.registers[offset]!=verification_[offset]) return false;
        for (unsigned offset:{5u,0x18u,0x19u,0x1bu,0x38u,0x39u,0x4au})
            if (decoder_.registers[offset]!=verification_[offset]) return false;
        if ((decoder_.registers[4]&64)!=(verification_[4]&64) ||
            (decoder_.registers[0x1a]&2)!=(verification_[0x1a]&2)) return false;
        for (unsigned offset=0x20;offset<0x38;++offset) if (decoder_.registers[offset]!=verification_[offset]) return false;
        for (unsigned offset=0x40;offset<0x4a;++offset) if (decoder_.registers[offset]!=verification_[offset]) return false;
        for (unsigned offset=0x70;offset<0x7c;++offset) if (decoder_.registers[offset]!=verification_[offset]) return false;
        return true;
    }
public:
    MemorySnapshot memory;
    Decoder published;
    CaptureSource published_source{};
    View published_view{};
    uint64_t revision=0,time_ns=0;
    std::string error;
    Capture(const SrhToolHostV1 *host,void *client):host_(host),client_(client) {}
    ~Capture() { cancel(); }
    bool busy() const { return stage_!=Stage::idle; }
    const View &requested_view() const { return view_; }
    void cancel() {
        if (request_) host_->memory_read_release(host_->context,client_,request_);
        request_=0;stage_=Stage::idle;ranges_.clear();batch_.clear();
    }
    void clear() { cancel();memory={};published={};published_source={};++revision; }
    void start(CaptureSource source,View view) {
        cancel();source_=source;view_=view;staging_={};decoder_={};decoder_.pal=source.pal;
        decoder_.read=[this](uint64_t a){return staging_.read(a);};
        ranges_={{SRH_INIT(SrhToolMemoryRange),source.io,source.base,128}};
        cursor_=0;stage_=Stage::registers;error.clear();
    }
    void advance() {
        if (!busy()) return;
        if (request_) {
            SrhToolMemoryResult result{SRH_INIT(SrhToolMemoryResult),0,0,0,0,0};
            const auto status=host_->memory_read_poll(host_->context,client_,request_,&result,nullptr,nullptr,0);
            if (status!=SRH_OK) { rejected(status);return; }
            if (!result.pending && result.status!=SRH_OK) { rejected(result.status);return; }
            if (!result.pending && result.generation!=source_.generation) { rejected(SRH_CONFLICT);return; }
            if (result.pending) return;
            unsigned expected=0;
            for (const auto &range:batch_) expected+=range.size;
            if (result.size!=expected) { error="Incomplete memory capture";cancel();return; }
            std::vector<uint8_t> bytes(result.size);
            std::vector<SrhStatus> statuses(result.size);
            if (host_->memory_read_poll(host_->context,client_,request_,&result,bytes.data(),statuses.data(),result.size)!=SRH_OK) {
                error="Memory capture copy failed";cancel();return;
            }
            host_->memory_read_release(host_->context,client_,request_);request_=0;
            size_t offset=0;
            for (const auto &range:batch_) for (unsigned i=0;i<range.size;++i,++offset) {
                if (stage_==Stage::registers) decoder_.registers[i]=statuses[offset]==SRH_OK?bytes[offset]:0;
                else if (stage_==Stage::verify) verification_[i]=statuses[offset]==SRH_OK?bytes[offset]:0;
                else staging_.put(range.address+i,bytes[offset],statuses[offset]);
            }
            time_ns=result.time_ns;
        }
        if (cursor_==ranges_.size()) {
            if (stage_==Stage::registers) {
                if (decoder_.value(0,4)!=0x034e5356 || decoder_.mode()>5) { error="VSN register capture unavailable";cancel();return; }
                maps();
            } else if (stage_==Stage::maps) patterns();
            else if (stage_==Stage::patterns) {
                ranges_={{SRH_INIT(SrhToolMemoryRange),source_.io,source_.base,128}};cursor_=0;stage_=Stage::verify;
            } else {
                if (!configuration_matches()) { const auto s=source_;const auto v=view_;start(s,v);return; }
                memory=std::move(staging_);published=decoder_;published.read={};published_source=source_;published_view=view_;
                ++revision;stage_=Stage::idle;return;
            }
        }
        batch_.clear();unsigned total=0;
        while (cursor_<ranges_.size() && batch_.size()<SRH_TOOL_MEMORY_MAX_RANGES &&
               ranges_[cursor_].size<=SRH_TOOL_MEMORY_MAX_BYTES-total) {
            total+=ranges_[cursor_].size;batch_.push_back(ranges_[cursor_++]);
        }
        if (batch_.empty()) return;
        const auto status=host_->memory_read_request(host_->context,client_,source_.generation,batch_.data(),uint32_t(batch_.size()),&request_);
        if (status==SRH_UNAVAILABLE) { cursor_-=batch_.size();return; }
        if (status!=SRH_OK) rejected(status);
    }
};
}
