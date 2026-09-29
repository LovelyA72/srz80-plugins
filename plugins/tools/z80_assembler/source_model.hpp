#pragma once
#include "assembler.hpp"
#include <string>
namespace srz80::assembler {
struct SourceModel {
    std::string text;
    std::string path;
    uint64_t revision = 1;
    uint32_t origin = 0;
    Target target = Target::z80;
    AssemblyResult result;
    bool fresh() const { return result.revision == revision; }
    void changed() { ++revision; }
    void assemble() { result = Assembler{}.assemble(text, target, origin, revision); }
    void set_target(Target value) { if (target != value) { target = value; changed(); } }
    void open_text(const std::string &document_path, std::string contents);
};
}
