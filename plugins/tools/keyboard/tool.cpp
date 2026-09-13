#include <boundary.hpp>
#include <srz80/tool.h>
#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using Json = nlohmann::json;

// HID Usage Tables, Keyboard/Keypad page.  The wire protocol is deliberately
// independent of SDL and ImGui: every event is {usage, flags}, where bit 0 is
// down and bit 1 is an explicit host repeat.
struct Key { ImGuiKey imgui; uint8_t usage; const char *name; };
constexpr Key keys[] = {
    {ImGuiKey_A, 0x04, "A"}, {ImGuiKey_B, 0x05, "B"}, {ImGuiKey_C, 0x06, "C"}, {ImGuiKey_D, 0x07, "D"},
    {ImGuiKey_E, 0x08, "E"}, {ImGuiKey_F, 0x09, "F"}, {ImGuiKey_G, 0x0a, "G"}, {ImGuiKey_H, 0x0b, "H"},
    {ImGuiKey_I, 0x0c, "I"}, {ImGuiKey_J, 0x0d, "J"}, {ImGuiKey_K, 0x0e, "K"}, {ImGuiKey_L, 0x0f, "L"},
    {ImGuiKey_M, 0x10, "M"}, {ImGuiKey_N, 0x11, "N"}, {ImGuiKey_O, 0x12, "O"}, {ImGuiKey_P, 0x13, "P"},
    {ImGuiKey_Q, 0x14, "Q"}, {ImGuiKey_R, 0x15, "R"}, {ImGuiKey_S, 0x16, "S"}, {ImGuiKey_T, 0x17, "T"},
    {ImGuiKey_U, 0x18, "U"}, {ImGuiKey_V, 0x19, "V"}, {ImGuiKey_W, 0x1a, "W"}, {ImGuiKey_X, 0x1b, "X"},
    {ImGuiKey_Y, 0x1c, "Y"}, {ImGuiKey_Z, 0x1d, "Z"},
    {ImGuiKey_1, 0x1e, "1"}, {ImGuiKey_2, 0x1f, "2"}, {ImGuiKey_3, 0x20, "3"}, {ImGuiKey_4, 0x21, "4"},
    {ImGuiKey_5, 0x22, "5"}, {ImGuiKey_6, 0x23, "6"}, {ImGuiKey_7, 0x24, "7"}, {ImGuiKey_8, 0x25, "8"},
    {ImGuiKey_9, 0x26, "9"}, {ImGuiKey_0, 0x27, "0"}, {ImGuiKey_Enter, 0x28, "Enter"},
    {ImGuiKey_Escape, 0x29, "Esc"}, {ImGuiKey_Backspace, 0x2a, "Backspace"}, {ImGuiKey_Tab, 0x2b, "Tab"},
    {ImGuiKey_Space, 0x2c, "Space"}, {ImGuiKey_Minus, 0x2d, "-"}, {ImGuiKey_Equal, 0x2e, "="},
    {ImGuiKey_LeftBracket, 0x2f, "["}, {ImGuiKey_RightBracket, 0x30, "]"}, {ImGuiKey_Backslash, 0x31, "\\"},
    {ImGuiKey_Semicolon, 0x33, ";"}, {ImGuiKey_Apostrophe, 0x34, "'"}, {ImGuiKey_GraveAccent, 0x35, "`"},
    {ImGuiKey_Comma, 0x36, ","}, {ImGuiKey_Period, 0x37, "."}, {ImGuiKey_Slash, 0x38, "/"},
    {ImGuiKey_CapsLock, 0x39, "CapsLock"}, {ImGuiKey_F1, 0x3a, "F1"}, {ImGuiKey_F2, 0x3b, "F2"},
    {ImGuiKey_F3, 0x3c, "F3"}, {ImGuiKey_F4, 0x3d, "F4"}, {ImGuiKey_F5, 0x3e, "F5"}, {ImGuiKey_F6, 0x3f, "F6"},
    {ImGuiKey_F7, 0x40, "F7"}, {ImGuiKey_F8, 0x41, "F8"}, {ImGuiKey_F9, 0x42, "F9"}, {ImGuiKey_F10, 0x43, "F10"},
    {ImGuiKey_F11, 0x44, "F11"}, {ImGuiKey_F12, 0x45, "F12"}, {ImGuiKey_PrintScreen, 0x46, "PrintScreen"},
    {ImGuiKey_ScrollLock, 0x47, "ScrollLock"}, {ImGuiKey_Pause, 0x48, "Pause"}, {ImGuiKey_Insert, 0x49, "Insert"},
    {ImGuiKey_Home, 0x4a, "Home"}, {ImGuiKey_PageUp, 0x4b, "PageUp"}, {ImGuiKey_Delete, 0x4c, "Delete"},
    {ImGuiKey_End, 0x4d, "End"}, {ImGuiKey_PageDown, 0x4e, "PageDown"}, {ImGuiKey_RightArrow, 0x4f, "Right"},
    {ImGuiKey_LeftArrow, 0x50, "Left"}, {ImGuiKey_DownArrow, 0x51, "Down"}, {ImGuiKey_UpArrow, 0x52, "Up"},
    {ImGuiKey_LeftCtrl, 0xe0, "Left Ctrl"}, {ImGuiKey_LeftShift, 0xe1, "Left Shift"}, {ImGuiKey_LeftAlt, 0xe2, "Left Alt"},
    {ImGuiKey_LeftSuper, 0xe3, "Left Super"}, {ImGuiKey_RightCtrl, 0xe4, "Right Ctrl"}, {ImGuiKey_RightShift, 0xe5, "Right Shift"},
    {ImGuiKey_RightAlt, 0xe6, "Right Alt"}, {ImGuiKey_RightSuper, 0xe7, "Right Super"},
};

