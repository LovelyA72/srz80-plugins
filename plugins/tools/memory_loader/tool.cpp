#include <boundary.hpp>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include "format.hpp"
#include <fstream>
#include <imgui.h>
#include <iterator>
#include <memory>
#include <srz80/imgui_input.hpp>
#include <srz80/tool.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Tool {
    const SrhToolHostV1 *host;
    SrhHandle space = 0;
    uint64_t address = 0;
    bool reset_before_load = false;
    std::vector<char> bytes{'0', '0', '\0'};
    enum class InputKind { text, binary, intel_hex } input_kind = InputKind::text;
    std::vector<uint8_t> binary;
    std::vector<srz80::memory_loader::Segment> hex_segments;
    SrhHandle dialog = 0;
    std::string status;
    std::string file_name;
};

void set_byte_text(Tool &tool, const std::vector<uint8_t> &bytes) {
    const auto text = srz80::memory_loader::format_hex(bytes);
    tool.bytes.assign(text.begin(), text.end());
    tool.bytes.push_back('\0');
}

void clear_file_source(Tool &tool) {
    tool.input_kind = Tool::InputKind::text;
    tool.binary.clear();
    tool.hex_segments.clear();
    tool.file_name.clear();
}

int hex_text_callback(ImGuiInputTextCallbackData *data) {
    auto &tool = *static_cast<Tool *>(data->UserData);
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        if (data->Buf != tool.bytes.data())
            return 0;
        tool.bytes.resize(static_cast<size_t>(data->BufTextLen) + 1);
        data->Buf = tool.bytes.data();
    } else if (data->EventFlag == ImGuiInputTextFlags_CallbackEdit) {
        clear_file_source(tool);
    }
    return 0;
}

bool has_file_dialog(const Tool &tool) {
    return srz80::sdk::has_field(tool.host, &SrhToolHostV1::file_dialog_request) &&
           tool.host->file_dialog_request &&
           srz80::sdk::has_field(tool.host, &SrhToolHostV1::file_dialog_poll) &&
           tool.host->file_dialog_poll &&
           srz80::sdk::has_field(tool.host, &SrhToolHostV1::file_dialog_release) &&
           tool.host->file_dialog_release;
}

bool has_segment_loader(const Tool &tool) {
    return srz80::sdk::has_field(tool.host, &SrhToolHostV1::load_memory_segments) &&
           tool.host->load_memory_segments;
}

std::string basename(std::string_view path) {
    const auto separator = path.find_last_of("/\\");
    return std::string(path.substr(separator == std::string_view::npos ? 0 : separator + 1));
}

bool looks_like_hex_file(std::string_view path, std::string_view content) {
    const auto separator = path.find_last_of("/\\");
    const auto filename = path.substr(separator == std::string_view::npos ? 0 : separator + 1);
    const auto dot = filename.find_last_of('.');
    if (dot != std::string_view::npos) {
        std::string extension(filename.substr(dot));
        for (char &character : extension)
            character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        if (extension == ".bin")
            return false;
        if (extension == ".hex" || extension == ".ihx" || extension == ".ihex")
            return true;
    }
    const auto first = content.find_first_not_of(" \t\r\n");
    return first != std::string_view::npos && content[first] == ':';
}

void open_file(Tool &tool, const std::string &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("Cannot open " + path);
    const std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (content.empty())
        throw std::invalid_argument("Selected file is empty");

    if (looks_like_hex_file(path, content)) {
        auto segments = srz80::memory_loader::parse_intel_hex(content);
        tool.hex_segments = std::move(segments);
        tool.binary.clear();
        tool.input_kind = Tool::InputKind::intel_hex;
        std::vector<uint8_t> preview;
        for (const auto &segment : tool.hex_segments)
            preview.insert(preview.end(), segment.bytes.begin(), segment.bytes.end());
        tool.file_name = basename(path);
        set_byte_text(tool, preview);
        tool.address = tool.hex_segments.front().address;
        tool.status = "Opened " + tool.file_name + " (Intel HEX, " +
                      std::to_string(preview.size()) + " bytes)";
    } else {
        tool.binary.assign(content.begin(), content.end());
        if (tool.binary.empty())
            throw std::invalid_argument("Selected file is empty");
        tool.hex_segments.clear();
        tool.input_kind = Tool::InputKind::binary;
        tool.file_name = basename(path);
        set_byte_text(tool, tool.binary);
        tool.status = "Opened " + tool.file_name + " (binary, " +
                      std::to_string(tool.binary.size()) + " bytes)";
    }
}

