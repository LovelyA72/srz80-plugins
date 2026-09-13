#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
namespace srz80::assembler {
enum class Severity { error, warning };
struct Diagnostic {
    Severity severity = Severity::error;
    size_t line, first_column, last_column;
    std::string message;
};
struct ListingLine {
    size_t source_line;
    std::optional<uint32_t> address;
    std::vector<uint8_t> bytes;
};
struct Symbol {
    std::string name;
    int64_t value;
    size_t definition_line;
    bool operator==(const Symbol &) const = default;
};
struct Segment {
    uint32_t first;
    std::vector<uint8_t> bytes;
    bool operator==(const Segment &) const = default;
};
struct AssemblyResult {
    uint64_t revision = 0;
    std::vector<ListingLine> listing;
    std::vector<Symbol> symbols;
    std::vector<Segment> segments;
    std::vector<Diagnostic> diagnostics;
    bool succeeded = false;
};
class Z80Assembler {
public:
    AssemblyResult assemble(const std::string &source, uint32_t origin = 0,
                            uint64_t revision = 0) const;
};
}