struct Provider { SrhHandle owner{}; std::string name, endpoint; Json data; };
struct Request { SrhHandle handle{}; };
struct Packet { uint8_t usage, flags; };

enum class TypeState { stopped, running, paused, complete };

// Convert the text-file character set to the HID keys the keyboard card
// understands.  Text files are deliberately kept to printable US-ASCII plus
// tab/newline: silently guessing a keyboard layout would make pasted programs
// differ between hosts.
bool character_packets(char character, std::vector<Packet> &packets) {
    uint8_t usage = 0;
    bool shift = false;
    if (character >= 'a' && character <= 'z') usage = uint8_t(0x04 + character - 'a');
    else if (character >= 'A' && character <= 'Z') { usage = uint8_t(0x04 + character - 'A'); shift = true; }
    else if (character >= '1' && character <= '9') usage = uint8_t(0x1e + character - '1');
    else if (character == '0') usage = 0x27;
    else {
        switch (character) {
        case '\n': usage = 0x28; break;
        case '\t': usage = 0x2b; break;
        case ' ': usage = 0x2c; break;
        case '-': usage = 0x2d; break; case '_': usage = 0x2d; shift = true; break;
        case '=': usage = 0x2e; break; case '+': usage = 0x2e; shift = true; break;
        case '[': usage = 0x2f; break; case '{': usage = 0x2f; shift = true; break;
        case ']': usage = 0x30; break; case '}': usage = 0x30; shift = true; break;
        case '\\': usage = 0x31; break; case '|': usage = 0x31; shift = true; break;
        case ';': usage = 0x33; break; case ':': usage = 0x33; shift = true; break;
        case '\'': usage = 0x34; break; case '"': usage = 0x34; shift = true; break;
        case '`': usage = 0x35; break; case '~': usage = 0x35; shift = true; break;
        case ',': usage = 0x36; break; case '<': usage = 0x36; shift = true; break;
        case '.': usage = 0x37; break; case '>': usage = 0x37; shift = true; break;
        case '/': usage = 0x38; break; case '?': usage = 0x38; shift = true; break;
        case '!': usage = 0x1e; shift = true; break; case '@': usage = 0x1f; shift = true; break;
        case '#': usage = 0x20; shift = true; break; case '$': usage = 0x21; shift = true; break;
        case '%': usage = 0x22; shift = true; break; case '^': usage = 0x23; shift = true; break;
        case '&': usage = 0x24; shift = true; break; case '*': usage = 0x25; shift = true; break;
        case '(': usage = 0x26; shift = true; break; case ')': usage = 0x27; shift = true; break;
        default: return false;
        }
    }
    packets.clear();
    if (shift) packets.push_back({0xe1, 1});
    packets.push_back({usage, 1});
    packets.push_back({usage, 0});
    if (shift) packets.push_back({0xe1, 0});
    return true;
}

