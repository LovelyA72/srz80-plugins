#include <boundary.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <imgui.h>
#include <map>
#include <memory>
#include <srz80/imgui_input.hpp>
#include <srz80/tool.h>
#include <string>
#include <vector>

namespace {

struct ToolProperty {
    uint32_t host_index = 0;
    std::string name;
    std::string group;
    std::string description;
    std::string enum_labels;
    uint32_t kind = SRH_UNSIGNED;
    uint32_t bits = 0;
    uint32_t base = 10;
    uint32_t ui_flags = 0;
    bool editable = false;
    SrhValue value{SRH_INIT(SrhValue), 0, 0, {}};
};

struct Tool {
    const SrhToolHostV1 *host = nullptr;
    SrhHandle selected = 0;
    std::vector<SrhToolCard> cards;
    std::vector<ToolProperty> properties;
    std::map<std::string, size_t> property_index;
    SrhHandle property_card = 0;
    uint32_t property_count = 0;
};

ToolProperty property_info_from_host(const SrhToolHostV1 *host, SrhHandle card, uint32_t index) {
    ToolProperty property;
    property.host_index = index;
    SrhToolProperty info{SRH_INIT(SrhToolProperty), {}, {}, {}, {}, 0, 0, 0, 0, 0};
    if (host->card_property_info(host->context, card, index, &info) != SRH_OK)
        return property;
    property.name = info.name;
    property.group = info.group;
    property.description = info.description;
    property.enum_labels = info.enum_labels;
    property.kind = info.kind;
    property.bits = info.bits;
    property.base = info.base;
    property.editable = info.editable != 0;
    property.ui_flags = info.ui_flags;
    return property;
}

void refresh_properties(Tool &tool, SrhHandle card) {
    const uint32_t count = tool.host->card_property_count(tool.host->context, card);
    if (!count) {
        tool.property_card = card;
        tool.property_count = 0;
        tool.properties.clear();
        tool.property_index.clear();
        return;
    }
    if (tool.property_card != card || tool.property_count != count) {
        tool.property_card = card;
        tool.property_count = count;
        tool.properties.clear();
        tool.property_index.clear();
        tool.properties.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            auto property = property_info_from_host(tool.host, card, i);
            if (property.name.empty())
                continue;
            tool.property_index[property.name] = tool.properties.size();
            tool.properties.push_back(std::move(property));
        }
    }
    for (auto &property : tool.properties) {
        SrhValue value{SRH_INIT(SrhValue), 0, 0, {}};
        if (tool.host->card_property_get(tool.host->context, card, property.host_index, &value) == SRH_OK)
            property.value = value;
    }
}

const ToolProperty *find_property(const Tool &tool, const char *name) {
    auto found = tool.property_index.find(name);
    return found == tool.property_index.end() ? nullptr : &tool.properties[found->second];
}

bool set_property(Tool &tool, const char *name, const SrhValue &value) {
    auto found = tool.property_index.find(name);
    if (found == tool.property_index.end() || !tool.properties[found->second].editable)
        return false;
    return tool.host->card_property_set(tool.host->context, tool.selected,
                                        tool.properties[found->second].host_index,
                                        &value) == SRH_OK;
}

void set_register_raw(Tool &tool, uint32_t reg, uint8_t value) {
    char name[16];
    std::snprintf(name, sizeof(name), "R%02X", reg);
    SrhValue v{SRH_INIT(SrhValue), value, 0, {}};
    set_property(tool, name, v);
}

void set_register_bit(Tool &tool, uint32_t reg, uint32_t bit, bool enabled) {
    char name[16];
    std::snprintf(name, sizeof(name), "R%02X", reg);
    const auto *property = find_property(tool, name);
    const uint8_t current = property ? static_cast<uint8_t>(property->value.unsigned_value) : 0;
    uint8_t value = current;
    if (enabled)
        value = static_cast<uint8_t>(value | (1u << bit));
    else
        value = static_cast<uint8_t>(value & ~(1u << bit));
    set_register_raw(tool, reg, value);
}

void set_channel_bit(Tool &tool, uint32_t channel, uint32_t bit, bool enabled) {
    set_register_bit(tool, 0x20 + channel, bit, enabled);
}

