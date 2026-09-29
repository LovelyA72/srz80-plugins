// SPDX-License-Identifier: MIT
// SRZ80 transport/lifecycle adapter. No tile/OAM/palette layout knowledge here.
#include <boundary.hpp>
#include <srz80/signals.h>
#include <nlohmann/json.hpp>
#include "vsn_core.hpp"
#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
using namespace vsn;
struct Settings {
    std::string io="cpu0.io", memory="cpu0.mem", nmi="NMI", irq="IRQ";
    Region region=Region::ntsc;
    uint64_t clock=21477272;
    bool strict=false;
};
void diagnostic(const SrhConfig *config, const std::string &message) {
    if (srz80::sdk::has_field(config,&SrhConfig::error_message_capacity) &&
        config->error_message && config->error_message_capacity) {
        const auto count=std::min<size_t>(message.size(),config->error_message_capacity-1);
        std::memcpy(config->error_message,message.data(),count);
        config->error_message[count]=0;
    }
}
Settings settings(const SrhConfig *config) {
    if (config->config_json_size>65536 || (!config->config_json && config->config_json_size))
        throw std::invalid_argument("VSN config must be a JSON object of at most 65536 bytes");
    auto json=config->config_json ? nlohmann::json::parse(config->config_json,
        config->config_json+config->config_json_size) : nlohmann::json::object();
    if (!json.is_object()) throw std::invalid_argument("VSN config must be an object");
    Settings out;
    for (const auto &[key,value] : json.items()) {
        if (key=="io_space" || key=="memory_space" || key=="nmi_signal" || key=="irq_signal" || key=="region") {
            if (!value.is_string()) throw std::invalid_argument(key+" must be a string");
            const auto text=value.get<std::string>();
            if (text.size()>128 || std::any_of(text.begin(),text.end(),[](unsigned char c){return c<32;}) ||
                ((key=="io_space" || key=="memory_space" || key=="region") && text.empty()))
                throw std::invalid_argument(key+" must be a valid name of at most 128 bytes");
            if (key=="io_space") out.io=text;
            else if (key=="memory_space") out.memory=text;
            else if (key=="nmi_signal") out.nmi=text;
            else if (key=="irq_signal") out.irq=text;
            else if (text=="NTSC") out.region=Region::ntsc;
            else if (text=="PAL") out.region=Region::pal;
            else if (text=="VGA") out.region=Region::vga;
            else throw std::invalid_argument("region must be NTSC, PAL or VGA");
        } else if (key=="strict_memory") {
            if (!value.is_boolean()) throw std::invalid_argument("strict_memory must be boolean");
            out.strict=value.get<bool>();
        } else if (key=="raster_clock_hz") {
            if (!value.is_number_unsigned() || value.get<uint64_t>()<1 || value.get<uint64_t>()>1000000000)
                throw std::invalid_argument("raster_clock_hz must be an integer from 1 to 1000000000");
        } else throw std::invalid_argument("unknown VSN config key: "+key);
    }
    out.clock=json.contains("raster_clock_hz") ? json["raster_clock_hz"].get<uint64_t>() : profile(out.region).clock;
    return out;
}

