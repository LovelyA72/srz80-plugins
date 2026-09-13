#pragma once
#include "source_model.hpp"
#include <srz80/tool.h>
namespace srz80::assembler {
std::string load(const SrhToolHostV1 &host, SrhHandle space, const SourceModel &source, bool reset);
}
