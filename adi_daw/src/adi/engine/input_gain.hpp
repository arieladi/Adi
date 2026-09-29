// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/engine/graph.hpp"
#include <atomic>
#include <algorithm>
#include <cmath>
namespace adi::engine {
// Session-owned, before audio inserts and after the instrument. Publishing a
// graph never destroys a gain node still named by a retired graph.
class InputGainNode final : public Node {
public:
    void setDb(double db) noexcept {
        target_.store(std::pow(10.0, std::clamp(db, -120.0, 60.0) / 20.0), std::memory_order_relaxed);
    }
    void process(const NodeIo& io) noexcept override {
        const double target = target_.load(std::memory_order_relaxed);
        if (!started_) { current_ = target; started_ = true; }
        if (target != goal_) {
            goal_ = target;
            remaining_ = std::max(1, static_cast<int>(io.sampleRate * 0.005));
            step_ = (goal_ - current_) / remaining_;
        }
        for (int i = 0; i < io.frames; ++i) {
            if (remaining_ > 0) { current_ += step_; if (--remaining_ == 0) current_ = goal_; }
            for (int c = 0; c < io.channels; ++c)
                io.out[c][io.blockOffset + i] = io.in ? static_cast<float>(io.in[c][io.blockOffset + i] * current_) : 0.0f;
        }
    }
    std::int64_t tailSamples() const noexcept override { return 0; }
    const char* name() const noexcept override { return "input gain"; }
private:
    std::atomic<double> target_{1.0};
    double current_ = 1.0, goal_ = 1.0, step_ = 0.0;
    int remaining_ = 0;
    bool started_ = false;
};
}
