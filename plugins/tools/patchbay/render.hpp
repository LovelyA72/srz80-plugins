#pragma once
#include "contract.hpp"
#include <functional>
#include <imgui.h>
namespace srz80::patchbay::ui {
struct Render {
    ImDrawList *draw;
    ImVec2 origin;
    float scale;
    const Json &state;
    const Json &config;
    std::function<void(uint32_t)> interact;
    ImVec2 point(float x, float y) const {
        return {origin.x + x * scale, origin.y + y * scale};
    }
    float brightness(size_t i) const {
        const auto b = state.value("brightness", Json::array());
        return i < b.size() ? b[i].get<float>() / 65535.f : 0.f;
    }
    uint32_t value() const {
        return state.value("value", 0u);
    }
    uint32_t color() const {
        return config.value("color", 0xe02020u);
    }
    void line(float x, float y, float x2, float y2, ImU32 color, float width = 7) const {
        draw->AddLine(point(x, y), point(x2, y2), color, width * scale);
    }
};
inline ImU32 red(float value) {
    return IM_COL32(static_cast<int>(45 + 210 * value), value > 0 ? 12 : 45, value > 0 ? 12 : 45, 255);
}
inline ImU32 lit_color(uint32_t rgb, float value) {
    const float glow = .15f + .85f * std::clamp(value, 0.f, 1.f);
    return IM_COL32(static_cast<int>(((rgb >> 16) & 255) * glow),
                    static_cast<int>(((rgb >> 8) & 255) * glow), static_cast<int>((rgb & 255) * glow), 255);
}
void draw_led(Render &);
void draw_rgb_led(Render &);
void draw_button(Render &);
void draw_switch(Render &);
void draw_dip8(Render &);
void draw_segment7(Render &);
void draw_segment11(Render &);
void draw_hc595(Render &);
inline void draw_segments(Render &r, bool eleven) {
    // a..g are conventional; h..k are upper/lower diagonals; DP is separate.
    const float lines[][4] = {{87, 30, 137, 30},   {144, 37, 144, 72}, {144, 88, 144, 123},
                              {87, 130, 137, 130}, {80, 88, 80, 123},  {80, 37, 80, 72},
                              {87, 80, 137, 80},   {88, 40, 109, 70},  {136, 40, 115, 70},
                              {88, 120, 109, 90},  {136, 120, 115, 90}};
    for (size_t i = 0; i < (eleven ? 11u : 7u); ++i)
        r.line(lines[i][0], lines[i][1], lines[i][2], lines[i][3], lit_color(r.color(), r.brightness(i)));
    r.draw->AddCircleFilled(r.point(160, 130), 4 * r.scale,
                            lit_color(r.color(), r.brightness(eleven ? 11 : 7)));
}
} // namespace srz80::patchbay::ui
