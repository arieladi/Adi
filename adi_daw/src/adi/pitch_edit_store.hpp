// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/audio/pitch_segments.hpp"
#include "adi/ops.hpp"

#include <map>
#include <string_view>

namespace adi {

[[nodiscard]] std::span<const OpDescriptor> pitchEditStoreOps();

namespace media { struct ClipPitchAnalysis; }

namespace clip_edit {

struct StoredPitchNoteEdit {
    std::uint64_t sourceStartFrame = 0, sourceEndFrame = 0;
    audio::PitchNoteEdit edit;
    bool operator==(const StoredPitchNoteEdit&) const = default;
};

struct ClipPitchEdits {
    std::int64_t clipId = 0;
    std::string sourceHash;
    std::uint32_t sampleRate = 0, analysisVersion = 0, analysisConfiguration = 0;
    std::uint64_t sourceFrames = 0;
    std::vector<StoredPitchNoteEdit> rows;
    std::optional<audio::PitchSegmentation> segmentation{};
    bool operator==(const ClipPitchEdits&) const = default;
};

struct ClipPitchEditsRead {
    std::optional<ClipPitchEdits> edits;
    std::string error;
    // The typed source still identifies a dormant record if its payload is corrupt.
    std::optional<std::string> failedSourceHash{};
    [[nodiscard]] bool blocksSource(std::string_view hash) const noexcept {
        return !error.empty() && (!failedSourceHash || *failedSourceHash == hash);
    }
};

struct ProjectPitchEditsRead {
    std::optional<std::map<std::int64_t, ClipPitchEditsRead>> clips;
    std::string error;
};

struct ClipPitchEditsResolve {
    std::optional<audio::PitchEditState> state;
    std::string error;
    // Geometry is projected from the immutable source analysis. Consumers
    // use these note bounds when present, including an explicit empty list.
    std::optional<audio::PitchAnalysis> adjustedAnalysis{};
};

struct ClipPitchEditRequest {
    std::optional<OpRequest> request;
    std::string error;
};

// Owner-side read of authored data. An absent row (including projects captured
// before this schema was installed) is an empty result with no error.
[[nodiscard]] ClipPitchEditsRead readClipPitchEdits(const Store&, std::int64_t clipId);
// Capture all authored rows in one ordered scan. Row errors identify their
// clip. The outer error is reserved for failure to read the table itself.
[[nodiscard]] ProjectPitchEditsRead readProjectPitchEdits(const Store&);

// Worker or owner. Source identity and each original bound must match before
// the stored destination edits can be evaluated against these analyzed notes.
[[nodiscard]] ClipPitchEditsResolve resolveClipPitchEdits(
    const ClipPitchEdits&, const audio::PitchAnalysis&, std::string_view sourceHash,
    std::uint32_t analysisVersion, std::uint32_t analysisConfiguration);
[[nodiscard]] ClipPitchEditsResolve resolveClipPitchEdits(const ClipPitchEdits&, const media::ClipPitchAnalysis&);

// Prepare one explicit durable replacement, including a clear when the state
// is empty. Journal commit checks the clip's source again in its transaction.
[[nodiscard]] ClipPitchEditRequest prepareClipPitchEdits(const media::ClipPitchAnalysis&, const audio::PitchEditState&,
                                                          const audio::PitchSegmentation* = nullptr);

} // namespace clip_edit
} // namespace adi
