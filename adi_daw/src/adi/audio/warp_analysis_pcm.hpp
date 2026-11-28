// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/audio/transients.hpp"

#include <functional>
#include <memory>

namespace adi::audio {

inline constexpr std::uint32_t kWarpAnalysisRate = 44100;

enum class WarpAnalysisLayout { Mono, MidSide };

// Converts arbitrary decoder blocks into one continuous 44.1 kHz stream.
// Mono averages the channels. MidSide preserves stereo phase opposition with
// a second lane and emits interleaved mid/side, or one lane for mono sources.
// The sink sees temporary bounded spans and must consume them synchronously.
// Construct, feed, finish and destroy this object on its owner worker.
class WarpAnalysisPcm {
public:
    using Sink = std::function<bool(std::span<const float>)>;

    WarpAnalysisPcm(std::uint32_t sourceRate, std::uint32_t channels, Sink,
                    WarpAnalysisLayout = WarpAnalysisLayout::Mono);
    ~WarpAnalysisPcm();
    WarpAnalysisPcm(const WarpAnalysisPcm&) = delete;
    WarpAnalysisPcm& operator=(const WarpAnalysisPcm&) = delete;

    [[nodiscard]] bool append(std::span<const float> interleaved, std::stop_token = {});
    [[nodiscard]] bool finish(std::stop_token = {});
    [[nodiscard]] AnalysisError error() const noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
    AnalysisError initialError_ = AnalysisError::InvalidSource;
};

} // namespace adi::audio
