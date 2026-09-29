#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace srz80::assembler {
enum class Target { z80, mc68000, w65c816, w65c02 };
struct TargetInfo {
    Target target;
    const char *id;
    const char *name;
    unsigned address_bits;
    bool big_endian;
    constexpr uint32_t address_limit() const { return uint32_t{1} << address_bits; }
    constexpr int address_digits() const { return static_cast<int>(address_bits / 4); }
};
inline constexpr std::array targets = {
    TargetInfo{Target::z80, "z80", "Z80", 16, false},
    TargetInfo{Target::mc68000, "mc68000", "MC68000", 24, true},
    TargetInfo{Target::w65c816, "w65c816", "W65C816", 24, false},
    TargetInfo{Target::w65c02, "w65c02", "W65C02", 16, false}};
inline constexpr const TargetInfo &target_info(Target target) {
    for (const auto &info : targets) if (info.target == target) return info;
    return targets.front();
}
inline std::optional<Target> target_from_id(std::string_view id) {
    for (const auto &info : targets) if (info.id == id) return info.target;
    return std::nullopt;
}
}
