#include "assembler.hpp"
#include "checked_operators.hpp"
#include <asm_z80.h>
#include <algorithm>
#include <cctype>
#include <map>
#include <sstream>
#include <set>

namespace srz80::assembler {
namespace {
std::string upper(std::string s) {
    for (auto &c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
void assign_value(libasm::Value &target, int64_t value) {
    if (value < 0) target.setSigned(static_cast<int32_t>(value));
    else target.setUnsigned(static_cast<uint32_t>(value));
}
struct Symbols final : libasm::SymbolTable {
    std::map<std::string, libasm::Value> values;
    const libasm::Value *lookupSymbol(const libasm::StrScanner &s) const override {
        auto i = values.find(upper(std::string(s.str(), s.size())));
        return i == values.end() ? nullptr : &i->second;
    }
    bool hasSymbol(const libasm::StrScanner &s) const override { return lookupSymbol(s); }
    const libasm::Functor *lookupFunction(const libasm::StrScanner &) const override { return nullptr; }
};
AssemblyResult pass(const std::vector<std::string> &lines, uint32_t origin,
                    const std::vector<Symbol> &previous) {
    AssemblyResult out;
    static const CheckedPlugins plugins;
    libasm::z80::AsmZ80 encoder(plugins);
    Symbols symbols;
    for (const auto &s : previous) assign_value(symbols.values[s.name], s.value);
    std::set<std::string> defined;
    std::map<uint32_t, uint8_t> memory;
    uint32_t pc = origin;
    for (size_t row = 0; row < lines.size(); ++row) {
        const auto &line = lines[row];
        const char *base = line.c_str();
        auto diagnostic = [&](const char *at, std::string message) {
            // Upstream may point at its own empty sentinel. Only compare integer addresses.
            auto p = reinterpret_cast<uintptr_t>(at), b = reinterpret_cast<uintptr_t>(base);
            size_t col = p >= b && p <= b + line.size() ? p - b : 0;
            out.diagnostics.push_back({Severity::error, row + 1, col + 1,
                                       std::min(col + 2, line.size() + 1), std::move(message)});
        };
        auto error = [&](const libasm::ErrorAt &e) {
            if (e.getError()) diagnostic(e.errorAt(), e.errorText_P());
        };
        libasm::StrScanner scan(base);
        scan.skipSpaces();
        ListingLine listing{row + 1, std::nullopt, {}};
        if (encoder.endOfLine(scan)) { out.listing.push_back(listing); continue; }
        auto token = [&]() {
            auto start = scan.str();
            while (*scan && (std::isalnum(static_cast<unsigned char>(*scan)) || *scan == '_' ||
                             *scan == '.' || *scan == '$' || *scan == '?')) ++scan;
            return upper(std::string(start, scan.str()));
        };
        auto start = scan;
        auto word = token();
        auto after_word = scan;
        scan.skipSpaces();
        std::string label;
        if (scan.expect(':')) {
            if (word.empty()) diagnostic(base, "Expected a label before colon");
            label = word;
            scan.skipSpaces(); start = scan; word = token(); scan.skipSpaces();
        } else {
            auto next = token();
            if (next == "EQU") { label = word; word = next; scan.skipSpaces(); }
            else { scan = after_word; scan.skipSpaces(); }
        }
        auto define = [&](int64_t value) {
            if (label.empty()) return;
            if (!(std::isalpha(static_cast<unsigned char>(label[0])) || label[0] == '_')) {
                diagnostic(base, "Labels must start with a letter or underscore"); return;
            }
            if (!defined.insert(label).second) diagnostic(base, "Duplicate symbol: " + label);
            else {
                assign_value(symbols.values[label], value);
                out.symbols.push_back({label, value, row + 1});
            }
        };
        bool expression_resolved = true;
        auto expr = [&]() {
            libasm::ErrorAt e; e.setAt(scan);
            libasm::ParserContext context(pc, &symbols, ',');
            auto before = scan.str();
            auto v = encoder.parser().eval(scan, e, context);
            error(e);
            expression_resolved = e.getError() == libasm::OK && v.isInteger();
            if (!v.isInteger()) diagnostic(before, "Integer required or integer overflow");
            if (scan.str() == before && !e.getError()) diagnostic(before, "Expected expression");
            scan.skipSpaces();
            return v;
        };
        if (word == "EQU") {
            if (label.empty()) diagnostic(start.str(), "EQU requires a label");
            auto value = expr();
            // An unresolved EQU must not become a fictitious zero-valued symbol
            // on the next pass (which would incorrectly resolve cyclic EQU chains).
            if (expression_resolved && !value.isUndefined()) define(CheckedOperators::integer(value));
        } else {
            define(pc);
            if (word.empty() && encoder.endOfLine(scan)) {
                listing.address = pc;
            } else if (word == "ORG" || word == "DS" || word == "DEFS") {
                listing.address = pc;
                auto at = scan.str(); auto v = expr();
                const int64_t n = v.getInteger();
                if (n < 0 || n > 65536 || (word == "ORG" ? n > 65535 : n + pc > 65536))
                    diagnostic(at, "Address outside Z80 range");
                else if (word == "ORG")
                    pc = static_cast<uint32_t>(n);
                else if (word == "DS")
                    pc += static_cast<uint32_t>(n);
                else {
                    uint8_t fill = 0;
                    if (scan.expect(',')) {
                        auto fill_at = scan.str();
                        auto fill_value = expr();
                        if (fill_value.overflowUint8())
                            diagnostic(fill_at, "Data value out of range");
                        fill = static_cast<uint8_t>(fill_value.getUnsigned());
                    }
                    listing.bytes.assign(static_cast<size_t>(n), fill);
                }
            } else if (word == "DB" || word == "DEFB" || word == "DEFM" || word == "DW" || word == "DEFW") {
                listing.address = pc;
                const bool wide = word == "DW" || word == "DEFW";
                do {
                    scan.skipSpaces(); auto at = scan.str();
                    if (!wide && *scan == '"') {
                        ++scan;
                        while (*scan && *scan != '"') {
                            libasm::ErrorAt e; e.setAt(scan);
                            const auto c = encoder.parser().readLetter(scan, e, '"');
                            error(e); listing.bytes.push_back(static_cast<uint8_t>(c));
                            if (e.getError()) break;
                        }
                        if (!scan.expect('"')) diagnostic(at, "Missing closing quote");
                        scan.skipSpaces();
                    } else {
                        auto v = expr();
                        if (wide ? v.overflowUint16() : v.overflowUint8()) diagnostic(at, "Data value out of range");
                        listing.bytes.push_back(static_cast<uint8_t>(v.getUnsigned()));
                        if (wide) listing.bytes.push_back(static_cast<uint8_t>(v.getUnsigned() >> 8));
                    }
                } while (scan.expect(','));
            } else {
                // Do not let upstream pseudo directives change options or allocate hidden data.
                static const std::set<std::string> instructions = {
                    "ADC","ADD","AND","BIT","CALL","CCF","CP","CPD","CPDR","CPI","CPIR",
                    "CPL","DAA","DEC","DI","DJNZ","EI","EX","EXX","HALT","IM","IN","INC",
                    "IND","INDR","INI","INIR","JP","JR","LD","LDD","LDDR","LDI","LDIR",
                    "NEG","NOP","OR","OTDR","OTIR","OUT","OUTD","OUTI","POP","PUSH","RES",
                    "RET","RETI","RETN","RL","RLA","RLC","RLCA","RLD","RR","RRA","RRC",
                    "RRCA","RRD","RST","SBC","SCF","SET","SLA","SRA","SRL","SUB","XOR"};
                if (!instructions.contains(word)) diagnostic(start.str(), "Unsupported instruction or directive: " + word);
                else {
                    libasm::Insn insn(pc);
                    encoder.encode(start.str(), insn, &symbols);
                    error(insn);
                    listing.address = pc;
                    listing.bytes.assign(insn.bytes(), insn.bytes() + insn.length());
                    scan = libasm::StrScanner(insn.errorAt());
                    // On success errorAt is the first unconsumed source character.
                    if (insn.getError()) scan = libasm::StrScanner(base + line.size());
                }
            }
        }
        if (!encoder.endOfLine(scan)) diagnostic(scan.str(), "Garbage at end of line");
        if (!listing.bytes.empty()) {
            if (pc > 65535 || listing.bytes.size() > 65536 - pc) diagnostic(base, "Emitted bytes wrap past FFFF");
            else {
                bool overlap = false;
                for (size_t i = 0; i < listing.bytes.size(); ++i)
                    if (!memory.emplace(pc + static_cast<uint32_t>(i), listing.bytes[i]).second) overlap = true;
                if (overlap) diagnostic(base, "Overlapping emitted bytes");
                pc += static_cast<uint32_t>(listing.bytes.size());
            }
        }
        out.listing.push_back(std::move(listing));
    }
    for (auto [address, byte] : memory) {
        if (out.segments.empty() || out.segments.back().first + out.segments.back().bytes.size() != address)
            out.segments.push_back({address, {}});
        out.segments.back().bytes.push_back(byte);
    }
    std::sort(out.symbols.begin(), out.symbols.end(), [](auto &a, auto &b) { return a.name < b.name; });
    return out;
}
}
AssemblyResult Z80Assembler::assemble(const std::string &source, uint32_t origin, uint64_t revision) const {
    if (origin > 65535 || source.find('\0') != std::string::npos) {
        AssemblyResult result;
        result.revision = revision;
        result.diagnostics.push_back({Severity::error, 1, 1, 1, "Invalid origin or embedded NUL in source"});
        return result;
    }
    std::vector<std::string> lines;
    std::istringstream input(source);
    for (std::string line; std::getline(input, line);) lines.push_back(std::move(line));
    if (lines.empty() || source.back() == '\n') lines.emplace_back();
    AssemblyResult previous;
    for (unsigned i = 0; i < 10; ++i) {
        auto next = pass(lines, origin, previous.symbols);
        if (i && next.symbols == previous.symbols && next.segments == previous.segments) {
            auto final = pass(lines, origin, next.symbols);
            final.revision = revision;
            final.succeeded = final.diagnostics.empty();
            return final;
        }
        previous = std::move(next);
    }
    previous.revision = revision;
    previous.diagnostics.push_back({Severity::error, 1, 1, 1, "Assembly did not converge after 10 passes"});
    return previous;
}
}
