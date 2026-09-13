#include "../render.hpp"
namespace srz80::patchbay::ui {
void draw_rgb_led(Render &r) {
    r.draw->AddCircleFilled(r.point(115, 58), 23 * r.scale,
                            IM_COL32(static_cast<int>(40 + 215 * r.brightness(0)),
                                     static_cast<int>(40 + 215 * r.brightness(1)),
                                     static_cast<int>(40 + 215 * r.brightness(2)), 255),
                            40);
}
} // namespace srz80::patchbay::ui