void poll_file(Tool &tool) {
    if (!tool.dialog)
        return;
    SrhToolFileResult result{SRH_INIT(SrhToolFileResult), 0, 0, 0};
    auto status = tool.host->file_dialog_poll(tool.host->context, tool.dialog, &result, nullptr, 0);
    if (status == SRH_OK && result.pending)
        return;
    std::string path;
    if (status == SRH_OK && !result.cancelled && result.required_size) {
        std::vector<char> buffer(static_cast<size_t>(result.required_size));
        status = tool.host->file_dialog_poll(tool.host->context, tool.dialog, &result,
                                              buffer.data(), buffer.size());
        if (status == SRH_OK)
            path = buffer.data();
    }
    tool.host->file_dialog_release(tool.host->context, tool.dialog);
    tool.dialog = 0;
    if (status != SRH_OK || path.empty()) {
        if (status != SRH_OK)
            tool.status = "File dialog failed";
        return;
    }
    try {
        open_file(tool, path);
    } catch (const std::exception &error) {
        tool.status = error.what();
    }
}

void request_file(Tool &tool) {
    if (!has_file_dialog(tool)) {
        tool.status = "This host does not provide file dialogs";
        return;
    }
    static const SrhToolFileFilter filters[] = {{"Binary files", "bin"},
                                                {"Intel HEX files", "hex;ihx;ihex"},
                                                {"All files", "*"}};
    const auto status = tool.host->file_dialog_request(tool.host->context, 0, filters, 3,
                                                       &tool.dialog);
    if (status != SRH_OK)
        tool.status = "Cannot open file dialog (status " + std::to_string(status) + ")";
}

