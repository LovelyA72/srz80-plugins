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

constexpr uint32_t voice_count = 32;
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

int voice_note(uint32_t pitch) {
    const int exponent = static_cast<int>(pitch >> 12) - ((pitch & 0x8000) ? 16 : 0);
    const double ratio = std::ldexp(static_cast<double>(pitch & 0xfff) / 2048.0, exponent);
    return ratio > 0 ? static_cast<int>(std::lround(60.0 + 12.0 * std::log2(ratio))) : -1;
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

void draw_voices(const Tool &tool) {
    const ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_BordersInnerV |
        ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("swp00 voices", 17, flags, ImVec2(0, 0))) return;
    ImGui::TableSetupScrollFreeze(2, 1);
    const char *labels[] = {"Ch", "Pan L/R", "Keyboard", "Format", "Pitch", "Global", "LFO R/P/A",
        "AR", "AL", "DR", "DL", "Dry/Rev/Cho/Var", "Address", "Pre-loop / Loop", "Envelope", "Volume", "Note"};
    const float widths[] = {34, 60, 426, 52, 48, 48, 82, 28, 28, 28, 28, 120, 65, 112, 106, 104, 42};
    for (int i = 0; i < 17; ++i) ImGui::TableSetupColumn(labels[i], ImGuiTableColumnFlags_WidthFixed, widths[i]);
    ImGui::TableHeadersRow();
    for (uint32_t voice = 0; voice < voice_count; ++voice) {
        ImGui::PushID(static_cast<int>(voice));
        auto v = [&](const char *name) { return static_cast<uint32_t>(value_of(tool, voice_property(voice, name))); };
        const bool active = v("Playing") != 0;
        const int note = voice_note(v("Pitch"));
        ImGui::TableNextRow(); ImGui::TableNextColumn();
        if (active) ImGui::TextColored(ImVec4(1, .38f, .34f, 1), "%02u", voice + 1);
        else ImGui::TextDisabled("%02u", voice + 1);
        ImGui::TableNextColumn(); ImGui::Text("%X/%X", v("Pan") >> 4, v("Pan") & 15);
        ImGui::TableNextColumn(); draw_piano(note, active);
        ImGui::TableNextColumn();
        const char *formats[] = {"16-bit", "12-bit", "8-bit", "DPCM"};
        ImGui::TextUnformatted(formats[(v("Format") >> 6) & 3]);
        ImGui::TableNextColumn(); ImGui::Text("%04X", v("Pitch"));
        ImGui::TableNextColumn(); ImGui::Text("%02X", v("Global"));
        ImGui::TableNextColumn(); ImGui::Text("%02X/%02X/%02X", v("LFORate"), v("LFOPitch"), v("LFOAmplitude"));
        for (const char *name : {"Attack", "AttackLevel", "Decay", "DecayLevel"}) {
            ImGui::TableNextColumn(); ImGui::Text("%02X", v(name));
        }
        ImGui::TableNextColumn(); ImGui::Text("%02X/%02X/%02X/%02X", v("Dry"), v("Reverb"), v("Chorus"), v("Variation"));
        ImGui::TableNextColumn(); ImGui::Text("%06X", v("Address"));
        ImGui::TableNextColumn(); ImGui::Text("%04X / %04X", v("Start"), v("Loop"));
        ImGui::TableNextColumn();
        const char *stage = !active ? "Off" : v("EnvelopeStage") == 0 ? "Attack" : v("EnvelopeStage") == 2 ? "Decay" : "Hold";
        const auto attenuation = v("Envelope");
        const float amplitude = active && attenuation < 0xfff
            ? std::ldexp(1.0f - static_cast<float>(attenuation & 255) / 512.0f,
                         -static_cast<int>(attenuation >> 8)) : 0.0f;
        ImGui::ProgressBar(amplitude, ImVec2(-1, 0), stage);
        ImGui::TableNextColumn();
        /* The core's per-voice peak is measured before the final 5-bit mixer
         * headroom shift.  Scale that native value to the full meter range. */
        ImGui::ProgressBar(std::min(1.0f, static_cast<float>(v("Output")) / 1024.0f), ImVec2(-1, 0), "");
        ImGui::TableNextColumn();
        if (active) ImGui::TextColored(ImVec4(1, .38f, .34f, 1), "%s", note_name(note).c_str());
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
            !srz80::sdk::has_field(host, &SrhToolHostV1::card_property_get) || !host->card_count ||
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
        if (ImGui::Begin("SWP00 Inspector", &visible)) {
            tool.cards.clear();
            const uint32_t count = tool.host->card_count(tool.host->context);
            for (uint32_t index = 0; index < count; ++index) {
                SrhToolCard card{SRH_INIT(SrhToolCard), 0, 0, {}, {}};
                if (tool.host->card_info(tool.host->context, index, &card) == SRH_OK &&
                    std::strcmp(card.type, "swp00") == 0)
                    tool.cards.push_back(card);
            }
            if (tool.cards.empty()) {
                ImGui::TextDisabled("No SWP00 cards");
            } else {
                auto selected = std::find_if(tool.cards.begin(), tool.cards.end(),
                    [&](const auto &card) { return card.id == tool.selected; });
                if (selected == tool.cards.end()) {
                    tool.selected = tool.cards.front().id;
                    selected = tool.cards.begin();
                }
                ImGui::SetNextItemWidth(260.0f);
                if (ImGui::BeginCombo("SWP00", selected->name)) {
                    for (const auto &card : tool.cards)
                        if (ImGui::Selectable(card.name, card.id == tool.selected)) {
                            tool.selected = card.id;
                            tool.property_card = 0;
                        }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                ImGui::TextDisabled("32 voices | Pitch relative to C4 | Pan/send values are attenuation");
                if (refresh_properties(tool)) {
                    ImGui::TextDisabled("MEG program %llu | %.1f Hz | R/P/A = LFO rate / pitch / amplitude",
                        static_cast<unsigned long long>((value_of(tool, "MEGControl") >> 6) & 3),
                        static_cast<double>(value_of(tool, "chip_clock_hz")) / 768.0);
                    draw_voices(tool);
                }
            }
        }
        ImGui::End();
        *open = visible ? 1u : 0u;
        return SRH_OK;
    });
}

const SrhToolPlugin api{SRH_INIT(SrhToolPlugin), "swp00_inspector", "SWP00 Inspector",
                        IMGUI_VERSION, create, destroy, draw, "Audio", 0, nullptr, nullptr, nullptr, nullptr};

} // namespace

extern "C" SRH_EXPORT const SrhToolPlugin *SRH_CALL srz80_tool_init(const SrhToolHostV1 *host) {
    return host && srz80::sdk::has_field(host, &SrhToolHostV1::card_count) ? &api : nullptr;
}
