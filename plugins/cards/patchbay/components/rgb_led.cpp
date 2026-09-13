#include "../engine.hpp"
namespace srz80::patchbay {
// A display is a logical consumer; brightness is integrated by Observer.
int rgb_led_drive(const Component &, const std::string &) {
    return Floating;
}
} // namespace srz80::patchbay
