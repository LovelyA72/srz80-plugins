#include "backend.hpp"
#include "path.hpp"
#include "piano.hpp"
#include "playback_messages.hpp"
#include <boundary.hpp>
#include <srz80/tool.h>
#include <imgui.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <set>

namespace {
using namespace srz80::midi;
using Json = nlohmann::json;
constexpr size_t playback_requests_per_tick = 64;
constexpr size_t playback_same_time_batch_bytes = 128;
struct Client {
    struct Request { SrhHandle handle; uint64_t identity; };
    uint64_t identity=1;
    std::vector<Request> requests;
};
struct Cursor {
    uint64_t epoch=0, next=0;
    Parser parser;
    void reset() { epoch=next=0; parser.reset(); }
};
struct Provider { SrhHandle owner; std::string name, endpoint; Json data; };
struct Tool {
    const SrhToolHostV1 *host;
    Backend backend;
    SrhToolRuntime runtime{SRH_INIT(SrhToolRuntime),0,0,1,0};
    std::vector<Provider> providers;
    SrhHandle selected=0, dialog=0;
    std::string endpoint, error, file_path, bundle;
    // The routed card is persisted by its stable provider endpoint: handles are
    // process-local, so a project restored in another session must be matched by
    // name. `preference_settled` distinguishes a project that explicitly saved no
    // card from one that never saved a choice at all.
    std::string endpoint_preference;
    bool preference_settled=false;
    std::string input_port, output_port;
    Client live, playback;
    Cursor output, monitor;
    Parser input_parser;
    Piano piano;
    std::array<bool,128> observed{};
    std::array<std::array<bool,128>,16> channel_notes{};
    std::deque<Bytes> live_queue;
    int channel=1, sustain_channel=0, velocity=100, bend=8192, modulation=0, volume=100, pan=64, expression=127, program=0;
    bool sustain=false, playing=false, loop=false, piano_visible=false;
    bool playback_started=false, playback_ended=false;
    uint64_t origin=0, start_position=0, position=0, last_time=0;
    size_t event_index=0;
    std::vector<FileEvent> events;
    std::optional<LoopRange> file_loop;
    struct Loaded { std::string path; ParsedSmf smf; };
    std::future<Loaded> loading;
    uint64_t load_generation=0, load_identity=0, active_load_identity=0;
    std::string pending_path;
    float seek_seconds=0;
    explicit Tool(const SrhToolHostV1 *h) : host(h) {}
    bool submit(Client &client, const Bytes &bytes, uint64_t time) {
        if(!selected || runtime.stopped || bytes.empty()) return false;
        SrhHandle request=0;
        auto status=host->input_submit(host->context,&client,runtime.generation,client.identity,selected,
            endpoint.c_str(),time,bytes.data(),bytes.size(),&request);
        if(status!=SRH_OK) { error="MIDI input busy or rejected ("+std::to_string(status)+")"; return false; }
        client.requests.push_back({request,client.identity}); return true;
    }
    void poll(Client &client) {
        for(auto it=client.requests.begin();it!=client.requests.end();) {
            SrhToolInputResult result{SRH_INIT(SrhToolInputResult),1,SRH_UNAVAILABLE};
            if(host->input_poll(host->context,&client,it->handle,&result)!=SRH_OK || result.pending) { ++it; continue; }
            if(result.status!=SRH_OK && it->identity==client.identity) {
                error="Worker rejected MIDI input ("+std::to_string(result.status)+")";
                if(&client==&playback && playing) playback_failed=true;
            }
            host->input_release(host->context,&client,it->handle); it=client.requests.erase(it);
        }
    }
    void cancel(Client &client) {
        ++client.identity;
        if(!runtime.generation) return;
        SrhHandle request=0;
        if(host->input_cancel(host->context,&client,runtime.generation,client.identity,&request)==SRH_OK)
            client.requests.push_back({request,client.identity});
        else error="MIDI cancellation could not be submitted";
    }
    void queue(Bytes bytes) {
        if(runtime.stopped || !selected) return;
        size_t queued_bytes=bytes.size();
        for(const auto &pending:live_queue) queued_bytes+=pending.size();
        if(live_queue.size()>=256 || queued_bytes>262144) { error="Piano input queue full"; return; }
        live_queue.push_back(std::move(bytes));
    }
    void release_keys() {
        piano.release([&](const Bytes &bytes) { queue(bytes); });
        if(sustain) { sustain=false; queue({uint8_t(0xb0|sustain_channel),64,0}); }
    }
    void panic() {
        release_keys();
        Bytes bytes;
        for(int ch=0;ch<16;++ch) for(int cc:{64,123,120}) {
            bytes.push_back(0xb0|ch); bytes.push_back(cc); bytes.push_back(0);
            backend.send({uint8_t(0xb0|ch),uint8_t(cc),0});
        }
        queue(std::move(bytes));
    }
    bool playback_failed=false;
    void pause_playback(bool notify=false) {
        const bool was_playing=playing;
        playing=false; cancel(playback);
        if(was_playing || notify) { release_keys(); queue(playback_stop_messages()); }
        playback_started=playback_ended=false;
    }
    void stop_playback(bool notify=false) { pause_playback(notify); position=0; event_index=0; }
    void start_playback() {
        if(!selected || runtime.stopped || events.empty() || runtime.time_ns>UINT64_MAX-100000000) return;
        if(playing) pause_playback();
        else cancel(playback);
        start_position=position; origin=runtime.time_ns+100000000;
        event_index=std::lower_bound(events.begin(),events.end(),position,[](const auto &e,uint64_t t){return e.time_ns<t;})-events.begin();
        playback_started=playback_ended=false;
        playing=true;
    }
    void select(SrhHandle owner,bool chosen) {
        // Release held notes on the old endpoint before changing the route.
        stop_playback(); release_keys();
        while(!live_queue.empty()) {
            if(!submit(live,live_queue.front(),UINT64_MAX)) break;
            live_queue.pop_front();
        }
        backend.disconnect();
        live_queue.clear(); selected=owner; endpoint.clear(); output.reset(); monitor.reset(); input_parser.reset(); observed.fill(false); channel_notes={};
        for(const auto &p:providers) if(p.owner==owner) endpoint=p.endpoint;
        if(chosen) { endpoint_preference=endpoint; preference_settled=true; }
    }
    // Settle the routed card against the discovered ones. A saved endpoint wins
    // while it exists; otherwise the first MIDI card on the rack is selected,
    // and a choice whose card disappeared falls back to it as well.
    void auto_selection() {
        if(std::any_of(providers.begin(),providers.end(),[&](const auto &p){return p.owner==selected;})) return;
        // A restored project that named no card keeps no card selected, and one
        // whose card is missing selects the first MIDI card on the rack.
        if(preference_settled && endpoint_preference.empty()) { if(selected) select(0,false); return; }
        auto preferred=std::find_if(providers.begin(),providers.end(),[&](const auto &p){return p.endpoint==endpoint_preference;});
        if(preferred!=providers.end()) { select(preferred->owner,false); return; }
        if(!providers.empty()) select(providers.front().owner,false);
        else if(selected) select(0,false);
    }
    void load_file(std::string path) {
        ++load_identity;
        stop_playback();
        if(loading.valid()) { pending_path=std::move(path); return; }
        active_load_identity=load_identity; load_generation=runtime.generation;
        loading=std::async(std::launch::async,[path=std::move(path)] {
            std::ifstream input(path_from_utf8(path),std::ios::binary|std::ios::ate);
            if(!input) throw std::runtime_error("Cannot open MIDI file");
            const auto size=input.tellg();
            if(size<0 || size>16*1024*1024) throw std::runtime_error("MIDI file exceeds 16 MiB");
            Bytes bytes(static_cast<size_t>(size)); input.seekg(0);
            if(!input.read(reinterpret_cast<char *>(bytes.data()),size)) throw std::runtime_error("Cannot read MIDI file");
            return Loaded{path,parse_smf(bytes)};
        });
    }
    void consume(Cursor &cursor,const Json &data,const Parser::Sink &sink) {
        const auto epoch=data.at("epoch").get<uint64_t>();
        const auto oldest=data.at("oldest_sequence").get<uint64_t>();
        const auto next=data.at("next_sequence").get<uint64_t>();
        if(cursor.epoch!=epoch) { cursor.reset(); cursor.epoch=epoch; cursor.next=next; return; }
        if(cursor.next<oldest) { cursor.parser.reset(); cursor.next=oldest; error="MIDI output history gap; partial message discarded"; }
        size_t count=0;
        for(const auto &event:data.at("events")) {
            auto sequence=event.at("sequence").get<uint64_t>();
            if(sequence<cursor.next) continue;
            if(sequence!=cursor.next) { cursor.parser.reset(); error="MIDI output sequence gap"; }
            auto hex=event.at("bytes_hex").get<std::string>();
            if(hex.size()%2 || hex.size()>131072) throw std::runtime_error("Invalid MIDI provider event");
            auto digit=[](char c)->uint8_t {
                if(c>='0'&&c<='9') return c-'0';
                if(c>='A'&&c<='F') return c-'A'+10;
                if(c>='a'&&c<='f') return c-'a'+10;
                throw std::runtime_error("Invalid MIDI provider hex");
            };
            for(size_t i=0;i<hex.size();i+=2) cursor.parser.feed((digit(hex[i])<<4)|digit(hex[i+1]),sink);
            cursor.next=sequence+1;
            if(++count==2048) break;
        }
    }
    void tick() {
        poll(live); poll(playback);
        auto previous=runtime;
        if(host->runtime_info(host->context,&runtime)!=SRH_OK) return;
        if(runtime.generation!=previous.generation || runtime.time_ns<last_time || (!previous.stopped && runtime.stopped)) {
            playing=false; cancel(playback); cancel(live); selected=0; endpoint.clear();
            piano.discard(); sustain=false; live_queue.clear(); backend.disconnect();
            output.reset(); monitor.reset(); input_parser.reset(); observed.fill(false); channel_notes={}; bundle.clear();
        }
        last_time=runtime.time_ns;
        if(playback_failed) { playback_failed=false; if(playing) stop_playback(); }
        if(loading.valid() && loading.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
            try {
                auto loaded=loading.get();
                if(load_generation==runtime.generation && active_load_identity==load_identity) {
                    file_path=std::move(loaded.path);
                    events=std::move(loaded.smf.events);
                    file_loop=loaded.smf.loop;
                    event_index=0;
                    position=0;
                }
            } catch(const std::exception &e) { error=e.what(); }
            if(!pending_path.empty()) { auto path=std::move(pending_path); pending_path.clear(); load_file(std::move(path)); }
        }
        if(dialog) {
            SrhToolFileResult result{SRH_INIT(SrhToolFileResult),0,0,0};
            std::array<char,8192> path{};
            auto status=host->file_dialog_poll(host->context,dialog,&result,path.data(),path.size());
            if(status==SRH_OK && !result.pending) {
                host->file_dialog_release(host->context,dialog); dialog=0;
                if(!result.cancelled) load_file(path.data());
            }
        }
        uint64_t size=0;
        if(host->provider_data(host->context,nullptr,&size)==SRH_OK && size && size<=16*1024*1024) {
            std::string text(size,'\0');
            if(host->provider_data(host->context,text.data(),&size)==SRH_OK) {
                text.resize(size-1);
                if(text!=bundle) {
                    auto root=Json::parse(text);
                    if(root.at("generation")==runtime.generation) {
                        providers.clear();
                        for(const auto &p:root.at("providers")) if(p.at("protocol")=="srz80.midi.v1") {
                            auto data=Json::parse(p.at("data").get<std::string>());
                            if(data.at("schema")!=1) continue;
                            providers.push_back({p.at("owner").get<uint64_t>(),p.at("display_name").get<std::string>(),data.at("endpoint").get<std::string>(),std::move(data)});
                        }
                        bundle=std::move(text);
                    }
                }
            }
        }
        auto_selection();
        auto selected_provider=std::find_if(providers.begin(),providers.end(),[&](const auto &p){return p.owner==selected;});
        if(selected_provider!=providers.end()) {
            const auto &data=selected_provider->data;
            if(output.epoch && output.epoch!=data.at("epoch")) { stop_playback(); piano.discard(); live_queue.clear(); channel_notes={}; observed.fill(false); }
            consume(output,data,[&](const Bytes &bytes){backend.send(bytes);});
            consume(monitor,data,[&](const Bytes &bytes) {
                if(bytes.size()==3 && ((bytes[0]&0xf0)==0x90 || (bytes[0]&0xf0)==0x80)) {
                    channel_notes[bytes[0]&15][bytes[1]]=(bytes[0]&0xf0)==0x90 && bytes[2]!=0;
                    observed[bytes[1]]=std::any_of(channel_notes.begin(),channel_notes.end(),
                        [&](const auto &notes){return notes[bytes[1]];});
                }
            });
        }
        for(auto &bytes:backend.receive()) if(!runtime.stopped && selected) {
            try { input_parser.feed(bytes,[&](const Bytes &message){queue(message);}); }
            catch(const std::exception &e) { error=e.what(); }
        }
        size_t work=0;
        while(!live_queue.empty() && work++<16 && live.requests.size()<32) {
            if(!submit(live,live_queue.front(),UINT64_MAX)) break;
            live_queue.pop_front();
        }
        if(playing && !runtime.stopped) {
            if(!live_queue.empty()) return; // cleanup from a previous run precedes Start
            // Simulated time is read from a published snapshot that trails the
            // running worker, and this callback does not run while the GUI thread
            // is stalled (window resize, focus change, a modal operation). A
            // correctly scheduled transport or note is therefore routinely behind
            // worker time by the time it is submitted. Deliver it as soon as
            // possible instead of retiming it to a stale absolute time; stopping
            // here would abort playback on any frame hitch.
            auto deliver=[&](const Bytes &bytes,uint64_t timestamp) {
                return submit(playback,bytes,timestamp<=runtime.time_ns?UINT64_MAX:timestamp);
            };
            // Enqueue transport first, on the same identity and timestamp as
            // the song. Cancellation therefore removes an undelivered Start.
            if(!playback_started) {
                if(!deliver(Bytes{uint8_t(start_position ? 0xfb : 0xfa)},origin)) return;
                playback_started=true;
            }
            if(runtime.time_ns>=origin) {
                if(runtime.time_ns-origin>UINT64_MAX-start_position) { error="MIDI playback time overflow"; stop_playback(); return; }
                position=start_position+(runtime.time_ns-origin);
            }
            size_t batches=0;
            while(event_index<events.size() && batches++<playback_requests_per_tick && playback.requests.size()<playback_requests_per_tick) {
                const auto &event=events[event_index];
                const auto playback_end=loop && file_loop ? file_loop->end_ns : events.back().time_ns;
                if(event.time_ns>=playback_end) { event_index=events.size(); break; }
                if(event.time_ns>position && event.time_ns-position>50000000) break;
                if(event.time_ns-start_position>UINT64_MAX-origin) { error="MIDI playback time overflow"; stop_playback(); break; }
                auto timestamp=origin+(event.time_ns-start_position);
                // A dense file commonly emits several messages at one tick.
                // They are equivalent to one input batch at that timestamp and
                // batching them keeps request traffic below the GUI cadence.
                Bytes batch=event.bytes;
                size_t next=event_index+1;
                while(next<events.size() && events[next].time_ns==event.time_ns &&
                      events[next].bytes.size()<=playback_same_time_batch_bytes &&
                      batch.size()<=playback_same_time_batch_bytes-events[next].bytes.size()) {
                    batch.insert(batch.end(),events[next].bytes.begin(),events[next].bytes.end());
                    ++next;
                }
                if(!batch.empty() && !deliver(batch,timestamp)) break;
                event_index=next;
            }
            if(playing && event_index==events.size() && !playback_ended) {
                const auto end_position=std::max(start_position,loop && file_loop ? file_loop->end_ns : events.back().time_ns);
                if(end_position-start_position>UINT64_MAX-origin) { error="MIDI playback time overflow"; stop_playback(); return; }
                const auto end_time=origin+(end_position-start_position);
                if(deliver(playback_stop_messages(),end_time)) {
                    const auto loop_start=loop && file_loop ? file_loop->start_ns : 0;
                    const auto loop_end=loop && file_loop ? file_loop->end_ns : events.back().time_ns;
                    if(loop && !events.empty() && loop_end>=start_position && loop_end-start_position<=UINT64_MAX-origin) {
                        // Queue both sides of the jump now. Waiting for the Stop
                        // acknowledgement adds a GUI-frame-sized audible gap.
                        origin=end_time;
                        start_position=position=loop_start;
                        event_index=std::lower_bound(events.begin(),events.end(),loop_start,[](const auto &e,uint64_t t){return e.time_ns<t;})-events.begin();
                        if(!deliver(Bytes{uint8_t(loop_start ? 0xfb : 0xfa)},origin)) return;
                        playback_started=true;
                        while(event_index<events.size() && events[event_index].time_ns==loop_start) {
                            Bytes batch=events[event_index].bytes;
                            size_t next=event_index+1;
                            while(next<events.size() && events[next].time_ns==loop_start &&
                                  events[next].bytes.size()<=playback_same_time_batch_bytes &&
                                  batch.size()<=playback_same_time_batch_bytes-events[next].bytes.size()) {
                                batch.insert(batch.end(),events[next].bytes.begin(),events[next].bytes.end());
                                ++next;
                            }
                            if(!batch.empty() && !deliver(batch,origin)) return;
                            event_index=next;
                        }
                    } else playback_ended=true;
                }
            }
            if(playing && playback_ended && playback.requests.empty() && position>=std::max(start_position,loop && file_loop ? file_loop->end_ns : events.back().time_ns)) {
                playing=false;
                position=0;
                event_index=0;
            }
        }
    }
    void draw(uint32_t *open) {
        bool visible=*open!=0;
        if(!ImGui::Begin("MIDI",&visible)) { ImGui::End(); *open=visible; release_keys(); return; }
        if(ImGui::BeginCombo("Card",endpoint.empty()?"Select MIDI card":endpoint.c_str())) {
            for(const auto &p:providers) {
                auto label=p.name+" — "+p.endpoint;
                if(ImGui::Selectable(label.c_str(),p.owner==selected)) select(p.owner,true);
            }
            ImGui::EndCombo();
        }
        for(const auto &provider:providers) if(provider.owner==selected) {
            const auto &data=provider.data;
            ImGui::Text("RX %llu / %llu; IRQ %s",
                static_cast<unsigned long long>(data.at("rx_fill").get<uint64_t>()),
                static_cast<unsigned long long>(data.at("rx_capacity").get<uint64_t>()),
                data.at("irq_pending").get<bool>()?"asserted":"idle");
            if(data.at("rx_overrun").get<bool>() || data.at("tx_overrun").get<bool>())
                ImGui::TextWrapped("Card overflow: RX %s, TX history %s. Clear sticky flags through STATUS.",
                    data.at("rx_overrun").get<bool>()?"lost bytes":"OK",
                    data.at("tx_overrun").get<bool>()?"evicted events":"OK");
            if(ImGui::CollapsingHeader("Recent output bytes")) {
                const auto &history=data.at("events");
                const size_t first=history.size()>32?history.size()-32:0;
                for(size_t i=first;i<history.size();++i) {
                    const auto text=history[i].at("bytes_hex").get<std::string>();
                    ImGui::Text("%llu: %s",static_cast<unsigned long long>(history[i].at("sequence").get<uint64_t>()),text.c_str());
                }
            }
        }
        if(!error.empty()) ImGui::TextWrapped("%s",error.c_str());
        if(runtime.stopped) ImGui::TextDisabled("Rack stopped");
        bool keyboard=false;
        if(ImGui::BeginTabBar("MIDI views")) {
            if(ImGui::BeginTabItem("Virtual Piano")) {
                keyboard=ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput;
                if(ImGui::SliderInt("Channel",&channel,1,16)) release_keys();
                ImGui::SliderInt("Velocity",&velocity,1,127);
                draw_piano(piano,channel-1,velocity,selected && !runtime.stopped,observed,
                    [&](const Bytes &bytes) { queue(bytes); });
                if(ImGui::Checkbox("Sustain",&sustain)) {
                    if(sustain) sustain_channel=channel-1;
                    queue({uint8_t(0xb0|sustain_channel),64,uint8_t(sustain?127:0)});
                }
                if(ImGui::SliderInt("Pitch bend",&bend,0,16383)) queue({uint8_t(0xe0|(channel-1)),uint8_t(bend&127),uint8_t(bend>>7)});
                for(auto [label,cc,value]: {std::tuple{"Modulation",1,&modulation}, {"Volume",7,&volume}, {"Pan",10,&pan}, {"Expression",11,&expression}})
                    if(ImGui::SliderInt(label,value,0,127)) queue({uint8_t(0xb0|(channel-1)),uint8_t(cc),uint8_t(*value)});
                if(ImGui::SliderInt("Program (wire 0–127)",&program,0,127)) queue({uint8_t(0xc0|(channel-1)),uint8_t(program)});
                if(ImGui::Button("Panic")) panic();
                ImGui::EndTabItem();
            }
            if(ImGui::BeginTabItem("MIDI File Player")) {
                if(ImGui::Button("Open MIDI file") && !dialog && !loading.valid()) {
                    const SrhToolFileFilter filter{"Standard MIDI File","mid;midi"};
                    host->file_dialog_request(host->context,0,&filter,1,&dialog);
                }
                if(!file_path.empty()) ImGui::TextDisabled("%s",file_path.c_str());
                ImGui::Text("%zu events; %.3f seconds",events.size(),double(position)/1e9);
                if(ImGui::Button("Play")) start_playback();
                ImGui::SameLine();
                if(ImGui::Button("Pause playback")) pause_playback(true);
                ImGui::SameLine(); if(ImGui::Button("Stop")) stop_playback(true);
                ImGui::Checkbox("Loop seamlessly",&loop);
                if(file_loop) ImGui::Text("SMF loop markers: %.3f to %.3f seconds",double(file_loop->start_ns)/1e9,double(file_loop->end_ns)/1e9);
                const auto duration=events.empty()?0:events.back().time_ns;
                const auto duration_seconds=float(double(duration)/1e9);
                ImGui::Text("%.3f / %.3f seconds",double(position)/1e9,duration_seconds);
                ImGui::SetNextItemWidth(-1);
                ImGui::SliderFloat("##Playback progress",&seek_seconds,0,duration_seconds,"",ImGuiSliderFlags_AlwaysClamp);
                if(ImGui::IsItemDeactivatedAfterEdit()) {
                    pause_playback(true);
                    position=uint64_t(std::clamp(seek_seconds,0.0f,duration_seconds)*1e9);
                } else if(!ImGui::IsItemActive()) seek_seconds=float(double(position)/1e9);
                if(ImGui::IsItemHovered())
                    ImGui::SetTooltip("Runs on simulated time with 50 ms lookahead\nSeeking backward does not rewind controller state");
                ImGui::EndTabItem();
            }
            if(ImGui::BeginTabItem("System Ports")) {
                auto view=backend.view();
                if(ImGui::Button("Refresh ports")) backend.refresh();
                auto ports=[&](bool input,const std::vector<std::string> &names,std::string &chosen) {
                    if(ImGui::BeginCombo(input?"Input port":"Output port",chosen.c_str())) {
                        for(const auto &name:names) if(ImGui::Selectable(name.c_str(),name==chosen)) chosen=name;
                        ImGui::EndCombo();
                    }
                    if(ImGui::Button(input?"Connect input":"Connect output") && selected && !chosen.empty()) {
                        backend.connect(input,chosen);
                        host->config_set(host->context,input?"midi.input_port":"midi.output_port",chosen.c_str());
                        if(!input) output.reset();
                    }
                };
                ports(true,view.inputs,input_port); ports(false,view.outputs,output_port);
                if(ImGui::Button("Disconnect")) backend.disconnect();
                ImGui::SameLine(); if(ImGui::Button("External panic")) panic();
                ImGui::Text("Input: %s; output: %s",view.input?"connected":"off",view.output?"connected":"off");
                if(!view.error.empty()) ImGui::TextWrapped("%s",view.error.c_str());
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        if((piano_visible && !keyboard) || !visible) release_keys();
        piano_visible=keyboard;
        ImGui::End(); *open=visible;
    }
};
SrhStatus SRH_CALL create(const SrhToolHostV1 *host,void **out) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if(!srz80::sdk::valid(host) || !out || !host->input_submit || !host->input_cancel || !host->runtime_info) return SRH_INVALID;
        ImGui::SetCurrentContext(static_cast<ImGuiContext *>(host->imgui_context));
        ImGui::SetAllocatorFunctions(host->imgui_alloc,host->imgui_free,host->imgui_allocator_context);
        auto tool=std::make_unique<Tool>(host);
        std::array<char,1024> text{};
        if(host->config_get(host->context,"midi.input_port",text.data(),text.size())==SRH_OK) tool->input_port=text.data();
        if(host->config_get(host->context,"midi.output_port",text.data(),text.size())==SRH_OK) tool->output_port=text.data();
        *out=tool.release(); return SRH_OK;
    });
}
void SRH_CALL destroy(void *p) {
    auto tool=std::unique_ptr<Tool>(static_cast<Tool *>(p));
    tool->stop_playback(); tool->release_keys();
    while(!tool->live_queue.empty()) { tool->submit(tool->live,tool->live_queue.front(),UINT64_MAX); tool->live_queue.pop_front(); }
    for(auto client:{&tool->live,&tool->playback}) for(auto request:client->requests)
        tool->host->input_release(tool->host->context,client,request.handle);
    if(tool->dialog) tool->host->file_dialog_release(tool->host->context,tool->dialog);
}
SrhStatus SRH_CALL draw(void *p,uint32_t *open) {
    return srz80::sdk::guard([&] { static_cast<Tool *>(p)->draw(open); return SRH_OK; });
}
SrhStatus SRH_CALL tick(void *p,uint32_t visible) {
    auto &tool=*static_cast<Tool *>(p);
    try { if(!visible) tool.release_keys(); tool.tick(); return SRH_OK; }
    catch(const std::exception &e) { tool.error=e.what(); tool.backend.disconnect(); tool.stop_playback(); return SRH_ERROR; }
}
SrhStatus SRH_CALL state_get(void *p,char *out,uint64_t *size) {
    if(!size) return SRH_INVALID;
    auto &tool=*static_cast<Tool *>(p);
    // The routed card is saved next to the file path. An empty endpoint means
    // that nothing was selected when the project was saved, so loading the
    // project must not invent a selection.
    Json state{{"schema",1},{"path",tool.file_path},{"endpoint",tool.endpoint}};
    const auto text=state.dump();
    auto capacity=*size; *size=text.size()+1;
    if(!out) return SRH_OK;
    if(capacity<*size) return SRH_UNAVAILABLE;
    std::memcpy(out,text.c_str(),*size); return SRH_OK;
}
SrhStatus SRH_CALL state_load(void *p,const char *state) {
    return srz80::sdk::guard([&] {
        auto &tool=*static_cast<Tool *>(p); tool.select(0,false); ++tool.load_identity; tool.pending_path.clear(); tool.file_path.clear(); tool.events.clear(); tool.file_loop.reset();
        // A restore owns the routing choice only when the state tracks one.
        // Without that it is settled by the cards discovered on the rack.
        tool.endpoint_preference.clear();
        tool.preference_settled=false;
        std::string path;
        if(state && *state) {
            // Projects saved before the card was persisted hold a plain path.
            uint32_t schema=0; Json root;
            if(*state=='{') {
                root=Json::parse(state,state+std::strlen(state),nullptr,false);
                if(!root.is_discarded()) schema=root.value("schema",0u);
            }
            if(schema==1) {
                path=root.value("path",std::string());
                tool.endpoint_preference=root.value("endpoint",std::string());
                tool.preference_settled=true;
            } else path=state;
        }
        tool.host->runtime_info(tool.host->context,&tool.runtime);
        if(!path.empty()) tool.load_file(path);
        return SRH_OK;
    });
}
const SrhToolPlugin api{SRH_INIT(SrhToolPlugin),"midi","MIDI",IMGUI_VERSION,create,destroy,draw,"I/O",0,state_get,state_load,nullptr,tick};
}
extern "C" SRH_EXPORT const SrhToolPlugin *SRH_CALL srz80_tool_init(const SrhToolHostV1 *host) {
    return host && host->abi_version==SRH_ABI ? &api : nullptr;
}
