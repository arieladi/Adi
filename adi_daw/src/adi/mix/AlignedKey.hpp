// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace adi::mix {
// A processing-chain key is captured before any in-place main processor runs.
// One captured, delayed view can then serve several downstream stages. This is
// audio transport only: it creates no route, detector or visual subscription.
// Single audio owner. Prepare/clear/destruction require stopped processing.
template <class Sample> class AlignedKey final {
    static_assert(std::is_same_v<Sample, float> || std::is_same_v<Sample, double>);

  public:
    AlignedKey() = default;
    AlignedKey(const AlignedKey &) = delete;
    AlignedKey &operator=(const AlignedKey &) = delete;
    AlignedKey(AlignedKey &&) = delete;
    AlignedKey &operator=(AlignedKey &&) = delete;
    struct View {
        const Sample *const *samples = nullptr;
        std::int32_t channels = 0;
        bool valid = false;
    };
    struct Statistics {
        std::uint64_t historyFrames = 0, materializedFrames = 0;
    };

    void prepare(std::int32_t maximumDelay, std::int32_t maximumFrames) {
        if (maximumDelay < 0 || maximumDelay == std::numeric_limits<std::int32_t>::max() ||
            maximumFrames <= 0)
            throw std::invalid_argument("Invalid aligned-key capacity");
        // Build first: failed allocation leaves the running object unchanged.
        std::array<std::vector<Sample>, 2> history, scratch;
        for (std::int32_t ch = 0; ch < 2; ++ch) {
            history[static_cast<std::size_t>(ch)].assign(static_cast<std::size_t>(maximumDelay) + 1, 0);
            scratch[static_cast<std::size_t>(ch)].assign(static_cast<std::size_t>(maximumFrames), 0);
        }
        history_ = std::move(history);
        scratch_ = std::move(scratch);
        maximumDelay_ = maximumDelay;
        maximumFrames_ = maximumFrames;
        prepared_ = true;
        position_ = 0;
        remaining_ = 0;
        statistics_ = {};
        for (std::int32_t ch = 0; ch < 2; ++ch)
            pointers_[static_cast<std::size_t>(ch)] = scratch_[static_cast<std::size_t>(ch)].data();
    }

    void clear() noexcept {
        for (auto &channel : history_)
            for (auto &sample : channel)
                sample = 0;
        position_ = 0;
        remaining_ = 0;
        statistics_ = {};
    }

    // The returned view lasts until the next capture. Mono keys broadcast to
    // two key planes; partial stereo planes are zero, never self-detection.
    // Key width is independent of main width: a mono main must not discard a
    // stereo key's right plane before its detector decides how to fold it.
    // A connected silent key stays connected. An absent key drains its delayed
    // history, then returns null and performs no sample work. Frames may be
    // nonuniform segments of a physical callback without restarting the delay.
    // materialize=false keeps upstream history while downstream stages are off,
    // without writing a scratch view. The caller must supply an admitted delay;
    // this helper never decides or changes the chain's PDC plan.
    [[nodiscard]] View capture(const Sample *const *source, std::int32_t keyChannels,
                               std::int32_t frames, std::int32_t delay,
                               bool materialize = true) noexcept {
        if (!prepared_ || frames < 0 || frames > maximumFrames_ || delay < 0 || delay > maximumDelay_ ||
            keyChannels < 0 || keyChannels > 2 || (keyChannels && !source))
            return {};
        bool connected = false;
        for (std::int32_t ch = 0; ch < keyChannels; ++ch)
            connected = connected || source[ch] != nullptr;
        if (!connected && remaining_ == 0)
            return {nullptr, 0, true};
        const auto capacity = static_cast<std::size_t>(maximumDelay_) + 1;
        const auto admittedDelay = static_cast<std::size_t>(delay);
        for (std::int32_t frame = 0; frame < frames; ++frame) {
            const auto read = position_ >= admittedDelay ? position_ - admittedDelay
                                                        : position_ + capacity - admittedDelay;
            for (std::int32_t ch = 0; ch < 2; ++ch) {
                const auto plane = static_cast<std::size_t>(ch);
                const auto keyPlane = keyChannels == 1 ? 0 : ch;
                const auto input = connected && keyPlane < keyChannels && source[keyPlane]
                                       ? source[keyPlane][frame]
                                       : Sample{0};
                history_[plane][position_] = input;
                if (materialize)
                    scratch_[plane][static_cast<std::size_t>(frame)] = history_[plane][read];
            }
            if (++position_ == capacity)
                position_ = 0;
            if (connected)
                remaining_ = maximumDelay_;
            else if (remaining_)
                --remaining_;
        }
        statistics_.historyFrames += static_cast<std::uint64_t>(frames);
        if (!materialize)
            return {nullptr, 0, true};
        statistics_.materializedFrames += static_cast<std::uint64_t>(frames);
        return {pointers_.data(), 2, true};
    }

    [[nodiscard]] Statistics statistics() const noexcept { return statistics_; }

  private:
    std::array<std::vector<Sample>, 2> history_, scratch_;
    std::array<const Sample *, 2> pointers_{};
    std::size_t position_ = 0;
    std::int32_t maximumDelay_ = 0, maximumFrames_ = 0, remaining_ = 0;
    bool prepared_ = false;
    Statistics statistics_;
};
} // namespace adi::mix
