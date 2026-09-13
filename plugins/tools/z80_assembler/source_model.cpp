#include "source_model.hpp"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
namespace srz80::assembler {
void SourceModel::new_document() { text.clear(); path.clear(); saved_text.clear(); changed(); }
void SourceModel::open(const std::string &approved_path) {
    std::ifstream file(std::filesystem::path(std::u8string(approved_path.begin(), approved_path.end())), std::ios::binary);
    if (!file) throw std::runtime_error("Cannot open source file");
    std::string content{std::istreambuf_iterator<char>(file), {}};
    if (file.bad()) throw std::runtime_error("Cannot read source file");
    if (content.find('\0') != std::string::npos) throw std::runtime_error("Source contains NUL bytes");
    if (content.starts_with("\xEF\xBB\xBF")) content.erase(0,3);
    text = std::move(content); path = approved_path; saved_text = text; changed();
}
void SourceModel::open_text(const std::string &document_path, std::string contents) {
    if (contents.find('\0') != std::string::npos)
        throw std::runtime_error("Source contains NUL bytes");
    text = std::move(contents);
    path = document_path;
    saved_text = text;
    changed();
}
void SourceModel::save(const std::string &approved_path) {
    std::ofstream file(std::filesystem::path(std::u8string(approved_path.begin(), approved_path.end())), std::ios::binary | std::ios::trunc);
    if (!file) throw std::runtime_error("Cannot open source file for writing");
    file.write(text.data(), static_cast<std::streamsize>(text.size())); file.close();
    if (!file) throw std::runtime_error("Cannot finish writing source file");
    path = approved_path; saved_text = text;
}
}
