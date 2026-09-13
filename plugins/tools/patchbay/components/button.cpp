#include "../render.hpp"
namespace srz80::patchbay::ui {
void draw_button(Render &r) {
    ImGui::SetCursorScreenPos(r.point(65, 40));
    ImGui::InvisibleButton("press", {100 * r.scale, 80 * r.scale});
    if (ImGui::IsItemActivated())
        r.interact(1);
    const bool down = ImGui::IsItemActive() && ImGui::IsMouseDown(0);
    if (ImGui::IsItemDeactivated())
        r.interact(0);
    r.draw->AddCircleFilled(r.point(115, 80), 33 * r.scale,
                            down ? IM_COL32(210, 70, 60, 255) : IM_COL32(60, 60, 60, 255), 32);
    r.draw->AddCircle(r.point(115, 80), 36 * r.scale, IM_COL32(170, 170, 170, 255), 32, 2 * r.scale);
}
} // namespace srz80::patchbay::ui
