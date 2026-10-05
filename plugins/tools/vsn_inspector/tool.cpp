// SPDX-License-Identifier: MIT
#include <boundary.hpp>
#include <srz80/tool.h>
#include <imgui.h>
#include <nlohmann/json.hpp>
#include "decoder.hpp"
#include "capture.hpp"
#include "image_cache.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {
using vsn::inspect::Decoder;
using vsn::inspect::Sprite;
struct Source {
    SrhHandle owner=0, memory=0, io=0;
    uint64_t base=0;
    uint64_t memory_maximum=0;
    std::string name;
    Decoder decoder;
};
struct Tool {
    const SrhToolHostV1 *host=nullptr;
    std::vector<Source> sources;
    SrhHandle selected=0;
    uint64_t generation=UINT64_MAX;
    bool windows[3]={true,true,true};
    bool auto_refresh=true, refresh=true, grid=true, hide_disabled=true, hide_offscreen=false;
    int zoom[3]={3,2,2};
    int refresh_interval_ms=500;
    int tile_source=1, tile_page=0, bank=0, selected_tile=0;
    int selected_sprite=0, layer=1, layer_view=0, page_x=0, page_y=0;
    unsigned cell_x=0, cell_y=0;
    double next_refresh=0;
    std::string error;
    vsn::inspect::Capture capture;
    vsn::inspect::ImageCache images[4];
    bool render_pending=false;
    uint64_t rendered_revision=0;
    explicit Tool(const SrhToolHostV1 *h):host(h),capture(h,this) {}
    void clear_sample() {
        capture.clear();
        for (auto &image:images) image.clear();
        refresh=true;
    }
    vsn::inspect::View view() const {
        return {windows[0],windows[1],windows[2],tile_source,tile_page,selected_sprite,layer,layer_view,page_x,page_y,cell_x,cell_y};
    }
};

