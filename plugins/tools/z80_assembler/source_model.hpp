#pragma once
#include "assembler.hpp"
#include <string>
namespace srz80::assembler {
struct SourceModel {
    std::string text = "; F6 cold-resets before loading; ORG does not set PC.\norg 0\nmain: nop\n    jr main\n";
    std::string path;
    std::string saved_text = text;
    uint64_t revision = 1;
    uint32_t origin = 0;
    AssemblyResult result;
    bool dirty() const { return text != saved_text; }
    bool fresh() const { return result.revision == revision; }
    void changed() { ++revision; }
    void assemble() { result = Z80Assembler{}.assemble(text, origin, revision); }
    void new_document();
    void open(const std::string &approved_path);
    void open_text(const std::string &document_path, std::string contents);
    void save(const std::string &approved_path);
};
}
