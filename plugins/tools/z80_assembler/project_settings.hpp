#pragma once
#include "target.hpp"
#include <map>
#include <string>

namespace srz80::assembler {
struct SourceSettings {
    Target target = Target::z80;
    uint32_t origin = 0;
    bool operator==(const SourceSettings &) const = default;
};
struct ProjectSettings {
    std::map<std::string, SourceSettings> sources;
    SourceSettings get(const std::string &key) const;
    std::string save() const;
    // Invalid state is rejected without changing the current settings.
    bool load(const std::string &text);
    static std::string source_key(const std::string &path, const std::string &project_root);
};
}
