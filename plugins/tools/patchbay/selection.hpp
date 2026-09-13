#pragma once
#include <algorithm>
#include <string>
#include <vector>
namespace srz80::patchbay::ui {
struct HitBox {
    std::string id;
    float left, top, right, bottom;
};
// Boxes are in paint order. Repeated clicks can reach a covered part, while
// exposed regions always select the part actually under the pointer.
inline std::vector<std::string> hit_parts(const std::vector<HitBox> &boxes, float x, float y) {
    std::vector<std::string> hits;
    for (auto it = boxes.rbegin(); it != boxes.rend(); ++it)
        if (x >= it->left && x < it->right && y >= it->top && y < it->bottom)
            hits.push_back(it->id);
    return hits;
}
inline std::string pick_part(const std::vector<std::string> &hits, const std::string &selected) {
    if (hits.empty())
        return {};
    return hits.size() > 1 && hits.front() == selected ? hits[1] : hits.front();
}
} // namespace srz80::patchbay::ui
