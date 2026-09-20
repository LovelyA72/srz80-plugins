#pragma once
#include "assembler.hpp"
#include <string>
namespace srz80::assembler {
struct SourceModel {
    std::string text;
    std::string path;
    uint64_t revision = 1;
    uint32_t origin = 0;
    AssemblyResult result;
    bool fresh() const { return result.revision == revision; }
    void changed() { ++revision; }
    void assemble() { result = Z80Assembler{}.assemble(text, origin, revision); }
    void open_text(const std::string &document_path, std::string contents);
};
}
