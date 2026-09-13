#include "piano.hpp"
#include <imgui.h>
#include <algorithm>
#include <string>
namespace srz80::midi {
void draw_piano(Piano &piano,int channel,int velocity,bool enabled,
                const std::array<bool,128> &observed,const Parser::Sink &sink) {
    if(ImGui::Button("Octave -")) { piano.release(sink); piano.first_note=std::max(0,piano.first_note-12); }
    ImGui::SameLine();
    if(ImGui::Button("Octave +")) { piano.release(sink); piano.first_note=std::min(96,piano.first_note+12); }
    ImGui::SameLine(); ImGui::SetNextItemWidth(130);
    if(ImGui::SliderInt("Octaves",&piano.octaves,1,4)) piano.release(sink);
    const auto origin=ImGui::GetCursorScreenPos();
    const float width=std::max(140.f,ImGui::GetContentRegionAvail().x), height=140;
    const int end=std::min(128,piano.first_note+12*piano.octaves);
    auto black=[](int note) { const int n=note%12; return n==1 || n==3 || n==6 || n==8 || n==10; };
    int whites=0;
    for(int note=piano.first_note;note<end;++note) if(!black(note)) ++whites;
    const float key_width=width/whites;
    struct Key { int note; ImVec2 a,b; bool black; };
    std::vector<Key> keys;
    int index=0;
    for(int note=piano.first_note;note<end;++note) {
        const bool is_black=black(note);
        const float x=is_black?(index-.32f)*key_width:index*key_width;
        keys.push_back({note,{origin.x+x,origin.y},{origin.x+x+key_width*(is_black?.64f:1.f),origin.y+height*(is_black?.64f:1.f)},is_black});
        if(!is_black) ++index;
    }
    ImGui::InvisibleButton("Piano keys",{width,height});
    int hovered=-1;
    float hovered_top=0, hovered_bottom=0;
    auto *draw=ImGui::GetWindowDrawList();
    for(bool is_black:{false,true}) for(const auto &key:keys) if(key.black==is_black) {
        const bool down=piano.active(key.note), sounding=observed[key.note];
        const auto color=down?IM_COL32(46,150,210,255):sounding?IM_COL32(60,170,105,255):is_black?IM_COL32(35,38,43,255):IM_COL32(237,238,240,255);
        draw->AddRectFilled(key.a,key.b,color,3);
        draw->AddRect(key.a,key.b,IM_COL32(15,17,20,255),3);
        if(key.note%12==0) {
            const auto label="C"+std::to_string(key.note/12-1);
            draw->AddText({key.a.x+4,key.b.y-22},IM_COL32(30,35,40,255),label.c_str());
        }
        if(ImGui::IsItemHovered() && ImGui::IsMouseHoveringRect(key.a,key.b)) {
            hovered=key.note;
            hovered_top=key.a.y;
            hovered_bottom=key.b.y;
        }
    }
    const bool dragging=enabled && ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const int mouse_note=dragging?hovered:-1;
    if(mouse_note!=piano.mouse_note) {
        piano.set(piano.mouse_note,Piano::mouse,false,channel,velocity,sink);
        // Match piano-roll velocity controls: clicking lower on the key plays
        // a stronger note. Normalize to each key so black keys use their full
        // visible height too.
        int mouse_velocity=velocity;
        if(mouse_note>=0) {
            const float position=(ImGui::GetIO().MousePos.y-hovered_top)/(hovered_bottom-hovered_top);
            mouse_velocity=std::clamp(1+int(position*126.f),1,127);
        }
        piano.set(mouse_note,Piano::mouse,true,channel,mouse_velocity,sink);
        piano.mouse_note=mouse_note;
    }
    static constexpr ImGuiKey keys_map[]{ImGuiKey_A,ImGuiKey_W,ImGuiKey_S,ImGuiKey_E,ImGuiKey_D,ImGuiKey_F,ImGuiKey_T,ImGuiKey_G,ImGuiKey_Y,ImGuiKey_H,ImGuiKey_U,ImGuiKey_J,ImGuiKey_K};
    const bool keyboard=enabled && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput;
    for(int i=0;i<13;++i) {
        if(keyboard && ImGui::IsKeyPressed(keys_map[i],false)) piano.set(piano.first_note+i,Piano::keyboard,true,channel,velocity,sink);
        if(!keyboard || ImGui::IsKeyReleased(keys_map[i])) piano.set(piano.first_note+i,Piano::keyboard,false,channel,velocity,sink);
    }
}
}
