#ifndef SRZ80_X1_010_MEM_INTF_HPP
#define SRZ80_X1_010_MEM_INTF_HPP

#include "../core.hpp"

namespace vgsound_emu {
class vgsound_emu_mem_intf : public vgsound_emu_core {
  public:
    vgsound_emu_mem_intf() : vgsound_emu_core("mem_intf") {}
    virtual u8 read_byte(u32) { return 0; }
    virtual u16 read_word(u32) { return 0; }
    virtual u32 read_dword(u32) { return 0; }
    virtual u64 read_qword(u32) { return 0; }
    virtual void write_byte(u32, u8) {}
    virtual void write_word(u32, u16) {}
    virtual void write_dword(u32, u32) {}
    virtual void write_qword(u32, u64) {}
};
} // namespace vgsound_emu

#endif
