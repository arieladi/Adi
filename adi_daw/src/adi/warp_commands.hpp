// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/clip_commands.hpp"
#include "adi/audio/auto_warp.hpp"
#include "adi/audio/warp.hpp"
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
}
