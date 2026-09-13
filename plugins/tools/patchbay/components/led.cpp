#include "../render.hpp"
namespace srz80::patchbay::ui {
void draw_led(Render &r) {
    r.draw->AddCircleFilled(r.point(100, 38), 20 * r.scale, lit_color(r.color(), r.brightness(0)), 40);
}
} // namespace srz80::patchbay::ui
