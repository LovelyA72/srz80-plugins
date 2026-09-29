#include "project_settings.hpp"
#include <filesystem>
#include <nlohmann/json.hpp>

namespace srz80::assembler {
SourceSettings ProjectSettings::get(const std::string &key) const {
    const auto found = sources.find(key);
    return found == sources.end() ? SourceSettings{} : found->second;
}
std::string ProjectSettings::source_key(const std::string &path, const std::string &project_root) {
    auto source = std::filesystem::path(std::u8string(path.begin(), path.end())).lexically_normal();
    if (!project_root.empty() && source.is_absolute()) {
        auto relative = source.lexically_relative(
            std::filesystem::path(std::u8string(project_root.begin(), project_root.end())).lexically_normal());
        if (!relative.empty()) source = std::move(relative);
    }
    const auto utf8 = source.generic_u8string();
    return std::string(utf8.begin(), utf8.end());
}
std::string ProjectSettings::save() const {
    auto entries = nlohmann::json::object();
    for (const auto &[path, settings] : sources)
        entries[path] = {{"target", target_info(settings.target).id}, {"origin", settings.origin}};
    return nlohmann::json{{"schema", 1}, {"sources", entries}}.dump();
}
bool ProjectSettings::load(const std::string &text) {
    std::map<std::string, SourceSettings> next;
    const auto first = text.find_first_not_of(" \t\r\n");
    // Older projects store a plain source path in tool_state[z80_assembler].
    // Those documents have no target settings and retain the Z80 default.
    if (first == std::string::npos || (text[first] != '{' && text[first] != '[')) {
        sources.clear();
        return true;
    }
    const auto state = nlohmann::json::parse(text, nullptr, false);
    if (!state.is_object() || !state.contains("schema") || state["schema"] != 1 ||
        !state.contains("sources") || !state["sources"].is_object()) return false;
    for (const auto &[path, value] : state["sources"].items()) {
        if (path.empty() || path.find('\0') != std::string::npos || !value.is_object() ||
            !value.contains("target") || !value["target"].is_string() ||
            !value.contains("origin") || !value["origin"].is_number_unsigned()) return false;
        const auto target = target_from_id(value["target"].get<std::string>());
        const auto origin = value["origin"].get<uint64_t>();
        // Preserve a currently invalid origin after a CPU switch, so reopening
        // still reports the assembly error instead of silently moving code.
        if (!target || origin > UINT32_MAX) return false;
        next.emplace(path, SourceSettings{*target, static_cast<uint32_t>(origin)});
    }
    sources = std::move(next);
    return true;
}
}