void set_register_field(Tool &tool, uint32_t reg, uint32_t value, uint32_t shift,
                        uint32_t mask) {
    char name[16];
    std::snprintf(name, sizeof(name), "R%02X", reg);
    const auto *property = find_property(tool, name);
    const uint8_t current = property ? static_cast<uint8_t>(property->value.unsigned_value) : 0;
    const uint8_t next = static_cast<uint8_t>((current & ~mask) | ((value << shift) & mask));
    set_register_raw(tool, reg, next);
}

void set_channel_fnum(Tool &tool, uint32_t channel, uint32_t fnum) {
    fnum &= 0x1FF;
    set_register_raw(tool, 0x10 + channel, static_cast<uint8_t>(fnum & 0xFF));
    set_register_field(tool, 0x20 + channel, fnum >> 8, 0, 0x01);
}

void set_channel_block(Tool &tool, uint32_t channel, uint32_t block) {
    block &= 0x7;
    set_register_field(tool, 0x20 + channel, block, 1, 0x0E);
}

void set_channel_patch(Tool &tool, uint32_t channel, uint32_t patch) {
    patch &= 0x0F;
    set_register_field(tool, 0x30 + channel, patch, 4, 0xF0);
}

void set_channel_volume(Tool &tool, uint32_t channel, uint32_t volume) {
    volume &= 0x0F;
    set_register_field(tool, 0x30 + channel, volume, 0, 0x0F);
}

bool get_register_bit(const Tool &tool, uint32_t reg, uint32_t bit) {
    char name[16];
    std::snprintf(name, sizeof(name), "R%02X", reg);
    const auto *property = find_property(tool, name);
    return property && ((property->value.unsigned_value >> bit) & 1u);
}

uint8_t get_register_field(const Tool &tool, uint32_t reg, uint32_t shift, uint32_t mask) {
    char name[16];
    std::snprintf(name, sizeof(name), "R%02X", reg);
    const auto *property = find_property(tool, name);
    if (!property)
        return 0;
    return static_cast<uint8_t>((property->value.unsigned_value & mask) >> shift);
}

void draw_interface(Tool &tool) {
    if (!ImGui::TreeNodeEx("Interface", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth))
        return;

    const auto *address_property = find_property(tool, "Address");
    const auto *data_property = find_property(tool, "Data");

    uint32_t address = address_property ? static_cast<uint32_t>(address_property->value.unsigned_value) : 0;
    uint32_t data = data_property ? static_cast<uint32_t>(data_property->value.unsigned_value) : 0;

    if (srz80::gui::input_hexadecimal("Address", address, 8)) {
        SrhValue v{SRH_INIT(SrhValue), address, 0, {}};
        set_property(tool, "Address", v);
    }
    ImGui::SameLine();
    if (srz80::gui::input_hexadecimal("Data", data, 8)) {
        SrhValue v{SRH_INIT(SrhValue), data, 0, {}};
        set_property(tool, "Data", v);
    }

    ImGui::TreePop();
}

void draw_raw_bytes(Tool &tool) {
    if (!ImGui::TreeNodeEx("Raw bytes", ImGuiTreeNodeFlags_SpanAvailWidth))
        return;
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(4, 2));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3, 1));
    if (ImGui::BeginTable("ym2413 raw", 17, ImGuiTableFlags_SizingFixedFit |
                                             ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 42.0f);
        for (uint32_t column = 0; column < 16; ++column) {
            char header[8];
            std::snprintf(header, sizeof(header), "%X", column);
            ImGui::TableSetupColumn(header, ImGuiTableColumnFlags_WidthFixed, 48.0f);
        }
        ImGui::TableHeadersRow();
        for (uint32_t row = 0; row < 4; ++row) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            char row_label[8];
            std::snprintf(row_label, sizeof(row_label), "%Xx", row);
            ImGui::TextUnformatted(row_label);
            for (uint32_t column = 0; column < 16; ++column) {
                ImGui::TableNextColumn();
                const uint32_t reg = row * 16 + column;
                char name[16];
                std::snprintf(name, sizeof(name), "R%02X", reg);
                const auto *property = find_property(tool, name);
                if (!property)
                    continue;
                uint32_t value = static_cast<uint32_t>(property->value.unsigned_value);
                ImGui::SetNextItemWidth(-1.0f);
                if (srz80::gui::input_hexadecimal(property->name.c_str(), value, 8)) {
                    SrhValue v{SRH_INIT(SrhValue), value & 0xFF, 0, {}};
                    set_property(tool, name, v);
                }
            }
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar(2);
    ImGui::TreePop();
}

