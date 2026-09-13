#ifndef SRZ80_X1_010_CORE_HPP
#define SRZ80_X1_010_CORE_HPP

#include "util.hpp"

namespace vgsound_emu {
class vgsound_emu_core {
  public:
    explicit vgsound_emu_core(std::string tag) : m_tag(std::move(tag)) {}
    std::string tag() const { return m_tag; }

  private:
    std::string m_tag;
};
} // namespace vgsound_emu

#endif
