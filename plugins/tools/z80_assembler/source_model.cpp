#include "source_model.hpp"
#include <stdexcept>
namespace srz80::assembler {
void SourceModel::open_text(const std::string &document_path, std::string contents) {
    if (contents.find('\0') != std::string::npos)
        throw std::runtime_error("Source contains NUL bytes");
    text = std::move(contents);
    path = document_path;
    changed();
}
}