void draw_control(Tool &tool) {
    if (!ImGui::TreeNodeEx("Control", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth))
        return;

    if (ImGui::BeginTable("ym2413 control", 2, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        bool rhythm = get_register_bit(tool, 0x0E, 5);
        if (ImGui::Checkbox("Rhythm Enable", &rhythm))
            set_register_bit(tool, 0x0E, 5, rhythm);

        ImGui::TableNextColumn();
        const auto *test_property = find_property(tool, "R0F");
        uint32_t test = test_property ? static_cast<uint32_t>(test_property->value.unsigned_value) : 0;
        if (srz80::gui::input_hexadecimal("Test", test, 8)) {
            set_register_raw(tool, 0x0F, static_cast<uint8_t>(test & 0xFF));
        }
        ImGui::EndTable();
    }
    ImGui::TreePop();
}

void draw_custom_instrument(Tool &tool) {
    if (!ImGui::TreeNodeEx("Instrument 0 (custom)", ImGuiTreeNodeFlags_SpanAvailWidth))
        return;

    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(4, 2));
    if (ImGui::BeginTable("ym2413 instrument", 14, ImGuiTableFlags_BordersInnerH |
                                                        ImGuiTableFlags_BordersInnerV |
                                                        ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableSetupColumn("Op", ImGuiTableColumnFlags_WidthFixed, 44.0f);
        const char *headers[] = {"AM", "PM", "EG", "KR", "ML", "KL", "TL", "WS", "FB",
                                 "AR", "DR", "SL", "RR"};
        for (const char *header : headers)
            ImGui::TableSetupColumn(header, ImGuiTableColumnFlags_WidthFixed, 52.0f);
        ImGui::TableHeadersRow();

        for (uint32_t op = 0; op < 2; ++op) {
            ImGui::PushID(op);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(op == 0 ? "Mod" : "Car");

            const uint32_t patch_reg = 0x00 + op;
            bool am = get_register_bit(tool, patch_reg, 7);
            bool pm = get_register_bit(tool, patch_reg, 6);
            bool eg = get_register_bit(tool, patch_reg, 5);
            bool kr = get_register_bit(tool, patch_reg, 4);
            int ml = get_register_field(tool, patch_reg, 0, 0x0F);
            int kl = get_register_field(tool, 0x02 + op, 6, 0xC0);
            int tl = get_register_field(tool, 0x02 + op, 0, 0x3F);
            bool ws = get_register_bit(tool, 0x03, op == 0 ? 3 : 4);
            int fb = op == 0 ? get_register_field(tool, 0x03, 0, 0x07) : 0;
            int ar = get_register_field(tool, 0x04 + op, 4, 0xF0);
            int dr = get_register_field(tool, 0x04 + op, 0, 0x0F);
            int sl = get_register_field(tool, 0x06 + op, 4, 0xF0);
            int rr = get_register_field(tool, 0x06 + op, 0, 0x0F);

            ImGui::TableNextColumn();
            if (ImGui::Checkbox("##am", &am))
                set_register_bit(tool, patch_reg, 7, am);
            ImGui::TableNextColumn();
            if (ImGui::Checkbox("##pm", &pm))
                set_register_bit(tool, patch_reg, 6, pm);
            ImGui::TableNextColumn();
            if (ImGui::Checkbox("##eg", &eg))
                set_register_bit(tool, patch_reg, 5, eg);
            ImGui::TableNextColumn();
            if (ImGui::Checkbox("##kr", &kr))
                set_register_bit(tool, patch_reg, 4, kr);

            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::DragInt("##ml", &ml, 1, 0, 15))
                set_register_field(tool, patch_reg, static_cast<uint32_t>(ml), 0, 0x0F);

            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::DragInt("##kl", &kl, 1, 0, 3))
                set_register_field(tool, 0x02 + op, static_cast<uint32_t>(kl), 6, 0xC0);

            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::DragInt("##tl", &tl, 1, 0, 63))
                set_register_field(tool, 0x02 + op, static_cast<uint32_t>(tl), 0, 0x3F);

            ImGui::TableNextColumn();
            if (ImGui::Checkbox("##ws", &ws))
                set_register_bit(tool, 0x03, op == 0 ? 3 : 4, ws);

            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1.0f);
            if (op == 0) {
                if (ImGui::DragInt("##fb", &fb, 1, 0, 7))
                    set_register_field(tool, 0x03, static_cast<uint32_t>(fb), 0, 0x07);
            } else {
                ImGui::TextUnformatted("-");
            }

            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::DragInt("##ar", &ar, 1, 0, 15))
                set_register_field(tool, 0x04 + op, static_cast<uint32_t>(ar), 4, 0xF0);

            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::DragInt("##dr", &dr, 1, 0, 15))
                set_register_field(tool, 0x04 + op, static_cast<uint32_t>(dr), 0, 0x0F);

            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::DragInt("##sl", &sl, 1, 0, 15))
                set_register_field(tool, 0x06 + op, static_cast<uint32_t>(sl), 4, 0xF0);

            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::DragInt("##rr", &rr, 1, 0, 15))
                set_register_field(tool, 0x06 + op, static_cast<uint32_t>(rr), 0, 0x0F);

            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
    ImGui::TreePop();
}

