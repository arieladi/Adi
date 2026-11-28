// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/audio/pitch_edit.hpp"

namespace adi::audio {

using PitchSegmentId = std::uint64_t;

struct PitchSegmentSpan {
    PitchSegmentId id = 0;
    std::uint64_t startFrame = 0;
    std::uint64_t endFrame = 0; // exclusive

    bool operator==(const PitchSegmentSpan&) const = default;
};

struct PitchSegmentation {
    // Ordered source geometry; an explicit empty list removes all segments.
    // Source analysis and pitch curves are derived, rebuildable data.
    std::vector<PitchSegmentSpan> segments;
    PitchSegmentId nextId = 1;

    bool operator==(const PitchSegmentation&) const = default;
};

struct PitchSegmentDraft {
    PitchSegmentation geometry;
    PitchAnalysis analysis; // derived from the immutable source analysis
    PitchEditState edits; // indexed into the effective analysis above
};

struct PitchSegmentDraftResult {
    PitchEditError error = PitchEditError::None;
    std::optional<PitchSegmentDraft> draft;

    [[nodiscard]] explicit operator bool() const noexcept { return error == PitchEditError::None && draft.has_value(); }
};

struct PitchSplitSegment { PitchSegmentId id; std::uint64_t frame; };
struct PitchGlueSegments { PitchSegmentId first; PitchSegmentId last; };
struct PitchDeleteSegment { PitchSegmentId id; };
struct PitchSetSegmentBoundary { PitchSegmentId id; PitchBoundary boundary; std::uint64_t sourceFrame; };
using PitchSegmentationCommand = std::variant<PitchSplitSegment, PitchGlueSegments, PitchDeleteSegment,
                                               PitchSetSegmentBoundary>;

// Structural checks for storage owners which do not have the decoded analysis.
[[nodiscard]] bool validPitchSegmentation(const PitchSegmentation&, std::uint64_t sourceFrames);

// Null geometry uses the automatic notes and assigns initial IDs in source
// order, starting at one. Explicit geometry uses actual observations from
// the automatic analysis to derive pitch summaries. A span can include
// unvoiced source frames and must contain at least one observation. Only
// owner-worker code prepares or changes a draft.
[[nodiscard]] PitchSegmentDraftResult draftPitchSegments(const PitchAnalysis&, const PitchEditState&,
                                                         const PitchSegmentation* geometry = nullptr);

// Apply a structural batch against the same immutable source used to create
// the draft. Split keeps the first ID, glue keeps the first in source order,
// and delete uncovers the original signal. Split children need measurements
// on both sides, and glued spans must touch. Changed segments reset their
// correction controls. Untouched edits retain their stable segment identity.
// Correcting an analyzed source boundary keeps the ID, explicit destination
// timing and existing controls. An unretimed range follows its source and a
// selected straightening range is clipped to the changed source bounds.
[[nodiscard]] PitchEditError applyPitchSegmentation(const PitchAnalysis& source, PitchSegmentDraft&,
                                                    std::span<const PitchSegmentationCommand>);

} // namespace adi::audio
