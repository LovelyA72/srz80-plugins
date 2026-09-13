#include "inline_editor.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <string>
namespace srz80::assembler {

SyntaxPalette syntax_palette(std::string_view theme) {
    // Values are authored for the stock dark ImGui style.  Pastel keeps the
    // same dark background while using soft candy, lavender, mint, and peach
    // accents that remain readable without becoming neon.
    if (theme == "Pastel" || theme == "Light")
        return {{.55f,.85f,.68f,1},{.78f,.65f,.98f,1},{.98f,.55f,.72f,1},
                {.55f,.78f,1.f,1},{.98f,.62f,.75f,1},{.98f,.88f,.52f,1},{.98f,.84f,.47f,1},{.98f,.64f,.68f,1}};
    if (theme == "Amber")
        return {{.55f,.68f,.30f,1},{.96f,.68f,.18f,1},{.98f,.58f,.70f,1},
                {.95f,.78f,.38f,1},{1.f,.58f,.16f,1},{.98f,.86f,.50f,1},{.98f,.72f,.28f,1},{.90f,.48f,.34f,1}};
    if (theme == "Ocean")
        return {{.38f,.75f,.68f,1},{.72f,.55f,.95f,1},{.98f,.58f,.76f,1},
                {.40f,.76f,1.f,1},{.98f,.66f,.38f,1},{.98f,.86f,.50f,1},{.48f,.82f,.98f,1},{.96f,.56f,.70f,1}};
    if (theme == "Monochrome")
        return {{.62f,.62f,.62f,1},{.90f,.90f,.90f,1},{.92f,.92f,.92f,1},
                {.82f,.82f,.82f,1},{.98f,.86f,.50f,1},{.86f,.86f,.86f,1},{.88f,.88f,.88f,1},{.76f,.76f,.76f,1}};
    return {{.50f,.75f,.50f,1},{.78f,.55f,.94f,1},{.98f,.55f,.72f,1},
            {.42f,.72f,1.f,1},{1.f,.68f,.32f,1},{.98f,.86f,.50f,1},{.96f,.72f,.32f,1},{.94f,.52f,.64f,1}};
}

namespace {
bool is_word(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '\'';
}

bool is_register(std::string_view token) {
    static constexpr std::string_view registers[] = {
        "A", "F", "B", "C", "D", "E", "H", "L", "I", "R", "AF", "BC", "DE", "HL",
        "IX", "IY", "SP", "PC", "IXH", "IXL", "IYH", "IYL", "AF'"};
    for (const auto name : registers)
        if (token == name)
            return true;
    return false;
}

bool is_directive(std::string_view token) {
    static constexpr std::string_view directives[] = {
        "ORG", "EQU", "SET", "DB", "DEFB", "DW", "DEFW", "DD", "DEFD", "DS", "DEFS",
        "DM", "DEFM", "DUP", "INCBIN", "INCLUDE", "ALIGN", "END"};
    for (const auto name : directives)
        if (token == name)
            return true;
    return false;
}

bool is_number_start(char c) {
    return std::isdigit(static_cast<unsigned char>(c)) || c == '$' || c == '%' || c == '#';
}

void add_source_text(ImDrawList *draw_list, ImVec2 position, std::string_view source,
                     const SyntaxPalette &palette) {
    const ImU32 normal = ImGui::GetColorU32(ImGuiCol_Text);
    size_t i = 0;
    bool first_token = true;
    auto put = [&](std::string_view text, ImU32 color) {
        if (text.empty())
            return;
        draw_list->AddText(position, color, text.data(), text.data() + text.size());
        position.x += ImGui::CalcTextSize(text.data(), text.data() + text.size()).x;
    };
    while (i < source.size()) {
        if (source[i] == ';') {
            put(source.substr(i), ImGui::ColorConvertFloat4ToU32(palette.comment));
            break;
        }
        if (std::isspace(static_cast<unsigned char>(source[i]))) {
            const size_t begin = i++;
            while (i < source.size() && std::isspace(static_cast<unsigned char>(source[i])))
                ++i;
            put(source.substr(begin, i - begin), normal);
            continue;
        }
        if (source[i] == '"' || source[i] == '\'') {
            const char quote = source[i++];
            const size_t begin = i - 1;
            while (i < source.size()) {
                if (source[i] == quote && (i == begin + 1 || source[i - 1] != '\\')) {
                    ++i;
                    break;
                }
                ++i;
            }
            put(source.substr(begin, i - begin), ImGui::ColorConvertFloat4ToU32(palette.string));
            first_token = false;
            continue;
        }
        if (is_word(source[i])) {
            const size_t begin = i++;
            while (i < source.size() && is_word(source[i]))
                ++i;
            auto token = source.substr(begin, i - begin);
            std::string upper(token);
            std::transform(upper.begin(), upper.end(), upper.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            ImU32 color = normal;
            if (first_token && i < source.size() && source[i] == ':')
                color = ImGui::ColorConvertFloat4ToU32(palette.label);
            else if (first_token)
                color = upper == "ORG" ? ImGui::ColorConvertFloat4ToU32(palette.org)
                                       : is_directive(upper) ? ImGui::ColorConvertFloat4ToU32(palette.directive)
                                                             : ImGui::ColorConvertFloat4ToU32(palette.mnemonic);
            else if (is_register(upper))
                color = ImGui::ColorConvertFloat4ToU32(palette.register_name);
            put(token, color);
            first_token = false;
            continue;
        }
        if (is_number_start(source[i])) {
            const size_t begin = i++;
            while (i < source.size() && (std::isalnum(static_cast<unsigned char>(source[i])) ||
                                         source[i] == '$' || source[i] == '%'))
                ++i;
            put(source.substr(begin, i - begin), ImGui::ColorConvertFloat4ToU32(palette.number));
            first_token = false;
            continue;
        }
        put(source.substr(i, 1), normal);
        ++i;
        first_token = false;
    }
}
} // namespace

void InlineEditor::navigate(SourceModel &s,size_t line,size_t column) {
    finish(s);selected=std::min(line?line-1:0,LineDocument::lines(s.text).size()-1);
    scroll=true;cursor_column=static_cast<int>(column?column-1:0);
}
void InlineEditor::edit(SourceModel &s) {
    if(editing)return;
    auto rows=LineDocument::lines(s.text);selected=std::min(selected,rows.size()-1);
    buffer=rows[selected];document.begin(s);editing=true;focus=true;scroll=true;
}
void InlineEditor::finish(SourceModel &s,bool cancel) {
    if(!editing)return;
    if(cancel)document.cancel(s);else document.commit(s);
    editing=false;cursor_column=-1;
}
void InlineEditor::insert(SourceModel &s,bool after) {
    finish(s);
    auto rows=LineDocument::lines(s.text);
    const size_t insertion=after?selected+1:selected;
    const size_t indentation_source=after?selected:std::min(selected,rows.size()-1);
    const auto indentation=LineDocument::leading_indentation(rows[indentation_source]);
    if(after)++selected;
    document.insert(s,insertion,indentation);edit(s);
    // InputText selects its initial contents when focused.  A newly inserted
    // line begins with inherited indentation, so place a collapsed cursor
    // after it instead of letting the first typed character replace it.
    cursor_column=static_cast<int>(buffer.size());
}
void InlineEditor::erase(SourceModel &s) {finish(s);document.erase(s,selected);selected=std::min(selected,LineDocument::lines(s.text).size()-1);scroll=true;}
int InlineEditor::callback(ImGuiInputTextCallbackData *data) {
    auto &e=*static_cast<InlineEditor *>(data->UserData);
    if(data->EventFlag==ImGuiInputTextFlags_CallbackResize) {
        e.buffer.resize(static_cast<size_t>(data->BufTextLen));data->Buf=e.buffer.data();
    } else if(e.cursor_column>=0) {
        data->CursorPos=std::min(e.cursor_column,data->BufTextLen);
        data->SelectionStart=data->SelectionEnd=data->CursorPos;e.cursor_column=-1;
    }
    format_text_edit(data,false);
    return 0;
}
bool InlineEditor::draw(SourceModel &s,ImVec2 size,const SyntaxPalette *palette) {
    auto revision=s.revision;
    bool focused=ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    bool command=focused && !ImGui::GetIO().WantTextInput;
    if(ImGui::Button("F1 Edit") || (command && ImGui::IsKeyPressed(ImGuiKey_F1)))edit(s);
    ImGui::SameLine();if(ImGui::Button("F2 Insert") || (command && ImGui::IsKeyPressed(ImGuiKey_F2)))insert(s);
    ImGui::SameLine();if(ImGui::Button("Insert after"))insert(s,true);
    ImGui::SameLine();if(ImGui::Button("F3 Delete") || (command && ImGui::IsKeyPressed(ImGuiKey_F3)))erase(s);
    ImGui::SameLine();ImGui::BeginDisabled(document.undo_stack.empty() && !document.active);
    if(ImGui::Button("Undo") || (command && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z))) {finish(s);document.undo(s);scroll=true;}
    ImGui::EndDisabled();ImGui::SameLine();ImGui::BeginDisabled(document.redo_stack.empty());
    if(ImGui::Button("Redo") || (command && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y))) {finish(s);document.redo(s);scroll=true;}
    ImGui::EndDisabled();
    if(editing) {
        ImGui::SameLine();if(ImGui::Button("Accept"))finish(s);
        ImGui::SameLine();if(ImGui::Button("Cancel"))finish(s,true);
    }
    auto rows=LineDocument::lines(s.text);selected=std::min(selected,rows.size()-1);
    if(command && !editing) {
        if(ImGui::IsKeyPressed(ImGuiKey_UpArrow) && selected) {--selected;scroll=true;}
        if(ImGui::IsKeyPressed(ImGuiKey_DownArrow) && selected+1<rows.size()) {++selected;scroll=true;}
        if(ImGui::IsKeyPressed(ImGuiKey_PageUp)) {selected=selected>15?selected-15:0;scroll=true;}
        if(ImGui::IsKeyPressed(ImGuiKey_PageDown)) {selected=std::min(selected+15,rows.size()-1);scroll=true;}
        if(ImGui::IsKeyPressed(ImGuiKey_Enter))edit(s);
        if(ImGui::IsKeyPressed(ImGuiKey_Insert))insert(s);
        if(ImGui::IsKeyPressed(ImGuiKey_Delete))erase(s);
        if(ImGui::GetIO().KeyCtrl) {
            if(ImGui::IsKeyPressed(ImGuiKey_C)) ImGui::SetClipboardText(rows[selected].c_str());
            if(ImGui::IsKeyPressed(ImGuiKey_X)) {ImGui::SetClipboardText(rows[selected].c_str());erase(s);}
            if(ImGui::IsKeyPressed(ImGuiKey_V)) {
                if(auto *text=ImGui::GetClipboardText()) document.insert(s,selected,text);
            }
        }
        rows=LineDocument::lines(s.text);selected=std::min(selected,rows.size()-1);
    }
    const SyntaxPalette fallback = syntax_palette("Dark");
    const auto &active_palette = palette ? *palette : fallback;
    // The listing controls above consume part of the caller's available
    // height.  Size the table from what remains so its scroll range includes
    // every source row without overflowing its non-scrolling layout pane.
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const ImVec2 table_size(size.x, std::max(1.f, std::min(size.y, available.y)));
    if(ImGui::BeginTable("Editable listing",4,ImGuiTableFlags_BordersInnerV|ImGuiTableFlags_RowBg|
        ImGuiTableFlags_Resizable|ImGuiTableFlags_ScrollY,table_size)) {
        ImGui::TableSetupColumn("Line",ImGuiTableColumnFlags_WidthFixed,42);
        ImGui::TableSetupColumn("Address",ImGuiTableColumnFlags_WidthFixed,64);
        ImGui::TableSetupColumn("Bytes",ImGuiTableColumnFlags_WidthFixed,145);
        ImGui::TableSetupColumn("Source / instruction");
        ImGui::TableSetupScrollFreeze(0,1);ImGui::TableHeadersRow();
        ImGuiListClipper clipper;clipper.Begin(static_cast<int>(rows.size()),ImGui::GetFrameHeightWithSpacing());
        if(scroll)clipper.IncludeItemByIndex(static_cast<int>(selected));
        for(;clipper.Step();) for(int index=clipper.DisplayStart;index<clipper.DisplayEnd;++index) {
            auto row=static_cast<size_t>(index);
            ImGui::PushID(index);ImGui::TableNextRow(0,ImGui::GetFrameHeightWithSpacing());
            if(row==selected) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,ImGui::GetColorU32(ImGuiCol_Header));
            ImGui::TableNextColumn();
            if(ImGui::Selectable(std::to_string(row+1).c_str(),row==selected,ImGuiSelectableFlags_None,ImVec2(0,ImGui::GetFrameHeight()))) {
                finish(s);selected=row;
            }
            bool error=false;
            for(auto &d:s.result.diagnostics) if(s.fresh() && d.line==row+1) error=true;
            ImGui::TableNextColumn();
            const ListingLine *line=s.fresh() && row<s.result.listing.size()?&s.result.listing[row]:nullptr;
            if(error)ImGui::TextColored(ImVec4(1,.4f,.35f,1),"ERROR");
            else if(line && line->address)ImGui::Text("%04X",*line->address);
            else ImGui::TextDisabled("----");
            ImGui::TableNextColumn();std::string bytes;
            if(line) for(size_t i=0;i<std::min<size_t>(line->bytes.size(),6);++i) {
                char b[4];std::snprintf(b,sizeof(b),"%02X ",line->bytes[i]);bytes+=b;
            }
            if(line && line->bytes.size()>6)bytes+="...";
            ImGui::TextUnformatted(bytes.c_str());
            if(line && line->bytes.size()>6 && ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();ImGui::Text("%zu bytes",line->bytes.size());
                for(size_t i=0;i<line->bytes.size();++i) {if(i%16)ImGui::SameLine();ImGui::Text("%02X",line->bytes[i]);}
                ImGui::EndTooltip();
            }
            ImGui::TableNextColumn();
            if(row==selected && editing) {
                if(focus) {ImGui::SetKeyboardFocusHere();focus=false;}
                ImGui::SetNextItemWidth(-1);
                auto before=buffer;
                bool accepted=ImGui::InputText("##instruction",buffer.data(),buffer.capacity()+1,
                    ImGuiInputTextFlags_EnterReturnsTrue|ImGuiInputTextFlags_CallbackResize|
                        ImGuiInputTextFlags_CallbackAlways|ImGuiInputTextFlags_CallbackEdit|
                        ImGuiInputTextFlags_AllowTabInput|ImGuiInputTextFlags_NoUndoRedo,
                    callback,this);
                buffer.resize(std::strlen(buffer.c_str()));
                if(buffer!=before)document.replace(s,row,buffer);
                if(buffer.empty() && ImGui::IsKeyPressed(ImGuiKey_Backspace)) {finish(s);erase(s);}
                else if(ImGui::IsKeyPressed(ImGuiKey_Escape))finish(s,true);
                else if(accepted) insert(s,true);
            } else {
                auto text_position=ImGui::GetCursorScreenPos();
                text_position.y+=ImGui::GetStyle().FramePadding.y;
                bool clicked=ImGui::Selectable("##row-source",false,
                    ImGuiSelectableFlags_AllowDoubleClick,ImVec2(0,ImGui::GetFrameHeight()));
                add_source_text(ImGui::GetWindowDrawList(),text_position,rows[row],active_palette);
                if(clicked) {
                    finish(s);selected=row;
                    if(ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))edit(s);
                }
                if(ImGui::BeginPopupContextItem("Line actions")) {
                    if(ImGui::MenuItem("Edit instruction / label")) {finish(s);selected=row;edit(s);}
                    if(ImGui::MenuItem("Insert before")) {finish(s);selected=row;insert(s);}
                    if(ImGui::MenuItem("Delete line")) {finish(s);selected=row;erase(s);}
                    ImGui::EndPopup();
                }
            }
            if(scroll && row==selected) {ImGui::SetScrollHereY(.5f);scroll=false;}
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    return revision!=s.revision;
}
}