void draw_channels(Tool &tool) {
    if (!ImGui::TreeNodeEx("Channels", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth))
        return;

    const bool rhythm = get_register_bit(tool, 0x0E, 5);
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(4, 2));
    if (ImGui::BeginTable("ym2413 channels", 10, ImGuiTableFlags_BordersInnerH |
                                                    ImGuiTableFlags_BordersInnerV |
                                                    ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableSetupColumn("Ch", ImGuiTableColumnFlags_WidthFixed, 32.0f);
        ImGui::TableSetupColumn("F.Num", ImGuiTableColumnFlags_WidthFixed, 56.0f);
        ImGui::TableSetupColumn("Block", ImGuiTableColumnFlags_WidthFixed, 48.0f);
        ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, 40.0f);
        ImGui::TableSetupColumn("Sus", ImGuiTableColumnFlags_WidthFixed, 40.0f);
        ImGui::TableSetupColumn("Patch", ImGuiTableColumnFlags_WidthFixed, 48.0f);
        ImGui::TableSetupColumn("Vol", ImGuiTableColumnFlags_WidthFixed, 40.0f);
        ImGui::TableSetupColumn("M Env", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("C Env", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Out", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableHeadersRow();

        for (uint32_t channel = 0; channel < 9; ++channel) {
            ImGui::PushID(channel);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();

            const bool is_rhythm = rhythm && channel >= 6;
            if (is_rhythm) {
                const char *label = channel == 6 ? "6 BD"
                                  : channel == 7 ? "7 HH/SD"
                                                 : "8 TOM/CYM";
                ImGui::TextUnformatted(label);
            } else {
                ImGui::Text("%u", channel);
            }

            char name[32];
            std::snprintf(name, sizeof(name), "R%02X", 0x10 + channel);
            const auto *fnum_low = find_property(tool, name);
            std::snprintf(name, sizeof(name), "R%02X", 0x20 + channel);
            const auto *reg20 = find_property(tool, name);
            std::snprintf(name, sizeof(name), "R%02X", 0x30 + channel);
            const auto *reg30 = find_property(tool, name);
            if (!fnum_low || !reg20 || !reg30) {
                ImGui::PopID();
                continue;
            }

            const uint8_t f_lo = static_cast<uint8_t>(fnum_low->value.unsigned_value);
            const uint8_t r20 = static_cast<uint8_t>(reg20->value.unsigned_value);
            const uint8_t r30 = static_cast<uint8_t>(reg30->value.unsigned_value);
            uint32_t fnum = f_lo | ((r20 & 1u) << 8);
            int block = (r20 >> 1) & 7u;
            bool key = (r20 & 0x10u) != 0;
            bool sus = (r20 & 0x20u) != 0;
            int patch = (r30 >> 4) & 0x0Fu;
            int volume = r30 & 0x0Fu;

            if (is_rhythm) {
                const bool has_second = channel != 6;
                bool first = channel == 6  ? get_register_bit(tool, 0x0E, 4)
                          : channel == 7  ? get_register_bit(tool, 0x0E, 0)
                                          : get_register_bit(tool, 0x0E, 2);
                bool second = has_second && get_register_bit(tool, 0x0E, channel == 7 ? 3 : 1);
                const uint32_t first_reg = channel == 6 ? 0x36 : channel == 7 ? 0x37 : 0x38;
                int first_vol = channel == 6 ? get_register_field(tool, 0x36, 0, 0x0F)
                            : channel == 7 ? get_register_field(tool, 0x37, 4, 0xF0)
                                           : get_register_field(tool, 0x38, 4, 0xF0);
                int second_vol = has_second ? get_register_field(tool, first_reg, 0, 0x0F) : 0;

                ImGui::TableNextColumn();
                ImGui::TextDisabled("-");
                ImGui::TableNextColumn();
                ImGui::TextDisabled("-");

                ImGui::TableNextColumn();
                if (ImGui::Checkbox("##r1", &first)) {
                    const uint32_t bit = channel == 6 ? 4u : channel == 7 ? 0u : 2u;
                    set_register_bit(tool, 0x0E, bit, first);
                }
                ImGui::TableNextColumn();
                if (has_second) {
                    if (ImGui::Checkbox("##r2", &second)) {
                        const uint32_t bit = channel == 7 ? 3u : 1u;
                        set_register_bit(tool, 0x0E, bit, second);
                    }
                }

                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::DragInt("##v1", &first_vol, 1, 0, 15)) {
                    const uint32_t shift = channel == 6 ? 0u : 4u;
                    const uint32_t mask = channel == 6 ? 0x0Fu : 0xF0u;
                    set_register_field(tool, first_reg, static_cast<uint32_t>(first_vol), shift, mask);
                }

                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1.0f);
                if (has_second) {
                    if (ImGui::DragInt("##v2", &second_vol, 1, 0, 15)) {
                        set_register_field(tool, first_reg, static_cast<uint32_t>(second_vol), 0, 0x0F);
                    }
                } else {
                    ImGui::TextDisabled("-");
                }
            } else {
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1.0f);
                if (srz80::gui::input_hexadecimal("##fnum", fnum, 12)) {
                    set_channel_fnum(tool, channel, fnum);
                }

                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::DragInt("##block", &block, 1, 0, 7)) {
                    set_channel_block(tool, channel, static_cast<uint32_t>(block));
                }

                ImGui::TableNextColumn();
                if (ImGui::Checkbox("##key", &key)) {
                    set_channel_bit(tool, channel, 4, key);
                }

                ImGui::TableNextColumn();
                if (ImGui::Checkbox("##sus", &sus)) {
                    set_channel_bit(tool, channel, 5, sus);
                }

                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::DragInt("##patch", &patch, 1, 0, 15)) {
                    set_channel_patch(tool, channel, static_cast<uint32_t>(patch));
                }

                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::DragInt("##volume", &volume, 1, 0, 15)) {
                    set_channel_volume(tool, channel, static_cast<uint32_t>(volume));
                }
            }

            const uint32_t mod_slot = channel * 2;
            const uint32_t car_slot = channel * 2 + 1;
            char slot_name[32];
            float mod_env = 0.f, car_env = 0.f, out = 0.f;
            std::snprintf(slot_name, sizeof(slot_name), "S%02u.Env", mod_slot);
            if (const auto *property = find_property(tool, slot_name))
                mod_env = static_cast<float>(property->value.unsigned_value) / 255.f;
            std::snprintf(slot_name, sizeof(slot_name), "S%02u.Env", car_slot);
            if (const auto *property = find_property(tool, slot_name))
                car_env = static_cast<float>(property->value.unsigned_value) / 255.f;
            std::snprintf(slot_name, sizeof(slot_name), "S%02u.Out", car_slot);
            if (const auto *property = find_property(tool, slot_name))
                out = std::clamp(std::abs(property->value.signed_value) / 32768.0f, 0.0f, 1.0f);

            ImGui::TableNextColumn();
            ImGui::ProgressBar(mod_env, ImVec2(0, 0));
            ImGui::TableNextColumn();
            ImGui::ProgressBar(car_env, ImVec2(0, 0));
            ImGui::TableNextColumn();
            ImGui::ProgressBar(out, ImVec2(0, 0));
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();

    ImGui::TreePop();
}