std::string basename(std::string_view path) {
    const auto separator = path.find_last_of("/\\\\");
    return std::string(path.substr(separator == std::string_view::npos ? 0 : separator + 1));
}

struct Tool {
    const SrhToolHostV1 *host{};
    SrhToolRuntime runtime{SRH_INIT(SrhToolRuntime), 0, 0, 1, 0};
    std::vector<Provider> providers;
    std::array<bool, 256> held{};
    std::vector<Request> requests;
    SrhHandle selected{};
    uint64_t identity = 1;
    std::string endpoint, endpoint_preference, bundle, error;
    bool preference_settled = false, focused_last_frame = false;
    SrhHandle dialog{};
    std::string source_name, source_text, type_status;
    size_t type_position{};
    uint64_t next_type_time{};
    float characters_per_second = 12.0f;
    TypeState type_state = TypeState::stopped;

    explicit Tool(const SrhToolHostV1 *source) : host(source) {}
    void select(SrhHandle owner, bool explicit_choice) {
        release_all(); selected = owner; endpoint.clear();
        for (const auto &provider : providers) if (provider.owner == owner) endpoint = provider.endpoint;
        if (explicit_choice) { endpoint_preference = endpoint; preference_settled = true; }
    }
    void choose_default() {
        if (std::any_of(providers.begin(), providers.end(), [&](const auto &p) { return p.owner == selected; })) return;
        auto preferred = std::find_if(providers.begin(), providers.end(), [&](const auto &p) { return p.endpoint == endpoint_preference; });
        if (preferred != providers.end()) select(preferred->owner, false);
        else if (!preference_settled && !providers.empty()) select(providers.front().owner, false);
        else if (selected) select(0, false);
    }
    bool submit(uint8_t usage, uint8_t flags) {
        if (!selected || runtime.stopped) return false;
        const uint8_t packet[]{usage, flags}; SrhHandle request{};
        const auto status = host->input_submit(host->context, this, runtime.generation, identity, selected,
                                               endpoint.c_str(), UINT64_MAX, packet, sizeof(packet), &request);
        if (status != SRH_OK) { error = "Keyboard input was not accepted (" + std::to_string(status) + ")"; return false; }
        requests.push_back({request}); return true;
    }
    void set_key(const Key &key, bool down, bool repeat = false) {
        if (held[key.usage] == down && !repeat) return;
        held[key.usage] = down;
        if (!submit(key.usage, uint8_t((down ? 1 : 0) | (repeat ? 2 : 0))) && !down) held[key.usage] = false;
    }
    void release_all() {
        for (const auto &key : keys) if (held[key.usage]) set_key(key, false);
    }
    void poll_requests() {
        for (auto it = requests.begin(); it != requests.end();) {
            SrhToolInputResult result{SRH_INIT(SrhToolInputResult), 1, SRH_UNAVAILABLE};
            if (host->input_poll(host->context, this, it->handle, &result) != SRH_OK || result.pending) { ++it; continue; }
            if (result.status != SRH_OK) error = "Keyboard event rejected by the simulation worker.";
            host->input_release(host->context, this, it->handle); it = requests.erase(it);
        }
    }
    bool has_file_dialog() const {
        return srz80::sdk::has_field(host, &SrhToolHostV1::file_dialog_request) && host->file_dialog_request &&
               srz80::sdk::has_field(host, &SrhToolHostV1::file_dialog_poll) && host->file_dialog_poll &&
               srz80::sdk::has_field(host, &SrhToolHostV1::file_dialog_release) && host->file_dialog_release;
    }
    void stop_typing() { type_state = TypeState::stopped; type_position = 0; next_type_time = 0; }
    void open_text_file(const std::string &path) {
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("Cannot open " + path);
        std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        if (text.size() > 1024 * 1024) throw std::invalid_argument("Text files are limited to 1 MiB");
        std::string normalized;
        normalized.reserve(text.size());
        for (size_t index = 0; index < text.size(); ++index) {
            const char character = text[index];
            if (character == '\r') {
                if (index + 1 < text.size() && text[index + 1] == '\n') ++index;
                normalized.push_back('\n');
            } else normalized.push_back(character);
        }
        std::vector<Packet> packets;
        for (size_t index = 0; index < normalized.size(); ++index)
            if (!character_packets(normalized[index], packets))
                throw std::invalid_argument("Unsupported character at byte " + std::to_string(index + 1) +
                                            "; text must be US-ASCII");
        source_text = std::move(normalized); source_name = basename(path); stop_typing();
        type_status = "Loaded " + source_name + " (" + std::to_string(source_text.size()) + " characters)";
    }
    void poll_file() {
        if (!dialog) return;
        SrhToolFileResult result{SRH_INIT(SrhToolFileResult), 0, 0, 0};
        auto status = host->file_dialog_poll(host->context, dialog, &result, nullptr, 0);
        if (status == SRH_OK && result.pending) return;
        std::string path;
        if (status == SRH_OK && !result.cancelled && result.required_size) {
            std::vector<char> buffer(static_cast<size_t>(result.required_size));
            status = host->file_dialog_poll(host->context, dialog, &result, buffer.data(), buffer.size());
            if (status == SRH_OK) path = buffer.data();
        }
        host->file_dialog_release(host->context, dialog); dialog = 0;
        if (status != SRH_OK) { type_status = "File dialog failed"; return; }
        if (path.empty()) return;
        try { open_text_file(path); } catch (const std::exception &exception) { type_status = exception.what(); }
    }
    void request_text_file() {
        if (!has_file_dialog()) { type_status = "This host does not provide file dialogs"; return; }
        static const SrhToolFileFilter filters[] = {{"Text files", "txt;bas;asm;csv"}, {"All files", "*"}};
        const auto status = host->file_dialog_request(host->context, 0, filters, 2, &dialog);
        if (status != SRH_OK) type_status = "Cannot open file dialog (status " + std::to_string(status) + ")";
    }
    void type_next_character() {
        if (type_position >= source_text.size()) { type_state = TypeState::complete; type_status = "Typing completed"; return; }
        std::vector<Packet> packets;
        if (!character_packets(source_text[type_position], packets)) { stop_typing(); type_status = "Text contains an unsupported character"; return; }
        for (const auto &packet : packets) if (!submit(packet.usage, packet.flags)) { type_state = TypeState::paused; type_status = "Typing paused: " + error; return; }
        ++type_position;
    }
    void advance_typing() {
        if (type_state != TypeState::running || !runtime.running || !selected) return;
        if (!next_type_time) next_type_time = runtime.time_ns;
        if (runtime.time_ns < next_type_time) return;
        type_next_character();
        const uint64_t interval = static_cast<uint64_t>(1'000'000'000.0 / std::max(characters_per_second, 0.1f));
        next_type_time = runtime.time_ns + interval;
    }
    void refresh_providers() {
        uint64_t size{};
        if (host->provider_data(host->context, nullptr, &size) != SRH_OK || !size || size > 16 * 1024 * 1024) return;
        std::string text(size, '\0');
        if (host->provider_data(host->context, text.data(), &size) != SRH_OK) return;
        text.resize(size - 1);
        if (text == bundle) return;
        auto root = Json::parse(text, nullptr, false);
        if (root.is_discarded() || root.value("generation", uint64_t{}) != runtime.generation) return;
        providers.clear();
        for (const auto &provider : root["providers"]) {
            if (provider.value("protocol", std::string()) != "srz80.keyboard.v1") continue;
            auto data = Json::parse(provider.value("data", std::string()), nullptr, false);
            if (data.is_discarded() || data.value("schema", 0) != 1) continue;
            providers.push_back({provider.value("owner", SrhHandle{}), provider.value("display_name", std::string("Keyboard")), data.value("endpoint", std::string()), std::move(data)});
        }
        bundle = std::move(text); choose_default();
    }
    void tick(uint32_t visible) {
        poll_requests();
        poll_file();
        SrhToolRuntime old = runtime;
        if (host->runtime_info(host->context, &runtime) != SRH_OK) return;
        if (runtime.generation != old.generation || runtime.stopped) { release_all(); selected = 0; endpoint.clear(); providers.clear(); bundle.clear(); stop_typing(); }
        if (!visible) release_all();
        refresh_providers();
        advance_typing();
    }
    void scan_keyboard(bool focused) {
        if (!focused) { if (focused_last_frame) release_all(); focused_last_frame = false; return; }
        for (const auto &key : keys) {
            if (ImGui::IsKeyPressed(key.imgui, false)) set_key(key, true);
            if (ImGui::IsKeyReleased(key.imgui)) set_key(key, false);
        }
        focused_last_frame = true;
    }
    void draw(uint32_t *open) {
        bool visible = *open != 0;
        // This is a keyboard sink, not an ImGui keyboard-navigation surface:
        // Tab must reach the virtual card instead of focusing the card combo.
        if (!ImGui::Begin("Keyboard", &visible, ImGuiWindowFlags_NoNavInputs)) { ImGui::End(); *open = visible; release_all(); return; }
        if (ImGui::BeginCombo("Card", endpoint.empty() ? "Select keyboard card" : endpoint.c_str())) {
            for (const auto &provider : providers) {
                const auto label = provider.name + " — " + provider.endpoint;
                if (ImGui::Selectable(label.c_str(), provider.owner == selected)) select(provider.owner, true);
            }
            ImGui::EndCombo();
        }
        ImGui::SeparatorText("Type from file");
        ImGui::BeginDisabled(dialog != 0 || !has_file_dialog());
        if (ImGui::Button("Open text file...")) request_text_file();
        ImGui::EndDisabled();
        if (!source_name.empty()) ImGui::SameLine(), ImGui::TextDisabled("%s", source_name.c_str());
        ImGui::SetNextItemWidth(180.0f);
        ImGui::SliderFloat("Characters/sec", &characters_per_second, 0.5f, 120.0f, "%.1f");
        if (runtime.running) ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
        ImGui::BeginDisabled(source_text.empty() || !selected || runtime.stopped || type_state == TypeState::running);
        if (ImGui::Button("Start")) { if (type_state == TypeState::complete) type_position = 0; type_state = TypeState::running; next_type_time = runtime.time_ns; type_status.clear(); }
        ImGui::EndDisabled(); ImGui::SameLine();
        ImGui::BeginDisabled(type_state != TypeState::running);
        if (ImGui::Button("Pause")) type_state = TypeState::paused;
        ImGui::EndDisabled(); ImGui::SameLine();
        ImGui::BeginDisabled(type_state == TypeState::stopped || type_state == TypeState::complete);
        if (ImGui::Button("Stop")) stop_typing();
        ImGui::EndDisabled();
        if (runtime.running) ImGui::PopItemFlag();
        if (!source_text.empty()) ImGui::Text("Progress: %llu / %llu", static_cast<unsigned long long>(type_position), static_cast<unsigned long long>(source_text.size()));
        if (!type_status.empty()) ImGui::TextWrapped("%s", type_status.c_str());
        const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && selected && !runtime.stopped;
        scan_keyboard(focused);
        ImGui::TextColored(focused ? ImVec4(0.45f, 0.72f, 0.52f, 1) : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled),
                           focused ? "Capture active" : "Click to capture keyboard");
        std::string held_text;
        for (const auto &key : keys) if (held[key.usage]) { if (!held_text.empty()) held_text += " + "; held_text += key.name; }
        ImGui::Text("Held: %s", held_text.empty() ? "none" : held_text.c_str());
        if (auto it = std::find_if(providers.begin(), providers.end(), [&](const auto &p) { return p.owner == selected; }); it != providers.end()) {
            const auto &data = it->data;
            ImGui::Text("Card: %llu held; FIFO %llu / %llu; IRQ %s", static_cast<unsigned long long>(data.value("held_count", 0ull)), static_cast<unsigned long long>(data.value("rx_fill", 0ull)), static_cast<unsigned long long>(data.value("rx_capacity", 0ull)), data.value("irq_pending", false) ? "asserted" : "idle");
            if (data.value("overflow", false)) ImGui::TextUnformatted("Card event FIFO overflowed; clear it through STATUS/CONTROL.");
        }
        if (!error.empty()) ImGui::TextWrapped("%s", error.c_str());
        if (runtime.stopped) ImGui::TextDisabled("Rack stopped");
        ImGui::End(); *open = visible;
    }
};

