// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
namespace adi::engine {
// Single driver-thread state. Commands execute BETWEEN callbacks (or with the
// driver stopped). A UI must marshal commands to that driver, never mutate this
// concurrently. Nodes read the same block origin; Session advances it once.
class Transport {
public:
    void play(bool enabled = true) noexcept { if (playing_ != enabled) { playing_ = enabled; ++revision_; } }
    void locate(std::int64_t sample) noexcept;
    void loop(std::int64_t begin, std::int64_t end, bool enabled = true) noexcept;
    [[nodiscard]] bool playing() const noexcept { return playing_; }
    [[nodiscard]] std::int64_t position() const noexcept { return position_; }
    [[nodiscard]] std::int64_t sampleAt(std::int64_t offset) const noexcept;
    // Number of consecutive timeline samples before the next wrap. Used to
    // request ranges without walking every sample of the read-ahead horizon.
    [[nodiscard]] std::int64_t contiguousFrames(std::int64_t offset, std::int64_t count) const noexcept;
    void advance(std::int32_t frames) noexcept;
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
private:
    std::int64_t position_ = 0, begin_ = 0, end_ = 0;
    std::uint64_t revision_ = 0;
    bool playing_ = false, looping_ = false;
};
}
