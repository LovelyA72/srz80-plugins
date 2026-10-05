// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <vector>

namespace vsn::inspect {
class ImageCache {
public:
    struct Run { unsigned x,length; uint32_t color; };
    std::vector<std::vector<Run>> rows;
    unsigned width=0;
    bool pending=false;
    template<class Pixel> void update(std::vector<uint64_t> key,unsigned w,unsigned h,Pixel pixel) {
        if (key!=key_) {
            key_=std::move(key);next_=0;building_.clear();building_.resize(h);pending=true;
        }
        if (!pending) return;
        const unsigned end=std::min(next_+64,h);
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(1);
        for (;next_<end;++next_) {
            auto &row=building_[next_];
            for (unsigned x=0;x<w;++x) {
                const auto p=pixel(x,next_);
                const unsigned alpha=p>>24,bg=((x/8+next_/8)&1)?56:40;
                unsigned color=0xff000000;
                for (unsigned c=0;c<3;++c)
                    color|=((((p>>(8*c))&255)*alpha+bg*(255-alpha)+127)/255)<<(8*c);
                if (!row.empty() && row.back().color==color) ++row.back().length;
                else row.push_back({x,1,color});
            }
            if (std::chrono::steady_clock::now()>=deadline) { ++next_;break; }
        }
        if (next_==h) { rows=std::move(building_);width=w;pending=false; }
    }
    void clear() { key_.clear();rows.clear();building_.clear();pending=false;width=0; }
private:
    std::vector<uint64_t> key_;
    std::vector<std::vector<Run>> building_;
    unsigned next_=0;
};
}
