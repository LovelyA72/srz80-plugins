#include "editor.hpp"
#include <algorithm>
#include <climits>
#include <cstring>
namespace srz80::assembler {
void Editor::open(const std::string &text, uint64_t cursor_offset) {
    ++document_id;
    previous_text=text;
    cursor=static_cast<int>(std::min<uint64_t>(cursor_offset,static_cast<uint64_t>(INT_MAX)));
    jump=cursor;
    selection_end=cursor;
}
void Editor::go_to(const std::string &text, size_t line, size_t column) {
    size_t offset=0;
    for (size_t n=1;n<line && offset<text.size();++n) {
        auto e=text.find('\n',offset); offset=e==std::string::npos?text.size():e+1;
    }
    auto end=text.find('\n',offset); if(end==std::string::npos) end=text.size();
    jump=static_cast<int>(std::min(offset+column-1,end)); selection_end=jump;
}
int Editor::callback(ImGuiInputTextCallbackData *data) {
    auto &editor=*static_cast<Editor *>(data->UserData);
    if (data->EventFlag==ImGuiInputTextFlags_CallbackResize) {
        editor.editing->resize(static_cast<size_t>(data->BufTextLen));
        data->Buf=editor.editing->data();
    } else {
        const bool inserted_text = data->EventFlag==ImGuiInputTextFlags_CallbackEdit &&
            data->BufTextLen>static_cast<int>(editor.previous_text.size());
        format_text_edit(data,inserted_text);
        if (editor.jump>=0) {
            data->CursorPos=std::min(editor.jump,data->BufTextLen);
            data->SelectionStart=data->CursorPos;
            data->SelectionEnd=std::min(editor.selection_end,data->BufTextLen);
            editor.jump=-1;
        }
        editor.cursor=data->CursorPos;
        editor.previous_text.assign(data->Buf,data->BufTextLen);
    }
    return 0;
}
bool Editor::draw(SourceModel &source, ImVec2 size) {
    editing=&source.text;
    if(jump>=0) ImGui::SetKeyboardFocusHere();
    ImGui::PushID(static_cast<int>(document_id));
    bool changed=ImGui::InputTextMultiline("##source",source.text.data(),source.text.capacity()+1,size,
        ImGuiInputTextFlags_CallbackResize|ImGuiInputTextFlags_CallbackAlways|
            ImGuiInputTextFlags_CallbackEdit|ImGuiInputTextFlags_AllowTabInput,
        callback,this);
    ImGui::PopID();
    if(changed) { source.text.resize(std::strlen(source.text.c_str())); source.changed(); }
    editing=nullptr;
    return changed;
}
} // namespace srz80::assembler
