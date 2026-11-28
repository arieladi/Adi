// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/applied_batches.hpp"
#include "adi/clip_commands.hpp"
#include "adi/engine/launcher.hpp"

#include <string_view>

namespace adi::engine {

class Session;

/// Owner-side snapshot. The publication sequence and head are those used to
/// build the graph that supplied the driver's completed render status.
struct SceneCaptureSnapshot {
    LaunchStatusHeader header;
    std::span<const LaunchTrackStatus> tracks;
    std::uint64_t currentIncarnation = 0;
    std::uint64_t publishedSequence = 0;
    std::optional<std::int64_t> publishedHead;
};

struct CapturedSceneTransfer {
    std::int64_t track = 0, oldScene = 0, oldClip = 0, newClip = 0, sourceTick = 0;
    std::uint64_t run = 0;
    bool operator==(const CapturedSceneTransfer&) const = default;
};

struct CaptureInsertedScene {
    clip_edit::PreparedEdit edit;
    AppliedSource source;
    std::int64_t scene = 0;
    std::uint64_t incarnation = 0, statusRevision = 0, sourceSequence = 0;
    std::vector<CapturedSceneTransfer> transfers;
};

/// Snapshot the currently running slots and insert authored copies below the
/// selected scene. When selection is absent, append. No launch gesture is
/// posted while preparing or committing; the published generation can take
/// over using the literal per-track identities in transfers.
[[nodiscard]] CaptureInsertedScene prepareCaptureAndInsertScene(
    Store&, const SceneCaptureSnapshot&, std::optional<std::int64_t> selectedScene,
    std::string_view name = {});

/// Commit all copies and scene placement as a single Undo group. A change of
/// the source Store connection, its publication sequence or head refuses.
[[nodiscard]] CommitResult commitCaptureAndInsertScene(Store&, const CaptureInsertedScene&,
                                                      Actor = Actor::User);

struct CaptureSceneCommitResult {
    CommitResult durable;
    bool retargetQueued = false;
    std::string retargetError;
};

/// Owner entry point for Live's immediate, non-retriggering handover. The
/// durable edit and the driver's gesture admission have independent outcomes.
[[nodiscard]] CaptureSceneCommitResult commitCaptureAndInsertScene(Store&, Session&,
    const CaptureInsertedScene&, Actor = Actor::User);

} // namespace adi::engine
