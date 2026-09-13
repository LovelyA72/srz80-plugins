#include "../render.hpp"
namespace srz80::patchbay::ui {
void draw_hc595(Render &r) {
    r.draw->AddRectFilled(r.point(80, 35), r.point(148, 245), IM_COL32(35, 35, 35, 255), 5 * r.scale);
    r.draw->AddCircleFilled(r.point(94, 50), 4 * r.scale, IM_COL32(170, 170, 170, 255));
    r.draw->AddText(r.point(85, 95), IM_COL32(220, 220, 220, 255), "74HC\n595");
}
} // namespace srz80::patchbay::ui
