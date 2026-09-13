#include "../render.hpp"
namespace srz80::patchbay::ui {
void draw_dip8(Render &r) {
    r.draw->AddRectFilled(r.point(40, 40), r.point(155, 245), IM_COL32(150, 30, 35, 255));
    for (int i = 0; i < 8; ++i) {
        const float y = 46.f + i * 24.f;
        const bool on = (r.value() & (1u << i)) != 0;
        ImGui::PushID(i);
        ImGui::SetCursorScreenPos(r.point(50, y));
        ImGui::InvisibleButton("dip", {92 * r.scale, 20 * r.scale});
        if (ImGui::IsItemClicked())
            r.interact(r.value() ^ (1u << i));
        r.draw->AddRectFilled(r.point(75, y), r.point(140, y + 18), IM_COL32(45, 45, 45, 255));
        r.draw->AddRectFilled(r.point(on ? 114 : 78, y + 2), r.point(on ? 137 : 101, y + 16),
                              IM_COL32(220, 220, 210, 255));
        const auto label = std::to_string(i + 1);
        r.draw->AddText(r.point(53, y), IM_COL32_WHITE, label.c_str());
        ImGui::PopID();
    }
}
} // namespace srz80::patchbay::ui
