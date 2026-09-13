#pragma once
#include "protocol.hpp"
#include <array>
#include <map>
namespace srz80::midi {
// Mouse and keyboard are independent owners of a key. A note-off uses the
// channel captured at note-on, even if the channel selector changes meanwhile.
class Piano {
    struct Held { uint8_t channel; unsigned sources; };
    std::map<int,Held> held_;
public:
    static constexpr unsigned mouse=1, keyboard=2;
    int mouse_note=-1;
    int first_note=48, octaves=3;
    bool active(int note) const { return held_.contains(note); }
    void set(int note,unsigned source,bool down,int channel,int velocity,const Parser::Sink &sink) {
        if(note<0 || note>127 || channel<0 || channel>15) return;
        auto found=held_.find(note);
        if(down) {
            if(found!=held_.end()) { found->second.sources|=source; return; }
            held_.emplace(note,Held{uint8_t(channel),source});
            sink({uint8_t(0x90|channel),uint8_t(note),uint8_t(velocity)});
        } else if(found!=held_.end()) {
            found->second.sources&=~source;
            if(!found->second.sources) {
                sink({uint8_t(0x80|found->second.channel),uint8_t(note),0}); held_.erase(found);
            }
        }
    }
    void release(const Parser::Sink &sink) {
        for(const auto &[note,held]:held_) sink({uint8_t(0x80|held.channel),uint8_t(note),0});
        held_.clear(); mouse_note=-1;
    }
    void discard() { held_.clear(); mouse_note=-1; }
};
void draw_piano(Piano &,int channel,int velocity,bool enabled,
                const std::array<bool,128> &observed,const Parser::Sink &sink);
}
