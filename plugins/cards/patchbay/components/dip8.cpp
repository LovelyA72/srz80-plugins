#include "../engine.hpp"
namespace srz80::patchbay {
int dip8_drive(const Component &c, const std::string &name) {
    return static_cast<int>(((c.value >> (name[1] - '0')) & 1) ^ (c.active() ? 0 : 1));
}
} // namespace srz80::patchbay
