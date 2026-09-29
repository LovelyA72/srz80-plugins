#include "assembler.hpp"
#include "checked_operators.hpp"
#include <asm_z80.h>
#include <asm_mc68000.h>
#include <asm_mos6502.h>
#include <table_z80.h>
#include <table_mc68000.h>
#include <table_mos6502.h>
#include <algorithm>
#include <cctype>
#include <map>
#include <memory>
#include <set>
#include <sstream>

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

std::unique_ptr<libasm::Assembler> make_encoder(Target target, const CheckedPlugins &plugins) {
    switch (target) {
    case Target::mc68000: return std::make_unique<libasm::mc68000::AsmMc68000>(plugins);
    case Target::w65c02:
    case Target::w65c816: {
        auto encoder = std::make_unique<libasm::mos6502::AsmMos6502>(plugins);
        encoder->setCpuType(target == Target::w65c816 ? libasm::mos6502::W65C816 : libasm::mos6502::W65C02S);
        return encoder;
    }
    default: return std::make_unique<libasm::z80::AsmZ80>(plugins);
    }
}

// Consult the encoder's instruction tables before encode(): upstream pseudo
// directives can otherwise change its location or options behind our back.
bool instruction(Target target, const std::string &word) {
    libasm::Insn insn(0);
    insn.nameBuffer().text(word.c_str());
    switch (target) {
    case Target::mc68000: {
        using namespace libasm::mc68000;
        AsmInsn probe(insn);
        probe.parseInsnSize();
        hasOperand(CpuSpec(MC68000, PMMU_NONE, FPU_NONE, Config::DEFAULT_FPU_CID), probe);
        return probe.isOK();
    }
    case Target::w65c02:
    case Target::w65c816: {
        using namespace libasm::mos6502;
        AsmInsn probe(insn);
        hasOperand(target == Target::w65c816 ? W65C816 : W65C02S, probe);
        return probe.isOK();
    }
    default: {
        using namespace libasm::z80;
        AsmInsn probe(insn);
        return searchName(Z80, probe) != libasm::UNKNOWN_INSTRUCTION;
    }
    }
}

std::string directive(std::string word) {
    if (!word.empty() && word.front() == '.') word.erase(0, 1);
    if (word == "DEFB" || word == "DEFM" || word == "BYTE" || word == "DC.B" || word == "FCB") return "DB";
    if (word == "DEFW" || word == "WORD" || word == "DC.W" || word == "DC" || word == "FDB") return "DW";
    if (word == "DEFD" || word == "LONG" || word == "DC.L" || word == "DL") return "DD";
    if (word == "RMB" || word == "DS.B") return "DS";
    if (word == "AL") return "A16";
    if (word == "AS") return "A8";
    if (word == "XL") return "I16";
    if (word == "XS") return "I8";
    return word;
}

bool is_directive(const std::string &word) {
    static const std::set<std::string> names = {
        "CPU", "ORG", "EQU", "DB", "DW", "DD", "DS", "DS.W", "DS.L", "DEFS",
        "ALIGN", "EVEN", "A8", "A16", "I8", "I16", "LONGA", "LONGI"};
    return names.contains(directive(word));
}