SrhStatus SRH_CALL create(const SrhToolHostV1 *host, void **out) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!srz80::sdk::valid(host) || !out || !host->input_submit || !host->input_poll ||
            !host->input_release || !host->runtime_info || !host->provider_data)
            return SRH_INVALID;
        ImGui::SetCurrentContext(static_cast<ImGuiContext *>(host->imgui_context));
        ImGui::SetAllocatorFunctions(host->imgui_alloc, host->imgui_free,
                                     host->imgui_allocator_context);
        *out = new Tool(host);
        return SRH_OK;
    });
}

void SRH_CALL destroy(void *instance) {
    auto *tool = static_cast<Tool *>(instance);
    if (!tool)
        return;
    tool->release_all();
    if (tool->dialog)
        tool->host->file_dialog_release(tool->host->context, tool->dialog);
    for (const auto &request : tool->requests)
        tool->host->input_release(tool->host->context, tool, request.handle);
    delete tool;
}

SrhStatus SRH_CALL draw(void *instance, uint32_t *open) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!instance || !open)
            return SRH_INVALID;
        static_cast<Tool *>(instance)->draw(open);
        return SRH_OK;
    });
}

SrhStatus SRH_CALL tick(void *instance, uint32_t visible) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!instance)
            return SRH_INVALID;
        static_cast<Tool *>(instance)->tick(visible);
        return SRH_OK;
    });
}

