// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/engine/transport.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace adi::engine {

/// Delays Session PCM and its Arrangement-replacement mask together. The
/// graph owner allocates the history, and only the render driver changes it.
class LaunchOverlayDelay {
public:
    LaunchOverlayDelay(std::int32_t frames, std::int32_t channels)
        : channels_(static_cast<std::size_t>(channels)),
          pcm_(static_cast<std::size_t>(frames) * channels_),
          replaces_(static_cast<std::size_t>(frames)) {}

    template<class Sample>
    void mix(const Transport& transport, std::int32_t offset, std::int32_t frames,
             Sample* const* source, const std::uint8_t* replaces, Sample* const* destination) noexcept {
        const auto streamed = transport.streamedFrames();
        if (!transport.playing() || streamed > std::numeric_limits<std::int64_t>::max() - offset - frames) {
            nextFrame_ = -1;
            return;
        }
        const auto first = streamed + offset;
        if (first != nextFrame_ || revision_ != transport.revision()) {
            // A locate or stop discards buffered launches without scanning
            // or clearing the ring on the callback.
            revision_ = transport.revision();
            filled_ = 0;
            write_ = 0;
        }
        nextFrame_ = first + frames;
        const auto size = replaces_.size();
        for (auto frame = offset; frame < offset + frames; ++frame) {
            auto* history = pcm_.data() + write_ * channels_;
            const bool ready = filled_ == size;
            const bool replace = ready && replaces_[write_] != 0;
            for (std::size_t channel = 0; channel < channels_; ++channel) {
                if (replace) destination[channel][frame] = static_cast<Sample>(history[channel]);
                else if (ready && history[channel] != 0)
                    destination[channel][frame] += static_cast<Sample>(history[channel]);
                history[channel] = source[channel][frame];
            }
            replaces_[write_] = replaces[frame];
            if (!ready) ++filled_;
            if (++write_ == size) write_ = 0;
        }
    }

private:
    const std::size_t channels_;
    std::vector<double> pcm_;
    std::vector<std::uint8_t> replaces_;
    std::size_t filled_ = 0, write_ = 0;
    std::uint64_t revision_ = 0;
    std::int64_t nextFrame_ = -1;
};

} // namespace adi::engine
