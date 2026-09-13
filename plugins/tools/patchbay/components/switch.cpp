#include "../render.hpp"
namespace srz80::patchbay::ui {
void draw_switch(Render &r) {
    ImGui::SetCursorScreenPos(r.point(60, 55));
    ImGui::InvisibleButton("slide", {110 * r.scale, 50 * r.scale});
    if (ImGui::IsItemClicked())
        r.interact(r.value() ? 0 : 1);
    r.draw->AddRectFilled(r.point(60, 55), r.point(170, 105), IM_COL32(40, 40, 40, 255), 5 * r.scale);
    const float x = r.value() ? 120.f : 65.f;
    r.draw->AddRectFilled(r.point(x, 60), r.point(x + 45, 100), IM_COL32(185, 185, 185, 255), 3 * r.scale);
}
} // namespace srz80::patchbay::ui
