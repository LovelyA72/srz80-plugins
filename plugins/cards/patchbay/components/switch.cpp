#include "../engine.hpp"
namespace srz80::patchbay {
int switch_drive(const Component &c, const std::string &) {
    return static_cast<int>(((c.value >> (0)) & 1) ^ (c.active() ? 0 : 1));
}
} // namespace srz80::patchbay