void context(const Tool &t) {
    ImGui::SetAllocatorFunctions(t.host->imgui_alloc,t.host->imgui_free,t.host->imgui_allocator_context);
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(t.host->imgui_context));
}
void poll(Tool &t) {
    uint64_t size=0;
    if (t.host->provider_data(t.host->context,nullptr,&size)!=SRH_OK || !size || size>8*SRH_PROVIDER_MAX_BYTES) {
        t.sources.clear(); t.error="VSN inspection data unavailable"; return;
    }
    std::vector<char> buffer(size);
    if (t.host->provider_data(t.host->context,buffer.data(),&size)!=SRH_OK || size>buffer.size()) {
        t.sources.clear(); t.error="VSN inspection data changed. Refresh to retry"; return;
    }
    try {
        const auto bundle=nlohmann::json::parse(buffer.data(),buffer.data()+size-(size && buffer[size-1]==0));
        const auto generation=bundle.at("generation").get<uint64_t>();
        if (t.generation!=generation) {
            t.generation=generation; t.selected=0; t.clear_sample(); t.selected_sprite=0;
            t.tile_page=0; t.selected_tile=0; t.page_x=t.page_y=0; t.cell_x=t.cell_y=0;
        }
        std::vector<Source> sources;
        std::map<SrhHandle,uint64_t> spaces;
        for (uint32_t i=0;i<t.host->space_count(t.host->context);++i) {
            SrhToolSpace space{SRH_INIT(SrhToolSpace),0,0,{}};
            if (t.host->space_info(t.host->context,i,&space)==SRH_OK) spaces[space.id]=space.maximum;
        }
        for (const auto &provider:bundle.at("providers")) {
            if (provider.value("protocol",std::string())!="srz80.vsn.inspector.v1") continue;
            const auto data=nlohmann::json::parse(provider.at("data").get<std::string>());
            Source s;
            s.owner=provider.at("owner").get<SrhHandle>();
            s.name=provider.value("display_name",provider.value("name",std::string("VSN")));
            s.memory=data.at("memory_space").get<SrhHandle>(); s.io=data.at("io_space").get<SrhHandle>();
            const auto space=spaces.find(s.memory);
            if (space==spaces.end()) continue;
            s.memory_maximum=space->second;
            s.base=data.at("io_base").get<uint64_t>(); s.decoder.pal=data.at("pal").get<bool>();
            const auto &r=data.at("registers");
            if (!r.is_array() || r.size()!=128 || !s.memory) continue;
            for (unsigned i=0; i<128; ++i) {
                const auto v=r[i].get<unsigned>();
                if (v>255) throw std::runtime_error("Invalid VSN register byte");
                s.decoder.registers[i]=uint8_t(v);
            }
            if (s.decoder.value(0,4)!=0x034e5356 || s.decoder.mode()>5) continue;
            sources.push_back(std::move(s));
        }
        t.sources=std::move(sources); t.error.clear();
        if (std::none_of(t.sources.begin(),t.sources.end(),[&](const Source &s){return s.owner==t.selected;})) {
            t.selected=t.sources.empty()?0:t.sources.front().owner; t.clear_sample();
        }
    } catch (const std::exception &) {
        t.sources.clear(); t.error="Invalid VSN inspection data";
    }
}
Source *source(Tool &t) {
    for (auto &s:t.sources) if (s.owner==t.selected) return &s;
    return nullptr;
}
void toolbar(Tool &t, unsigned window) {
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("Windows")) {
            const char *names[]={"Tiles","Sprites","Layers"};
            for (unsigned i=0; i<3; ++i) ImGui::MenuItem(names[i],nullptr,&t.windows[i]);
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
    auto *s=source(t);
    ImGui::SetNextItemWidth(190);
    if (ImGui::BeginCombo("Card",s?s->name.c_str():"No VSN card")) {
        for (const auto &candidate:t.sources) {
            ImGui::PushID(static_cast<int>(candidate.owner));
            if (ImGui::Selectable(candidate.name.c_str(),candidate.owner==t.selected)) {
                t.selected=candidate.owner; t.clear_sample();
                t.tile_page=t.selected_sprite=t.selected_tile=0; t.cell_x=t.cell_y=0;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine(); ImGui::Checkbox("Auto refresh",&t.auto_refresh);
    ImGui::SameLine(); if (ImGui::Button("Refresh")) t.refresh=true;
    ImGui::SetNextItemWidth(110); ImGui::SliderInt("Zoom",&t.zoom[window],1,8,"%dx");
    ImGui::SameLine(); ImGui::SetNextItemWidth(110);
    ImGui::BeginDisabled(!t.auto_refresh);
    if (ImGui::DragInt("Refresh interval",&t.refresh_interval_ms,50,50,60000,"%d ms",ImGuiSliderFlags_AlwaysClamp))
        t.next_refresh=ImGui::GetTime()+t.refresh_interval_ms/1000.0;
    ImGui::EndDisabled();
}

// Draw horizontal runs, clipped to the visible canvas
// Transparent pixels use a checkerboard without changing the guest palette
struct Canvas {
    ImVec2 origin;
    int width,height,scale;
    template<class Pixel> Canvas(const char *id,int w,int h,int z,Pixel pixel,
                                vsn::inspect::ImageCache &image,std::vector<uint64_t> key):width(w),height(h),scale(z) {
        image.update(std::move(key),w,h,pixel);
        ImGui::InvisibleButton(id,ImVec2(float(w*z),float(h*z)));
        origin=ImGui::GetItemRectMin();
        auto *draw=ImGui::GetWindowDrawList();
        draw->PushClipRect(origin,ImVec2(origin.x+w*z,origin.y+h*z),true);
        const int y0=std::clamp(int((draw->GetClipRectMin().y-origin.y)/z),0,int(image.rows.size()));
        const int y1=std::clamp(int((draw->GetClipRectMax().y-origin.y)/z)+1,0,int(image.rows.size()));
        for (int y=y0;y<y1;++y) for (const auto &run:image.rows[y]) {
            const float x0=origin.x+run.x*z,x1=origin.x+(run.x+run.length)*z;
            if (x1<=draw->GetClipRectMin().x || x0>=draw->GetClipRectMax().x) continue;
            draw->AddRectFilled(ImVec2(x0,origin.y+y*z),ImVec2(x1,origin.y+(y+1)*z),run.color);
        }
        draw->PopClipRect();
    }
    void outline(int x,int y,int w,int h) const {
        const int x0=std::max(0,x),y0=std::max(0,y);
        const int x1=std::min(width,x+w),y1=std::min(height,y+h);
        if (x0>=x1 || y0>=y1) return;
        auto *draw=ImGui::GetWindowDrawList();
        draw->PushClipRect(origin,ImVec2(origin.x+width*scale,origin.y+height*scale),true);
        draw->AddRect(ImVec2(origin.x+x0*scale,origin.y+y0*scale),
            ImVec2(origin.x+x1*scale,origin.y+y1*scale),ImGui::GetColorU32(ImGuiCol_PlotHistogram),0.0f,2.0f);
        draw->PopClipRect();
    }
    void grid(int w,int h) const {
        const auto color=IM_COL32(160,160,160,90);
        auto *draw=ImGui::GetWindowDrawList();
        for (int x=0; x<=width; x+=w) draw->AddLine(ImVec2(origin.x+x*scale,origin.y),ImVec2(origin.x+x*scale,origin.y+height*scale),color);
        for (int y=0; y<=height; y+=h) draw->AddLine(ImVec2(origin.x,origin.y+y*scale),ImVec2(origin.x+width*scale,origin.y+y*scale),color);
    }
    bool pick(unsigned &x,unsigned &y) const {
        if (!ImGui::IsItemHovered() || !ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return false;
        x=std::min(unsigned(width-1),unsigned((ImGui::GetIO().MousePos.x-origin.x)/scale));
        y=std::min(unsigned(height-1),unsigned((ImGui::GetIO().MousePos.y-origin.y)/scale));
        return true;
    }
};
void tiles(Tool &t,Decoder &d) {
    const char *names[]={"BG0","BG1","Sprites"};
    ImGui::SetNextItemWidth(110); ImGui::Combo("Source",&t.tile_source,names,3);
    if (d.mode()==0 && t.tile_source==0) { t.tile_source=1; }
    const bool sprite=t.tile_source==2;
    const unsigned layer=t.tile_source==0?0:1;
    auto g=sprite?vsn::layout::PackedGeometry{8,8}:d.geometry(layer);
    if (sprite && !d.oam()) {
        // Geometry is per sprite, so use the selected record for its tile atlas
        const auto s=d.sprite(unsigned(t.selected_sprite)%d.sprite_count());
        g={s.width,s.width};
    }
    ImGui::SameLine(); ImGui::SetNextItemWidth(100); ImGui::InputInt("Page",&t.tile_page);
    const int max_tile=d.mode()==0?255:(sprite&&!d.oam()?0xfffff:4095);
    t.tile_page=std::clamp(t.tile_page,0,max_tile/64);
    ImGui::SameLine(); ImGui::Checkbox("Grid",&t.grid);
    if (d.depth(sprite,layer)<8) {
        ImGui::SetNextItemWidth(140); ImGui::SliderInt("Palette bank",&t.bank,0,d.mode()==0?3:15);
    }
    ImGui::SetNextItemWidth(120);
    const bool tile_changed=ImGui::InputInt("Tile",&t.selected_tile);
    t.selected_tile=std::clamp(t.selected_tile,0,max_tile);
    if (tile_changed) t.tile_page=t.selected_tile/64;
    ImGui::SameLine(); ImGui::Text("Address %08llX",static_cast<unsigned long long>(d.pattern_address(sprite,layer,t.selected_tile,g)));
    const unsigned first=unsigned(t.tile_page)*64;
    ImGui::Text("Tiles %u-%u   Base %08X   %ux%u, %u bpp",first,first+63,d.tile_base(sprite,layer),g.width,g.height,d.depth(sprite,layer));
    ImGui::BeginChild("tile canvas",ImVec2(0,0),ImGuiChildFlags_None,ImGuiWindowFlags_HorizontalScrollbar);
    Canvas canvas("atlas",g.width*8,g.height*8,t.zoom[0],[&](unsigned x,unsigned y) {
        const auto tile=first+(y/g.height)*8+x/g.width;
        const auto p=d.pixel(sprite,layer,tile,g,x%g.width,y%g.height);
        return p?d.color(d.index(sprite,layer,t.bank,p)):0;
    },t.images[0],{t.capture.revision,unsigned(t.tile_source),first,unsigned(t.bank),g.width,g.height});
    t.render_pending|=t.images[0].pending;
    unsigned x,y;
    if (canvas.pick(x,y)) t.selected_tile=int(first+(y/g.height)*8+x/g.width);
    if (t.grid) canvas.grid(g.width,g.height);
    if (unsigned(t.selected_tile)>=first && unsigned(t.selected_tile)<first+64) {
        const unsigned i=unsigned(t.selected_tile)-first;
        canvas.outline((i%8)*g.width,(i/8)*g.height,g.width,g.height);
    }
    ImGui::EndChild();
}
int pick_sprite(Decoder &d,const std::vector<Sprite> &sprites,unsigned x,unsigned y) {
    int picked=-1;
    for (unsigned i=0;i<sprites.size();++i) {
        const auto &s=sprites[i];
        if (!s.enabled || int(x)<s.x || int(y)<s.y || int(x)>=s.x+int(s.width) || int(y)>=s.y+int(s.height)) continue;
        if (picked<0) picked=int(i);
        if (d.sprite_pixel(s,unsigned(int(x)-s.x),unsigned(int(y)-s.y))) return int(i);
    }
    return picked;
}
void sprites(Tool &t,Decoder &d) {
    const int width=d.mode()==5?512:256, height=d.mode()==5?480:240;
    ImGui::Checkbox("Hide disabled",&t.hide_disabled); ImGui::SameLine(); ImGui::Checkbox("Hide offscreen",&t.hide_offscreen);
    std::vector<Sprite> sprites;
    for (unsigned i=0; i<d.sprite_count(); ++i) sprites.push_back(d.sprite(i));
    t.selected_sprite=std::clamp(t.selected_sprite,0,int(sprites.size())-1);
    const auto &selected=sprites[t.selected_sprite];
    ImGui::Text("Sprite %d   Tile %u   %dx%d at %d, %d   %s",t.selected_sprite,selected.tile,
        selected.width,selected.height,selected.x,selected.y,selected.enabled?"Enabled":"Disabled");
    ImGui::SameLine(); ImGui::Text("%s%s%s",selected.flip_x?"Flip X  ":"",selected.flip_y?"Flip Y  ":"",selected.behind?"Behind BG":"In front");
    std::string raw;
    char byte[4];
    for (unsigned i=0; i<(d.oam()?4u:16u); ++i) { std::snprintf(byte,sizeof(byte),"%02X ",selected.raw[i]); raw+=byte; }
    ImGui::Text("Record %08llX   %s",static_cast<unsigned long long>(uint64_t(d.value(0x30,4))+t.selected_sprite*(d.oam()?4:16)),raw.c_str());
    Canvas preview("selected sprite",selected.width,selected.height,4,[&](unsigned x,unsigned y) {
        return d.sprite_pixel(selected,x,y);
    },t.images[1],{t.capture.revision,unsigned(t.selected_sprite)});
    t.render_pending|=t.images[1].pending;
    if (!ImGui::BeginTable("sprite layout",2,ImGuiTableFlags_Resizable)) return;
    ImGui::TableSetupColumn("Plane",ImGuiTableColumnFlags_WidthStretch,1.2f);
    ImGui::TableSetupColumn("Records",ImGuiTableColumnFlags_WidthStretch,1);
    ImGui::TableNextRow(); ImGui::TableNextColumn();
    ImGui::BeginChild("sprite canvas",ImVec2(0,0),ImGuiChildFlags_None,ImGuiWindowFlags_HorizontalScrollbar);
    Canvas canvas("plane",width,height,t.zoom[1],[&](unsigned x,unsigned y) -> uint32_t {
        for (const auto &s:sprites) {
            if (!s.enabled || int(x)<s.x || int(y)<s.y || int(x)>=s.x+int(s.width) || int(y)>=s.y+int(s.height)) continue;
            const auto p=d.sprite_pixel(s,unsigned(int(x)-s.x),unsigned(int(y)-s.y));
            if (p) return p;
        }
        return 0;
    },t.images[2],{t.capture.revision});
    t.render_pending|=t.images[2].pending;
    unsigned x,y;
    if (canvas.pick(x,y)) {
        const int picked=pick_sprite(d,sprites,x,y);
        if (picked>=0) t.selected_sprite=picked;
    }
    const auto &highlight=sprites[t.selected_sprite];
    canvas.outline(highlight.x,highlight.y,highlight.width,highlight.height);
    ImGui::EndChild(); ImGui::TableNextColumn();
    if (ImGui::BeginTable("records",7,ImGuiTableFlags_ScrollY|ImGuiTableFlags_ScrollX|ImGuiTableFlags_RowBg|ImGuiTableFlags_BordersInnerH,ImVec2(0,0))) {
        const char *labels[]={"#","X","Y","Size","Tile","Bank","Flags"};
        for (const auto *label:labels) ImGui::TableSetupColumn(label);
        ImGui::TableSetupScrollFreeze(1,1); ImGui::TableHeadersRow();
        for (unsigned i=0; i<sprites.size(); ++i) {
            const auto &s=sprites[i];
            const bool off=s.x>=width || s.y>=height || s.x+int(s.width)<=0 || s.y+int(s.height)<=0;
            if ((t.hide_disabled&&!s.enabled) || (t.hide_offscreen&&off)) continue;
            ImGui::PushID(int(i)); ImGui::TableNextRow(); ImGui::TableNextColumn();
            const auto label=std::to_string(i);
            if (ImGui::Selectable(label.c_str(),int(i)==t.selected_sprite,ImGuiSelectableFlags_SpanAllColumns)) t.selected_sprite=int(i);
            ImGui::TableNextColumn(); ImGui::Text("%d",s.x); ImGui::TableNextColumn(); ImGui::Text("%d",s.y);
            ImGui::TableNextColumn(); ImGui::Text("%ux%u",s.width,s.height); ImGui::TableNextColumn(); ImGui::Text("%u",s.tile);
            ImGui::TableNextColumn(); ImGui::Text("%u",s.bank); ImGui::TableNextColumn();
            ImGui::Text("%s%s%s%s",s.enabled?"":"Off ",s.flip_x?"X ":"",s.flip_y?"Y ":"",s.behind?"Back":"Front");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndTable();
}
void layers(Tool &t,Decoder &d) {
    if (d.mode()==0) t.layer=1;
    ImGui::BeginDisabled(d.mode()==0); ImGui::RadioButton("BG0",&t.layer,0); ImGui::EndDisabled();
    ImGui::SameLine(); ImGui::RadioButton("BG1",&t.layer,1);
    ImGui::SameLine(); ImGui::SetNextItemWidth(130);
    const char *views[]={"Viewport","Map page"}; ImGui::Combo("View",&t.layer_view,views,2);
    ImGui::SameLine(); ImGui::Checkbox("Tile grid",&t.grid);
    const unsigned layer=t.layer;
    const auto g=d.geometry(layer);
    const unsigned mw=d.map_width(layer)*g.width, mh=d.map_height(layer)*g.height;
    unsigned ox=d.scroll(layer,false),oy=d.scroll(layer,true);
    const int width=d.mode()==5?512:256,height=d.mode()==5?480:240;
    if (t.layer_view) {
        ImGui::SetNextItemWidth(100); ImGui::InputInt("Page X",&t.page_x);
        ImGui::SameLine(); ImGui::SetNextItemWidth(100); ImGui::InputInt("Page Y",&t.page_y);
        t.page_x=std::clamp(t.page_x,0,int((mw-1)/width)); t.page_y=std::clamp(t.page_y,0,int((mh-1)/height));
        ox=t.page_x*width; oy=t.page_y*height;
    }
    const bool enabled=layer?(d.value(4)&2):(d.value(0x1a)&1);
    ImGui::Text("%s   Map %ux%u   Scroll %u, %u",enabled?"Enabled":"Disabled",
        d.map_width(layer),d.map_height(layer),d.scroll(layer,false),d.scroll(layer,true));
    ImGui::Text("Map %08X   Patterns %08X",d.map_base(layer),d.tile_base(false,layer));
    ImGui::Text("%ux%u tiles, %u bpp   Row stride %u   Opacity %u%%",g.width,g.height,d.depth(false,layer),
        d.value(layer?0x28:0x78,2),d.mode()==0?100:((d.value(0x1c)>>(layer*2))&3)*100/3);
    int cx=int(t.cell_x),cy=int(t.cell_y);
    ImGui::SetNextItemWidth(80); ImGui::InputInt("Cell X",&cx);
    ImGui::SameLine(); ImGui::SetNextItemWidth(80); ImGui::InputInt("Cell Y",&cy);
    t.cell_x=unsigned(std::clamp(cx,0,int(d.map_width(layer))-1));
    t.cell_y=unsigned(std::clamp(cy,0,int(d.map_height(layer))-1));
    t.cell_x%=d.map_width(layer); t.cell_y%=d.map_height(layer);
    const auto cell=d.cell(layer,t.cell_x,t.cell_y);
    ImGui::SameLine(); if (ImGui::Button("Show tile")) {
        t.tile_source=t.layer; t.selected_tile=int(cell.tile); t.tile_page=int(cell.tile/64); t.bank=int(cell.bank); t.windows[0]=true;
    }
    ImGui::Text("%08llX: %04X   Tile %u   Bank %u",static_cast<unsigned long long>(cell.address),cell.raw,cell.tile,cell.bank);
    ImGui::BeginChild("layer canvas",ImVec2(0,0),ImGuiChildFlags_None,ImGuiWindowFlags_HorizontalScrollbar);
    Canvas canvas("layer",width,height,t.zoom[2],[&](unsigned x,unsigned y) {
        if (t.layer_view && (ox+x>=mw || oy+y>=mh)) return uint32_t(0);
        return d.background(layer,(ox+x)%mw,(oy+y)%mh);
    },t.images[3],{t.capture.revision,layer,unsigned(t.layer_view),ox,oy});
    t.render_pending|=t.images[3].pending;
    unsigned x,y;
    if (canvas.pick(x,y) && (!t.layer_view || (ox+x<mw && oy+y<mh))) {
        t.cell_x=((ox+x)%mw)/g.width; t.cell_y=((oy+y)%mh)/g.height;
    }
    if (t.grid) {
        auto *draw=ImGui::GetWindowDrawList();
        for (unsigned x=(g.width-ox%g.width)%g.width; x<unsigned(width); x+=g.width)
            draw->AddLine(ImVec2(canvas.origin.x+x*canvas.scale,canvas.origin.y),ImVec2(canvas.origin.x+x*canvas.scale,canvas.origin.y+height*canvas.scale),IM_COL32(160,160,160,90));
        for (unsigned y=(g.height-oy%g.height)%g.height; y<unsigned(height); y+=g.height)
            draw->AddLine(ImVec2(canvas.origin.x,canvas.origin.y+y*canvas.scale),ImVec2(canvas.origin.x+width*canvas.scale,canvas.origin.y+y*canvas.scale),IM_COL32(160,160,160,90));
    }
    const unsigned sx=(t.cell_x*g.width+mw-ox%mw)%mw,sy=(t.cell_y*g.height+mh-oy%mh)%mh;
    if (sx<unsigned(width) && sy<unsigned(height)) canvas.outline(sx,sy,g.width,g.height);
    ImGui::EndChild();
}
SrhStatus SRH_CALL create(const SrhToolHostV1 *host,void **out) {
    if (out) *out=nullptr;
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!out || !srz80::sdk::valid(host) || !host->imgui_version || std::strcmp(host->imgui_version,IMGUI_VERSION) ||
            !host->imgui_context || !host->imgui_alloc || !host->imgui_free || !host->space_count || !host->space_info ||
            !srz80::sdk::has_field(host,&SrhToolHostV1::provider_data) || !host->provider_data) return SRH_INVALID;
        if (!srz80::sdk::has_field(host,&SrhToolHostV1::memory_read_release) ||
            !host->memory_read_request || !host->memory_read_poll || !host->memory_read_release || !host->runtime_info)
            return SRH_UNAVAILABLE;
        auto t=std::make_unique<Tool>(host); context(*t); *out=t.release(); return SRH_OK;
    });
}
void SRH_CALL destroy(void *instance) { delete static_cast<Tool*>(instance); }
SrhStatus SRH_CALL background_tick(void *instance,uint32_t visible) {
    if (!instance) return SRH_INVALID;
    auto &tool=*static_cast<Tool*>(instance);
    if (!visible && tool.capture.busy()) { tool.capture.cancel();tool.refresh=true; }
    return SRH_OK;
}
SrhStatus SRH_CALL draw(void *instance,uint32_t *open) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!instance || !open) return SRH_INVALID;
        if (!*open) return SRH_OK;
        auto &t=*static_cast<Tool*>(instance); context(t);
        if (!t.windows[0] && !t.windows[1] && !t.windows[2]) for (auto &w:t.windows) w=true;
        SrhToolRuntime runtime{SRH_INIT(SrhToolRuntime),0,0,0,0};
        if (t.host->runtime_info(t.host->context,&runtime)!=SRH_OK) return SRH_UNAVAILABLE;
        if (runtime.generation!=t.generation || t.refresh ||
            (t.auto_refresh && !t.capture.busy() && !t.render_pending && ImGui::GetTime()>=t.next_refresh))
            poll(t);
        t.capture.advance();
        if (t.capture.revision!=t.rendered_revision) {
            t.rendered_revision=t.capture.revision;t.next_refresh=ImGui::GetTime()+t.refresh_interval_ms/1000.0;
        }
        const bool sampling=t.capture.busy() || t.render_pending;
        t.render_pending=false;
        const char *titles[]={"VSN Inspector - Tiles","VSN Inspector - Sprites","VSN Inspector - Layers"};
        for (unsigned i=0; i<3; ++i) {
            if (!t.windows[i]) continue;
            const auto *viewport=ImGui::GetMainViewport();
            const float left=viewport->WorkSize.x*0.46f;
            const float half=viewport->WorkSize.y*0.5f;
            ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x+(i==1?left:0),
                viewport->WorkPos.y+(i==2?half:0)),ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(i==1?viewport->WorkSize.x-left:left,
                i==1?viewport->WorkSize.y:half),ImGuiCond_FirstUseEver);
            if (ImGui::Begin(titles[i],&t.windows[i],ImGuiWindowFlags_MenuBar)) {
                toolbar(t,i);
                auto *s=source(t);
                if (!t.error.empty()) ImGui::TextUnformatted(t.error.c_str());
                else if (!s) ImGui::TextUnformatted("No VSN card with inspection support");
                else {
                    if (!t.capture.error.empty()) ImGui::TextDisabled("%s",t.capture.error.c_str());
                    else if (t.capture.memory.failed) ImGui::TextDisabled("Graphics data unavailable");
                    else ImGui::TextDisabled("%s",sampling?"Sampling memory...":" ");
                    if (t.capture.published_source.owner==t.selected) {
                        auto decoder=t.capture.published;
                        decoder.read=[&t](uint64_t a){return t.capture.memory.read(a);};
                        if (i==0) tiles(t,decoder);
                        if (i==1) sprites(t,decoder);
                        if (i==2) layers(t,decoder);
                    }
                }
            }
            ImGui::End();
        }
        auto *selected=source(t);
        if (selected && runtime.generation==t.generation) {
            const auto view=t.view();
            const auto &captured=t.capture.published_source;
            const bool changed=captured.owner!=t.selected || view!=t.capture.published_view;
            if (t.refresh || (t.capture.busy() && view!=t.capture.requested_view()) ||
                (changed && !t.capture.busy() && (t.capture.error.empty() || view!=t.capture.requested_view())) ||
                (t.auto_refresh && !t.capture.busy() && !t.render_pending && ImGui::GetTime()>=t.next_refresh)) {
                t.capture.start({selected->owner,selected->memory,selected->io,selected->base,t.generation,
                    selected->decoder.pal,selected->memory_maximum},view);
                t.refresh=false;t.next_refresh=ImGui::GetTime()+t.refresh_interval_ms/1000.0;
            }
        }
        *open=t.windows[0] || t.windows[1] || t.windows[2];
        return SRH_OK;
    });
}
const SrhToolPlugin api{SRH_INIT(SrhToolPlugin),"vsn_inspector","VSN Inspector",IMGUI_VERSION,
    create,destroy,draw,"Video",0,nullptr,nullptr,nullptr,background_tick};
}
extern "C" SRH_EXPORT const SrhToolPlugin *SRH_CALL srz80_tool_init(const SrhToolHostV1 *host) {
    return srz80::sdk::valid(host)?&api:nullptr;
}
