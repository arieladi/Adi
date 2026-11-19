// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/audio/warp.hpp"
#include "adi/audio/warp_map.hpp"
#include "adi/engine/snapshot.hpp"
namespace adi::engine {
/// Source musical origin = clip timeline origin minus its content offset.
/// Extends the initial tempo before tick zero for trimmed source material.
/// Owner-thread only; no pointer/Store survives into the audio callback.
[[nodiscard]] std::int64_t warpSourceSamples(const TempoMap&,std::int64_t sourceOrigin,
                                            std::int64_t sourceTick,double rate);
/// Adds project-tempo boundaries inside each source-map segment before
/// converting to output frames. Sub-sample/overflowing anchors refuse by name.
[[nodiscard]] std::vector<audio::WarpAnchor> projectWarpAnchors(std::span<const WarpMarker>,
    const TempoMap&,std::int64_t sourceOrigin,double rate);
}
