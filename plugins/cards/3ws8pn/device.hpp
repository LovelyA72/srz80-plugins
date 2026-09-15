#pragma once
#include <array>
#include <algorithm>
#include <cstdint>
#include <vector>
namespace srz80::threews8pn {
struct Channel { std::array<uint8_t,256> wave{}; uint16_t frequency=0; uint8_t type=0,volume=0,pan=0xff,mod=0,target=0,target_mode=0; uint16_t p1=0,p2=0; uint32_t start=0,end=0,loop=0; uint8_t control=0; double phase=0; uint32_t lfsr=0x12d4803c; int16_t last=0; bool active=true; Channel(){wave.fill(0x80);} };
struct Device {
 static constexpr uint32_t register_size=0x1000,pcm_size=1u<<20,channel_count=8,sample_rate=48'000,subsample_count=4; std::array<Channel,channel_count> ch{}; std::vector<uint8_t> pcm = std::vector<uint8_t>(pcm_size, 0);
 void reset(){std::fill(pcm.begin(),pcm.end(),0);for(auto &c:ch){auto w=c.wave;c=Channel{};c.wave=w;}}
 void write_pcm(uint32_t a,uint8_t v){if(a<pcm_size)pcm[a]=v;}
 uint8_t read_pcm(uint32_t a) const{return a<pcm_size?pcm[a]:0;}
 uint8_t read(uint32_t a) const {if(a<0x800)return ch[a/256].wave[a&255];if(a<0x900){auto &c=ch[(a-0x800)/32];auto o=(a-0x800)&31;if(o<2)return uint8_t(c.frequency>>(8*(1-o)));if(o==2)return c.type;if(o==3)return c.volume;if(o==4)return c.pan;if(o==5)return uint8_t((c.mod<<3)|(c.target&7));if(o==10)return 0;if(o==11)return c.target_mode;if(o>=16&&o<=18)return uint8_t(c.start>>(8*(18-o)));if(o>=19&&o<=21)return uint8_t(c.end>>(8*(21-o)));if(o>=22&&o<=24)return uint8_t(c.loop>>(8*(24-o)));if(o==25)return c.control;}return 0;}
 void write(uint32_t a,uint8_t v){if(a<0x800){ch[a/256].wave[a&255]=v;return;}if(a<0x900){auto &c=ch[(a-0x800)/32];auto o=(a-0x800)&31;if(o<2)c.frequency=(o?uint16_t(c.frequency&0xff00)|v:uint16_t(v)<<8|uint8_t(c.frequency));else if(o==2)c.type=v;else if(o==3)c.volume=v;else if(o==4)c.pan=v;else if(o==5){c.mod=v>>3;c.target=v&7;}else if(o<8)c.p1=uint16_t((o==6?uint16_t(v)<<8:v)|(o==6?c.p1&255:c.p1&0xff00));else if(o<10)c.p2=uint16_t((o==8?uint16_t(v)<<8:v)|(o==8?c.p2&255:c.p2&0xff00));else if(o==10)c.phase=0;else if(o==11)c.target_mode=v&1;else if(o>=16&&o<=18)c.start=(c.start&~(0xffu<<(8*(18-o))))|uint32_t(v)<<(8*(18-o));else if(o>=19&&o<=21)c.end=(c.end&~(0xffu<<(8*(21-o))))|uint32_t(v)<<(8*(21-o));else if(o>=22&&o<=24)c.loop=(c.loop&~(0xffu<<(8*(24-o))))|uint32_t(v)<<(8*(24-o));else if(o==25){c.control=v;if(c.type==1)c.active=(v&1)!=0;}}
 }
 int16_t sample(uint32_t i){auto &c=ch[i];if(c.type==1&&!c.active)return 0;double step=double(c.frequency)/double(subsample_count*sample_rate);int32_t s=0;if(c.type==0){s=int32_t(c.wave[uint32_t(c.phase*256.0)&255])-128;c.phase+=step;}else if(c.type==1){if(c.end<=c.start||c.start>=pcm_size)return 0;auto p=c.start+uint32_t(c.phase*256.0);if(p>=c.end){if((c.control&2)&&c.loop<c.end)c.phase=double(c.loop-c.start)/256.0;else return 0;p=c.start+uint32_t(c.phase*256.0);}s=int32_t(pcm[std::min(p,pcm_size-1u)])-128;c.phase+=step/8.0;}else if(c.type==2){auto b=((c.lfsr>>0)^(c.lfsr>>10)^(c.lfsr>>30)^(c.lfsr>>31))&1;c.lfsr=(c.lfsr>>1)|(b<<31);s=(c.lfsr&1)?127:-128;c.phase+=step;}c.last=int16_t(std::clamp(s*int32_t(c.volume)/4,-32768,32767));return c.last;}
 void render(uint32_t n,int16_t*out){std::fill(out,out+2*n,0);for(uint32_t f=0;f<n;++f)for(uint32_t sub=0;sub<subsample_count;++sub)for(uint32_t i=0;i<channel_count;++i){auto s=sample(i);auto l=(ch[i].pan>>4)*s/15;auto r=(ch[i].pan&15)*s/15;out[2*f]=int16_t(std::clamp(int(out[2*f])+l,-32768,32767));out[2*f+1]=int16_t(std::clamp(int(out[2*f+1])+r,-32768,32767));}}
}; }
