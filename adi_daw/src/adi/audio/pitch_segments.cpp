// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/pitch_segments.hpp"

#include "adi/audio/io_audit.hpp"

#include <algorithm>
#include <limits>
#include <type_traits>
#include <utility>

namespace adi::audio {
namespace {

constexpr auto maximumStoredId = static_cast<PitchSegmentId>(std::numeric_limits<std::int64_t>::max());

auto firstNoteOverlapping(const PitchAnalysis& source, std::uint64_t frame) {
    return std::upper_bound(source.notes.begin(), source.notes.end(), frame,
        [](std::uint64_t from, const PitchNote& note) { return from < note.endFrame; });
}

auto firstPointAt(const PitchNote& note, std::uint64_t frame) {
    return std::lower_bound(note.curve.begin(), note.curve.end(), frame,
        [](const PitchPoint& point, std::uint64_t from) { return point.frame < from; });
}

bool hasObservation(const PitchAnalysis& source, std::uint64_t start, std::uint64_t end) {
    for (auto note = firstNoteOverlapping(source, start); note != source.notes.end() && note->startFrame < end; ++note) {
        const auto point = firstPointAt(*note, start);
        if (point != note->curve.end() && point->frame < end) return true;
    }
    return false;
}

bool resolveNote(const PitchAnalysis& source, const PitchSegmentSpan& span, PitchNote& result, std::vector<double>& scratch) {
    auto note = firstNoteOverlapping(source, span.startFrame);
    if (note == source.notes.end()) return false;
    if (note->startFrame == span.startFrame && note->endFrame == span.endFrame) {
        result = *note;
        return true;
    }

    result.startFrame = span.startFrame;
    result.endFrame = span.endFrame;
    for (; note != source.notes.end() && note->startFrame < span.endFrame; ++note) {
        const auto first = firstPointAt(*note, span.startFrame);
        const auto last = firstPointAt(*note, span.endFrame);
        result.curve.insert(result.curve.end(), first, last);
    }
    return summarizePitchNote(result, scratch);
}

PitchSegmentation originalGeometry(const PitchAnalysis& source) {
    PitchSegmentation geometry;
    geometry.segments.reserve(source.notes.size());
    for (const auto& note : source.notes) geometry.segments.push_back({geometry.nextId++, note.startFrame, note.endFrame});
    return geometry;
}

bool matchesDraft(const PitchSegmentDraft& draft, const PitchAnalysis& source) {
    if (draft.analysis.sampleRate != source.sampleRate || draft.analysis.sourceFrames != source.sourceFrames ||
        draft.geometry.segments.size() != draft.analysis.notes.size()) return false;
    for (std::size_t index = 0; index < draft.analysis.notes.size(); ++index) {
        const auto& note = draft.analysis.notes[index];
        const auto& span = draft.geometry.segments[index];
        if (note.startFrame != span.startFrame || note.endFrame != span.endFrame) return false;
    }
    return true;
}

std::optional<std::size_t> indexOf(const PitchSegmentation& geometry, PitchSegmentId id) {
    const auto position = std::find_if(geometry.segments.begin(), geometry.segments.end(),
        [id](const auto& span) { return span.id == id; });
    if (position == geometry.segments.end()) return {};
    return static_cast<std::size_t>(position - geometry.segments.begin());
}

void replaceEditIndices(PitchEditState& edits, std::size_t first, std::size_t last, std::size_t replacements) {
    std::erase_if(edits.notes, [first, last](const auto& edit) { return first <= edit.noteIndex && edit.noteIndex < last; });
    for (auto& edit : edits.notes) {
        if (edit.noteIndex >= last) edit.noteIndex = edit.noteIndex - (last - first) + replacements;
    }
}

PitchEditError split(const PitchAnalysis& source, PitchSegmentation& geometry, PitchEditState& edits,
                     PitchSplitSegment change) {
    const auto index = indexOf(geometry, change.id);
    if (!index) return PitchEditError::UnknownNote;
    auto& left = geometry.segments[*index];
    if (geometry.nextId == maximumStoredId || change.frame <= left.startFrame || change.frame >= left.endFrame ||
        !hasObservation(source, left.startFrame, change.frame) || !hasObservation(source, change.frame, left.endFrame))
        return PitchEditError::InvalidValue;
    const PitchSegmentSpan right{geometry.nextId++, change.frame, left.endFrame};
    left.endFrame = change.frame;
    geometry.segments.insert(geometry.segments.begin() + static_cast<std::ptrdiff_t>(*index + 1), right);
    replaceEditIndices(edits, *index, *index + 1, 2);
    return PitchEditError::None;
}

PitchEditError glue(PitchSegmentation& geometry, PitchEditState& edits, PitchGlueSegments change) {
    const auto first = indexOf(geometry, change.first);
    const auto last = indexOf(geometry, change.last);
    if (!first || !last) return PitchEditError::UnknownNote;
    if (*first >= *last) return PitchEditError::InvalidValue;
    for (auto index = *first; index < *last; ++index) {
        if (geometry.segments[index].endFrame != geometry.segments[index + 1].startFrame) return PitchEditError::InvalidValue;
    }
    geometry.segments[*first].endFrame = geometry.segments[*last].endFrame;
    geometry.segments.erase(geometry.segments.begin() + static_cast<std::ptrdiff_t>(*first + 1),
                            geometry.segments.begin() + static_cast<std::ptrdiff_t>(*last + 1));
    replaceEditIndices(edits, *first, *last + 1, 1);
    return PitchEditError::None;
}

PitchEditError remove(PitchSegmentation& geometry, PitchEditState& edits, PitchDeleteSegment change) {
    const auto index = indexOf(geometry, change.id);
    if (!index) return PitchEditError::UnknownNote;
    geometry.segments.erase(geometry.segments.begin() + static_cast<std::ptrdiff_t>(*index));
    replaceEditIndices(edits, *index, *index + 1, 0);
    return PitchEditError::None;
}

void reconcileBoundary(PitchNoteEdit& edit, const PitchSegmentSpan& previous, const PitchSegmentSpan& corrected) {
    if (edit.startFrame == previous.startFrame && edit.endFrame == previous.endFrame) {
        edit.startFrame = corrected.startFrame;
        edit.endFrame = corrected.endFrame;
    }
    if (!edit.straightenRange) return;
    const PitchStraightenRange selection{
        std::max(edit.straightenRange->startFrame, corrected.startFrame),
        std::min(edit.straightenRange->endFrame, corrected.endFrame)
    };
    if (selection.startFrame >= selection.endFrame) {
        edit.straighten = 0;
        edit.straightenRange.reset();
    } else {
        edit.straightenRange = selection;
    }
}

PitchEditError correctBoundary(const PitchAnalysis& source, PitchSegmentation& geometry, PitchEditState& edits,
                               PitchSetSegmentBoundary change) {
    const auto index = indexOf(geometry, change.id);
    if (!index) return PitchEditError::UnknownNote;
    auto& span = geometry.segments[*index];
    auto corrected = span;
    switch (change.boundary) {
    case PitchBoundary::Start: corrected.startFrame = change.sourceFrame; break;
    case PitchBoundary::End: corrected.endFrame = change.sourceFrame; break;
    default: return PitchEditError::InvalidValue;
    }
    if (corrected == span) return PitchEditError::None;
    if (corrected.startFrame >= corrected.endFrame || corrected.endFrame > source.sourceFrames)
        return PitchEditError::InvalidValue;
    if ((*index > 0 && geometry.segments[*index - 1].endFrame > corrected.startFrame) ||
        (*index + 1 < geometry.segments.size() && geometry.segments[*index + 1].startFrame < corrected.endFrame))
        return PitchEditError::Overlap;

    const auto edit = std::lower_bound(edits.notes.begin(), edits.notes.end(), *index,
        [](const PitchNoteEdit& row, std::size_t target) { return row.noteIndex < target; });
    if (edit != edits.notes.end() && edit->noteIndex == *index) {
        reconcileBoundary(*edit, span, corrected);
        if (*edit == PitchNoteEdit{*index, corrected.startFrame, corrected.endFrame}) edits.notes.erase(edit);
    }
    span = corrected;
    return PitchEditError::None;
}

} // namespace

bool validPitchSegmentation(const PitchSegmentation& geometry, std::uint64_t sourceFrames) {
    if (onAudioThread || sourceFrames == 0 || geometry.nextId == 0 || geometry.nextId > maximumStoredId) return false;
    std::vector<PitchSegmentId> ids;
    ids.reserve(geometry.segments.size());
    std::uint64_t previousEnd = 0;
    for (const auto& span : geometry.segments) {
        if (span.startFrame < previousEnd || span.endFrame <= span.startFrame || span.endFrame > sourceFrames ||
            span.id == 0 || span.id >= geometry.nextId) return false;
        ids.push_back(span.id);
        previousEnd = span.endFrame;
    }
    std::sort(ids.begin(), ids.end());
    return std::adjacent_find(ids.begin(), ids.end()) == ids.end();
}

PitchSegmentDraftResult draftPitchSegments(const PitchAnalysis& source, const PitchEditState& edits,
                                           const PitchSegmentation* geometry) {
    if (onAudioThread) return {PitchEditError::Realtime, {}};
    if (!validPitchAnalysis(source)) return {PitchEditError::InvalidAnalysis, {}};
    if (!geometry) {
        const auto validation = validatePitchEdits(source, edits);
        if (validation != PitchEditError::None) return {validation, {}};
        return {PitchEditError::None, PitchSegmentDraft{originalGeometry(source), source, edits}};
    }
    if (!validPitchSegmentation(*geometry, source.sourceFrames)) return {PitchEditError::InvalidState, {}};
    PitchAnalysis effective{source.sampleRate, source.sourceFrames, {}, source.boundaryUncertainty};
    effective.notes.reserve(geometry->segments.size());
    std::vector<double> scratch;
    for (const auto& span : geometry->segments) {
        PitchNote note;
        if (!resolveNote(source, span, note, scratch)) return {PitchEditError::InvalidValue, {}};
        effective.notes.push_back(std::move(note));
    }
    const auto validation = validatePitchEdits(effective, edits);
    if (validation != PitchEditError::None) return {validation, {}};
    return {PitchEditError::None, PitchSegmentDraft{*geometry, std::move(effective), edits}};
}

PitchEditError applyPitchSegmentation(const PitchAnalysis& source, PitchSegmentDraft& draft,
                                      std::span<const PitchSegmentationCommand> commands) {
    if (onAudioThread) return PitchEditError::Realtime;
    if (!validPitchAnalysis(source)) return PitchEditError::InvalidAnalysis;
    if (!matchesDraft(draft, source) || !validPitchSegmentation(draft.geometry, source.sourceFrames))
        return PitchEditError::InvalidState;
    const auto initial = validatePitchEdits(draft.analysis, draft.edits);
    if (initial != PitchEditError::None || commands.empty()) return initial;

    auto geometry = draft.geometry;
    auto edits = draft.edits;
    for (const auto& command : commands) {
        const auto error = std::visit([&](const auto& change) {
            using Change = std::decay_t<decltype(change)>;
            if constexpr (std::is_same_v<Change, PitchSplitSegment>) return split(source, geometry, edits, change);
            else if constexpr (std::is_same_v<Change, PitchGlueSegments>) return glue(geometry, edits, change);
            else if constexpr (std::is_same_v<Change, PitchDeleteSegment>) return remove(geometry, edits, change);
            else return correctBoundary(source, geometry, edits, change);
        }, command);
        if (error != PitchEditError::None) return error;
    }
    auto updated = draftPitchSegments(source, edits, &geometry);
    if (updated) draft = std::move(*updated.draft);
    return updated.error;
}

} // namespace adi::audio
