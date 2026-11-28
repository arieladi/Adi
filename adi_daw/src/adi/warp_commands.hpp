// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/clip_commands.hpp"
#include "adi/audio/auto_warp.hpp"
#include "adi/audio/warp.hpp"
#include <optional>
#include <span>
namespace adi::clip_edit {
struct WarpOptions {
    bool enabled=true;
    audio::WarpMode mode=audio::WarpMode::Beats;
    audio::AutoWarpOptions analysis;
    std::stop_token stop;
};
/// Owner/worker connection, off the callback. Toggle Warp while preserving the
/// audible source interval of a placed clip. An explicit source BPM also corrects
/// an already-warped clip, keeping its source markers and bounds.
/// Requesting a different mode on an already-warped clip without a source BPM
/// changes only the algorithm, using the existing source clocks and map.
/// Commit with commitPrepared on the same document generation. Both directions
/// capture actual source clocks and duration; replay never analyzes a file.
[[nodiscard]] PreparedEdit prepareSetWarp(Store&,std::int64_t clipId,const WarpOptions&,double referenceRate);
/// Scale a warped clip's musical source clock and place its stretched span
/// together. This uses the existing map and source window without reading PCM.
[[nodiscard]] PreparedEdit prepareStretchWarpedClip(Store&,std::int64_t clipId,std::int64_t position,
    std::int64_t length,double referenceRate);
enum class WarpMarkerChange { Add, Move, Remove };
/// Edit an interior anchor by its source frame; the endpoint anchors stay fixed.
[[nodiscard]] PreparedEdit prepareWarpMarkerEdit(Store&,std::int64_t clipId,WarpMarkerChange,
    WarpMarker,double referenceRate);
/// Promote missing cached file transients before snapping the map's interior markers.
[[nodiscard]] PreparedEdit prepareQuantizeWarpMarkers(Store&,std::int64_t clipId,std::int64_t gridTicks,
    double referenceRate,std::span<const std::uint64_t> fileFrames={});
[[nodiscard]] PreparedEdit prepareWarpMarkersAtTransients(Store&,std::int64_t clipId,
    std::span<const std::uint64_t> fileFrames,double referenceRate);
/// Rebuild only the right side of an existing map. The cached transient frames
/// refer to the original file. The anchor is never trimmed by Auto-Warp.
[[nodiscard]] PreparedEdit prepareWarpFromHere(Store&,std::int64_t clipId,std::int64_t startTick,
    std::span<const std::uint64_t> fileFrames,audio::AutoWarpOptions,double referenceRate);

struct WarpLoopSelection {std::int64_t from,to;};
/// Fit the whole warped source or a source-ruler range to a musical loop
/// length. Captures the map and loop together without resizing the placement.
[[nodiscard]] PreparedEdit prepareWarpAsLoop(Store&,std::int64_t clipId,std::optional<WarpLoopSelection>,
    std::int64_t lengthTicks,double referenceRate);
}
