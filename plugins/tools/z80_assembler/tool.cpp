#include "editor.hpp"
#include "inline_editor.hpp"
#include "loader.hpp"
#include <boundary.hpp>
#include <array>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <srz80/imgui_input.hpp>
#include <stdexcept>
#include <string_view>

namespace {
using namespace srz80::assembler;
struct Tool {
    const SrhToolHostV1 *host = nullptr;
    SourceModel source;
    Editor editor;
    InlineEditor inline_editor;
    bool text_mode = false;
    void navigate(size_t line, size_t column = 1) {
        if (text_mode) editor.go_to(source.text, line, column);
        else { inline_editor.navigate(source, line, column); inline_editor.edit(source); }
    }
    std::string space_name, status;
    double last_edit = 0;
    bool project_state_loaded = false;
    std::string syntax_theme = "Dark";
    SyntaxPalette syntax_colors = syntax_palette("Dark");
    std::array<char,256> find{}, replacement{};
    bool show_find = false;
};

void set_syntax_theme(Tool &tool, const char *value) {
    std::string candidate = value ? value : "";
    // Migrate the old option name when an existing config.ini is opened.
    if (candidate == "Light")
        candidate = "Pastel";
    static constexpr std::string_view themes[] = {"Dark", "Pastel", "Amber", "Ocean", "Monochrome"};
    for (const auto theme : themes) {
        if (candidate == theme) {
            tool.syntax_theme = candidate;
            tool.syntax_colors = syntax_palette(candidate);
            return;
        }
    }
    tool.syntax_theme = "Dark";
    tool.syntax_colors = syntax_palette("Dark");
}

SrhStatus SRH_CALL syntax_theme_get(void *context, char *value, uint32_t capacity) {
    if (!context || !value || !capacity)
        return SRH_INVALID;
    const auto &tool = *static_cast<Tool *>(context);
    std::snprintf(value, capacity, "%s", tool.syntax_theme.c_str());
    return SRH_OK;
}

SrhStatus SRH_CALL syntax_theme_set(void *context, const char *value) {
    if (!context || !value)
        return SRH_INVALID;
    set_syntax_theme(*static_cast<Tool *>(context), value);
    return SRH_OK;
}

SrhStatus SRH_CALL open_project_source(void *context, const char *path, const char *text,
                                       uint64_t size, uint64_t cursor) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!context || !path || !*path || !text || size > SIZE_MAX)
            return SRH_INVALID;
        auto &tool = *static_cast<Tool *>(context);
        try {
            tool.source.open_text(path, std::string(text, static_cast<size_t>(size)));
            tool.source.assemble();
            tool.inline_editor.reset();
            tool.editor.jump = static_cast<int>(std::min<uint64_t>(cursor, tool.source.text.size()));
            tool.editor.selection_end = tool.editor.jump;
            const auto line = static_cast<size_t>(std::count(tool.source.text.begin(),
                                                              tool.source.text.begin() + tool.editor.jump,
                                                              '\n')) + 1;
            tool.inline_editor.navigate(tool.source, line);
            tool.last_edit = ImGui::GetTime();
            tool.status = "Project source opened";
            return SRH_OK;
        } catch (const std::exception &exception) {
            tool.status = exception.what();
            return SRH_ERROR;
        }
    });
}

