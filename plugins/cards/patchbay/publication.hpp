#pragma once
#include "engine.hpp"
#include <chrono>

namespace srz80::patchbay {
// Protocol-v1 readers can join at any time, so each publication is complete.
// Wall time throttles serialization only; all model/PWM time remains simulated.
class Publication {
  public:
    using Clock = std::chrono::steady_clock;
    void invalidate() {
        bytes_.clear();
        size_query_pending_ = false;
    }
    const std::string &snapshot(const Engine &engine, uint64_t now, bool size_query,
                                Clock::time_point wall = Clock::now()) {
        const bool paired_fill = !size_query && size_query_pending_;
        const bool boundary = engine.revision != revision_ || now < time_;
        const bool changed = engine.runtime_revision != runtime_ || now != time_;
        const bool elapsed = wall - published_at_ >= std::chrono::milliseconds(16);
        if (bytes_.empty() || (!paired_fill && (boundary || (changed && elapsed)))) {
            bytes_ = engine.snapshot(now).dump();
            revision_ = engine.revision;
            runtime_ = engine.runtime_revision;
            time_ = now;
            published_at_ = wall;
        }
        size_query_pending_ = size_query;
        return bytes_;
    }

  private:
    std::string bytes_;
    uint64_t revision_ = 0, runtime_ = 0, time_ = 0;
    Clock::time_point published_at_{};
    bool size_query_pending_ = false;
};
} // namespace srz80::patchbay