AssemblyResult pass(const std::vector<std::string> &lines, Target target, uint32_t origin,
                    const std::vector<Symbol> &previous) {
    AssemblyResult out;
    const auto &info = target_info(target);
    const auto limit = info.address_limit();
    const CheckedPlugins plugins(target);
    auto backend = make_encoder(target, plugins);
    auto &encoder = *backend;
    Symbols symbols;
    for (const auto &s : previous) assign_value(symbols.values[s.name], s.value);
    std::set<std::string> defined;
    std::map<uint32_t, std::vector<uint8_t>> memory;
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
        if (encoder.endOfLine(scan) || (target == Target::mc68000 && *scan == '*')) {
            out.listing.push_back(listing); continue;
        }
        auto token = [&]() {
            auto start = scan.str();
            while (*scan && (std::isalnum(static_cast<unsigned char>(*scan)) || *scan == '_' ||
                             *scan == '.' || *scan == '$' || *scan == '?')) ++scan;
            return upper(std::string(start, scan.str()));
        };
        auto start = scan;
        std::string label;
        std::string word;
        // The 65816 dialect also accepts the conventional '* = address'.
        if ((target == Target::w65c816 || target == Target::w65c02) &&
            scan.expect('*') && scan.skipSpaces().expect('=')) {
            word = "ORG";
            scan.skipSpaces();
        } else {
            scan = start;
            word = token();
            auto after_word = scan;
            scan.skipSpaces();
            if (scan.expect(':')) {
                if (word.empty()) diagnostic(base, "Expected a label before colon");
                label = word;
                scan.skipSpaces(); start = scan; word = token(); scan.skipSpaces();
            } else if (scan.expect('=')) {
                label = word; word = "EQU"; scan.skipSpaces();
            } else {
                auto next = token();
                if (directive(next) == "EQU") { label = word; word = "EQU"; scan.skipSpaces(); }
                else if (target != Target::z80 && start.str() == base && !word.empty() &&
                         word.front() != '.' && !is_directive(word) && !instruction(target, word)) {
                    // Motorola/Mostek labels may omit ':' in the first column.
                    label = word; scan = after_word; scan.skipSpaces();
                    start = scan; word = token(); scan.skipSpaces();
                } else { scan = after_word; scan.skipSpaces(); }
            }
        }
        const auto op = directive(word);
        auto define = [&](int64_t value) {
            if (label.empty()) return;
            libasm::StrScanner name(label.c_str()), parsed;
            if (encoder.parser().readSymbol(name, parsed) != libasm::OK || *name) {
                diagnostic(base, "Invalid label: " + label); return;
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
        if (op == "EQU") {
            if (label.empty()) diagnostic(start.str(), "EQU requires a label");
            auto value = expr();
            // Do not resolve cyclic EQU chains through fictitious zero values.
            if (expression_resolved && !value.isUndefined()) define(CheckedOperators::integer(value));
        } else {
            define(pc);
            if (word.empty() && encoder.endOfLine(scan)) {
                listing.address = pc;
            } else if (op == "CPU") {
                const bool quoted = scan.expect('"');
                auto name = token();
                if (quoted && !scan.expect('"')) diagnostic(scan.str(), "Missing closing quote");
                scan.skipSpaces();
                const bool matches = target == Target::z80 ? name == "Z80" :
                    target == Target::mc68000 ? name == "MC68000" || name == "68000" :
                    target == Target::w65c02 ? name == "W65C02" || name == "W65C02S" || name == "65C02" :
                    name == "W65C816" || name == "W65C816S" || name == "65816";
                if (!matches) diagnostic(start.str(), "CPU directive must match the selected project target");
            } else if (op == "A8" || op == "A16" || op == "I8" || op == "I16" ||
                       op == "LONGA" || op == "LONGI") {
                bool wide = op == "A16" || op == "I16";
                if (op == "LONGA" || op == "LONGI") {
                    const auto value = token(); scan.skipSpaces();
                    wide = value == "ON";
                    if (value != "ON" && value != "OFF") diagnostic(start.str(), "Expected ON or OFF");
                }
                if (target != Target::w65c816) diagnostic(start.str(), "Register width directives require W65C816");
                else {
                    auto &mos = static_cast<libasm::mos6502::AsmMos6502 &>(encoder);
                    if (op.front() == 'A' || op == "LONGA") mos.setLongAccumulator(wide);
                    else mos.setLongIndex(wide);
                }
            } else if (op == "ORG" || op == "DS" || op == "DS.W" || op == "DS.L" || op == "DEFS") {
                listing.address = pc;
                auto at = scan.str(); auto v = expr();
                const int64_t unit = op == "DS.W" ? 2 : op == "DS.L" ? 4 : 1;
                const int64_t n = CheckedOperators::integer(v) * unit;
                if (n < 0 || n > limit || (op == "ORG" ? n >= limit : n + pc > limit))
                    diagnostic(at, std::string("Address outside ") + info.name + " range");
                else if (op == "ORG") pc = static_cast<uint32_t>(n);
                else if (op != "DEFS") pc += static_cast<uint32_t>(n);
                else {
                    uint8_t fill = 0;
                    if (scan.expect(',')) {
                        auto fill_at = scan.str(); auto value = expr();
                        if (value.overflowUint8()) diagnostic(fill_at, "Data value out of range");
                        fill = static_cast<uint8_t>(value.getUnsigned());
                    }
                    listing.bytes.assign(static_cast<size_t>(n), fill);
                }
            } else if (op == "ALIGN" || op == "EVEN") {
                listing.address = pc;
                const int64_t alignment = op == "EVEN" ? 2 : CheckedOperators::integer(expr());
                if (alignment <= 0 || alignment > limit) diagnostic(start.str(), "Alignment out of range");
                else {
                    const auto padding = static_cast<uint32_t>((alignment - pc % alignment) % alignment);
                    if (padding > limit - pc) diagnostic(start.str(), "Alignment exceeds address range");
                    else listing.bytes.assign(padding, 0);
                }
            } else if (op == "DB" || op == "DW" || op == "DD") {
                listing.address = pc;
                const unsigned width = op == "DD" ? 4 : op == "DW" ? 2 : 1;
                do {
                    scan.skipSpaces(); auto at = scan.str();
                    if (width == 1 && *scan == '"') {
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
                        if ((width == 1 && v.overflowUint8()) || (width == 2 && v.overflowUint16()))
                            diagnostic(at, "Data value out of range");
                        for (unsigned i = 0; i < width; ++i) {
                            const unsigned shift = 8 * (info.big_endian ? width - i - 1 : i);
                            listing.bytes.push_back(static_cast<uint8_t>(v.getUnsigned() >> shift));
                        }
                    }
                } while (scan.expect(','));
            } else if (!instruction(target, word)) {
                diagnostic(start.str(), "Unsupported instruction or directive: " + word);
            } else {
                if (target == Target::mc68000 && (pc & 1))
                    diagnostic(start.str(), "MC68000 instructions require an even address");
                libasm::Insn insn(pc);
                encoder.encode(start.str(), insn, &symbols);
                error(insn);
                listing.address = pc;
                listing.bytes.assign(insn.bytes(), insn.bytes() + insn.length());
                if (target == Target::w65c816 && (pc & 0xFFFF) + insn.length() > 0x10000)
                    diagnostic(start.str(), "Instruction crosses a W65C816 program bank boundary");
                // On success errorAt is the first unconsumed source character.
                scan = libasm::StrScanner(insn.getError() ? base + line.size() : insn.errorAt());
            }
        }
        if (!encoder.endOfLine(scan)) diagnostic(scan.str(), "Garbage at end of line");
        if (!listing.bytes.empty()) {
            if (pc >= limit || listing.bytes.size() > limit - pc)
                diagnostic(base, "Emitted bytes exceed target address range");
            else {
                auto next = memory.lower_bound(pc);
                const auto end = pc + listing.bytes.size();
                const bool overlap = (next != memory.end() && next->first < end) ||
                    (next != memory.begin() && std::prev(next)->first + std::prev(next)->second.size() > pc);
                if (overlap) diagnostic(base, "Overlapping emitted bytes");
                else memory.emplace_hint(next, pc, listing.bytes);
                pc += static_cast<uint32_t>(listing.bytes.size());
            }
        }
        out.listing.push_back(std::move(listing));
    }
    for (auto &[address, bytes] : memory) {
        if (out.segments.empty() || out.segments.back().first + out.segments.back().bytes.size() != address)
            out.segments.push_back({address, std::move(bytes)});
        else out.segments.back().bytes.insert(out.segments.back().bytes.end(), bytes.begin(), bytes.end());
    }
    std::sort(out.symbols.begin(), out.symbols.end(), [](auto &a, auto &b) { return a.name < b.name; });
    return out;
}
}
bool is_assembly_directive(std::string_view word) { return is_directive(upper(std::string(word))); }

AssemblyResult Assembler::assemble(const std::string &source, Target target, uint32_t origin, uint64_t revision) const {
    if (origin >= target_info(target).address_limit() || source.find('\0') != std::string::npos) {
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
        auto next = pass(lines, target, origin, previous.symbols);
        if (i && next.symbols == previous.symbols && next.segments == previous.segments) {
            auto final = pass(lines, target, origin, next.symbols);
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