SrhStatus SRH_CALL create(const SrhToolHostV1 *host, void **result) {
    if(result) *result=nullptr;
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (host && host->abi_version == SRH_ABI && !srz80::sdk::valid(host) &&
            srz80::sdk::has_field(host, &SrhToolHostV1::log) && host->log)
            host->log(host->context, "Z80 assembler: incompatible host; project-document services are required.");
        if(!srz80::sdk::valid(host) || !result || !host->imgui_version ||
           std::strcmp(host->imgui_version,IMGUI_VERSION) || !host->imgui_context ||
           !host->imgui_alloc || !host->imgui_free || !host->space_count || !host->space_info ||
           !host->run_state || !host->run || !host->load_memory_segments ||
           !srz80::sdk::has_field(host, &SrhToolHostV1::project_file_update) ||
           !host->project_file_update || !host->file_handler_register) return SRH_INVALID;
        ImGui::SetAllocatorFunctions(host->imgui_alloc,host->imgui_free,host->imgui_allocator_context);
        ImGui::SetCurrentContext(static_cast<ImGuiContext *>(host->imgui_context));
        auto tool=std::make_unique<Tool>(); tool->host=host; tool->source.assemble();
        if (srz80::sdk::has_field(host, &SrhToolHostV1::config_get) && host->config_get) {
            char initial[32]{};
            if (host->config_get(host->context, "z80_assembler.syntax_theme",
                                 initial, sizeof(initial)) == SRH_OK)
                set_syntax_theme(*tool, initial);
        }
        if (srz80::sdk::has_field(host, &SrhToolHostV1::config_register) && host->config_register) {
            SrhConfigEntry entry{SRH_INIT(SrhConfigEntry),
                                 "Tools/Z80 assembler",
                                 "z80_assembler.syntax_theme",
                                 "Syntax highlighting",
                                 "Colors used for comments, directives, instructions, registers, and values",
                                 Srh_CONFIG_ENUM,
                                 "Dark|Pastel|Amber|Ocean|Monochrome",
                                 "Dark",
                                 tool.get(),
                                 0,
                                 syntax_theme_get,
                                 syntax_theme_set};
            host->config_register(host->context, &entry);
        }
        if (srz80::sdk::has_field(host, &SrhToolHostV1::file_handler_register) &&
            host->file_handler_register) {
            for (const char *extension : {"asm", "s", "z80"}) {
                SrhToolFileHandler handler{SRH_INIT(SrhToolFileHandler), "z80_assembler", extension,
                                            "Z80 assembler", tool.get(), open_project_source};
                if (host->file_handler_register(host->context, &handler) != SRH_OK)
                    return SRH_ERROR;
            }
        }
        *result=tool.release(); return SRH_OK;
    });
}
void SRH_CALL destroy(void *instance) {
    auto *tool=static_cast<Tool *>(instance);
    if(tool && srz80::sdk::has_field(tool->host, &SrhToolHostV1::file_handler_unregister) &&
       tool->host->file_handler_unregister)
        tool->host->file_handler_unregister(tool->host->context, tool);
    delete tool;
}
SrhStatus SRH_CALL project_state_get(void *instance, char *value, uint64_t *size) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!instance || !size)
            return SRH_INVALID;
        const auto &path = static_cast<Tool *>(instance)->source.path;
        const uint64_t required = path.size() + 1;
        if (!value) {
            *size = required;
            return SRH_OK;
        }
        if (*size < required) {
            *size = required;
            return SRH_UNAVAILABLE;
        }
        std::memcpy(value, path.c_str(), static_cast<size_t>(required));
        *size = required;
        return SRH_OK;
    });
}
SrhStatus SRH_CALL project_state_load(void *instance, const char *value) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!instance || !value)
            return SRH_INVALID;
        auto &tool = *static_cast<Tool *>(instance);
        try {
            if (*value) {
                tool.source.open(value);
                tool.source.assemble();
                tool.status = "Project source opened";
            } else if (tool.project_state_loaded) {
                tool.source.new_document();
                tool.source.assemble();
                tool.status.clear();
            }
            tool.project_state_loaded = true;
            tool.inline_editor.reset();
            tool.editor.go_to(tool.source.text, 1);
            tool.last_edit = ImGui::GetTime();
            return SRH_OK;
        } catch (const std::exception &exception) {
            tool.status = exception.what();
            return SRH_ERROR;
        }
    });
}
SrhStatus SRH_CALL project_save(void *instance) {
    return instance ? SRH_OK : SRH_INVALID;
}

// Keep the visual language of the tool deliberately quiet: source code is the
// primary object here, while build and rack state provide context around it.
void section_label(const char *label) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
}

void status_dot(const ImVec4 &colour) {
    const auto position=ImGui::GetCursorScreenPos();
    const ImVec2 center(position.x+5.f,position.y+ImGui::GetTextLineHeight()*.5f);
    ImGui::GetWindowDrawList()->AddCircleFilled(center,4.f,ImGui::ColorConvertFloat4ToU32(colour));
    ImGui::Dummy(ImVec2(12.f,ImGui::GetTextLineHeight()));
}

