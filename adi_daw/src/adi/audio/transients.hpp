// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/audio/wav_file.hpp"
#include <stop_token>
#include <vector>
namespace adi::audio {
enum class AnalysisError {None,Realtime,InvalidSource,Cancelled,TooManyOnsets,Io};
struct TransientResult {
    AnalysisError error=AnalysisError::None;
    std::vector<std::uint64_t> frames; // source-window-relative, no synthetic origin
    [[nodiscard]] explicit operator bool()const noexcept{return error==AnalysisError::None;}
};
/// Worker-only shared onset map for Auto-Warp and the native Beats player.
/// Reads bounded 5-ms windows and preserves the caller's source file. The
/// supplied reader is borrowed; its position changes. No callback I/O/allocation.
[[nodiscard]] TransientResult detectTransients(WavReader&,std::uint64_t sourceStart,
                                              std::uint64_t frames,int sensitivity=50,
                                              std::stop_token={});
}