// This is the only class that knows SRZ80 memory handles. A different backing
// store, MMU or bulk-read extension can replace it without touching rendering.
class HostMemory final : public Memory {
public:
    HostMemory(const ShouryoHost &host, SrhHandle owner, SrhHandle space,
               SrhHandle io, uint64_t base)
        : host_(host),owner_(owner),space_(space),io_(io),base_(base) {}
    void set_read_word(SrhHostReadWord callback) { read_word_=callback; }
    bool active() const { return active_; }
    bool read(uint64_t address, uint8_t &value) override {
        if (!allowed(address)) return false;
        Busy busy(active_);
        return host_.read(host_.context,owner_,space_,address,&value)==SRH_OK;
    }
    bool read_word(uint64_t address, uint32_t &value) override {
        // The host word read needs all four bytes in one allowed mapping; on
        // any refusal the caller falls back to per-byte reads.
        if (!read_word_) return false;
        for (unsigned i=0; i<4; ++i)
            if (!allowed(address+i)) return false;
        Busy busy(active_);
        return read_word_(host_.context,owner_,space_,address,&value)==SRH_OK;
    }
    bool write(uint64_t address, uint8_t value) override {
        if (!allowed(address)) return false;
        Busy busy(active_);
        return host_.write(host_.context,owner_,space_,address,value)==SRH_OK;
    }
private:
    struct Busy {
        bool &active;
        explicit Busy(bool &flag):active(flag){active=true;}
        ~Busy(){active=false;}
    };
    bool allowed(uint64_t address) const {
        return !active_ && address<=UINT32_MAX &&
            !(space_==io_ && address>=base_ && address-base_<128);
    }
    const ShouryoHost &host_;
    SrhHandle owner_,space_,io_;
    uint64_t base_;
    SrhHostReadWord read_word_=nullptr;
    bool active_=false;
};
class Card {
public:
    Card(const ShouryoHost &host, SrhHandle owner, const SrhConfig &config,
         const Settings &settings, SrhHandle memory)
        : host_(host),owner_(owner),base_(config.base),
          memory_(host,owner,memory,config.space,config.base),
          core_(memory_,settings.region,settings.strict),
          clock_(settings.region,settings.clock) {}
    ~Card() {
        if (event_) host_.cancel(host_.context,event_);
        if (mapping_) host_.unmap(host_.context,mapping_);
        if (signals_ && signals_->release) {
            if (nmi_signal_) signals_->release(signals_->context,owner_,nmi_signal_);
            if (irq_signal_) signals_->release(signals_->context,owner_,irq_signal_);
        }
        // Video has no unregister callback: the ABI host tears down providers
        // by owner on failed creation/destroy, as for sibling video cards.
    }
    void connect_signals(const SrhHostSignalsV1 *signals, SrhHandle nmi, SrhHandle irq) {
        signals_=signals; nmi_signal_=nmi; irq_signal_=irq;
    }
    void set_read_word(SrhHostReadWord callback) { memory_.set_read_word(callback); }
    SrhStatus start(const SrhConfig &config, const SrhHostVideoV1 &video) {
        SrhMapping mapping{SRH_INIT(SrhMapping),config.space,base_,base_+127,config.priority,
                           this,read,write,read,nullptr};
        auto result=host_.map(host_.context,owner_,&mapping,&mapping_);
        if (result!=SRH_OK) return result;
        result=video.register_video_ex(video.context,owner_,Core::surface_width,Core::surface_height,
            SRH_VIDEO_RGBA8,pixels,this,SRH_VIDEO_ALLOW_SHADER,&surface_);
        if (result!=SRH_OK) return result;
        result=video.set_video_timing(video.context,surface_,timing,this);
        if (result!=SRH_OK) return result;
        return arm(clock_.next_delay(),event_);
    }
    SrhStatus reset() {
        if (event_) {
            const auto result=host_.cancel(host_.context,event_);
            if (result!=SRH_OK) return result;
            event_=0;
        }
        core_.reset(); clock_.remainder=0;
        update_signals();
        return arm(clock_.next_delay(),event_);
    }
    static SrhStatus SRH_CALL read(void *context, uint64_t address, uint8_t *out) {
        auto &card=*static_cast<Card*>(context);
        if (!out || address<card.base_ || address-card.base_>=128 || card.memory_.active()) return SRH_INVALID;
        *out=card.core_.read(unsigned(address-card.base_));
        return SRH_OK;
    }
    static SrhStatus SRH_CALL write(void *context, uint64_t address, uint8_t value) {
        auto &card=*static_cast<Card*>(context);
        if (address<card.base_ || address-card.base_>=128 || card.memory_.active()) return SRH_INVALID;
        card.core_.write(unsigned(address-card.base_),value);
        card.update_signals();
        return SRH_OK;
    }
    static constexpr unsigned property_count=18;
    static SrhStatus property_info(unsigned index, SrhProperty *out) {
        static constexpr const char *names[]={"mode","width","height","frame","scanline",
            "map_base","tile_base","sprite_base","palette_base","status","fault_address",
            "fault_was_write","fault_count","raster_compare","irq_pending","irq_enable",
            "dma_status","dma_remaining"};
        if (index>=property_count || !srz80::sdk::valid(out)) return SRH_INVALID;
        *out={SRH_INIT(SrhProperty),names[index],"VSN","Read-only runtime state",SRH_UNSIGNED,
              64,10,0,nullptr,SRH_PROPERTY_RUNTIME};
        return SRH_OK;
    }
    SrhStatus property_get(unsigned index, SrhValue *out) const {
        if (index>=property_count || !srz80::sdk::valid(out)) return SRH_INVALID;
        const uint64_t values[]={core_.read(5),Core::surface_width,Core::surface_height,core_.frame(),core_.line(),
            core_.value(0x20,4),core_.value(0x24,4),core_.value(0x30,4),core_.value(0x40,4),
            core_.read(6),core_.fault_address(),core_.fault_was_write(),core_.fault_count(),
            core_.value(0x50,2),core_.read(8),core_.read(9),core_.read(0x6d),core_.value(0x68,4)};
        *out={SRH_INIT(SrhValue),values[index],0,{0}};
        return SRH_OK;
    }
private:
    // Recompute both physical interrupt lines from the core's pending/enable
    // state. Asserting drives the signal high; releasing yields ownership so
    // other IRQ sources can still drive a shared line. Best-effort: the host
    // validated the callbacks at creation, and a transient drive failure must
    // never stop the raster.
    void drive_signal(SrhHandle signal, bool level, bool &cached) {
        if (!signal || level==cached) return;
        if (level) {
            if (host_.signal_drive)
                host_.signal_drive(host_.context,owner_,signal,1000,0);
        } else {
            if (signals_ && signals_->release)
                signals_->release(signals_->context,owner_,signal);
            else if (host_.signal_drive)
                host_.signal_drive(host_.context,owner_,signal,0,0);
        }
        cached=level;
    }
    void update_signals() {
        drive_signal(nmi_signal_,core_.nmi_asserted(),nmi_level_);
        drive_signal(irq_signal_,core_.irq_asserted(),irq_level_);
    }
    SrhStatus arm(uint64_t delay, SrhHandle &event) {
        return host_.schedule(host_.context,owner_,delay,tick,this,&event);
    }

