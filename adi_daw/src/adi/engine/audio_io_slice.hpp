// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/engine/process.hpp"
#include <array>
#include <limits>

namespace adi::engine {
/// Stack-only view of part of a physical callback. Null channel pointers stay
/// null. Both output widths, input channels and the linear stream clock advance
/// together; no pointer in the view refers to a temporary pointer array.
class AudioIoSlice final {
public:
    static constexpr std::int32_t maximumChannels = 256;
    AudioIoSlice(const AudioIo& source, std::int32_t offset, std::int32_t frames) noexcept {
        if (source.frames < 0 || offset < 0 || frames < 0 || offset > source.frames ||
            frames > source.frames - offset || source.numOut < 0 || source.numIn < 0 ||
            source.numOut > maximumChannels || source.numIn > maximumChannels ||
            source.streamTimeSamples > std::numeric_limits<std::int64_t>::max() - offset)
            return;
        view_ = source;
        view_.frames = frames;
        view_.streamTimeSamples += offset;
        for (std::int32_t c = 0; c < source.numOut; ++c) {
            const auto index = static_cast<std::size_t>(c);
            if (source.out) narrow_[index] = source.out[c] ? source.out[c] + offset : nullptr;
            if (source.out64) wide_[index] = source.out64[c] ? source.out64[c] + offset : nullptr;
        }
        for (std::int32_t c = 0; c < source.numIn; ++c) {
            const auto index = static_cast<std::size_t>(c);
            if (source.in) input_[index] = source.in[c] ? source.in[c] + offset : nullptr;
        }
        view_.out = source.out ? narrow_.data() : nullptr;
        view_.out64 = source.out64 ? wide_.data() : nullptr;
        view_.in = source.in ? input_.data() : nullptr;
        valid_ = true;
    }
    AudioIoSlice(const AudioIoSlice&) = delete;
    AudioIoSlice& operator=(const AudioIoSlice&) = delete;
    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] const AudioIo& io() const noexcept { return view_; }
private:
    std::array<float*, maximumChannels> narrow_{};
    std::array<double*, maximumChannels> wide_{};
    std::array<const float*, maximumChannels> input_{};
    AudioIo view_{};
    bool valid_ = false;
};
} // namespace adi::engine
