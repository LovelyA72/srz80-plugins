#pragma once
#include <imgui.h>
#include <string>

namespace srz80::assembler {

constexpr size_t editor_indent_width = 4;

inline std::string expand_tabs(const std::string &text) {
    std::string result;
    result.reserve(text.size());
    for (char c : text) {
        if (c == '\t') result.append(editor_indent_width, ' ');
        else result.push_back(c);
    }
    return result;
}

inline std::string leading_indentation(const std::string &line) {
    const auto first = line.find_first_not_of(" \t");
    return expand_tabs(line.substr(0, first == std::string::npos ? line.size() : first));
}

// ImGui inserts TAB as a character when AllowTabInput is enabled. Expand it
// after the edit and copy the previous line's indentation after a newline.
inline void format_text_edit(ImGuiInputTextCallbackData *data, bool auto_indent = true) {
    if (data->EventFlag != ImGuiInputTextFlags_CallbackEdit) return;

    int cursor = data->CursorPos;
    bool changed = false;
    for (int pos = 0; pos < data->BufTextLen;) {
        if (data->Buf[pos] != '\t') {
            ++pos;
            continue;
        }
        data->DeleteChars(pos, 1);
        data->InsertChars(pos, "    ");
        if (pos < cursor) cursor += static_cast<int>(editor_indent_width - 1);
        pos += static_cast<int>(editor_indent_width);
        changed = true;
    }

    if (auto_indent && cursor > 0 && cursor <= data->BufTextLen && data->Buf[cursor - 1] == '\n') {
        int line_start = cursor - 1;
        while (line_start > 0 && data->Buf[line_start - 1] != '\n') --line_start;
        const std::string indentation = leading_indentation(
            std::string(data->Buf + line_start, static_cast<size_t>(cursor - 1 - line_start)));
        if (!indentation.empty()) {
            data->InsertChars(cursor, indentation.c_str());
            cursor += static_cast<int>(indentation.size());
            changed = true;
        }
    }

    if (changed) {
        data->CursorPos = cursor;
        data->SelectionStart = data->SelectionEnd = cursor;
        data->BufDirty = true;
    }
}

} // namespace srz80::assembler
