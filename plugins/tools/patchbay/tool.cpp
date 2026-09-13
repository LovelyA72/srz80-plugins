#include "render.hpp"
#include "selection.hpp"
#include <boundary.hpp>
#include <cmath>
#include <string_view>
#include <cstring>
#include <map>
#include <memory>
#include <srz80/font_awesome.h>
#include <srz80/tool.h>
namespace srz80::patchbay::ui {
namespace {
struct Tool {
    const SrhToolHostV1 *host = nullptr;
    Json layout = {{"layouts", Json::object()}, {"flips", Json::object()}, {"zoom", 1.f}, {"pan", {30.f, 50.f}}};
    Json snapshot, topology, providers = Json::array();
    uint64_t generation = 0, revision = 0, owner = 0;
    size_t provider_index = 0;
    uint32_t provider_flags = 0;
    std::string provider_buffer = std::string(SRH_PROVIDER_MAX_BYTES, '\0');
    std::string last_bundle;
    size_t last_provider_index = SIZE_MAX;
    bool can_edit() const {
        return host->run_state(host->context) == SRT_STOPPED ||
               (provider_flags & SRH_PROVIDER_LIVE_CONFIG);
    }
    std::string selected, wire, error, held;
    size_t wire_provider = SIZE_MAX;
    uint64_t held_owner = 0, held_generation = 0, held_revision = 0;
    std::vector<std::string> order;
    std::string dragging_part, click_selection;
    bool drag_moved = false, panning = false;
    float zoom = 1;
    ImVec2 pan{30, 50};
    void send(uint32_t kind, const Json &payload) {
        if (kind == 1) release();
        const auto text = payload.dump();
        const auto status =
            host->provider_command(host->context, owner, generation, kind, revision, text.data(), text.size());
        if (status != SRH_OK)
            error = "Command rejected: stop the rack for edits; refresh if the circuit changed.";
        else {
            error.clear();
            // Interaction commands synchronously publish an authoritative
            // provider snapshot (bounded by the controller's 30 ms budget).
            // Consume it before drawing downstream devices this frame.
            if (kind == 0)
                refresh();
            if (kind == 1) {
                topology = payload;
                ++revision;
            }
        }
    }
    void release() {
        if (held.empty())
            return;
        const auto text = Json({{"id", held}, {"value", 0}}).dump();
        host->provider_command(host->context, held_owner, held_generation, 0, held_revision, text.data(),
                            text.size());
        held.clear();
    }
    void refresh() {
        // The host caches serialization; consume each new publication on the
        // next UI frame without adding another independently phased timer.
        uint64_t size = provider_buffer.size();
        if (host->provider_data(host->context, provider_buffer.data(), &size) != SRH_OK || size == 0 ||
            size > provider_buffer.size())
            return;
        const std::string_view text(provider_buffer.data(), size - 1);
        if (text == last_bundle && last_provider_index == provider_index) return;
        auto bundle = Json::parse(text);
        last_bundle = text;
        last_provider_index = provider_index;
        const uint64_t next_generation = bundle.at("generation");
        providers = Json::array();
        for (const auto &provider : bundle.at("providers"))
            if (provider.value("protocol", "") == protocol_id) providers.push_back(provider);
        if (next_generation != generation) {
            release();
            owner = 0;
            selected.clear();
            wire.clear();
            revision = 0;
            order.clear();
            generation = next_generation;
        }
        if (providers.empty()) {
            release();
            owner = 0;
            return;
        }
        if (provider_index >= providers.size())
            provider_index = 0;
        auto &provider = providers[provider_index];
        const uint64_t next_owner = provider.at("owner");
        if (next_owner != owner) {
            release();
            order.clear();
            owner = next_owner;
            revision = 0;
            selected.clear();
            wire.clear();
        }
        provider_flags = provider.value("flags", 0u);
        auto data = Json::parse(provider.at("data").get<std::string>());
        if (data.value("protocol", 0) != 1) {
            error = "Unsupported logical I/O protocol";
            owner = 0;
            return;
        }
        if (data.at("revision").get<uint64_t>() >= revision) {
            snapshot = data;
            topology = data.at("topology");
            revision = data.at("revision");
        }
    }
    Json &positions() {
        // All logical GPIO cards share one visual workspace. Card-local
        // component ids are qualified before they reach this map.
        return layout["layouts"]["combined"];
    }
    Json &flips() {
        return layout["flips"]["combined"];
    }
    ImVec2 position(const std::string &id, size_t index) {
        auto &positions_json = positions();
        if (!positions_json.is_object())
            positions_json = Json::object();
        if (!positions_json.contains(id))
            positions_json[id] = {static_cast<float>((index % 4) * 270),
                                  static_cast<float>((index / 4) * 390)};
        auto &v = positions_json[id];
        return {v[0].get<float>(), v[1].get<float>()};
    }
    bool flipped(const std::string &id) {
        auto &flips_json = flips();
        if (!flips_json.is_object())
            flips_json = Json::object();
        return flips_json.value(id, false);
    }
    Json provider_snapshot(size_t index) const {
        return Json::parse(providers.at(index).at("data").get<std::string>());
    }
    void activate_provider(size_t index) {
        if (index >= providers.size())
            return;
        provider_index = index;
        const auto &provider = providers[index];
        owner = provider.at("owner");
        provider_flags = provider.value("flags", 0u);
        snapshot = provider_snapshot(index);
        topology = snapshot.at("topology");
        revision = snapshot.at("revision");
    }
    std::string new_id(const char *prefix) const {
        for (uint64_t n = 1;; ++n) {
            auto id = std::string(prefix) + std::to_string(n);
            bool found = false;
            for (const auto &c : topology["components"])
                found |= c["id"] == id;
            for (const auto &net : topology["nets"])
                found |= net["id"] == id;
            if (!found)
                return id;
        }
    }
    void connect(const std::string &pin, size_t index) {
        if (wire.empty()) {
            wire = pin;
            wire_provider = index;
            return;
        }
        if (index != wire_provider) {
            error = "Wiring stays within one GPIO unit.";
            wire.clear();
            wire_provider = SIZE_MAX;
            return;
        }
        if (wire == pin) {
            wire.clear();
            wire_provider = SIZE_MAX;
            return;
        }
        const std::string prefix = "gpio-" + std::to_string(owner) + "/";
        const auto local_pin = [&](const std::string &value) { return value.substr(prefix.size()); };
        const auto first = local_pin(wire), second = local_pin(pin);
        auto next = topology;
        auto &nets = next["nets"];
        int a = -1, b = -1;
        for (size_t i = 0; i < nets.size(); ++i)
            for (const auto &p : nets[i]["pins"]) {
                if (p == first)
                    a = static_cast<int>(i);
                if (p == second)
                    b = static_cast<int>(i);
            }
        if (a < 0 && b < 0)
            nets.push_back({{"id", new_id("net")}, {"pins", {first, second}}});
        else if (a < 0)
            nets[b]["pins"].push_back(first);
        else if (b < 0)
            nets[a]["pins"].push_back(second);
        else if (a != b) {
            for (const auto &p : nets[b]["pins"])
                nets[a]["pins"].push_back(p);
            nets.erase(nets.begin() + b);
        }
        wire.clear();
        wire_provider = SIZE_MAX;
        send(1, next);
    }
    void disconnect(const std::string &pin) {
        auto next = topology;
        for (auto &net : next["nets"]) {
            auto &p = net["pins"];
            p.erase(std::remove(p.begin(), p.end(), Json(pin)), p.end());
        }
        auto &nets = next["nets"];
        nets.erase(
            std::remove_if(nets.begin(), nets.end(), [](const Json &n) { return n["pins"].size() < 2; }),
            nets.end());
        send(1, next);
    }
};
using Renderer = void (*)(Render &);
const std::map<std::string, Renderer> renderers = {
    {"led", draw_led},   {"rgb", draw_rgb_led},       {"button", draw_button},       {"switch", draw_switch},
    {"dip8", draw_dip8}, {"segment7", draw_segment7}, {"segment11", draw_segment11}, {"595", draw_hc595}};
void canvas(Tool &t) {
    const size_t edit_provider = t.provider_index;
    ImGui::BeginChild("canvas", {0, 0}, ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const ImVec2 origin = ImGui::GetCursorScreenPos(), size = ImGui::GetContentRegionAvail();
    auto *draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, {origin.x + size.x, origin.y + size.y}, IM_COL32(15, 17, 19, 255));
    if (ImGui::IsWindowHovered()) {
        const auto &io = ImGui::GetIO();
        if (io.MouseWheel != 0) {
            const float old = t.zoom;
            t.zoom = std::clamp(t.zoom + io.MouseWheel * .1f, .75f, 1.8f);
            const ImVec2 m = io.MousePos;
            t.pan.x = m.x - origin.x - (m.x - origin.x - t.pan.x) * t.zoom / old;
            t.pan.y = m.y - origin.y - (m.y - origin.y - t.pan.y) * t.zoom / old;
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape))
        t.wire.clear();
    const float grid = 32 * t.zoom;
    for (float x = std::fmod(t.pan.x, grid); x < size.x; x += grid)
        draw->AddLine({origin.x + x, origin.y}, {origin.x + x, origin.y + size.y}, IM_COL32(28, 31, 35, 255));
    for (float y = std::fmod(t.pan.y, grid); y < size.y; y += grid)
        draw->AddLine({origin.x, origin.y + y}, {origin.x + size.x, origin.y + y}, IM_COL32(28, 31, 35, 255));
    struct Node {
        std::string id;
        std::string local_id;
        size_t provider_index = 0;
        Json definition, descriptor;
        ImVec2 pos;
        float height;
        float width = 220;
        bool flipped = false;
    };
    std::vector<Node> nodes;
    for (size_t provider_index = 0; provider_index < t.providers.size(); ++provider_index) {
        const auto source = t.provider_snapshot(provider_index);
        const auto owner = t.providers[provider_index].at("owner").get<uint64_t>();
        const std::string prefix = "gpio-" + std::to_string(owner) + "/";
        Json gpio_pins = Json::array();
        for (const auto &p : source["gpio"]["pins"])
            gpio_pins.push_back({{"name", p}, {"direction", "bidirectional"}});
        const std::string gpio_id = prefix + "gpio";
        nodes.push_back({gpio_id, "gpio", provider_index, Json::object(),
                         {{"name", "GPIO #" + std::to_string(owner)}, {"pins", gpio_pins}, {"renderer", "gpio"}},
                         t.position(gpio_id, nodes.size()), 280, 220, t.flipped(gpio_id)});
        const auto definitions = source["topology"]["components"];
        for (const auto &def : definitions) {
            Json desc;
            for (const auto &d : source["catalogue"])
                if (d["type"] == def["type"] && d["schema"] == def["schema"])
                    desc = d;
            if (desc.is_null())
                desc = {{"name", def.at("type")}, {"pins", Json::array()}, {"properties", Json::object()},
                        {"renderer", "generic"}, {"schema", def.at("schema")}};
            size_t inputs = 0, outputs = 0;
            for (const auto &p : desc["pins"])
                p["direction"] == "input" ? ++inputs : ++outputs;
            const std::string local_id = def["id"], id = prefix + local_id;
            nodes.push_back({id, local_id, provider_index, def, desc, t.position(id, nodes.size()),
                             std::max(170.f, 45.f + std::max(inputs, outputs) * 26.f), 220, t.flipped(id)});
        }
    }
    // Controls must consume clicks before consumers are painted so the
    // synchronous interaction snapshot lights downstream devices this frame.
    const auto control = [](const Node &n) {
        const auto renderer = n.descriptor.value("renderer", "");
        return renderer == "button" || renderer == "switch" || renderer == "dip8";
    };
    std::stable_sort(nodes.begin(), nodes.end(), [&](const Node &a, const Node &b) {
        return control(a) && !control(b);
    });
    for (auto &n : nodes) {
        const auto renderer = n.descriptor.value("renderer", "");
        if (renderer == "led") {
            n.width = 150;
            n.height = 90;
        }
        if (renderer == "rgb") {
            n.width = 170;
            n.height = 125;
        }
        if (renderer == "dip8") {
            // The control needs room for its fixed-size switch bank while the
            // output labels stay attached to the component's right edge.
            n.width = 200;
            n.height = 253;
        }
        if (std::find(t.order.begin(), t.order.end(), n.id) == t.order.end())
            t.order.push_back(n.id);
    }
    std::stable_sort(nodes.begin(), nodes.end(), [&](const Node &a, const Node &b) {
        return std::find(t.order.begin(), t.order.end(), a.id) <
               std::find(t.order.begin(), t.order.end(), b.id);
    });
    std::map<std::string, ImVec2> points;
    auto screen = [&](ImVec2 p) {
        return ImVec2(origin.x + t.pan.x + p.x * t.zoom, origin.y + t.pan.y + p.y * t.zoom);
    };
    // Component artwork shares the canvas transform, so it remains
    // proportionate to its frame while zooming in and out.
    const float content_scale = t.zoom;
    for (const auto &n : nodes) {
        int left = 0, right = 0;
        const bool dip8 = n.descriptor.value("renderer", "") == "dip8";
        const ImVec2 pos = screen(n.pos);
        for (const auto &p : n.descriptor["pins"]) {
            const bool out = p["direction"] != "input";
            const bool right_side = out != n.flipped;
            int row = right_side ? right++ : left++;
            // DIP outputs are centred on their physical switch rows rather
            // than the generic 26-unit pin grid.  This preserves a direct
            // visual path from switch N to Q(N - 1) at every canvas zoom.
            const float pin_y = dip8 ? 55.f + row * 24.f : 45.f + row * 26.f;
            points[n.id + "." + p["name"].get<std::string>()] =
                {pos.x + (right_side ? n.width * t.zoom : 0.f),
                 pos.y + (n.descriptor.value("renderer", "") == "led" ? 70.f : pin_y) * content_scale};
        }
    }
    std::vector<HitBox> boxes;
    for (const auto &n : nodes) {
        const auto p = screen(n.pos);
        boxes.push_back({n.id, p.x, p.y - 28 * t.zoom, p.x + n.width * t.zoom, p.y + n.height * t.zoom});
    }
    const auto mouse = ImGui::GetIO().MousePos;
    const auto hits = hit_parts(boxes, mouse.x, mouse.y);
    const bool canvas_hovered = ImGui::IsWindowHovered();
    if (canvas_hovered &&
        (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) ||
         (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hits.empty())))
        t.panning = true;
    if (t.panning) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
            t.pan.x += ImGui::GetIO().MouseDelta.x;
            t.pan.y += ImGui::GetIO().MouseDelta.y;
        } else {
            t.panning = false;
        }
    }
    const bool canvas_click = canvas_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    if (canvas_click && hits.empty()) {
        t.wire.clear();
        t.selected.clear();
    }
    draw->ChannelsSplit(2);
    draw->ChannelsSetCurrent(1);
    for (auto &n : nodes) {
        t.activate_provider(n.provider_index);
        const bool node_editable = t.can_edit();
        const ImVec2 pos = screen(n.pos);
        ImGui::PushID(n.id.c_str());
        if (n.local_id != "gpio" && node_editable && !hits.empty() && hits.front() == n.id &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            t.selected = n.id;
            ImGui::OpenPopup("settings");
        }
        // Occluded widgets cannot steal a click from the visually upper part.
        ImGui::BeginDisabled(ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
                             !hits.empty() && hits.front() != n.id);
        draw->AddRectFilled(pos, {pos.x + n.width * t.zoom, pos.y + n.height * t.zoom},
                            IM_COL32(94, 94, 94, 255));
        if (t.selected == n.id) {
            const ImU32 yellow = IM_COL32(240, 250, 50, 255);
            for (float x = 0; x < n.width; x += 9) {
                draw->AddLine({pos.x + x * t.zoom, pos.y}, {pos.x + std::min(x + 5, n.width) * t.zoom, pos.y},
                              yellow, 2);
                draw->AddLine({pos.x + x * t.zoom, pos.y + n.height * t.zoom},
                              {pos.x + std::min(x + 5, n.width) * t.zoom, pos.y + n.height * t.zoom}, yellow,
                              2);
            }
            for (float y = 0; y < n.height; y += 9) {
                draw->AddLine({pos.x, pos.y + y * t.zoom},
                              {pos.x, pos.y + std::min(y + 5, n.height) * t.zoom}, yellow, 2);
                draw->AddLine({pos.x + n.width * t.zoom, pos.y + y * t.zoom},
                              {pos.x + n.width * t.zoom, pos.y + std::min(y + 5, n.height) * t.zoom}, yellow,
                              2);
            }
        }
        ImGui::SetCursorScreenPos({pos.x, pos.y - 27 * t.zoom});
        ImGui::InvisibleButton("title", {std::max(50.f, n.width - 114) * t.zoom, 25 * t.zoom});
        if (ImGui::IsItemClicked()) {
            t.selected = n.id;
        }
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0)) {
            n.pos.x += ImGui::GetIO().MouseDelta.x / t.zoom;
            n.pos.y += ImGui::GetIO().MouseDelta.y / t.zoom;
            t.positions()[n.id] = {n.pos.x, n.pos.y};
        }
        const std::string name = n.descriptor["name"];
        draw->AddText({pos.x, pos.y - 24 * t.zoom}, IM_COL32_WHITE, name.c_str());
        if (n.local_id != "gpio" && t.selected == n.id) {
            ImGui::SetCursorScreenPos({pos.x + (n.width - 112) * t.zoom, pos.y - 28 * t.zoom});
            ImGui::BeginDisabled(!node_editable);
            const auto icon_button = [](const char *icon, const char *tooltip) {
                const bool clicked = ImGui::Button(icon, {ImGui::GetFrameHeight(), ImGui::GetFrameHeight()});
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", tooltip);
                return clicked;
            };
            if (icon_button(SRZ80_FA_TRASH, "Remove component")) {
                auto next = t.topology;
                auto &cs = next["components"];
                cs.erase(std::remove_if(cs.begin(), cs.end(), [&](const Json &c) { return c["id"] == n.local_id; }),
                         cs.end());
                for (auto &net : next["nets"]) {
                    auto &ps = net["pins"];
                    ps.erase(std::remove_if(
                                 ps.begin(), ps.end(),
                                 [&](const Json &p) { return p.get<std::string>().starts_with(n.local_id + "."); }),
                             ps.end());
                }
                auto &nets = next["nets"];
                nets.erase(std::remove_if(nets.begin(), nets.end(),
                                          [](const Json &net) { return net["pins"].size() < 2; }),
                           nets.end());
                t.send(1, next);
                t.selected.clear();
            }
            ImGui::SameLine(0, 3);
            if (icon_button(SRZ80_FA_CLONE, "Copy component")) {
                auto next = t.topology;
                auto def = n.definition;
                const auto id = t.new_id("c");
                def["id"] = id;
                next["components"].push_back(def);
                const std::string qualified_id = "gpio-" + std::to_string(t.owner) + "/" + id;
                t.positions()[qualified_id] = {n.pos.x + 35, n.pos.y + 35};
                t.send(1, next);
                t.selected = qualified_id;
            }
            ImGui::SameLine(0, 3);
            if (icon_button(SRZ80_FA_EXCHANGE, n.flipped ? "Put ports back on their normal sides"
                                                         : "Swap left and right ports")) {
                t.flips()[n.id] = !n.flipped;
            }
            ImGui::SameLine(0, 3);
            if (icon_button(SRZ80_FA_ELLIPSIS_H, "Component settings"))
                ImGui::OpenPopup("settings");
            if (ImGui::BeginPopup("settings")) {
                ImGui::TextUnformatted(name.c_str());
                ImGui::Separator();
                auto cfg = n.definition.value("config", Json::object());
                bool changed = false;
                const std::string renderer = n.descriptor.value("renderer", "");
                for (const auto &[key, prop] : n.descriptor["properties"].items()) {
                    if (prop["type"] == "boolean") {
                        bool v = cfg.value(key, prop["default"].get<bool>());
                        const char *label = key == "active_high"
                                                ? ((renderer == "segment7" || renderer == "segment11")
                                                       ? "Common cathode (off = anode)"
                                                   : renderer == "595" ? "Clock active high (rising edge)"
                                                                       : "Active high")
                                            : key == "oe_active_high" ? "OE active high"
                                                                      : "MR active high";
                        if (ImGui::Checkbox(label, &v)) {
                            cfg[key] = v;
                            changed = true;
                        }
                    } else if (prop["type"] == "integer") {
                        int value = cfg.value(key, prop["default"].get<int>());
                        if (ImGui::InputInt(key.c_str(), &value)) {
                            cfg[key] = std::clamp(value, prop["min"].get<int>(), prop["max"].get<int>());
                            changed = true;
                        }
                    } else if (prop["type"] == "color") {
                        const uint32_t packed = cfg.value(key, prop["default"].get<uint32_t>());
                        float color[] = {static_cast<float>((packed >> 16) & 255) / 255.f,
                                         static_cast<float>((packed >> 8) & 255) / 255.f,
                                         static_cast<float>(packed & 255) / 255.f};
                        if (ImGui::ColorEdit3("Color", color, ImGuiColorEditFlags_NoInputs)) {
                            const auto channel = [](float v) {
                                return static_cast<uint32_t>(std::clamp(v, 0.f, 1.f) * 255.f + .5f);
                            };
                            cfg[key] = (channel(color[0]) << 16) | (channel(color[1]) << 8) | channel(color[2]);
                            changed = true;
                        }
                    }
                }
                if (changed) {
                    auto next = t.topology;
                    for (auto &def : next["components"])
                        if (def["id"] == n.local_id)
                            def["config"] = cfg;
                    t.send(1, next);
                }
                ImGui::EndPopup();
            }
            ImGui::EndDisabled();
        }
        const std::string renderer = n.descriptor.value("renderer", "");
        Json state = t.snapshot.value("runtime", Json::object()).value(n.id, Json::object());
        if (auto it = renderers.find(renderer);
            it != renderers.end() && n.descriptor.value("schema", 1) == 1) {
            const Json config = n.definition.value("config", Json::object());
            Render r{draw, pos, content_scale, state, config, [&](uint32_t value) {
                         if (renderer == "button") {
                             if (value) {
                                 t.held = n.id;
                                 t.held_owner = t.owner;
                                 t.held_generation = t.generation;
                                 t.held_revision = t.revision;
                             } else
                                 t.held.clear();
                         }
                         t.send(0, {{"id", n.id}, {"value", value}});
                     }};
            it->second(r);
        }
        for (const auto &p : n.descriptor["pins"]) {
            std::string name_pin = p["name"], key = n.id + "." + name_pin;
            const bool out = p["direction"] != "input";
            const bool right_side = out != n.flipped;
            const auto point = points.at(key);
            const float width = std::max(42.f, ImGui::CalcTextSize(name_pin.c_str()).x + 10.f);
            ImVec2 button{point.x - (right_side ? width : 0), point.y - 10};
            ImGui::SetCursorScreenPos(button);
            ImGui::PushID(name_pin.c_str());
            ImGui::InvisibleButton("pin", {width, 21});
            if (ImGui::IsItemClicked()) {
                t.selected = n.id;
                if (node_editable)
                    t.connect(key, n.provider_index);
            }
            if (ImGui::IsItemClicked(1) && node_editable)
                t.disconnect(n.local_id + "." + name_pin);
            const int level = t.snapshot["pins"].value(n.local_id + "." + name_pin, 2);
            draw->AddRectFilled(button, {button.x + width, button.y + 21},
                                level == 3 ? IM_COL32(160, 50, 55, 255) : IM_COL32(58, 77, 119, 255));
            draw->AddText({button.x + 4, button.y + 1}, IM_COL32_WHITE, name_pin.c_str());
            if (ImGui::IsItemHovered()) {
                const char *levels[] = {"Low", "High", "Disconnected", "Conflict"};
                ImGui::SetTooltip("%s — %s\nClick this pin, then another, to connect them\nRight-click to disconnect", key.c_str(),
                                  levels[std::clamp(level, 0, 3)]);
            }
            ImGui::PopID();
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    draw->ChannelsSetCurrent(0);
    for (size_t provider_index = 0; provider_index < t.providers.size(); ++provider_index) {
        const auto source = t.provider_snapshot(provider_index);
        const std::string prefix = "gpio-" + std::to_string(t.providers[provider_index].at("owner").get<uint64_t>()) + "/";
        for (const auto &net : source["topology"]["nets"]) {
            const auto &ps = net["pins"];
            if (ps.empty())
                continue;
            const std::string first = prefix + ps[0].get<std::string>();
            if (!points.contains(first))
                continue;
            const auto a = points.at(first);
            for (size_t i = 1; i < ps.size(); ++i) {
                const std::string pin = ps[i], qualified_pin = prefix + pin;
                if (!points.contains(qualified_pin))
                    continue;
                const auto b = points.at(qualified_pin);
                const int level = source["pins"].value(pin, 2);
                const ImU32 color = level == 3   ? IM_COL32(255, 80, 75, 255)
                                   : level == 1 ? IM_COL32(80, 200, 185, 255)
                                                : IM_COL32(45, 95, 110, 255);
                draw->AddBezierCubic(a, {a.x + 70 * t.zoom, a.y}, {b.x - 70 * t.zoom, b.y}, b, color, 2 * t.zoom);
            }
        }
    }
    if (!t.wire.empty() && points.contains(t.wire))
        draw->AddLine(points[t.wire], ImGui::GetIO().MousePos, IM_COL32(230, 230, 70, 255), 2);
    draw->ChannelsSetCurrent(1);
    // Register the canvas after its widgets: exposed body/background pixels
    // capture the mouse, while pin labels and controls keep their own gestures.
    // This also prevents Dear ImGui's background-window drag from taking over.
    ImGui::SetCursorScreenPos(origin);
    ImGui::InvisibleButton("canvas_input", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    if (ImGui::IsItemActivated()) {
        t.panning = ImGui::IsMouseClicked(ImGuiMouseButton_Middle) ||
                    (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hits.empty());
        if (t.panning) {
            t.dragging_part.clear();
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            t.click_selection = t.selected;
            t.dragging_part = hits.empty() ? std::string() : hits.front();
            t.drag_moved = false;
            if (!t.dragging_part.empty())
                t.selected = t.dragging_part;
        }
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0) && !t.dragging_part.empty()) {
        auto &position = t.positions()[t.dragging_part];
        position[0] = position[0].get<float>() + ImGui::GetIO().MouseDelta.x / t.zoom;
        position[1] = position[1].get<float>() + ImGui::GetIO().MouseDelta.y / t.zoom;
        t.drag_moved = true;
    }
    if (ImGui::IsItemDeactivated()) {
        if (!t.drag_moved && !t.dragging_part.empty())
            t.selected = pick_part(hits, t.click_selection);
        t.dragging_part.clear();
        t.panning = false;
    }
    if (!t.selected.empty()) {
        t.order.erase(std::remove(t.order.begin(), t.order.end(), t.selected), t.order.end());
        t.order.push_back(t.selected);
    }
    draw->ChannelsMerge();
    ImGui::EndChild();
    t.activate_provider(edit_provider);
}
SrhStatus SRH_CALL create(const SrhToolHostV1 *host, void **out) {
    return sdk::guard([&]() -> SrhStatus {
        if (!host || !out || host->abi_version != SRH_ABI ||
            !sdk::has_field(host, &SrhToolHostV1::provider_command) || !host->provider_command ||
            !host->provider_data || !host->imgui_version ||
            std::strcmp(host->imgui_version, IMGUI_VERSION) || !host->imgui_context || !host->imgui_alloc ||
            !host->imgui_free || !host->run_state)
            return SRH_INVALID;
        ImGui::SetAllocatorFunctions(host->imgui_alloc, host->imgui_free, host->imgui_allocator_context);
        ImGui::SetCurrentContext(static_cast<ImGuiContext *>(host->imgui_context));
        auto t = std::make_unique<Tool>();
        t->host = host;
        *out = t.release();
        return SRH_OK;
    });
}
void SRH_CALL destroy(void *p) {
    auto *t = static_cast<Tool *>(p);
    t->release();
    delete t;
}
SrhStatus SRH_CALL draw(void *p, uint32_t *open) {
    return sdk::guard([&]() -> SrhStatus {
        if (!p || !open)
            return SRH_INVALID;
        auto &t = *static_cast<Tool *>(p);
        ImGui::SetCurrentContext(static_cast<ImGuiContext *>(t.host->imgui_context));
        t.refresh();
        if (!ImGui::IsMouseDown(0))
            t.release();
        bool visible = *open != 0;
        ImGui::SetNextWindowSize({1050, 720}, ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Patchbay", &visible)) {
            if (!t.owner)
                ImGui::TextDisabled("No patchbay card");
            else {
                if (ImGui::Button("Add component"))
                    ImGui::OpenPopup("palette");
                if (ImGui::BeginPopup("palette")) {
                    for (size_t provider_index = 0; provider_index < t.providers.size(); ++provider_index) {
                        t.activate_provider(provider_index);
                        const auto owner = t.owner;
                        ImGui::BeginDisabled(!t.can_edit());
                        for (const auto &d : t.snapshot["catalogue"]) {
                            if (d.value("schema", 0) != 1)
                                continue;
                            const std::string label = "GPIO #" + std::to_string(owner) + " / " +
                                                      d.at("name").get<std::string>();
                            if (ImGui::MenuItem((label + "##" + std::to_string(provider_index)).c_str())) {
                                auto next = t.topology;
                                const auto id = t.new_id("c");
                                next["components"].push_back({{"id", id},
                                                              {"type", d["type"]},
                                                              {"schema", d["schema"]},
                                                              {"config", Json::object()}});
                                const std::string qualified_id = "gpio-" + std::to_string(owner) + "/" + id;
                                t.positions()[qualified_id] = {80.f - t.pan.x / t.zoom, 80.f - t.pan.y / t.zoom};
                                t.send(1, next);
                                t.selected = qualified_id;
                            }
                        }
                        ImGui::EndDisabled();
                    }
                    ImGui::EndPopup();
                }
                ImGui::SameLine();
                ImGui::TextDisabled("Pan: middle drag  ·  Zoom: wheel");
                if (!t.error.empty())
                    ImGui::TextColored({1, .4f, .35f, 1}, "%s", t.error.c_str());
                canvas(t);
            }
        } else
            t.release();
        ImGui::End();
        if (!visible)
            t.release();
        *open = visible ? 1u : 0u;
        return SRH_OK;
    });
}
SrhStatus SRH_CALL state_get(void *p, char *buffer, uint64_t *size) {
    return sdk::guard([&]() -> SrhStatus {
        if (!p || !size)
            return SRH_INVALID;
        auto &t = *static_cast<Tool *>(p);
        t.layout["zoom"] = t.zoom;
        t.layout["pan"] = {t.pan.x, t.pan.y};
        const auto text = t.layout.dump();
        const uint64_t capacity = *size;
        *size = text.size() + 1;
        if (!buffer)
            return SRH_OK;
        if (capacity < *size)
            return SRH_INVALID;
        std::memcpy(buffer, text.c_str(), static_cast<size_t>(*size));
        return SRH_OK;
    });
}
SrhStatus SRH_CALL state_load(void *p, const char *text) {
    return sdk::guard([&]() -> SrhStatus {
        if (!p || !text)
            return SRH_INVALID;
        auto &t = *static_cast<Tool *>(p);
        t.release();
        if (!*text) {
            t.layout = {{"layouts", Json::object()}, {"flips", Json::object()}};
            t.zoom = 1;
            t.pan = {30, 50};
            return SRH_OK;
        }
        auto layout = Json::parse(text);
        if (!layout.is_object() || !layout.at("layouts").is_object())
            return SRH_INVALID;
        float zoom = layout.value("zoom", 1.f);
        auto pan = layout.value("pan", Json::array({30.f, 50.f}));
        if (!std::isfinite(zoom) || !pan.is_array() || pan.size() != 2)
            return SRH_INVALID;
        for (const auto &[provider, positions] : layout["layouts"].items()) {
            if (!positions.is_object())
                return SRH_INVALID;
            for (const auto &[id, v] : positions.items())
                if (!v.is_array() || v.size() != 2 || !v[0].is_number() || !v[1].is_number() ||
                    !std::isfinite(v[0].get<float>()) || !std::isfinite(v[1].get<float>()))
                    return SRH_INVALID;
        }
        if (layout.contains("flips")) {
            if (!layout["flips"].is_object())
                return SRH_INVALID;
            for (const auto &[provider, provider_flips] : layout["flips"].items()) {
                if (!provider_flips.is_object())
                    return SRH_INVALID;
                for (const auto &[id, flipped] : provider_flips.items())
                    if (!flipped.is_boolean())
                        return SRH_INVALID;
            }
        } else
            layout["flips"] = Json::object();
        if (!std::isfinite(pan[0].get<float>()) || !std::isfinite(pan[1].get<float>()))
            return SRH_INVALID;
        t.layout = layout;
        t.zoom = std::clamp(zoom, .75f, 1.8f);
        t.pan = {pan[0].get<float>(), pan[1].get<float>()};
        return SRH_OK;
    });
}
const SrhToolPlugin api{
    SRH_INIT(SrhToolPlugin),     "patch_bay", "Patchbay", IMGUI_VERSION, create, destroy, draw, "I/O",
    Srh_TOOL_PROJECT_STATE_TEXT, state_get,   state_load, nullptr};
} // namespace
} // namespace srz80::patchbay::ui
extern "C" SRH_EXPORT const SrhToolPlugin *SRH_CALL srz80_tool_init(const SrhToolHostV1 *host) {
    return host && srz80::sdk::has_field(host, &SrhToolHostV1::provider_command) ? &srz80::patchbay::ui::api
                                                                              : nullptr;
}
