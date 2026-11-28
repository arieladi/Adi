// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/audio/wav_file.hpp"
#include <span>
#include <stop_token>
#include <vector>
namespace adi::audio {
enum class AnalysisError {None,Realtime,InvalidSource,Cancelled,TooManyOnsets,Io};
struct TransientResult {
    AnalysisError error=AnalysisError::None;
    std::vector<std::uint64_t> frames; // source-window-relative, no synthetic origin
    [[nodiscard]] explicit operator bool()const noexcept{return error==AnalysisError::None;}
};
// The same detector for callers which already stream PCM to other analysers.
// Append whole interleaved frames; consecutive blocks need no alignment.
class TransientAnalyzer {
public:
    TransientAnalyzer(std::uint32_t sampleRate, std::uint32_t channels, int sensitivity=50);
    [[nodiscard]] bool append(std::span<const float> interleaved);
    [[nodiscard]] TransientResult finish();
private:
    void measure(std::span<const float>);
    TransientResult result_;
    std::vector<float> pending_;
    std::size_t pendingSamples_ = 0;
    std::uint32_t channels_ = 0;
    std::uint64_t separation_ = 0, position_ = 0, last_ = 0;
    double multiplier_ = 0, previous_ = 0, baseline_ = 0;
};
/// Worker-only shared onset map for Auto-Warp and the native Beats player.
/// Reads bounded 5-ms windows and preserves the caller's source file. The
/// supplied reader is borrowed; its position changes. No callback I/O/allocation.
[[nodiscard]] TransientResult detectTransients(WavReader&,std::uint64_t sourceStart,
                                              std::uint64_t frames,int sensitivity=50,
                                              std::stop_token={});
}
