#include <boundary.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <imgui.h>
#include <map>
#include <memory>
#include <srz80/tool.h>
#include <string>
#include <vector>

namespace {

constexpr uint32_t voice_count = 28;
constexpr int first_piano_note = 24;
constexpr int last_piano_note = 119;

struct ToolProperty {
    uint32_t host_index = 0;
    std::string name;
    SrhValue value{SRH_INIT(SrhValue), 0, 0, {}};
};

struct Tool {
    const SrhToolHostV1 *host = nullptr;
    SrhHandle selected = 0;
    SrhHandle property_card = 0;
    uint32_t property_count = 0;
    std::vector<SrhToolCard> cards;
    std::vector<ToolProperty> properties;
    std::map<std::string, size_t> property_index;
};

bool refresh_properties(Tool &tool) {
    const uint32_t count = tool.host->card_property_count(tool.host->context, tool.selected);
    if (!count)
        return false;
    if (tool.property_card != tool.selected || tool.property_count != count) {
        tool.property_card = tool.selected;
        tool.property_count = count;
        tool.properties.clear();
        tool.property_index.clear();
        tool.properties.reserve(count);
        for (uint32_t index = 0; index < count; ++index) {
            SrhToolProperty info{SRH_INIT(SrhToolProperty), {}, {}, {}, {}, 0, 0, 0, 0, 0};
            if (tool.host->card_property_info(tool.host->context, tool.selected, index, &info) != SRH_OK ||
                !info.name[0])
                continue;
            ToolProperty property;
            property.host_index = index;
            property.name = info.name;
            tool.property_index[property.name] = tool.properties.size();
            tool.properties.push_back(std::move(property));
        }
    }
    for (auto &property : tool.properties) {
        SrhValue value{SRH_INIT(SrhValue), 0, 0, {}};
        if (tool.host->card_property_get(tool.host->context, tool.selected, property.host_index, &value) == SRH_OK)
            property.value = value;
    }
    return true;
}

uint64_t value_of(const Tool &tool, const std::string &name) {
    const auto found = tool.property_index.find(name);
    return found == tool.property_index.end() ? 0 : tool.properties[found->second].value.unsigned_value;
}

std::string voice_property(uint32_t voice, const char *suffix) {
    char name[64];
    std::snprintf(name, sizeof(name), "V%02u.%s", voice, suffix);
    return name;
}

uint8_t voice_register(const Tool &tool, uint32_t voice, uint32_t reg) {
    char name[32];
    std::snprintf(name, sizeof(name), "V%02u.R%u", voice, reg);
    return static_cast<uint8_t>(value_of(tool, name));
}

bool black_key(int note) {
    constexpr std::array<bool, 12> black{false, true, false, true, false, false,
                                         true, false, true, false, true, false};
    return black[static_cast<size_t>(note % 12)];
}

std::string note_name(int midi) {
    static const char *names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    if (midi < 0 || midi > 127)
        return "--";
    return std::string(names[midi % 12]) + std::to_string(midi / 12 - 1);
}

int voice_note(uint16_t f_number, int octave) {
    const double ratio = static_cast<double>(f_number + 1024u) / 1024.0;
    const double semitones = static_cast<double>(octave) * 12.0 + 12.0 * std::log2(ratio);
    // Register octave zero is displayed around middle C. The sample's own
    // root pitch is not part of the live register set, so this is the useful
    // keyboard-relative note represented by the chip pitch controls.
    return static_cast<int>(std::lround(60.0 + semitones));
}

void draw_piano(int active_note, bool active) {
    const float height = std::max(14.0f, ImGui::GetFrameHeight() - 2.0f);
    const ImVec2 size(420.0f, height);
    ImGui::InvisibleButton("##piano", size);
    const ImVec2 minimum = ImGui::GetItemRectMin();
    const ImVec2 maximum = ImGui::GetItemRectMax();
    ImDrawList *draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(minimum, maximum, true);

    int white_count = 0;
    for (int note = first_piano_note; note <= last_piano_note; ++note)
        if (!black_key(note))
            ++white_count;
    const float white_width = (maximum.x - minimum.x) / static_cast<float>(white_count);
    const float black_width = std::max(2.0f, white_width * 0.58f);
    const ImU32 white = IM_COL32(224, 226, 230, 255);
    const ImU32 white_active = IM_COL32(245, 82, 82, 255);
    const ImU32 black = IM_COL32(42, 45, 52, 255);
    const ImU32 black_active = IM_COL32(255, 103, 92, 255);
    const ImU32 outline = IM_COL32(70, 73, 82, 255);

    int white_index = 0;
    for (int note = first_piano_note; note <= last_piano_note; ++note) {
        if (black_key(note))
            continue;
        const float x0 = minimum.x + white_width * static_cast<float>(white_index++);
        const float x1 = minimum.x + white_width * static_cast<float>(white_index);
        draw->AddRectFilled(ImVec2(x0, minimum.y), ImVec2(x1, maximum.y),
                            active && note == active_note ? white_active : white);
        draw->AddRect(ImVec2(x0, minimum.y), ImVec2(x1, maximum.y), outline);
    }
    white_index = 0;
    for (int note = first_piano_note; note <= last_piano_note; ++note) {
        if (!black_key(note)) {
            ++white_index;
            continue;
        }
        const float center = minimum.x + white_width * static_cast<float>(white_index);
        draw->AddRectFilled(ImVec2(center - black_width * 0.5f, minimum.y),
                            ImVec2(center + black_width * 0.5f, minimum.y + height * 0.62f),
                            active && note == active_note ? black_active : black);
    }
    draw->PopClipRect();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", active ? note_name(active_note).c_str() : "Idle");
}

const char *pan_name(uint8_t pan) {
    static const char *labels[] = {"C", "R1", "R2", "R3", "R4", "R5", "R6", "R",
                                    "--", "L", "L6", "L5", "L4", "L3", "L2", "L1"};
    return labels[pan & 0x0f];
}

const char *envelope_name(uint32_t stage) {
    static const char *labels[] = {"Off", "Attack", "Decay", "Sustain", "Release"};
    return stage < 5 ? labels[stage] : "?";
}

void draw_voices(const Tool &tool) {
    constexpr int columns = 18;
    const ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_BordersInnerV |
                                  ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("ymw258 voices", columns, flags, ImVec2(0, 0)))
        return;
    ImGui::TableSetupScrollFreeze(2, 1);
    ImGui::TableSetupColumn("Ch", ImGuiTableColumnFlags_WidthFixed, 34.0f);
    ImGui::TableSetupColumn("Pan", ImGuiTableColumnFlags_WidthFixed, 38.0f);
    ImGui::TableSetupColumn("Keyboard", ImGuiTableColumnFlags_WidthFixed, 426.0f);
    ImGui::TableSetupColumn("Smp", ImGuiTableColumnFlags_WidthFixed, 42.0f);
    ImGui::TableSetupColumn("FNum", ImGuiTableColumnFlags_WidthFixed, 48.0f);
    ImGui::TableSetupColumn("Oct", ImGuiTableColumnFlags_WidthFixed, 34.0f);
    ImGui::TableSetupColumn("TL", ImGuiTableColumnFlags_WidthFixed, 34.0f);
    ImGui::TableSetupColumn("LFO R/P/A", ImGuiTableColumnFlags_WidthFixed, 72.0f);
    ImGui::TableSetupColumn("AR", ImGuiTableColumnFlags_WidthFixed, 28.0f);
    ImGui::TableSetupColumn("D1R", ImGuiTableColumnFlags_WidthFixed, 30.0f);
    ImGui::TableSetupColumn("DL", ImGuiTableColumnFlags_WidthFixed, 28.0f);
    ImGui::TableSetupColumn("D2R", ImGuiTableColumnFlags_WidthFixed, 30.0f);
    ImGui::TableSetupColumn("RR", ImGuiTableColumnFlags_WidthFixed, 28.0f);
    ImGui::TableSetupColumn("KRS", ImGuiTableColumnFlags_WidthFixed, 32.0f);
    ImGui::TableSetupColumn("Start / End / Loop", ImGuiTableColumnFlags_WidthFixed, 152.0f);
    ImGui::TableSetupColumn("Envelope", ImGuiTableColumnFlags_WidthFixed, 106.0f);
    ImGui::TableSetupColumn("Volume", ImGuiTableColumnFlags_WidthFixed, 104.0f);
    ImGui::TableSetupColumn("Note", ImGuiTableColumnFlags_WidthFixed, 42.0f);
    ImGui::TableHeadersRow();