SrhStatus SRH_CALL state_get(void *instance, char *out, uint64_t *size) {
    if (!instance || !size)
        return SRH_INVALID;
    const auto text =
        Json{{"schema", 1}, {"endpoint", static_cast<Tool *>(instance)->endpoint}}.dump();
    const auto capacity = *size;
    *size = text.size() + 1;
    if (!out)
        return SRH_OK;
    if (capacity < *size)
        return SRH_UNAVAILABLE;
    std::memcpy(out, text.c_str(), *size);
    return SRH_OK;
}

SrhStatus SRH_CALL state_load(void *instance, const char *text) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!instance)
            return SRH_INVALID;
        auto &tool = *static_cast<Tool *>(instance);
        tool.release_all();
        tool.endpoint_preference.clear();
        tool.preference_settled = false;
        if (text && *text) {
            auto state = Json::parse(text, nullptr, false);
            if (!state.is_discarded() && state.value("schema", 0) == 1) {
                tool.endpoint_preference = state.value("endpoint", std::string());
                tool.preference_settled = true;
            }
        }
        return tool.host->runtime_info(tool.host->context, &tool.runtime);
    });
}

const SrhToolPlugin api{SRH_INIT(SrhToolPlugin),
                        "keyboard",
                        "Keyboard",
                        IMGUI_VERSION,
                        create,
                        destroy,
                        draw,
                        "I/O",
                        Srh_TOOL_CLAIM_FILE_SHORTCUTS | Srh_TOOL_PROJECT_STATE_TEXT,
                        state_get,
                        state_load,
                        nullptr,
                        tick};
} // namespace

extern "C" SRH_EXPORT const SrhToolPlugin *SRH_CALL srz80_tool_init(const SrhToolHostV1 *host) {
    return host && host->abi_version == SRH_ABI ? &api : nullptr;
}
