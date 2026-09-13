#include "loader.hpp"
#include <cstdio>
namespace srz80::assembler {
std::string load(const SrhToolHostV1 &host, SrhHandle space, const SourceModel &source, bool reset) {
    if (!source.fresh() || !source.result.succeeded || source.result.segments.empty())
        return "Load rejected: assemble the current source successfully first.";
    std::vector<SrhToolMemorySegment> segments;
    for (auto &s:source.result.segments) segments.push_back({SRH_INIT(SrhToolMemorySegment),s.first,s.bytes.data(),s.bytes.size()});
    uint32_t failed=UINT32_MAX; uint64_t written=0;
    auto status=host.load_memory_segments(host.context,space,segments.data(),static_cast<uint32_t>(segments.size()),reset,&failed,&written);
    if(status==SRH_OK) return "Loaded "+std::to_string(written)+" bytes. PC unchanged by loading.";
    std::string message="Load failed (status "+std::to_string(status)+") after "+std::to_string(written)+" completed writes";
    if(failed<segments.size()) {
        uint64_t earlier=0; for(uint32_t i=0;i<failed;++i) earlier+=segments[i].size;
        char address[32]; std::snprintf(address,sizeof(address),"%04llX",static_cast<unsigned long long>(segments[failed].address+(written>=earlier?written-earlier:0)));
        message+="; segment "+std::to_string(failed+1)+", address "+address;
    }
    return message+=". A failing transaction may have side effects; no rollback was attempted.";
}
}
