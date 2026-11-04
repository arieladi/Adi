// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/clip_commands.hpp"
#include "adi/audio/auto_warp.hpp"
#include "adi/audio/warp.hpp"
#include "adi/render/queue.hpp"
namespace adi::clip_edit {
struct MaterialFiles;
struct MaterialEdit {
    PreparedEdit edit;
    std::shared_ptr<MaterialFiles> files;
};
struct ImportOptions {
    std::filesystem::path source;
    std::int64_t track=0,clipId=0,mediaId=0,importedUtc=0;
    TimePoint at;
    bool copyIn=false,warp=false,edgeFades=false; // Create Fades defaults off
    audio::WarpMode warpMode=audio::WarpMode::Beats;
    audio::AutoWarpOptions warpAnalysis;
    std::stop_token stop;
};
/// Off-callback, on this connection's owner. Does not change project rows.
/// Keep the document generation with the job. Hosts keep their thread contract.
[[nodiscard]] MaterialEdit prepareImportAudio(Store&,const ImportOptions&,double referenceRate);
struct BounceOptions {
    render::Job job; // source/range/rate/encoding; project/directory assigned by capture
    ImportOptions destination;
    std::optional<RangeSelection> replace; // explicit Bounce in Place selection
};
/// Captures a private SQLite backup (including WAL) before rendering; the live
/// Session is never processed. The output becomes media+clip in one undo step.
[[nodiscard]] MaterialEdit prepareBounce(Store&,const BounceOptions&,engine::DeviceLoader={},render::Control={});
/// Publication is no-clobber, immediately before SQL commit. Failure removes
/// only files this attempt created. Successful undo retains imported media.
[[nodiscard]] CommitResult commitMaterial(Store&,MaterialEdit&,Actor=Actor::User);
}
