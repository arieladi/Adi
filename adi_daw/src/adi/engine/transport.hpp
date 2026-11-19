// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
namespace adi::engine {
// Single driver-thread state. Commands execute BETWEEN callbacks (or with the
// driver stopped). A UI must marshal commands to that driver, never mutate this
// concurrently. Nodes read the same block origin; Session advances it once.
class Transport {
public:
  void play(bool enabled = true) noexcept {
      if (playing_ != enabled || countInLeft_ > 0) {
          countInLeft_ = 0;
          playing_ = enabled;
          if (enabled)
              streamed_ = 0;
          ++revision_;
      }
  }
  /// Driver-only lead-in: hold the located timeline, then start at the exact
  /// frame boundary. Stop/play/locate cancel it. No authored timeline is negative.
  void countIn(std::int64_t frames) noexcept;
  [[nodiscard]] std::int64_t countInRemaining() const noexcept { return countInLeft_; }
  [[nodiscard]] std::int64_t countInElapsed() const noexcept {
      return countInTotal_ - countInLeft_;
  }
    void locate(std::int64_t sample) noexcept;
    void loop(std::int64_t begin, std::int64_t end, bool enabled = true) noexcept;
    /// Driver-only clock projection of the same authored loop. Preserve play
    /// revision and elapsed stream history; shortened bounds can cause a real
    /// wrap, exactly as advancing through the loop end would.
    void retimeLoop(std::int64_t begin, std::int64_t end, bool enabled = true) noexcept;
    [[nodiscard]] bool looping() const noexcept { return looping_; }
    [[nodiscard]] std::int64_t loopBegin() const noexcept { return begin_; }
    [[nodiscard]] std::int64_t loopEnd() const noexcept { return end_; }
    [[nodiscard]] bool playing() const noexcept { return playing_; }
    [[nodiscard]] std::int64_t position() const noexcept { return position_; }
    [[nodiscard]] std::int64_t sampleAt(std::int64_t offset) const noexcept;
    /// Timeline heard after a compensated output delay. Negative means the
    /// post-seek/start pipeline has not filled. Accounts for prior loop wraps.
    [[nodiscard]] std::int64_t delayedSample(std::int32_t offset,std::int32_t delay) const noexcept;
    // Number of consecutive timeline samples before the next wrap. Used to
    // request ranges without walking every sample of the read-ahead horizon.
    [[nodiscard]] std::int64_t contiguousFrames(std::int64_t offset, std::int64_t count) const noexcept;
    void advance(std::int32_t frames) noexcept;
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
private:
    std::int64_t position_ = 0, begin_ = 0, end_ = 0;
    std::uint64_t revision_ = 0;
    bool playing_ = false, looping_ = false;
    bool wrapped_ = false;
    std::int64_t streamed_ = 0;
    std::int64_t countInTotal_ = 0, countInLeft_ = 0;
};
}
