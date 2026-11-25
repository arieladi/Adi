// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace adi::dsp {

class AdiShaperAudioTrigger {
  public:
    enum Mode { Sync, Loop, OneShot };
    struct Clock {
        std::uint64_t start, endpoint, untilHold;
        [[nodiscard]] std::uint64_t at(std::uint64_t step, std::uint64_t offset) const noexcept {
            return offset >= untilHold ? endpoint : start + step * offset;
        }
    };
    void prepare(double rate) noexcept;
    void mode(std::size_t band, int value) noexcept;
    void endpoint(std::size_t band, double value) noexcept;
    void detector(int profile, bool smoothing) noexcept;
    void detail(float value) noexcept;
    void threshold(float value) noexcept;
    void shift(float value) noexcept;
    [[nodiscard]] bool smoothing() const noexcept { return smoothing_; }
    [[nodiscard]] int profile() const noexcept { return profile_; }
    [[nodiscard]] int latency() const noexcept { return latency_; }
    [[nodiscard]] bool tick(float& left, float& right) noexcept;
    [[nodiscard]] Clock clock(std::size_t band, std::uint64_t synced, std::uint64_t step) const noexcept;
    void restart() noexcept { hit_ = true; age_ = 0; phases_.fill(0); }
    void advance(std::size_t frames, const std::array<std::uint64_t, 3>& steps) noexcept;
    [[nodiscard]] float fade(std::size_t band, std::size_t offset) const noexcept;

  private:
    struct Frame { float left = 0, right = 0; std::uint64_t event = 0; };
    void timing() noexcept;
    void level() noexcept;
    void clear() noexcept;
    std::array<int, 3> modes_{};
    std::array<std::uint64_t, 3> phases_{};
    std::array<std::uint64_t, 3> ends_{{0xfc00000000000000ULL, 0xfc00000000000000ULL, 0xfc00000000000000ULL}};
    std::vector<Frame> history_;
    std::size_t mask_ = 0, filled_ = 0;
    std::uint64_t tick_ = 0, age_ = 0;
    double rate_ = 48000;
    float threshold_ = .08838835f, detail_ = .5f, shift_ = .5f, onsetLevel_ = .1f, peak_ = 0, release_ = 0;
    int profile_ = 0, latency_ = 0, lead_ = 0, smoothingFrames_ = 0, hold_ = 0, separation_ = 0;
    bool smoothing_ = false, hit_ = false;
};

} // namespace adi::dsp
