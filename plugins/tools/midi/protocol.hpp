#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>
#include <stdexcept>
namespace srz80::midi {
using Bytes = std::vector<uint8_t>;
class Parser {
    uint8_t running_ = 0, status_ = 0;
    size_t needed_ = 0;
    Bytes partial_;
    bool sysex_ = false;
public:
    using Sink = std::function<void(const Bytes &)>;
    void reset() { running_ = status_ = 0; needed_ = 0; partial_.clear(); sysex_ = false; }
    void feed(uint8_t byte, const Sink &sink) {
        if (byte >= 0xf8) { sink(Bytes{byte}); return; }
        if (sysex_) {
            if (byte == 0xf7) { if (partial_.size() == 65536) { reset(); throw std::runtime_error("SysEx exceeds 64 KiB tool limit"); } partial_.push_back(byte); sink(partial_); reset(); return; }
            if (byte < 0x80) {
                if (partial_.size() == 65536) { reset(); throw std::runtime_error("SysEx exceeds 64 KiB tool limit"); }
                partial_.push_back(byte); return;
            }
            reset();
        }
        if (byte & 0x80) {
            partial_.clear(); status_ = byte;
            if (byte < 0xf0) { running_ = byte; needed_ = ((byte & 0xe0) == 0xc0) ? 1 : 2; }
            else {
                running_ = 0;
                if (byte == 0xf0) { sysex_ = true; partial_ = {byte}; return; }
                needed_ = byte == 0xf2 ? 2 : ((byte == 0xf1 || byte == 0xf3) ? 1 : 0);
            }
            partial_.push_back(byte);
            if (!needed_) { sink(partial_); partial_.clear(); status_ = 0; }
            return;
        }
        if (!status_) {
            if (!running_) return;
            status_ = running_; needed_ = ((status_ & 0xe0) == 0xc0) ? 1 : 2;
            partial_ = {status_};
        }
        partial_.push_back(byte);
        if (--needed_ == 0) { sink(partial_); partial_.clear(); status_ = 0; }
    }
    void feed(const Bytes &bytes, const Sink &sink) { for (auto byte : bytes) feed(byte, sink); }
};
// Empty bytes preserve an SMF End-of-Track timing boundary, not a wire message.
struct FileEvent { uint64_t time_ns; Bytes bytes; };
struct LoopRange { uint64_t start_ns, end_ns; };
struct ParsedSmf {
    std::vector<FileEvent> events;
    std::optional<LoopRange> loop;
};
ParsedSmf parse_smf(const Bytes &file);
}