    static SrhStatus SRH_CALL tick(void *context) {
        return srz80::sdk::guard([&] {
            auto &card=*static_cast<Card*>(context);
            card.event_=0;
            card.core_.tick();
            card.update_signals();
            return card.arm(card.clock_.next_delay(),card.event_);
        });
    }
    static SrhStatus SRH_CALL pixels(void *context, uint64_t offset, uint8_t *out,
                                     uint32_t *size, uint32_t *total) {
        if (!size || !total || (!out && *size)) return SRH_INVALID;
        const auto pixels=static_cast<Card*>(context)->core_.pixels();
        *total=uint32_t(pixels.size());
        const auto count=offset>=pixels.size() ? 0 : std::min<uint64_t>(*size,pixels.size()-offset);
        if (count) std::memcpy(out,pixels.data()+offset,size_t(count));
        *size=uint32_t(count); return SRH_OK;
    }
    static SrhStatus SRH_CALL timing(void *context, SrhVideoTiming *out) {
        if (!srz80::sdk::valid(out)) return SRH_INVALID;
        const auto &core=static_cast<Card*>(context)->core_;
        *out={SRH_INIT(SrhVideoTiming),core.published_frame(),core.published_line(),core.lines()};
        return SRH_OK;
    }
    const ShouryoHost &host_;
    SrhHandle owner_,mapping_=0,surface_=0,event_=0;
    SrhHandle nmi_signal_=0,irq_signal_=0;
    const SrhHostSignalsV1 *signals_=nullptr;
    bool nmi_level_=false,irq_level_=false;
    uint64_t base_;
    HostMemory memory_;
    Core core_;
    RasterClock clock_;
};
SrhStatus SRH_CALL create(const ShouryoHost *host, SrhHandle owner, const SrhConfig *config, void **out) {
    if (out) *out=nullptr;
    if (!srz80::sdk::valid(config)) return SRH_INVALID;
    try {
        if (!srz80::sdk::valid(host) || !out || !host->map || !host->unmap || !host->read ||
            !host->write || !host->schedule || !host->cancel || !host->query ||
            !config->space || config->size!=128 || config->base>UINT64_MAX-127) {
            diagnostic(config,"VSN requires host memory/scheduler callbacks and a non-overflowing 128-byte mapping");
            return SRH_INVALID;
        }
        const auto parsed=settings(config);
        const void *extension=nullptr;
        if (host->query(host->context,"host.resources.v1",&extension)!=SRH_OK || !extension) {
            diagnostic(config,"VSN requires host.resources.v1"); return SRH_UNAVAILABLE;
        }
        const auto *resources=static_cast<const SrhHostResourcesV1*>(extension);
        if (!srz80::sdk::valid(resources) || !resources->lookup) {
            diagnostic(config,"VSN requires valid host.resources.v1 lookup"); return SRH_UNAVAILABLE;
        }
        SrhHandle io=0,memory=0;
        if (resources->lookup(resources->context,"space",parsed.io.c_str(),&io)!=SRH_OK ||
            !io || io!=config->space) {
            diagnostic(config,"VSN io_space must resolve to the supplied mapping space: "+parsed.io); return SRH_INVALID;
        }
        if (resources->lookup(resources->context,"space",parsed.memory.c_str(),&memory)!=SRH_OK || !memory) {
            diagnostic(config,"VSN memory_space not found: "+parsed.memory); return SRH_INVALID;
        }
        extension=nullptr;
        if (host->query(host->context,"host.video.v1",&extension)!=SRH_OK || !extension) {
            diagnostic(config,"VSN requires host.video.v1"); return SRH_UNAVAILABLE;
        }
        const auto *video=static_cast<const SrhHostVideoV1*>(extension);
        if (!srz80::sdk::valid(video) || !srz80::sdk::has_field(video,&SrhHostVideoV1::set_video_timing) ||
            !video->register_video_ex || !video->set_video_timing) {
            diagnostic(config,"VSN requires extended video registration and timing callbacks"); return SRH_UNAVAILABLE;
        }
        // Optional bulk memory read. A missing host.memory.v1 is not an error:
        // rendering falls back to byte reads. The word read may still refuse
        // per access (tracing, breakpoints, mapping boundaries), so the core
        // always retains the byte fallback.
        SrhHostReadWord read_word=nullptr;
        extension=nullptr;
        if (host->query(host->context,"host.memory.v1",&extension)==SRH_OK && extension) {
            const auto *memory_ext=static_cast<const SrhHostMemoryV1*>(extension);
            if (srz80::sdk::valid(memory_ext) &&
                srz80::sdk::has_field(memory_ext,&SrhHostMemoryV1::read_word) &&
                memory_ext->read_word)
                read_word=memory_ext->read_word;
        }
        // Interrupt signals are optional: an empty nmi_signal/irq_signal config
        // disables the corresponding line and skips the signals requirement.
        const SrhHostSignalsV1 *signals=nullptr;
        SrhHandle nmi=0,irq=0;
        if (!parsed.nmi.empty() || !parsed.irq.empty()) {
            if (host->query(host->context,"host.signals.v1",&extension)!=SRH_OK || !extension) {
                diagnostic(config,"VSN requires host.signals.v1"); return SRH_UNAVAILABLE;
            }
            signals=static_cast<const SrhHostSignalsV1*>(extension);
            if (!srz80::sdk::valid(signals) || !signals->release || !host->signal_find || !host->signal_drive) {
                diagnostic(config,"VSN requires valid host.signals.v1 release and signal find/drive callbacks"); return SRH_UNAVAILABLE;
            }
            if (!parsed.nmi.empty() &&
                (host->signal_find(host->context,parsed.nmi.c_str(),&nmi)!=SRH_OK || !nmi)) {
                diagnostic(config,"VSN NMI signal not found: "+parsed.nmi); return SRH_NOT_FOUND;
            }
            if (!parsed.irq.empty() &&
                (host->signal_find(host->context,parsed.irq.c_str(),&irq)!=SRH_OK || !irq)) {
                diagnostic(config,"VSN IRQ signal not found: "+parsed.irq); return SRH_NOT_FOUND;
            }
        }
        auto card=std::make_unique<Card>(*host,owner,*config,parsed,memory);
        card->connect_signals(signals,nmi,irq);
        card->set_read_word(read_word);
        const auto status=card->start(*config,*video);
        if (status!=SRH_OK) { diagnostic(config,"VSN could not map MMIO, register video or schedule raster"); return status; }
        *out=card.release(); return SRH_OK;
    } catch (const std::exception &error) {
        diagnostic(config,error.what()); return SRH_INVALID;
    } catch (...) { diagnostic(config,"VSN creation failed"); return SRH_ERROR; }
}
void SRH_CALL destroy(void *context) { delete static_cast<Card*>(context); }
SrhStatus SRH_CALL reset(void *context, uint32_t) {
    return srz80::sdk::guard([&]{return static_cast<Card*>(context)->reset();});
}
uint32_t SRH_CALL property_count(void *) { return Card::property_count; }
SrhStatus SRH_CALL property_info(void *, uint32_t index, SrhProperty *out) { return Card::property_info(index,out); }
SrhStatus SRH_CALL property_get(void *context, uint32_t index, SrhValue *out) { return static_cast<Card*>(context)->property_get(index,out); }
SrhStatus SRH_CALL property_set(void *, uint32_t, const SrhValue *) { return SRH_INVALID; }
const SrhCardDescriptor descriptor{SRH_INIT(SrhCardDescriptor),"Video","SR Visual Synthesizer",
    "Native shared-memory tile and NES graphics renderer",0x80,128,0,0,0,SRH_CARD_REQUIRES_IO_SPACE,
    R"({"io_space":"cpu0.io","memory_space":"cpu0.mem","region":"NTSC","nmi_signal":"NMI","irq_signal":"IRQ","strict_memory":false})",
    "io_space",nullptr,nullptr,0};
const SrhPlugin api{SRH_INIT(SrhPlugin),"vsn",create,destroy,reset,property_count,property_info,
    property_get,property_set,nullptr,nullptr,&descriptor,nullptr,nullptr};
} // namespace
extern "C" SRH_EXPORT const SrhPlugin *SRH_CALL srz80_plugin_init(const ShouryoHost *) { return &api; }
