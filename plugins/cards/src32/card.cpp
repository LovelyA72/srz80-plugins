#include <boundary.hpp>
#include <cstdint>
#include <cstring>
#include <memory>
#include <srz80/abi.h>

namespace {
constexpr uint32_t CPU_ID = 0x53524332u;
constexpr uint32_t FEATURES = 0x3Fu;
constexpr uint32_t NORMAL = 4;
constexpr uint32_t SHORT = 2;

enum class Mode : uint8_t { Normal, Short };
struct Cpu {
    const ShouryoHost* host{}; SrhHandle owner{}, space{}; uint32_t pc{}, reset{};
    uint32_t r[32]{}; bool running{true}; Mode mode{Mode::Normal}; uint64_t cycles{};
    SrhStatus read8(uint32_t a, uint8_t& v) { return host->read(host->context, owner, space, a, &v); }
    SrhStatus write8(uint32_t a, uint8_t v) { return host->write(host->context, owner, space, a, v); }
    uint32_t reg(unsigned n) const { return n == 0 ? 0 : r[n & 31]; }
    void set(unsigned n, uint32_t v) { if (n) r[n & 31] = v; }
    SrhStatus read16(uint32_t a, uint16_t& v) { uint8_t h,l; auto s=read8(a,h); if(s!=SRH_OK)return s; s=read8(a+1,l); v=(uint16_t(h)<<8)|l; return s; }
    SrhStatus read32(uint32_t a, uint32_t& v) { uint8_t b[4]; for(int i=0;i<4;i++){auto s=read8(a+uint32_t(i),b[i]);if(s!=SRH_OK)return s;} v=(uint32_t(b[0])<<24)|(uint32_t(b[1])<<16)|(uint32_t(b[2])<<8)|b[3]; return SRH_OK; }
    SrhStatus write32(uint32_t a,uint32_t v){ for(int i=0;i<4;i++){auto s=write8(a+uint32_t(i),uint8_t(v>>(24-8*i)));if(s!=SRH_OK)return s;} return SRH_OK; }
    static uint32_t add(uint32_t a,int16_t b){return a+uint32_t(int32_t(b));}
    static uint32_t branch(uint32_t a,int16_t b){return a+uint32_t(int32_t(b));}
    void reset_cpu(){std::memset(r,0,sizeof(r));pc=reset;running=true;mode=Mode::Normal;cycles=0;}
    SrhStatus normal(){uint32_t raw; auto s=read32(pc,raw); if(s!=SRH_OK)return s; uint32_t next=pc+4; pc=next; uint8_t op=raw>>26,rd=(raw>>21)&31,a=(raw>>16)&31,b=(raw>>11)&31; int16_t imm=int16_t(raw); uint16_t u=uint16_t(raw); auto wr=[&](uint32_t x){set(rd,x);};
      switch(op){
      case 0: break; case 1:{uint32_t x; s=read32(add(reg(a),imm),x);wr(x);break;} case 2:s=write32(add(reg(a),imm),reg(rd));break;
      case 3:wr(reg(a)+reg(b));break; case 4:wr(reg(a)+uint32_t(int32_t(imm)));break; case 5:wr(reg(a)-reg(b));break;
      case 6:wr(uint32_t(int32_t(reg(a))<int32_t(reg(b))));break; case 7:if(reg(a)==reg(rd))pc=branch(next,imm);break; case 8:if(reg(a)!=reg(rd))pc=branch(next,imm);break;
      case 9:pc=branch(next,imm);break; case 10:set(31,next);pc=branch(next,imm);break; case 11:pc=reg(rd);break;
      case 12:wr(reg(a)&reg(b));break; case 13:wr(reg(a)|reg(b));break; case 14:wr(reg(a)^reg(b));break;
      case 15:wr(reg(a)<<(reg(b)&31));break; case 16:wr(reg(a)>>(reg(b)&31));break; case 17:wr(reg(a)<<(reg(b)&31));break; case 18:wr(uint32_t(int32_t(reg(a))>>(reg(b)&31)));break;
      case 19:{uint8_t x;s=read8(add(reg(a),imm),x);wr(x);break;} case 20:{uint16_t x;s=read16(add(reg(a),imm),x);wr(x);break;}
      case 21:s=write8(add(reg(a),imm),uint8_t(reg(rd)));break; case 22:{uint32_t x=reg(rd);s=write8(add(reg(a),imm),uint8_t(x>>8));if(s==SRH_OK)s=write8(add(reg(a),imm)+1,uint8_t(x));break;}
      case 23:wr(uint32_t(reg(a)<reg(b)));break; case 24:wr(uint32_t(uint64_t(reg(a))*reg(b)));break;
      case 25:{int32_t x=int32_t(reg(a)),y=int32_t(reg(b));wr(y?uint32_t((x==INT32_MIN&&y==-1)?INT32_MIN:x/y):0);break;} case 26:{int32_t x=int32_t(reg(a)),y=int32_t(reg(b));wr(y?uint32_t((x==INT32_MIN&&y==-1)?0:x%y):0);break;}
      case 27:wr(uint32_t((int64_t(int32_t(reg(a)))*int64_t(int32_t(reg(b))))>>32));break; case 28:wr(reg(b)?reg(a)/reg(b):0);break;
      case 29:pc=branch(next,imm);mode=Mode::Short;break; case 30:set(31,next);pc=branch(next,imm);mode=Mode::Short;break; case 31:pc=reg(rd);mode=Mode::Short;break;
      case 33:set(31,next);pc=reg(rd);break; case 34:{uint32_t t=reg(rd);set(31,next);pc=t;mode=Mode::Short;break;}
      case 0x20:return SRH_INVALID; case 0x3c:wr((reg(rd)&0xffff0000u)|uint32_t(u));break; case 0x3d:wr((reg(rd)&0xffffu)|(uint32_t(u)<<16));break; case 0x3e:set(1,CPU_ID);set(2,FEATURES);break; case 0x3f:running=false;break; default:return SRH_INVALID; }
      return s;
    }
    SrhStatus short_step(){uint16_t raw;auto s=read16(pc,raw);if(s!=SRH_OK)return s;uint32_t next=pc+2;pc=next;uint8_t op=raw>>12,rd=(raw>>8)&15,a=(raw>>4)&15,b=raw&15;auto R=[&](uint8_t x){return reg(x<15?x:31);};auto W=[&](uint8_t x,uint32_t v){set(x<15?x:31,v);};int8_t i8=uint8_t(raw);int16_t i12=int16_t(raw&0xfff);if(i12&0x800)i12|=int16_t(0xf000);switch(op){case 0:W(rd,R(a));break;case 1:W(rd,R(a)+R(b));break;case 2:W(rd,R(rd)+uint32_t(int32_t(i8)));break;case 3:{uint32_t x;s=read32(R(a),x);W(rd,x);break;}case 4:s=write32(R(rd),R(a));break;case 5:if(!R(rd))pc=branch(next,i8);break;case 6:if(R(rd))pc=branch(next,i8);break;case 7:pc=R(rd);break;case 8:set(31,next);pc=branch(next,i12);break;case 9:W(rd,uint8_t(raw));break;case 15:mode=Mode::Normal;break;default:return SRH_INVALID;}return s;}
    SrhStatus tick(){if(!running)return SRH_OK;auto s=host->boundary(host->context,owner,space,pc);if(s==SRH_STOP)return SRH_OK;if(s!=SRH_OK)return s;s=mode==Mode::Normal?normal():short_step();if(s==SRH_OK)cycles+=3;return s;}
};
SrhStatus SRH_CALL tick(void* p){return srz80::sdk::guard([&]{return static_cast<Cpu*>(p)->tick();});}
void set_error_message(const SrhConfig* config,const char* message){
    if(!config||!message||!srz80::sdk::has_field(config,&SrhConfig::error_message)||
       !config->error_message||config->error_message_capacity==0)return;
    const uint32_t limit=config->error_message_capacity-1;
    uint32_t count=static_cast<uint32_t>(std::strlen(message));
    if(count>limit)count=limit;
    std::memcpy(config->error_message,message,count);
    config->error_message[count]='\0';
}
SrhStatus SRH_CALL create(const ShouryoHost* h,SrhHandle owner,const SrhConfig* cfg,void** out){return srz80::sdk::guard([&]()->SrhStatus{
    if(!out)return SRH_INVALID;
    *out=nullptr;
    if(!srz80::sdk::valid(h)||!srz80::sdk::valid(cfg))return SRH_INVALID;
    if(!h->subscribe_clock){set_error_message(cfg,"host clock service is unavailable");return SRH_UNAVAILABLE;}
    if(cfg->clock>=3){set_error_message(cfg,"clock index must be 0, 1, or 2");return SRH_INVALID;}
    auto c=std::make_unique<Cpu>();c->host=h;c->owner=owner;c->space=cfg->space;c->reset=uint32_t(cfg->reset_vector);c->reset_cpu();SrhHandle sub=0;auto s=h->subscribe_clock(h->context,owner,cfg->clock,tick,c.get(),&sub);if(s!=SRH_OK)return s;*out=c.release();return SRH_OK;
});}
void SRH_CALL destroy(void* p){delete static_cast<Cpu*>(p);} SrhStatus SRH_CALL reset(void* p,uint32_t){static_cast<Cpu*>(p)->reset_cpu();return SRH_OK;}
uint32_t SRH_CALL count(void*) { return 36; }
SrhStatus SRH_CALL info(void*, uint32_t index, SrhProperty* out) {
    if (!srz80::sdk::valid(out) || index >= count(nullptr))
        return SRH_INVALID;
    if (index == 0) {
        *out = {SRH_INIT(SrhProperty), "PC", "Control", "Program counter", SRH_UNSIGNED,
                32, 16, 1, nullptr, 0};
    } else if (index <= 32) {
        static const char* names[] = {"R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7",
                                      "R8", "R9", "R10", "R11", "R12", "R13", "R14", "R15",
                                      "R16", "R17", "R18", "R19", "R20", "R21", "R22", "R23",
                                      "R24", "R25", "R26", "R27", "R28", "R29", "R30", "R31"};
        *out = {SRH_INIT(SrhProperty), names[index - 1], "Registers", "General-purpose register",
                SRH_UNSIGNED, 32, 16, index != 1, nullptr, 0};
    } else if (index == 33) {
        *out = {SRH_INIT(SrhProperty), "MODE", "Control", "Instruction encoding mode", SRH_ENUM,
                1, 10, 0, "Normal|Short", 0};
    } else if (index == 34) {
        *out = {SRH_INIT(SrhProperty), "RUNNING", "Control", "Whether the CPU is running",
                SRH_BOOLEAN, 1, 10, 0, nullptr, 0};
    } else {
        *out = {SRH_INIT(SrhProperty), "CYCLES", "Control", "Cycles executed", SRH_UNSIGNED,
                64, 10, 0, nullptr, 0};
    }
    return SRH_OK;
}
SrhStatus SRH_CALL get(void* p, uint32_t index, SrhValue* out) {
    if (!srz80::sdk::valid(out) || index >= count(nullptr))
        return SRH_INVALID;
    auto& c = *static_cast<Cpu*>(p);
    if (index == 0)
        out->unsigned_value = c.pc;
    else if (index <= 32)
        out->unsigned_value = c.reg(index - 1);
    else if (index == 33)
        out->unsigned_value = static_cast<uint32_t>(c.mode);
    else if (index == 34)
        out->unsigned_value = c.running;
    else
        out->unsigned_value = c.cycles;
    return SRH_OK;
}
SrhStatus SRH_CALL set(void* p, uint32_t index, const SrhValue* in) {
    if (!srz80::sdk::valid(in) || index >= count(nullptr) ||
        (index >= 1 && index <= 32 && index == 1) || index >= 33 ||
        in->unsigned_value > UINT32_MAX)
        return SRH_INVALID;
    auto& c = *static_cast<Cpu*>(p);
    if (index == 0)
        c.pc = static_cast<uint32_t>(in->unsigned_value);
    else
        c.set(index - 1, static_cast<uint32_t>(in->unsigned_value));
    return SRH_OK;
}
SrhStatus SRH_CALL save_state(void* p,uint8_t* b,uint64_t* n){if(!n)return SRH_INVALID;constexpr uint64_t z=32*4+4+1+1+8;if(!b){*n=z;return SRH_OK;}if(*n<z){*n=z;return SRH_UNAVAILABLE;}auto&c=*static_cast<Cpu*>(p);std::memcpy(b,c.r,128);std::memcpy(b+128,&c.pc,4);b[132]=c.running;b[133]=uint8_t(c.mode);std::memcpy(b+134,&c.cycles,8);*n=z;return SRH_OK;}
SrhStatus SRH_CALL load_state(void* p,const uint8_t* b,uint64_t n){if(!b||n!=142)return SRH_INVALID;auto&c=*static_cast<Cpu*>(p);std::memcpy(c.r,b,128);std::memcpy(&c.pc,b+128,4);c.running=b[132];c.mode=Mode(b[133]&1);std::memcpy(&c.cycles,b+134,8);c.r[0]=0;return SRH_OK;}
const SrhCardDescriptor descriptor{SRH_INIT(SrhCardDescriptor),"CPU","SRC32-ALMSI","SRC32 CPU",0,0,0,0,0,SRH_CARD_SHOW_CLOCK,"{}",nullptr,nullptr,nullptr,0};
const SrhPlugin api{SRH_INIT(SrhPlugin),"src32",create,destroy,reset,count,info,get,set,save_state,load_state,&descriptor,nullptr,nullptr};
}
extern "C" SRH_EXPORT const SrhPlugin* SRH_CALL srz80_plugin_init(const ShouryoHost* h){return srz80::sdk::valid(h)?&api:nullptr;}
