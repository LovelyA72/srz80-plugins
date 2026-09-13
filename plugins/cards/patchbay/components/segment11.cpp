#include "../engine.hpp"
namespace srz80::patchbay {
// A display is a logical consumer; brightness is integrated by Observer.
int segment11_drive(const Component &, const std::string &) {
    return Floating;
}
} // namespace srz80::patchbay
