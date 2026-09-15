#include <boundary.hpp>
#include "device.hpp"
#include <cstring>
#include <cstdio>
#include <memory>
#include <string>
namespace {
using srz80::threews8pn::Device; struct Card{Device d;uint64_t base=0;SrhHandle mapping=0,stream=0;};
SrhStatus access(void *c, uint64_t a, uint8_t *v, bool) {
    auto &x = *static_cast<Card *>(c);
    if (!v || a < x.base || a - x.base >= Device::register_size) return SRH_INVALID;
    *v = x.d.read(uint32_t(a - x.base));
    return SRH_OK;
}
SrhStatus SRH_CALL read(void*c,uint64_t a,uint8_t*v){return access(c,a,v,false);} SrhStatus SRH_CALL peek(void*c,uint64_t a,uint8_t*v){return access(c,a,v,true);} 
SrhStatus SRH_CALL write(void*c,uint64_t a,uint8_t v){auto &x=*static_cast<Card*>(c);if(a<x.base||a-x.base>=Device::register_size)return SRH_INVALID;x.d.write(uint32_t(a-x.base),v);return SRH_OK;}
SrhStatus SRH_CALL render(void*c,uint64_t,uint32_t n,int16_t*out){if(!out)return SRH_INVALID;static_cast<Card*>(c)->d.render(n,out);return SRH_OK;}
SrhStatus SRH_CALL create(const ShouryoHost *h, SrhHandle owner, const SrhConfig *cfg, void **out) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!srz80::sdk::valid(h) || !srz80::sdk::valid(cfg) || !out || !h->map || !cfg->space ||
            cfg->size != Device::register_size || cfg->base > UINT64_MAX - (Device::register_size - 1))
            return SRH_INVALID;
        *out = nullptr;
        const void *ext = nullptr;
        if (!h->query || h->query(h->context, "host.audio.v1", &ext) != SRH_OK || !ext)
            return SRH_UNAVAILABLE;
        const auto *audio = static_cast<const SrhHostAudioV1 *>(ext);
        if (!srz80::sdk::valid(audio) || !audio->register_source ||
            audio->channels != 2 || audio->format != SRH_AUDIO_S16_STEREO)
            return SRH_INVALID;
        auto card = std::make_unique<Card>();
        card->base = cfg->base;
        SrhMapping mapping{SRH_INIT(SrhMapping), cfg->space, cfg->base,
                           cfg->base + Device::register_size - 1, cfg->priority,
                           card.get(), read, write, peek, nullptr};
        auto status = h->map(h->context, owner, &mapping, &card->mapping);
        if (status != SRH_OK) return status;
        status = audio->register_source(audio->context, owner, Device::sample_rate, 2,
                                        SRH_AUDIO_S16_STEREO, "3WS8PN", render,
                                        card.get(), &card->stream);
        if (status != SRH_OK) return status;
        *out = card.release();
        return SRH_OK;
    });
}
void SRH_CALL destroy(void*c){delete static_cast<Card*>(c);} SrhStatus SRH_CALL reset(void*c,uint32_t){static_cast<Card*>(c)->d.reset();return SRH_OK;}
constexpr uint32_t pc=8*5; const char* suffix[] = {"Frequency","Waveform","Volume","Pan","LastSample"};
uint32_t SRH_CALL count(void*){return pc;} SrhStatus SRH_CALL info(void*,uint32_t i,SrhProperty*o){if(!o||i>=pc)return SRH_INVALID;static thread_local char n[32],g[32];auto c=i/5,f=i%5;std::snprintf(n,sizeof(n),"C%02u.%s",c,suffix[f]);std::snprintf(g,sizeof(g),"Channel %u",c+1);*o={SRH_INIT(SrhProperty),n,g,n,SRH_UNSIGNED,f==0?16u:(f==4?16u:8u),f==0?10u:16u,0,nullptr,SRH_PROPERTY_RUNTIME|SRH_PROPERTY_HIDE_UI};return SRH_OK;}
SrhStatus SRH_CALL get(void*c,uint32_t i,SrhValue*o){if(!o||i>=pc)return SRH_INVALID;auto &x=static_cast<Card*>(c)->d.ch[i/5];auto f=i%5;o->unsigned_value=f==0?x.frequency:f==1?x.type:f==2?x.volume:f==3?x.pan:x.last;return SRH_OK;} SrhStatus SRH_CALL set(void*,uint32_t,const SrhValue*){return SRH_INVALID;}
SrhStatus SRH_CALL save(void*,uint8_t*,uint64_t*s){if(!s)return SRH_INVALID;*s=0;return SRH_OK;} SrhStatus SRH_CALL load(void*,const uint8_t*,uint64_t s){return s?SRH_INVALID:SRH_OK;}
const SrhCardDescriptor desc{
    SRH_INIT(SrhCardDescriptor),
    "Audio",
    "3WS8PN",
    "8-channel wavetable/PCM/noise sound card",
    0xC0,
    Device::register_size,
    0,
    0,
    0,
    0,
    R"({})",
    nullptr,
    nullptr,
    nullptr,
    0};
const SrhPlugin api{SRH_INIT(SrhPlugin), "3ws8pn", create, destroy, reset, count, info, get, set,
                    save, load, &desc, nullptr, nullptr};
}
extern "C" SRH_EXPORT const SrhPlugin* SRH_CALL srz80_plugin_init(const ShouryoHost*h){return srz80::sdk::valid(h)?&api:nullptr;}