SrhStatus SRH_CALL create(const SrhToolHostV1 *host, void **result) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!srz80::sdk::valid(host) || !result || !host->imgui_version ||
            std::strcmp(host->imgui_version, IMGUI_VERSION) != 0 || !host->imgui_context ||
            !host->imgui_alloc || !host->imgui_free || !host->space_count || !host->space_info ||
            !host->run_state || !host->run || !srz80::sdk::has_field(host, &SrhToolHostV1::card_count) ||
            !host->card_count || !srz80::sdk::has_field(host, &SrhToolHostV1::card_info) ||
            !host->card_info || !srz80::sdk::has_field(host, &SrhToolHostV1::card_property_count) ||
            !host->card_property_count || !srz80::sdk::has_field(host, &SrhToolHostV1::card_property_info) ||
            !host->card_property_info || !srz80::sdk::has_field(host, &SrhToolHostV1::card_property_get) ||
            !host->card_property_get || !srz80::sdk::has_field(host, &SrhToolHostV1::card_property_set) ||
            !host->card_property_set)
            return SRH_INVALID;
        ImGui::SetAllocatorFunctions(host->imgui_alloc, host->imgui_free,
                                     host->imgui_allocator_context);
        ImGui::SetCurrentContext(static_cast<ImGuiContext *>(host->imgui_context));
        auto tool = std::make_unique<Tool>();
        tool->host = host;
        *result = tool.release();
        return SRH_OK;
    });
}

