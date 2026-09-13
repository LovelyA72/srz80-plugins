#include "../engine.hpp"
namespace srz80::patchbay {
int hc595_drive(const Component &c, const std::string &name) {
    if (name == "Q7S")
        return (c.shift_unknown & 128) ? Conflict : static_cast<int>((c.shift >> 7) & 1);
    if (name.size() != 2 || name[0] != 'Q')
        return Floating;
    const int enable = c.pins.at("OE");
    if (enable == Conflict)
        return Conflict;
    const bool active = c.definition["config"].value("oe_active_high", false);
    if (enable != Floating && enable != (active ? High : Low))
        return Floating;
    const int bit = name[1] - '0';
    return (c.latch_unknown & (1u << bit)) ? Conflict : static_cast<int>((c.latch >> bit) & 1);
}
void hc595_update(Component &c, uint64_t) {
    const int active = c.active() ? High : Low, inactive = c.active() ? Low : High;
    auto clock_level = [&](int value) { return value == Floating ? inactive : value; };
    const int clock = clock_level(c.pins.at("SRCLK")), latch_clock = clock_level(c.pins.at("RCLK"));
    // Simultaneous storage/shift edges capture the pre-edge shift register.
    if (latch_clock == active && clock_level(c.latch_clock) == inactive) {
        c.latch = c.shift;
        c.latch_unknown = c.shift_unknown;
    } else if (latch_clock == Conflict)
        c.latch_unknown = 255;
    const int reset = c.pins.at("MR");
    if (reset == (c.definition["config"].value("mr_active_high", false) ? High : Low)) {
        c.shift = 0;
        c.shift_unknown = 0;
    } else if (reset == Conflict || clock == Conflict)
        c.shift_unknown = 255;
    else if (clock == active && clock_level(c.clock) == inactive) {
        c.shift = ((c.shift << 1) | (c.pins.at("SER") == High ? 1u : 0u)) & 255;
        c.shift_unknown = ((c.shift_unknown << 1) | (c.pins.at("SER") == Conflict ? 1u : 0u)) & 255;
    }
    c.clock = clock;
    c.latch_clock = latch_clock;
}
} // namespace srz80::patchbay