    for (uint32_t voice = 0; voice < voice_count; ++voice) {
        ImGui::PushID(static_cast<int>(voice));
        const uint8_t r0 = voice_register(tool, voice, 0);
        const uint8_t r1 = voice_register(tool, voice, 1);
        const uint8_t r2 = voice_register(tool, voice, 2);
        const uint8_t r3 = voice_register(tool, voice, 3);
        const uint8_t r4 = voice_register(tool, voice, 4);
        const uint8_t r6 = voice_register(tool, voice, 6);
        const uint8_t r7 = voice_register(tool, voice, 7);
        const uint16_t sample = static_cast<uint16_t>(r1 | ((r2 & 1u) << 8));
        const uint16_t f_number = static_cast<uint16_t>(((r3 & 0x0fu) << 6) | (r2 >> 2));
        const int octave = static_cast<int>((r3 >> 4) ^ 8u) - 8;
        const bool key_down = (r4 & 0x80u) != 0;
        const uint32_t start = static_cast<uint32_t>(value_of(tool, voice_property(voice, "Start")));
        const uint32_t loop = static_cast<uint32_t>(value_of(tool, voice_property(voice, "Loop")));
        const uint32_t length = static_cast<uint32_t>(value_of(tool, voice_property(voice, "Length")));
        const uint32_t envelope = static_cast<uint32_t>(value_of(tool, voice_property(voice, "Envelope")));
        const uint32_t output = static_cast<uint32_t>(value_of(tool, voice_property(voice, "Output")));
        const int note = voice_note(f_number, octave);

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (key_down) ImGui::TextColored(ImVec4(1.0f, 0.38f, 0.34f, 1.0f), "%02u", voice + 1);
        else ImGui::TextDisabled("%02u", voice + 1);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(pan_name(r0 >> 4));
        ImGui::TableNextColumn(); draw_piano(note, key_down);
        ImGui::TableNextColumn(); ImGui::Text("%03X", sample);
        ImGui::TableNextColumn(); ImGui::Text("%03X", f_number);
        ImGui::TableNextColumn(); ImGui::Text("%+d", octave);
        ImGui::TableNextColumn(); ImGui::Text("%02X", voice_register(tool, voice, 5) >> 1);
        ImGui::TableNextColumn(); ImGui::Text("%u/%u/%u", (r6 >> 3) & 7u, r6 & 7u, r7 & 7u);
        ImGui::TableNextColumn(); ImGui::Text("%X", static_cast<unsigned>(value_of(tool, voice_property(voice, "Attack"))));
        ImGui::TableNextColumn(); ImGui::Text("%X", static_cast<unsigned>(value_of(tool, voice_property(voice, "Decay1"))));
        ImGui::TableNextColumn(); ImGui::Text("%X", static_cast<unsigned>(value_of(tool, voice_property(voice, "DecayLevel"))));
        ImGui::TableNextColumn(); ImGui::Text("%X", static_cast<unsigned>(value_of(tool, voice_property(voice, "Decay2"))));
        ImGui::TableNextColumn(); ImGui::Text("%X", static_cast<unsigned>(value_of(tool, voice_property(voice, "Release"))));
        ImGui::TableNextColumn(); ImGui::Text("%X", static_cast<unsigned>(value_of(tool, voice_property(voice, "RateCorrection"))));
        ImGui::TableNextColumn(); ImGui::Text("%06X / %06X / %06X", start, start + length, start + loop);
        ImGui::TableNextColumn();
        const auto stage = static_cast<uint32_t>(value_of(tool, voice_property(voice, "EnvelopeStage")));
        ImGui::ProgressBar(static_cast<float>(envelope) / 1023.0f, ImVec2(-1.0f, 0), envelope_name(stage));
        ImGui::TableNextColumn();
        const float normalized_output = std::min(
            1.0f, static_cast<float>(output) * static_cast<float>(voice_count-2) / 32767.0f);
        ImGui::ProgressBar(normalized_output, ImVec2(-1.0f, 0), "");
        ImGui::TableNextColumn();
        if (key_down) ImGui::TextColored(ImVec4(1.0f, 0.38f, 0.34f, 1.0f), "%s", note_name(note).c_str());
        else ImGui::TextDisabled("--");
        ImGui::PopID();
    }
    ImGui::EndTable();
}

