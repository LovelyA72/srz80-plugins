#include "script_card.h"

namespace srz80_script {

bool valid_utf8_text(const char *data, size_t size) {
    if (!data && size)
        return false;
    // Configuration text and module names must not contain embedded NUL bytes.
    return std::find(data, data + size, '\0') == data + size;
}

std::string path_utf8(const fs::path &path) {
    const auto value = path.u8string();
    return std::string(reinterpret_cast<const char *>(value.data()), value.size());
}

fs::path path_from_utf8(std::string_view value) {
#if defined(_WIN32)
    // Rust's canonicalize returns Windows extended-length paths. MinGW's
    // std::filesystem does not recognize their \\?\ prefix as a root name.
    std::string ordinary;
    if (value.size() >= 7 && value.substr(0, 4) == "\\\\?\\" &&
        std::isalpha(static_cast<unsigned char>(value[4])) &&
        value[5] == ':' && (value[6] == '\\' || value[6] == '/')) {
        value.remove_prefix(4);
    } else if (value.size() >= 8 && value.substr(0, 8) == "\\\\?\\UNC\\") {
        ordinary = "\\\\";
        ordinary.append(value.substr(8));
        value = ordinary;
    }
#endif
    std::u8string encoded;
    encoded.reserve(value.size());
    for (unsigned char byte : value)
        encoded.push_back(static_cast<char8_t>(byte));
    return fs::path(encoded);
}

bool path_is_within(const fs::path &root, const fs::path &candidate) {
    auto r = root.begin();
    auto c = candidate.begin();
    for (; r != root.end() && c != candidate.end(); ++r, ++c) {
        auto left = path_utf8(*r);
        auto right = path_utf8(*c);
#if defined(_WIN32)
        std::transform(left.begin(), left.end(), left.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        std::transform(right.begin(), right.end(), right.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
#endif
        if (left != right)
            return false;
    }
    return r == root.end();
}

bool normalize_project_path(std::string_view input, std::string &out) {
    if (input.empty() || input.size() > 4096 || input.find('\0') != std::string_view::npos)
        return false;
    if (input.front() == '/' || input.front() == '\\' ||
        (input.size() >= 2 && std::isalpha(static_cast<unsigned char>(input[0])) &&
         input[1] == ':'))
        return false;
    std::vector<std::string_view> parts;
    size_t first = 0;
    while (first <= input.size()) {
        size_t last = input.find_first_of("/\\", first);
        if (last == std::string_view::npos)
            last = input.size();
        const auto part = input.substr(first, last - first);
        if (part.empty() || part == ".") {
        } else if (part == "..") {
            if (parts.empty())
                return false;
            parts.pop_back();
        } else {
            if (part.find(':') != std::string_view::npos)
                return false;
            parts.push_back(part);
        }
        if (last == input.size())
            break;
        first = last + 1;
    }
    if (parts.empty())
        return false;
    out.clear();
    for (const auto part : parts) {
        if (!out.empty())
            out.push_back('/');
        out.append(part);
    }
    return true;
}

bool resolve_project_include(std::string_view importer, std::string_view include,
                             std::string &out) {
    if (include.empty() || include.size() > 4096 ||
        include.find('\0') != std::string_view::npos ||
        include.front() == '/' || include.front() == '\\' ||
        (include.size() >= 2 && std::isalpha(static_cast<unsigned char>(include[0])) &&
         include[1] == ':'))
        return false;
    const auto slash = importer.find_last_of('/');
    std::string joined;
    if (slash != std::string_view::npos) {
        joined.assign(importer.substr(0, slash + 1));
    }
    joined.append(include);
    return normalize_project_path(joined, out);
}

std::string trim_bom(std::string bytes) {
    if (bytes.size() >= 3 && static_cast<uint8_t>(bytes[0]) == 0xEF &&
        static_cast<uint8_t>(bytes[1]) == 0xBB && static_cast<uint8_t>(bytes[2]) == 0xBF)
        bytes.erase(0, 3);
    return bytes;
}

std::pair<uint64_t, uint64_t> file_fingerprint(std::string_view bytes) {
    uint64_t hash = 14695981039346656037ull;
    for (const unsigned char byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return {bytes.size(), hash};
}

} // namespace srz80_script
