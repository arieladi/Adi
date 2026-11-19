// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/audio/warp_map.hpp"
#include <optional>
#include <stop_token>
namespace adi::audio {
struct AutoWarpOptions {
    bool detectTempoChanges=true,fromFirstBeat=true;
    int numerator=4,denominator=4;
    double tempoHint=120;
    std::optional<double> sourceBpm;
};
enum class AutoWarpError {None,Realtime,InvalidSource,InvalidOptions,NoTempo,Precision,TooManyMarkers,Cancelled};
struct AutoWarpResult {
    AutoWarpError error=AutoWarpError::None;
    std::uint64_t trimFrames=0;
    double estimatedBpm=0,confidence=0;
    bool variableTempo=false;
    std::vector<WarpMarker> markers; // relative to the trimmed source window
    [[nodiscard]] explicit operator bool()const noexcept{return error==AutoWarpError::None;}
};
/// Worker-only metadata analysis. Onsets use the source window's frame axis.
/// Confidence measures timing-grid fit; tempo octave ambiguity remains possible.
/// An explicit sourceBpm supplies the clock for short or nonpercussive audio.
[[nodiscard]] AutoWarpResult autoWarpFromOnsets(std::span<const std::uint64_t> onsets,
    std::uint64_t sourceFrames,std::uint32_t sourceRate,const AutoWarpOptions& = {},std::stop_token={});
}