bool primary_button(const char *label) {
    ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(.46f,.18f,.22f,1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(.60f,.23f,.28f,1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,ImVec4(.70f,.27f,.32f,1.f));
    const bool pressed=ImGui::Button(label);
    ImGui::PopStyleColor(3);
    return pressed;
}

SrhStatus SRH_CALL draw(void *instance,uint32_t *open) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if(!instance || !open) return SRH_INVALID;
        auto &t=*static_cast<Tool *>(instance); auto &s=t.source; auto &h=*t.host;
        ImGui::SetCurrentContext(static_cast<ImGuiContext *>(h.imgui_context));
        bool visible=*open!=0;
        ImGui::SetNextWindowSize(ImVec2(1050,740),ImGuiCond_FirstUseEver);
        auto title=std::string("Z80 assembler")+(s.dirty()?" *":"")+"###z80_assembler";
        if(ImGui::Begin(title.c_str(),&visible)) {
            try {
                bool focused=ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
                bool ctrl=focused && ImGui::GetIO().KeyCtrl;
                std::vector<SrhToolSpace> spaces;
                for(uint32_t i=0;i<h.space_count(h.context);++i) {
                    SrhToolSpace candidate{SRH_INIT(SrhToolSpace),0,0,{}};
                    if(h.space_info(h.context,i,&candidate)==SRH_OK) spaces.push_back(candidate);
                }
                auto selected=std::find_if(spaces.begin(),spaces.end(),[&](auto &a){return t.space_name==a.name;});
                if(selected==spaces.end() && !spaces.empty()) { selected=spaces.begin(); t.space_name=selected->name; }
                const SrhHandle space=selected==spaces.end()?0:selected->id;
                const bool stopped=h.run_state(h.context)==SRT_STOPPED;
                const bool loadable=stopped && space && s.fresh() && s.result.succeeded && !s.result.segments.empty();
                size_t bytes=0;for(auto &seg:s.result.segments) bytes+=seg.bytes.size();
                const ImVec4 build_colour=!s.fresh()?ImVec4(.91f,.62f,.17f,1.f):s.result.succeeded?ImVec4(.25f,.74f,.45f,1.f):ImVec4(.90f,.30f,.30f,1.f);
                const char *build_label=!s.fresh()?"Changes pending":s.result.succeeded?"Build ready":"Build has errors";

                const auto slash=s.path.find_last_of("/\\");
                const auto document_name=s.path.empty()?std::string("Untitled source"):s.path.substr(slash==std::string::npos?0:slash+1);
                const auto &style=ImGui::GetStyle();
                const float outer_width=std::max(1.f,ImGui::GetContentRegionAvail().x);
                const float toolbar_width=std::max(1.f,outer_width-2.f*style.WindowPadding.x);
                const float item_spacing=style.ItemSpacing.x;
                const float frame_padding=2.f*style.FramePadding.x;
                const auto button_width=[&](const char *label) {
                    return ImGui::CalcTextSize(label).x+frame_padding;
                };
                const float space_width=std::max(142.f,ImGui::CalcTextSize("No address spaces").x+frame_padding*2.f);
                const float origin_width=std::max(74.f,ImGui::CalcTextSize("0000").x+frame_padding+10.f);
                std::vector<float> toolbar_items={
                    ImGui::CalcTextSize("Z80 Assembly").x,
                    ImGui::CalcTextSize((document_name+(s.dirty()?"  •  edited":"")).c_str()).x,
                    button_width("Assemble  F5"),space_width,ImGui::CalcTextSize("ORG").x,origin_width,
                    button_width("Load + reset  F6"),button_width("Keep PC"),button_width("Run  F4"),12.f,
                    ImGui::CalcTextSize(build_label).x,
                    ImGui::CalcTextSize("999 err · 999999 B").x};
                size_t toolbar_rows=1;
                float toolbar_used=0.f;
                for(size_t index=0;index<toolbar_items.size();++index) {
                    if(index==3 && toolbar_used>0.f) {
                        ++toolbar_rows;
                        toolbar_used=0.f;
                    }
                    const float item=toolbar_items[index];
                    if(toolbar_used>0.f && toolbar_used+item_spacing+item>toolbar_width) {
                        ++toolbar_rows;
                        toolbar_used=item;
                    } else {
                        toolbar_used+=toolbar_used>0.f?item_spacing+item:item;
                    }
                }
                const float row_height=ImGui::GetFrameHeight()+style.ItemSpacing.y;
                // NewLine() advances from the previous item's baseline.  Keep
                // enough breathing room for that advance and the scaled frame
                // padding, otherwise the last row gets clipped at high DPI.
                const float toolbar_height=toolbar_rows*row_height+2.f*style.WindowPadding.y+
                    style.ItemSpacing.y+2.f*style.FramePadding.y+6.f;
                ImGui::BeginChild("toolbar",ImVec2(0,toolbar_height),false,ImGuiWindowFlags_NoScrollbar);
                float row_used=0.f;
                auto place=[&](float item_width) {
                    if(row_used>0.f && row_used+item_spacing+item_width>toolbar_width) {
                        ImGui::NewLine();
                        row_used=item_width;
                    } else {
                        if(row_used>0.f) ImGui::SameLine();
                        row_used+=row_used>0.f?item_spacing+item_width:item_width;
                    }
                };
                place(toolbar_items[0]); ImGui::TextUnformatted("Z80 Assembly");
                place(toolbar_items[1]); ImGui::TextDisabled("%s%s",document_name.c_str(),s.dirty()?"  •  edited":"");
                if(!s.path.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s",s.path.c_str());
                place(toolbar_items[2]); if(primary_button("Assemble  F5") || (focused && ImGui::IsKeyPressed(ImGuiKey_F5,false)) || (ctrl && ImGui::IsKeyPressed(ImGuiKey_Enter,false))) s.assemble();
                // Keep target selection and execution controls together.  A
                // narrow or zoomed window may wrap this second band further,
                // but it should never interleave with document actions.
                ImGui::NewLine();
                row_used=0.f;
                place(toolbar_items[3]); ImGui::SetNextItemWidth(space_width);
                if(ImGui::BeginCombo("##space",space?t.space_name.c_str():"No address spaces")) {
                    for(auto &candidate:spaces) if(ImGui::Selectable(candidate.name,t.space_name==candidate.name)) t.space_name=candidate.name;
                    ImGui::EndCombo();
                }
                if(ImGui::IsItemHovered()) ImGui::SetTooltip("Where assembled bytes will go");
                // The combo is self-describing; its tooltip carries the full
                // label while the compact ORG marker keeps the row readable.
                place(toolbar_items[4]); ImGui::TextDisabled("ORG");
                place(toolbar_items[5]); ImGui::SetNextItemWidth(origin_width);
                if (srz80::gui::input_hexadecimal("##origin", s.origin, 16)) {
                    s.changed();
                    t.last_edit = ImGui::GetTime();
                }
                place(toolbar_items[6]); ImGui::BeginDisabled(!loadable);
                if((primary_button("Load + reset  F6") || (focused && ImGui::IsKeyPressed(ImGuiKey_F6,false))) && loadable) t.status=load(h,space,s,true);
                place(toolbar_items[7]); if(ImGui::Button("Keep PC")) t.status=load(h,space,s,false);
                ImGui::EndDisabled();
                place(toolbar_items[8]); if(ImGui::Button("Run  F4") || (focused && ImGui::IsKeyPressed(ImGuiKey_F4,false))) { auto r=h.run(h.context); t.status=r==SRH_OK?"Rack resumed":"Resume failed"; }
                place(toolbar_items[9]); status_dot(build_colour);
                place(toolbar_items[10]); ImGui::TextColored(build_colour,"%s",build_label);
                place(toolbar_items[11]); ImGui::TextDisabled("%zu err · %zu B",s.result.diagnostics.size(),bytes);
                ImGui::EndChild();
                if(ImGui::Button(t.show_find?"Hide find":"Find & replace") || (ctrl && ImGui::IsKeyPressed(ImGuiKey_F,false))) t.show_find=!t.show_find;
                if(t.show_find) {
                    ImGui::SameLine(); ImGui::SetNextItemWidth(170); ImGui::InputText("Find",t.find.data(),t.find.size());
                    ImGui::SameLine(); ImGui::SetNextItemWidth(170); ImGui::InputText("Replace with",t.replacement.data(),t.replacement.size());
                    ImGui::SameLine(); if(ImGui::Button("Next") && t.find[0]) {
                        auto pos=s.text.find(t.find.data(),static_cast<size_t>(t.editor.cursor)+1);
                        if(pos==std::string::npos) pos=s.text.find(t.find.data());
                        if(pos!=std::string::npos) { t.inline_editor.finish(s);t.text_mode=true;t.editor.jump=static_cast<int>(pos);t.editor.selection_end=static_cast<int>(pos+std::strlen(t.find.data())); }
                        else t.status="Text not found";
                    }
                    ImGui::SameLine(); if(ImGui::Button("Replace all") && t.find[0]) {
                        t.inline_editor.finish(s);t.inline_editor.document.begin(s);
                        size_t pos=0,count=0;
                        while((pos=s.text.find(t.find.data(),pos))!=std::string::npos) {
                            s.text.replace(pos,std::strlen(t.find.data()),t.replacement.data());pos+=std::strlen(t.replacement.data());++count;
                        }
                        if(count) {s.changed();t.last_edit=ImGui::GetTime();}
                        t.inline_editor.document.commit(s);
                        t.status="Replaced "+std::to_string(count)+" occurrences";
                    }
                }
                const float footer_height=94.f;
                const float height=std::max(200.f,ImGui::GetContentRegionAvail().y-footer_height);
                if(ImGui::BeginTable("Workspace",2,ImGuiTableFlags_Resizable)) {
                    ImGui::TableSetupColumn("Assembly",ImGuiTableColumnFlags_WidthStretch,4);
                    ImGui::TableSetupColumn("Symbols",ImGuiTableColumnFlags_WidthStretch,1);
                    ImGui::TableNextRow();ImGui::TableNextColumn();
                    // The inline listing table owns vertical scrolling.  This
                    // enclosing pane is only a layout frame; letting it scroll
                    // as well produces a second, competing scrollbar.
                    ImGui::BeginChild("assembly-pane", ImVec2(0, height), true,
                                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
                    section_label("ASSEMBLY"); ImGui::SameLine();
                    bool previous_mode=t.text_mode;
                    if(ImGui::SmallButton(t.text_mode?"Listing view":"Source text")) t.text_mode=!t.text_mode;
                    if(previous_mode!=t.text_mode) t.inline_editor.finish(s);
                    const ImVec2 editor_size(-1.f,std::max(1.f,ImGui::GetContentRegionAvail().y));
                    if(t.text_mode) {
                        t.inline_editor.document.begin(s);
                        if(t.editor.draw(s,editor_size)) t.last_edit=ImGui::GetTime();
                        t.inline_editor.document.commit(s);
                    } else if(t.inline_editor.draw(s,editor_size,&t.syntax_colors)) t.last_edit=ImGui::GetTime();
                    ImGui::EndChild();
                    ImGui::TableNextColumn();
                    ImGui::BeginChild("symbols-pane",ImVec2(0,height),true);
                    section_label("SYMBOLS"); ImGui::SameLine(); ImGui::TextDisabled("%zu defined",s.result.symbols.size());
                    ImGui::Separator();
                    ImGui::BeginChild("symbols-scroll",ImVec2(0,0));
                    ImGui::BeginDisabled(!s.fresh());
                    for(auto &symbol:s.result.symbols) {
                        char value[32];std::snprintf(value,sizeof(value),"%04llX",static_cast<unsigned long long>(symbol.value));
                        auto label=symbol.name+"  "+value;
                        if(ImGui::Selectable(label.c_str())) t.navigate(symbol.definition_line);
                    }
                    ImGui::EndDisabled();ImGui::EndChild();ImGui::EndChild();ImGui::EndTable();
                }
                if(!s.fresh() && ImGui::GetTime()-t.last_edit>=.2) s.assemble();
                ImGui::BeginChild("feedback",ImVec2(0,0),false);
                if(!s.fresh()) ImGui::TextDisabled("Assembling after changes stop…");
                else if(!s.result.diagnostics.empty()) {
                    ImGui::TextColored(ImVec4(.90f,.30f,.30f,1.f),"ISSUES  %zu",s.result.diagnostics.size());
                    ImGui::BeginDisabled(!s.fresh());
                    for(auto &d:s.result.diagnostics) {
                        auto label=std::to_string(d.line)+":"+std::to_string(d.first_column)+"  "+d.message;
                        ImGui::PushID(&d); if(ImGui::Selectable(label.c_str())) t.navigate(d.line,d.first_column); ImGui::PopID();
                    }
                    ImGui::EndDisabled();
                } else if(!t.status.empty()) ImGui::TextWrapped("%s",t.status.c_str());
                ImGui::EndChild();
            } catch(const std::exception &e) { t.status=e.what(); }
        }
        ImGui::End();
        if (srz80::sdk::has_field(t.host, &SrhToolHostV1::project_file_update) &&
            t.host->project_file_update && !s.path.empty()) {
            uint64_t cursor = t.text_mode ? static_cast<uint64_t>(std::max(0, t.editor.cursor)) : 0;
            if (!t.text_mode) {
                const auto rows = LineDocument::lines(s.text);
                for (size_t index = 0; index < std::min(t.inline_editor.selected, rows.size()); ++index)
                    cursor += rows[index].size() + 1;
            }
            t.host->project_file_update(t.host->context, &t, s.path.c_str(), s.text.data(),
                                        s.text.size(), cursor);
        }
        *open=visible?1u:0u;return SRH_OK;
    });
}
const SrhToolPlugin api{SRH_INIT(SrhToolPlugin),"z80_assembler","Z80 assembler",IMGUI_VERSION,create,destroy,draw,"CPU",0,project_state_get,project_state_load,project_save};
}
extern "C" SRH_EXPORT const SrhToolPlugin *SRH_CALL srz80_tool_init(const SrhToolHostV1 *host) {
    if(!host || host->abi_version!=SRH_ABI) return nullptr;
    return &api;
}
