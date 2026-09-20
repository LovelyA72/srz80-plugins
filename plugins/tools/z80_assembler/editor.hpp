#pragma once
#include "source_model.hpp"
#include "text_editing.hpp"
#include <imgui.h>
namespace srz80::assembler {
struct Editor {
    int jump = -1;
    int selection_end = -1;
    int cursor = 0;
    uint32_t document_id = 0;
    std::string previous_text;
    void open(const std::string &text, uint64_t cursor_offset);
    void go_to(const std::string &text, size_t line, size_t column = 1);
    bool draw(SourceModel &source, ImVec2 size);
    static int callback(ImGuiInputTextCallbackData *data);
    std::string *editing = nullptr;
};
}
