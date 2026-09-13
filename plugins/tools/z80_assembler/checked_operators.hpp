#pragma once
#include <value_parser.h>
#include <cctype>
#include <limits>
namespace srz80::assembler {
// The pinned NOFLOAT Value arithmetic wraps at 32 bits. Keep its parser and
// precedence, but reject arithmetic overflow before performing the operation.
struct CheckedOperators final : libasm::OperatorParser {
    using V=libasm::Value;
    using Stack=libasm::ValueStack;
    using Context=libasm::ParserContext;
    using Error=libasm::Error;
    static int64_t integer(const V &v) { return v.isSigned()?v.getSigned():static_cast<int64_t>(v.getUnsigned()); }
    static Error push(Stack &s,int64_t v) {
        V result;
        if(v < INT32_MIN || v > UINT32_MAX) {s.push(result);return libasm::OVERFLOW_RANGE;}
        if(v<0) result.setSigned(static_cast<int32_t>(v));else result.setUnsigned(static_cast<uint32_t>(v));
        s.push(result);return libasm::OK;
    }
    template<char Op> static Error arithmetic(Stack &s,Context &) {
        auto b=s.pop(),a=s.pop();
        if(a.isUndefined() || b.isUndefined()) {s.push(V{});return libasm::OK;}
        if(!a.isInteger() || !b.isInteger()) {s.push(V{});return libasm::INTEGER_REQUIRED;}
        auto x=integer(a),y=integer(b);
        if constexpr(Op=='+') return push(s,x+y);
        if constexpr(Op=='-') return push(s,x-y);
        if constexpr(Op=='*') {
            uint64_t magnitude=static_cast<uint64_t>(x<0?-x:x)*static_cast<uint64_t>(y<0?-y:y);
            bool negative=(x<0)!=(y<0);
            if(magnitude>(negative?uint64_t{2147483648}:uint64_t{UINT32_MAX})) {s.push(V{});return libasm::OVERFLOW_RANGE;}
            return push(s,negative?-static_cast<int64_t>(magnitude):static_cast<int64_t>(magnitude));
        }
        if constexpr(Op=='<') {
            if(y<0 || y>31) {s.push(V{});return libasm::OVERFLOW_RANGE;}
            return push(s,x*(int64_t{1}<<y));
        }
        return libasm::INTERNAL_ERROR;
    }
    const libasm::Operator *readPrefix(libasm::StrScanner &s,Stack &v,Context &c) const override {
        // Zilog's upstream operator parser recognizes LOW and HIGH by trimming
        // only alphanumeric characters.  That makes a legal Intel-style symbol
        // such as low_lead look like the LOW operator followed by `_lead`.
        // Preserve the complete symbol token when the operator name is followed
        // by a symbol continuation character.
        auto p=s;
        p.trimStart([](char ch) { return std::isalnum(static_cast<unsigned char>(ch)); });
        const libasm::StrScanner name(s.str(),p.str());
        const bool low_or_high=name.iequals_P(PSTR("LOW")) || name.iequals_P(PSTR("HIGH"));
        const bool low_or_high_word=name.iequals_P(PSTR("LOW16")) || name.iequals_P(PSTR("HIGH16"));
        const bool logical_not=name.iequals_P(PSTR("LNOT"));
        if ((low_or_high || low_or_high_word || logical_not) && (*p=='_' || *p=='?')) return nullptr;
        return libasm::ZilogOperatorParser::singleton().readPrefix(s,v,c);
    }
    const libasm::Operator *readInfix(libasm::StrScanner &s,Stack &v,Context &c) const override {
        auto op=libasm::ZilogOperatorParser::singleton().readInfix(s,v,c);
        using O=libasm::Operator;
        static const O add(6,O::LEFT,2,arithmetic<'+'>), sub(6,O::LEFT,2,arithmetic<'-'>),
                       mul(5,O::LEFT,2,arithmetic<'*'>), shift(7,O::LEFT,2,arithmetic<'<'>);
        if(op==&O::OP_ADD)return &add;
        if(op==&O::OP_SUB)return &sub;
        if(op==&O::OP_MUL)return &mul;
        if(op==&O::OP_SHIFT_LEFT)return &shift;
        return op;
    }
};
// libasm's generic floating-point probe accepts a bare E followed by digits
// (for example, E4) as an exponent-only float.  Float expressions are not
// supported by this tool, and E4/E5 are entirely valid Z80 symbols (and
// common note names), so let the normal symbol lookup handle tokens which
// did not begin with an integer.
struct Z80NumberParser final : libasm::NumberParser {
    libasm::Error parseNumber(libasm::StrScanner &scan, libasm::Value &value,
                               libasm::Radix radix) const override {
        return libasm::IntelNumberParser::singleton().parseNumber(scan, value, radix);
    }
    libasm::Error parseFloat(libasm::StrScanner &scan, const libasm::StrScanner &tail,
                             libasm::Value &value, libasm::Error integer_error,
                             char delimiter) const override {
        if (integer_error == libasm::NOT_AN_EXPECTED)
            return libasm::NOT_AN_EXPECTED;
        return libasm::IntelNumberParser::singleton().parseFloat(scan, tail, value, integer_error,
                                                                   delimiter);
    }
};
struct CheckedPlugins final : libasm::ValueParser::IntelPlugins {
    const libasm::NumberParser &number() const override { static const Z80NumberParser p; return p; }
    const libasm::OperatorParser &operators() const override {static const CheckedOperators p;return p;}
};
}