void SRH_CALL destroy(void *instance) {
    delete static_cast<Tool *>(instance);
}

SrhStatus SRH_CALL draw(void *instance, uint32_t *open) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!instance || !open)
            return SRH_INVALID;
        auto &tool = *static_cast<Tool *>(instance);
        ImGui::SetCurrentContext(static_cast<ImGuiContext *>(tool.host->imgui_context));

        bool visible = *open != 0;
        if (ImGui::Begin("YM2413 Inspector", &visible)) {
            tool.cards.clear();
            const uint32_t card_count = tool.host->card_count(tool.host->context);
            for (uint32_t i = 0; i < card_count; ++i) {
                SrhToolCard card{SRH_INIT(SrhToolCard), 0, 0, {}, {}};
                if (tool.host->card_info(tool.host->context, i, &card) == SRH_OK &&
                    std::strcmp(card.type, "ym2413") == 0)
                    tool.cards.push_back(card);
            }

            if (tool.cards.empty()) {
                ImGui::TextDisabled("No YM2413 cards");
                ImGui::End();
                *open = visible ? 1u : 0u;
                return SRH_OK;
            }

            auto selected_it =
                std::find_if(tool.cards.begin(), tool.cards.end(),
                             [&](const auto &card) { return card.id == tool.selected; });
            if (selected_it == tool.cards.end()) {
                tool.selected = tool.cards.front().id;
                selected_it = tool.cards.begin();
            }

            const char *selected_name = selected_it->name;
            if (ImGui::BeginCombo("YM2413", selected_name)) {
                for (const auto &card : tool.cards) {
                    if (ImGui::Selectable(card.name, card.id == tool.selected))
                        tool.selected = card.id;
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();

            refresh_properties(tool, tool.selected);
            draw_interface(tool);
            draw_raw_bytes(tool);
            draw_control(tool);
            draw_custom_instrument(tool);
            draw_channels(tool);
        }
        ImGui::End();
        *open = visible ? 1u : 0u;
        return SRH_OK;
    });
}

const SrhToolPlugin api{SRH_INIT(SrhToolPlugin), "ym2413_inspector", "YM2413 Inspector",
                        IMGUI_VERSION, create, destroy, draw, "Audio", 0, nullptr, nullptr, nullptr};

} // namespace

extern "C" SRH_EXPORT const SrhToolPlugin *SRH_CALL srz80_tool_init(const SrhToolHostV1 *host) {
    return host && srz80::sdk::has_field(host, &SrhToolHostV1::card_count) ? &api : nullptr;
}
