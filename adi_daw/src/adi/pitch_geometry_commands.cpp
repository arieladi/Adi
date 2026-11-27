// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/pitch_geometry_commands.hpp"

#include "adi/audio/io_audit.hpp"
#include "adi/media/clip_pitch.hpp"
#include "adi/pitch_edit_store.hpp"
#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <algorithm>
#include <array>
#include <numeric>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace adi::clip_edit {
namespace {

using namespace audio;

std::string_view editError(PitchEditError error) noexcept {
    switch (error) {
    case PitchEditError::UnknownNote: return "Pitch segment no longer exists";
    case PitchEditError::Overlap: return "Pitch segment geometry or destination overlaps another segment";
    case PitchEditError::InvalidValue: return "Pitch segment change needs ordered bounds and voiced measurements";
    default: return "Pitch segment draft is invalid";
    }
}

void requireSource(SQLite::Database& database, const media::ClipPitchAnalysis& captured) {
    SQLite::Statement row(database,
        "SELECT m.hash_blake3,m.sample_rate,m.frames,m.channels,a.src_start_frames,a.src_len_frames,a.reverse "
        "FROM clips c JOIN audio_clips a ON c.id=a.clip_id JOIN media_files m ON a.media_id=m.id "
        "WHERE c.id=? AND c.kind='audio'");
    row.bind(1, captured.clipId);
    if (!row.executeStep()) throw std::runtime_error("Pitch segment action requires a sourced audio clip");
    const auto matches = [&](int column, std::uint64_t expected) {
        const auto value = row.getColumn(column);
        return value.isNull() || (value.getInt64() >= 0 && static_cast<std::uint64_t>(value.getInt64()) == expected);
    };
    const auto start = row.getColumn(4).getInt64();
    const auto length = row.getColumn(5).getInt64();
    if (row.getColumn(0).getString() != captured.source.hash || !matches(1, captured.source.sampleRate) ||
        !matches(2, captured.source.frames) || !matches(3, captured.source.channels) ||
        start < 0 || length <= 0 || static_cast<std::uint64_t>(start) != captured.sourceStartFrame ||
        captured.sourceEndFrame <= captured.sourceStartFrame ||
        static_cast<std::uint64_t>(length) != captured.sourceEndFrame - captured.sourceStartFrame ||
        (row.getColumn(6).getInt() != 0) != captured.reversed)
        throw std::runtime_error("Pitch segment source or clip window changed after analysis");
}

std::size_t indexOf(const PitchSegmentation& geometry, PitchSegmentId id) {
    const auto found = std::find_if(geometry.segments.begin(), geometry.segments.end(),
        [id](const auto& span) { return span.id == id; });
    if (found == geometry.segments.end()) throw std::runtime_error("Pitch segment no longer exists");
    return static_cast<std::size_t>(found - geometry.segments.begin());
}

bool hasConstantCorrection(const PitchNoteEdit& edit) noexcept {
    return edit.straighten == 0 && !edit.straightenRange && edit.drift == 1 && edit.transitionFrames == 0 &&
           edit.tiltCents == 0 && edit.tiltGeometry.startCents == 0 && edit.tiltGeometry.endCents == 0;
}

PitchNoteEdit controls(PitchNoteEdit edit) noexcept {
    edit.noteIndex = 0;
    edit.startFrame = 0;
    edit.endFrame = 0;
    return edit;
}

// Reduced frame ratios permit exact affine timing checks without multiplying
// large source and destination frame counts.
std::pair<std::uint64_t, std::uint64_t> timingRatio(const PitchSegmentSpan& span, const PitchNoteEdit& edit) noexcept {
    const auto sourceLength = span.endFrame - span.startFrame;
    const auto destinationLength = edit.endFrame - edit.startFrame;
    const auto divisor = std::gcd(sourceLength, destinationLength);
    return {sourceLength / divisor, destinationLength / divisor};
}

void applyShape(const PitchAnalysis& source, PitchSegmentDraft& draft, const PitchSegmentationCommand& change) {
    const auto result = applyPitchSegmentation(source, draft, {&change, 1});
    if (result != PitchEditError::None) throw std::runtime_error(std::string(editError(result)));
}

void replaceEdits(PitchSegmentDraft& draft, const PitchEditState& previous, std::size_t first, std::size_t last,
                  std::span<const PitchNoteEdit> replacements) {
    const auto beforeFirst = std::lower_bound(previous.notes.begin(), previous.notes.end(), first,
        [](const auto& edit, auto index) { return edit.noteIndex < index; });
    const auto afterLast = std::upper_bound(beforeFirst, previous.notes.end(), last,
        [](auto index, const auto& edit) { return index < edit.noteIndex; });
    auto& edits = draft.edits.notes;
    edits.reserve(previous.notes.size() + replacements.size());
    edits.insert(edits.end(), previous.notes.begin(), beforeFirst);
    for (const auto& edit : replacements) {
        const auto& note = draft.analysis.notes[edit.noteIndex];
        if (edit != PitchNoteEdit{edit.noteIndex, note.startFrame, note.endFrame}) edits.push_back(edit);
    }
    for (auto row = afterLast; row != previous.notes.end(); ++row) {
        auto shifted = *row;
        shifted.noteIndex = first + replacements.size() + (row->noteIndex - last - 1);
        edits.push_back(std::move(shifted));
    }
}

void split(const PitchAnalysis& source, PitchSegmentDraft& draft, PitchSplitSegment change) {
    const auto index = indexOf(draft.geometry, change.id);
    const auto& span = draft.geometry.segments[index];
    if (change.frame <= span.startFrame || change.frame >= span.endFrame)
        throw std::runtime_error("Pitch split must be inside the source segment");
    const auto edit = resolvePitchNoteEdit(draft.analysis, draft.edits, index);
    if (!hasConstantCorrection(edit)) throw std::runtime_error("Pitch split cannot preserve this segment's contour correction");
    const auto [sourceUnit, destinationUnit] = timingRatio(span, edit);
    const auto sourceOffset = change.frame - span.startFrame;
    if (sourceOffset % sourceUnit != 0)
        throw std::runtime_error("Pitch split does not fall on an exact destination frame");
    const auto destinationSplit = edit.startFrame + (sourceOffset / sourceUnit) * destinationUnit;
    auto left = edit;
    left.endFrame = destinationSplit;
    auto right = edit;
    right.noteIndex = index + 1;
    right.startFrame = destinationSplit;

    // Changed ranges can keep the old destination even if an unedited source
    // placeholder at that position would conflict with another edited note.
    const auto previous = std::exchange(draft.edits, {});
    applyShape(source, draft, change);
    const std::array replacements{left, right};
    replaceEdits(draft, previous, index, index, replacements);
}

void merge(const PitchAnalysis& source, PitchSegmentDraft& draft, PitchGlueSegments change) {
    const auto first = indexOf(draft.geometry, change.first);
    const auto last = indexOf(draft.geometry, change.last);
    if (first >= last) throw std::runtime_error("Pitch merge needs at least two segments in source order");
    auto merged = resolvePitchNoteEdit(draft.analysis, draft.edits, first);
    if (!hasConstantCorrection(merged)) throw std::runtime_error("Pitch merge cannot preserve these contour corrections");
    const auto initialControls = controls(merged);
    const auto& spans = draft.geometry.segments;
    const auto ratio = timingRatio(spans[first], merged);
    for (auto index = first + 1; index <= last; ++index) {
        const auto edit = resolvePitchNoteEdit(draft.analysis, draft.edits, index);
        if (spans[index - 1].endFrame != spans[index].startFrame)
            throw std::runtime_error("Pitch merge needs touching source segments");
        if (controls(edit) != initialControls)
            throw std::runtime_error("Pitch merge cannot preserve different note corrections");
        if (edit.startFrame != merged.endFrame || timingRatio(spans[index], edit) != ratio)
            throw std::runtime_error("Pitch merge cannot preserve discontinuous destination timing");
        merged.endFrame = edit.endFrame;
    }
    const auto previous = std::exchange(draft.edits, {});
    applyShape(source, draft, change);
    replaceEdits(draft, previous, first, last, {&merged, 1});
}

PitchGeometryEdit prepare(const Store& store, const media::ClipPitchAnalysis& source,
                          const PitchSegmentationCommand& change, const char* opType, const char* label,
                          PitchGeometryCorrections corrections = PitchGeometryCorrections::PreserveExact) {
    if (onAudioThread) return {{}, "audio callback"};
    if (corrections != PitchGeometryCorrections::PreserveExact && corrections != PitchGeometryCorrections::ResetChanged)
        return {{}, "invalid pitch geometry correction policy"};
    try {
        auto& database = const_cast<SQLite::Database&>(store.db());
        std::optional<SQLite::Transaction> snapshot;
        if (sqlite3_get_autocommit(database.getHandle()) != 0)
            snapshot.emplace(database, SQLite::TransactionBehavior::DEFERRED);
        requireSource(database, source);
        const auto saved = readClipPitchEdits(store, source.clipId);
        if (saved.blocksSource(source.source.hash)) return {{}, saved.error};
        PitchSegmentDraftResult draft;
        if (saved.edits && saved.edits->sourceHash == source.source.hash) {
            auto resolved = resolveClipPitchEdits(*saved.edits, source);
            if (!resolved.state) return {{}, std::move(resolved.error)};
            if (resolved.adjustedAnalysis && saved.edits->segmentation) {
                draft.draft = PitchSegmentDraft{*saved.edits->segmentation, std::move(*resolved.adjustedAnalysis),
                                               std::move(*resolved.state)};
            } else {
                draft = draftPitchSegments(source.analysis, *resolved.state);
            }
        } else {
            draft = draftPitchSegments(source.analysis, {});
        }
        if (!draft) return {{}, std::string(editError(draft.error))};

        PreparedEdit prepared;
        {
            SQLite::Statement path(database, "SELECT file FROM pragma_database_list WHERE name='main'");
            if (!path.executeStep()) return {{}, "Pitch segment action needs a project connection"};
            prepared.sourceFile = path.getColumn(0).getString();
        }
        prepared.options.expectHead.emplace(OpJournal(const_cast<Store&>(store)).headSeq());
        auto& geometry = *draft.draft;
        if (corrections == PitchGeometryCorrections::ResetChanged) {
            applyShape(source.analysis, geometry, change);
        } else if (const auto* cut = std::get_if<PitchSplitSegment>(&change)) {
            split(source.analysis, geometry, *cut);
        } else if (const auto* glue = std::get_if<PitchGlueSegments>(&change)) {
            merge(source.analysis, geometry, *glue);
        } else {
            const auto prior = geometry.geometry;
            applyShape(source.analysis, geometry, change);
            if (prior == geometry.geometry) {
                if (snapshot) snapshot->commit();
                prepared.ok = true;
                return {std::move(prepared), {}};
            }
        }
        auto request = prepareClipPitchEdits(source, geometry.edits, &geometry.geometry);
        if (!request.request) return {{}, std::move(request.error)};
        request.request->opType = opType;
        request.request->label = label;
        prepared.requests.push_back(std::move(*request.request));
        if (snapshot) snapshot->commit();
        prepared.ok = true;
        return {std::move(prepared), {}};
    } catch (const std::exception& error) {
        return {{}, error.what()};
    }
}

} // namespace

PitchGeometryEdit prepareSplitPitchSegment(const Store& store, const media::ClipPitchAnalysis& source,
                                          PitchSplitSegment change, PitchGeometryCorrections corrections) {
    return prepare(store, source, change, "clip.splitPitchSegment", "Split pitch segment", corrections);
}

PitchGeometryEdit prepareMergePitchSegments(const Store& store, const media::ClipPitchAnalysis& source,
                                           PitchGlueSegments change, PitchGeometryCorrections corrections) {
    return prepare(store, source, change, "clip.mergePitchSegments", "Merge pitch segments", corrections);
}

PitchGeometryEdit prepareSetPitchSegmentBoundary(const Store& store, const media::ClipPitchAnalysis& source,
                                                 PitchSetSegmentBoundary change) {
    return prepare(store, source, change, "clip.setPitchEdits", change.boundary == PitchBoundary::Start ?
        "Set pitch segment start" : "Set pitch segment end");
}

} // namespace adi::clip_edit
