#pragma once
#include "line_document.hpp"
#include "text_editing.hpp"
#include <imgui.h>
#include <string_view>
namespace srz80::assembler {

// The palette is deliberately small and semantic.  Keeping the token roles
// here means the assembler can offer selectable themes without coupling the
// source model to Dear ImGui's global style.
struct SyntaxPalette {
    ImVec4 comment;
    ImVec4 directive;
    ImVec4 org;
    ImVec4 mnemonic;
    ImVec4 label;
    ImVec4 register_name;
    ImVec4 number;
    ImVec4 string;
};

SyntaxPalette syntax_palette(std::string_view theme);

struct InlineEditor {
    LineDocument document;
    size_t selected=0;
    bool editing=false,focus=false,scroll=false;
    std::string buffer;
    int cursor_column=-1;
    int active_cursor_column=0;
    void navigate(SourceModel &source,size_t line,size_t column=1);
    void edit(SourceModel &source);
    void finish(SourceModel &source,bool cancel=false);
    void insert(SourceModel &source,bool after=false);
    void erase(SourceModel &source);
    bool draw(SourceModel &source,ImVec2 size,const SyntaxPalette *palette=nullptr);
    void reset() {document.clear();selected=0;editing=false;scroll=true;active_cursor_column=0;}
    static int callback(ImGuiInputTextCallbackData *data);
};
}