SrhStatus SRH_CALL create(const SrhToolHostV1 *host, void **result) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if ((!host || host->abi_version != SRH_ABI || host->struct_size < offsetof(SrhToolHostV1, load_memory_segments)) || !result || !host->imgui_version ||
            std::string(host->imgui_version) != IMGUI_VERSION || !host->imgui_context ||
            !host->imgui_alloc || !host->imgui_free || !host->space_count || !host->space_info ||
            !host->load_memory || !host->run_state || !host->run)
            return SRH_INVALID;
        ImGui::SetAllocatorFunctions(host->imgui_alloc, host->imgui_free,
                                     host->imgui_allocator_context);
        ImGui::SetCurrentContext(static_cast<ImGuiContext *>(host->imgui_context));
        auto tool = std::make_unique<Tool>();
        tool->host = host;
        if (srz80::sdk::has_field(host, &SrhToolHostV1::config_get) && host->config_get) {
            char initial[16]{};
            if (host->config_get(host->context, "memory_loader.reset_before_load",
                                 initial, sizeof(initial)) == SRH_OK)
                tool->reset_before_load = std::strcmp(initial, "1") == 0;
        }
        if (srz80::sdk::has_field(host, &SrhToolHostV1::config_register) && host->config_register) {
            SrhConfigEntry entry{SRH_INIT(SrhConfigEntry),
                                 "Tools/Memory loader",
                                 "memory_loader.reset_before_load",
                                 "Cold reset before loading",
                                 "",
                                 Srh_CONFIG_BOOL,
                                 nullptr,
                                 "0",
                                 tool.get(),
                                 0,
                                 [](void *context, char *value, uint32_t capacity) -> SrhStatus {
                                     auto *tool_instance = static_cast<Tool *>(context);
                                     if (!value || !capacity)
                                         return SRH_INVALID;
                                     std::snprintf(value, capacity, "%d",
                                                   tool_instance->reset_before_load ? 1 : 0);
                                     return SRH_OK;
                                 },
                                 [](void *context, const char *value) -> SrhStatus {
                                     auto *tool_instance = static_cast<Tool *>(context);
                                     tool_instance->reset_before_load = std::strcmp(value, "1") == 0;
                                     return SRH_OK;
                                 }};
            host->config_register(host->context, &entry);
        }
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
        if (ImGui::Begin("Memory loader tool", &visible)) {
            poll_file(tool);
            SrhToolSpace current{SRH_INIT(SrhToolSpace), 0, 0, {}};
            SrhToolSpace first{SRH_INIT(SrhToolSpace), 0, 0, {}};
            const char *preview = "No address spaces";
            const auto count = tool.host->space_count(tool.host->context);
            bool have_first = false;
            bool found_current = false;
            for (uint32_t index = 0; index < count; ++index) {
                SrhToolSpace candidate{SRH_INIT(SrhToolSpace), 0, 0, {}};
                if (tool.host->space_info(tool.host->context, index, &candidate) != SRH_OK)
                    continue;
                if (!have_first) {
                    first = candidate;
                    have_first = true;
                }
                if (candidate.id == tool.space) {
                    current = candidate;
                    preview = current.name;
                    found_current = true;
                }
            }
            if (!found_current && have_first) {
                tool.space = first.id;
                current = first;
                preview = current.name;
            }
            if (ImGui::BeginCombo("Address space", preview)) {
                for (uint32_t index = 0; index < count; ++index) {
                    SrhToolSpace candidate{SRH_INIT(SrhToolSpace), 0, 0, {}};
                    if (tool.host->space_info(tool.host->context, index, &candidate) == SRH_OK &&
                        ImGui::Selectable(candidate.name, candidate.id == tool.space))
                        tool.space = candidate.id;
                }
                ImGui::EndCombo();
            }
            srz80::gui::input_hexadecimal("Load address", tool.address, 16);
            ImGui::InputTextMultiline("Bytes (hex)", tool.bytes.data(), tool.bytes.size(), ImVec2(420, 100),
                                      ImGuiInputTextFlags_CharsHexadecimal |
                                          ImGuiInputTextFlags_CharsUppercase |
                                          ImGuiInputTextFlags_CallbackResize |
                                          ImGuiInputTextFlags_CallbackEdit,
                                      hex_text_callback, &tool);
            if (!tool.file_name.empty())
                ImGui::TextDisabled("Source: %s", tool.file_name.c_str());
            if (ImGui::Checkbox("Cold reset before loading", &tool.reset_before_load) &&
                srz80::sdk::has_field(tool.host, &SrhToolHostV1::config_set) && tool.host->config_set)
                tool.host->config_set(tool.host->context, "memory_loader.reset_before_load",
                                      tool.reset_before_load ? "1" : "0");
            const bool stopped = tool.host->run_state(tool.host->context) == SRT_STOPPED;
            ImGui::BeginDisabled(!stopped || !tool.space);
            if (ImGui::Button("Load")) {
                try {
                    std::vector<srz80::memory_loader::Segment> source;
                    if (tool.input_kind == Tool::InputKind::intel_hex) {
                        source = tool.hex_segments;
                    } else {
                        auto bytes = tool.input_kind == Tool::InputKind::binary
                                          ? tool.binary
                                          : srz80::memory_loader::parse_byte_text(tool.bytes.data());
                        source.push_back({tool.address, std::move(bytes)});
                    }
                    if (source.empty())
                        throw std::invalid_argument("Enter at least one byte");
                    std::vector<SrhToolMemorySegment> segments;
                    segments.reserve(source.size());
                    for (const auto &segment : source)
                        segments.push_back({SRH_INIT(SrhToolMemorySegment), segment.address,
                                            segment.bytes.data(), segment.bytes.size()});
                    uint64_t written = 0;
                    SrhStatus result = SRH_INVALID;
                    if (segments.size() == 1 || !has_segment_loader(tool)) {
                        if (segments.size() != 1)
                            throw std::runtime_error("Host cannot load sparse Intel HEX files");
                        const auto &segment = segments.front();
                        result = tool.host->load_memory(
                            tool.host->context, tool.space, segment.address, segment.bytes,
                            segment.size, tool.reset_before_load ? 1u : 0u, &written);
                    } else {
                        uint32_t failed = UINT32_MAX;
                        result = tool.host->load_memory_segments(
                            tool.host->context, tool.space, segments.data(), segments.size(),
                            tool.reset_before_load ? 1u : 0u, &failed, &written);
                    }
                    if (result == SRH_OK) {
                        tool.status = "Loaded " + std::to_string(written) + " bytes";
                    } else {
                        tool.status = "Load failed after " + std::to_string(written) +
                                      " bytes (status " + std::to_string(result) + ")";
                    }
                } catch (const std::exception &exception) {
                    tool.status = exception.what();
                }
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(tool.dialog != 0 || !has_file_dialog(tool));
            if (ImGui::Button("Open file..."))
                request_file(tool);
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Run"))
                tool.host->run(tool.host->context);
            if (!stopped)
                ImGui::TextDisabled("Stop the rack to load memory");
            if (!tool.status.empty())
                ImGui::TextWrapped("%s", tool.status.c_str());
        }
        ImGui::End();
        *open = visible ? 1u : 0u;
        return SRH_OK;
    });
}

const SrhToolPlugin api{SRH_INIT(SrhToolPlugin), "memory_loader", "Memory loader", IMGUI_VERSION,
                        create, destroy, draw, "Memory", 0, nullptr, nullptr, nullptr};

} // namespace

extern "C" SRH_EXPORT const SrhToolPlugin *SRH_CALL srz80_tool_init(const SrhToolHostV1 *host) {
    return host && host->abi_version == SRH_ABI && host->struct_size >= offsetof(SrhToolHostV1, load_memory_segments) ? &api : nullptr;
}