SrhStatus SRH_CALL create(const SrhToolHostV1 *host, void **result) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!srz80::sdk::valid(host) || !result || !host->imgui_version ||
            std::strcmp(host->imgui_version, IMGUI_VERSION) != 0 || !host->imgui_context ||
            !host->imgui_alloc || !host->imgui_free ||
            !srz80::sdk::has_field(host, &SrhToolHostV1::card_count) || !host->card_count ||
            !host->card_info || !host->card_property_count || !host->card_property_info ||
            !host->card_property_get)
            return SRH_INVALID;
        ImGui::SetAllocatorFunctions(host->imgui_alloc, host->imgui_free, host->imgui_allocator_context);
        ImGui::SetCurrentContext(static_cast<ImGuiContext *>(host->imgui_context));
        auto tool = std::make_unique<Tool>();
        tool->host = host;
        *result = tool.release();
        return SRH_OK;
    });
}

void SRH_CALL destroy(void *instance) { delete static_cast<Tool *>(instance); }

SrhStatus SRH_CALL draw(void *instance, uint32_t *open) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!instance || !open)
            return SRH_INVALID;
        auto &tool = *static_cast<Tool *>(instance);
        ImGui::SetCurrentContext(static_cast<ImGuiContext *>(tool.host->imgui_context));
        bool visible = *open != 0;
        if (ImGui::Begin("YMW258 Inspector", &visible)) {
            tool.cards.clear();
            const uint32_t count = tool.host->card_count(tool.host->context);
            for (uint32_t index = 0; index < count; ++index) {
                SrhToolCard card{SRH_INIT(SrhToolCard), 0, 0, {}, {}};
                if (tool.host->card_info(tool.host->context, index, &card) == SRH_OK &&
                    std::strcmp(card.type, "ymw258") == 0)
                    tool.cards.push_back(card);
            }
            if (tool.cards.empty()) {
                ImGui::TextDisabled("No YMW258 cards");
            } else {
                auto selected = std::find_if(tool.cards.begin(), tool.cards.end(),
                    [&](const auto &card) { return card.id == tool.selected; });
                if (selected == tool.cards.end()) {
                    tool.selected = tool.cards.front().id;
                    selected = tool.cards.begin();
                }
                ImGui::SetNextItemWidth(260.0f);
                if (ImGui::BeginCombo("YMW258", selected->name)) {
                    for (const auto &card : tool.cards)
                        if (ImGui::Selectable(card.name, card.id == tool.selected)) {
                            tool.selected = card.id;
                            tool.property_card = 0;
                        }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                ImGui::TextDisabled("28 voices | R/P/A = LFO rate, pitch depth, amplitude depth");
                if (refresh_properties(tool))
                    draw_voices(tool);
            }
        }
        ImGui::End();
        *open = visible ? 1u : 0u;
        return SRH_OK;
    });
}

const SrhToolPlugin api{SRH_INIT(SrhToolPlugin), "ymw258_inspector", "YMW258 Inspector",
                        IMGUI_VERSION, create, destroy, draw, "Audio", 0, nullptr, nullptr, nullptr};

} // namespace

extern "C" SRH_EXPORT const SrhToolPlugin *SRH_CALL srz80_tool_init(const SrhToolHostV1 *host) {
    return host && srz80::sdk::has_field(host, &SrhToolHostV1::card_count) ? &api : nullptr;
}
