/*
 * Small compatibility layer for the Furnace/vgsound_emu X1-010 core.
 * The card deliberately keeps this dependency private to the plugin.
 */
#ifndef SRZ80_X1_010_CORE_UTIL_HPP
#define SRZ80_X1_010_CORE_UTIL_HPP

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace vgsound_emu {
using u8 = unsigned char;
using u16 = unsigned short;
using u32 = unsigned int;
using u64 = unsigned long long;
using s8 = signed char;
using s16 = signed short;
using s32 = signed int;
using s64 = signed long long;
using f32 = float;
using f64 = double;

template <typename T> inline T bitfield(T in, u8 pos, u8 len = 1) {
    return (in >> pos) & (len ? (T(1 << len) - 1) : 1);
}
} // namespace vgsound_emu

#endif
