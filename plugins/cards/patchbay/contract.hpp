#pragma once
#include <algorithm>
#include <nlohmann/json.hpp>
#include <srz80/abi.h>
#include <string>
#include <vector>
namespace srz80::patchbay {
using Json = nlohmann::json;
inline constexpr const char *protocol_id = "srz80.patchbay.v1";
enum Level { Low, High, Floating, Conflict };
inline Json catalogue() {
    Json out = Json::array();
    auto add = [&](const char *id, const char *name, std::vector<std::string> inputs,
                   std::vector<std::string> outputs, const char *renderer) {
        Json pins = Json::array();
        for (const auto &p : inputs)
            pins.push_back({{"name", p}, {"direction", "input"}});
        for (const auto &p : outputs)
            pins.push_back({{"name", p}, {"direction", "output"}});
        out.push_back({{"type", id},
                       {"schema", 1},
                       {"name", name},
                       {"category", "Logic"},
                       {"pins", pins},
                       {"renderer", renderer},
                       {"properties",
                        {{"active_high", {{"type", "boolean"}, {"default", true}}},
                         {"window_ns",
                          {{"type", "integer"}, {"min", 1000}, {"max", 1000000000}, {"default", 20000000}}}}},
                       {"runtime_fields", {"pins", "brightness", "value"}}});
    };
    add("logic.led", "LED", {"IN"}, {}, "led");
    add("logic.rgb_led", "RGB LED", {"R", "G", "B"}, {}, "rgb");
    add("logic.button", "Push button", {}, {"OUT"}, "button");
    add("logic.switch", "Slide switch", {}, {"OUT"}, "switch");
    add("logic.dip8", "DIP switch 8", {}, {"Q0", "Q1", "Q2", "Q3", "Q4", "Q5", "Q6", "Q7"}, "dip8");
    add("logic.segment7", "7 segment", {"a", "b", "c", "d", "e", "f", "g", "DP"}, {}, "segment7");
    add("logic.segment11", "11 segment", {"a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "DP"}, {},
        "segment11");
    add("logic.74hc595", "74HC595", {"SER", "SRCLK", "RCLK", "OE", "MR"},
        {"Q0", "Q1", "Q2", "Q3", "Q4", "Q5", "Q6", "Q7", "Q7S"}, "595");
    out.back()["properties"]["oe_active_high"] = {{"type", "boolean"}, {"default", false}};
    out.back()["properties"]["mr_active_high"] = {{"type", "boolean"}, {"default", false}};
    for (auto &descriptor : out) {
        const std::string renderer = descriptor["renderer"];
        if (renderer == "button" || renderer == "switch" || renderer == "dip8" || renderer == "595")
            descriptor["properties"].erase("window_ns");
        if (renderer == "led" || renderer == "segment7" || renderer == "segment11")
            descriptor["properties"]["color"] = {{"type", "color"}, {"default", 0xe02020}};
    }
    return out;
}
inline Json empty_topology() {
    return {{"schema", 1}, {"components", Json::array()}, {"nets", Json::array()}};
}
} // namespace srz80::patchbay
