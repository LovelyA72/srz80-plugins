#include "protocol.hpp"
#include <algorithm>
#include <string>
#include <cctype>
namespace srz80::midi {
namespace {
uint64_t add(uint64_t a, uint64_t b) {
    if (b > UINT64_MAX - a) throw std::runtime_error("MIDI time overflow");
    return a + b;
}
uint64_t mul(uint64_t a, uint64_t b) {
    if (b && a > UINT64_MAX / b) throw std::runtime_error("MIDI time overflow");
    return a * b;
}
struct Reader {
    const Bytes &data; size_t pos, end;
    uint8_t byte() { if (pos == end) throw std::runtime_error("Truncated MIDI file"); return data[pos++]; }
    uint32_t number(unsigned n) { uint32_t v=0; while(n--) v=(v<<8)|byte(); return v; }
    uint32_t vlq() {
        uint32_t v=0;
        for(int i=0;i<4;++i) { auto b=byte(); v=(v<<7)|(b&127); if(!(b&128)) return v; }
        throw std::runtime_error("Invalid MIDI variable-length quantity");
    }
    Bytes bytes(size_t n) {
        if(n>end-pos) throw std::runtime_error("Truncated MIDI event");
        Bytes out(data.begin()+pos,data.begin()+pos+n); pos+=n; return out;
    }
};
enum class LoopMarker { None, MetaStart, MetaEnd, CcStart };
LoopMarker loop_marker(const Bytes &value) {
    std::string marker;
    for(const auto byte:value) if(std::isalnum(byte)) marker.push_back(static_cast<char>(std::tolower(byte)));
    if(marker=="loopstart") return LoopMarker::MetaStart;
    if(marker=="loopend") return LoopMarker::MetaEnd;
    return LoopMarker::None;
}
struct TickEvent { uint64_t tick; Bytes bytes; uint32_t tempo=0; LoopMarker loop=LoopMarker::None; };
}
ParsedSmf parse_smf(const Bytes &file) {
    if(file.size()>16*1024*1024) throw std::runtime_error("MIDI file exceeds 16 MiB");
    Reader r{file,0,file.size()};
    if(r.number(4)!=0x4d546864) throw std::runtime_error("Missing MThd");
    auto header=r.number(4);
    if(header<6 || header>r.end-r.pos) throw std::runtime_error("Invalid MIDI header");
    auto format=r.number(2), tracks=r.number(2), division=r.number(2);
    if(format>1 || !tracks || (format==0 && tracks!=1)) throw std::runtime_error("Only SMF formats 0 and 1 are supported");
    if(!division || (division&0x8000)) throw std::runtime_error("SMPTE division is unsupported");
    r.bytes(header-6);
    std::vector<TickEvent> events;
    for(unsigned track=0;track<tracks;++track) {
        if(r.number(4)!=0x4d54726b) throw std::runtime_error("Missing MTrk");
        auto length=r.number(4);
        if(length>r.end-r.pos) throw std::runtime_error("Truncated track");
        Reader t{file,r.pos,r.pos+length}; r.pos+=length;
        uint64_t tick=0; uint8_t running=0; bool ended=false;
        while(t.pos<t.end) {
            tick=add(tick,t.vlq()); auto status=t.byte(); Bytes bytes;
            if(status<0x80) {
                if(!running) throw std::runtime_error("MIDI running status without channel status");
                bytes={running,status}; status=running;
            } else bytes={status};
            if(status==0xff) {
                running=0; auto type=t.byte(); auto value=t.bytes(t.vlq());
                if(type==0x2f) {
                    if(!value.empty() || t.pos!=t.end) throw std::runtime_error("Invalid End of Track");
                    events.push_back({tick,{}}); // retain duration, including trailing silence
                    ended=true; break;
                }
                if(type==0x51) {
                    if(value.size()!=3) throw std::runtime_error("Invalid tempo event");
                    auto tempo=(uint32_t(value[0])<<16)|(uint32_t(value[1])<<8)|value[2];
                    if(!tempo) throw std::runtime_error("Zero tempo");
                    events.push_back({tick,{},tempo});
                }
                if(type==0x06) events.push_back({tick,{},0,loop_marker(value)});
            } else if(status==0xf0 || status==0xf7) {
                running=0; auto payload=t.bytes(t.vlq());
                if(status==0xf7) bytes.clear();
                bytes.insert(bytes.end(),payload.begin(),payload.end());
                if(bytes.size()>65536) throw std::runtime_error("SMF SysEx event exceeds 64 KiB tool limit");
                if(!bytes.empty()) events.push_back({tick,std::move(bytes)});
            } else {
                if(status<0xf0) running=status;
                else if(status<0xf8) running=0;
                unsigned count=status<0xf0 ? (((status&0xe0)==0xc0)?1:2) :
                    (status==0xf2?2:((status==0xf1||status==0xf3)?1:0));
                while(bytes.size()<count+1) { auto b=t.byte(); if(b&128) throw std::runtime_error("Invalid channel data"); bytes.push_back(b); }
                const auto loop=(status&0xf0)==0xb0 && bytes.size()==3 && bytes[1]==111
                    ? LoopMarker::CcStart : LoopMarker::None;
                events.push_back({tick,std::move(bytes),0,loop});
            }
            if(events.size()>1000000) throw std::runtime_error("Too many MIDI events");
        }
        if(!ended) throw std::runtime_error("Missing End of Track");
    }
    // Bytes after the declared MTrk chunks are ignored; each declared chunk was
    // validated strictly above.
    std::stable_sort(events.begin(),events.end(),[](const auto &a,const auto &b){return a.tick<b.tick;});
    uint64_t tick=0, time=0, remainder=0, tempo=500000;
    ParsedSmf result;
    std::optional<uint64_t> meta_loop_start, cc_loop_start;
    for(auto &event:events) {
        const auto delta=event.tick-tick, factor=tempo*1000;
        time=add(time,mul(delta/division,factor));
        auto fraction=add(mul(delta%division,factor),remainder);
        time=add(time,fraction/division); remainder=fraction%division; tick=event.tick;
        if(event.tempo) tempo=event.tempo;
        else {
            if(event.loop==LoopMarker::MetaStart) meta_loop_start=time;
            else if(event.loop==LoopMarker::MetaEnd && meta_loop_start && time>*meta_loop_start && !result.loop)
                result.loop=LoopRange{*meta_loop_start,time};
            else if(event.loop==LoopMarker::CcStart && !cc_loop_start) cc_loop_start=time;
            result.events.push_back({time,std::move(event.bytes)});
        }
    }
    // RPG Maker's CC#111 marks a loop start; its loop end is the song's
    // End-of-Track boundary. Explicit Marker pairs above take precedence.
    if(!result.loop && cc_loop_start && time>*cc_loop_start) result.loop=LoopRange{*cc_loop_start,time};
    return result;
}
}
