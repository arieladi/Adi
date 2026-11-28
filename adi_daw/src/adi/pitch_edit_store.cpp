// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/pitch_edit_store.hpp"

#include "adi/audio/io_audit.hpp"
#include "adi/media/clip_pitch.hpp"
#include "adi/row_capture.hpp"
#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace adi {
namespace {

using clip_edit::ClipPitchEdits;
using clip_edit::ClipPitchEditsRead;
using clip_edit::StoredPitchNoteEdit;
constexpr auto table = "clip_pitch_edits";
constexpr auto clipKey = "clip_id";
constexpr auto opType = "clip.setPitchEdits";
constexpr std::size_t maxRows = 32768;
constexpr std::size_t maxEditBytes = 16u * 1024 * 1024;
constexpr auto lowestRate = 8000;
constexpr auto highestRate = 384000;
constexpr auto maxSqlInt = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());

constexpr std::array rangeFields = {
    Field{"start", FieldType::Int, true, "First original source frame", {}, 0},
    Field{"end", FieldType::Int, true, "Exclusive original source frame", {}, 1},
};
constexpr std::array rowFields = {
    Field{"note", FieldType::Int, true, "Original source analysis note index", {}, 0},
    Field{"sourceStart", FieldType::Int, true, "Original note start frame", {}, 0},
    Field{"sourceEnd", FieldType::Int, true, "Original exclusive note end frame", {}, 1},
    Field{"start", FieldType::Int, true, "Edited note start frame", {}, 0},
    Field{"end", FieldType::Int, true, "Edited exclusive note end frame", {}, 1},
    Field{"pitch", FieldType::Real, true, "Pitch shift in cents"},
    Field{"straighten", FieldType::Real, true, "Fraction of deviation to remove", {}, 0, 1},
    Field{"straightenRange", FieldType::Object, false, "Original-frame range to straighten", {}, {}, {}, {}, {}, rangeFields},
    Field{"tilt", FieldType::Real, true, "Rotation delta in cents"},
    Field{"tiltAnchor", FieldType::Real, true, "Position of tilt anchor", {}, 0, 1},
    Field{"tiltStart", FieldType::Real, true, "Start-side tilt in cents"},
    Field{"tiltEnd", FieldType::Real, true, "End-side tilt in cents"},
    Field{"formant", FieldType::Real, true, "Formant shift in cents"},
    Field{"gain", FieldType::Real, true, "Finite gain in dB, zero when muted"},
    Field{"muted", FieldType::Bool, true, "Mute note"},
    Field{"drift", FieldType::Real, true, "Fraction of drift retained", {}, 0, 1},
    Field{"preserveExpression", FieldType::Bool, true, "Retain nonlinear drift residual"},
    Field{"transition", FieldType::Int, true, "Incoming transition in destination frames", {}, 0},
};
constexpr std::array segmentFields = {
    Field{"id", FieldType::Int, true, "Stable segment identity", {}, 1},
    Field{"start", FieldType::Int, true, "First original source frame", {}, 0},
    Field{"end", FieldType::Int, true, "Exclusive original source frame", {}, 1},
};
constexpr std::array segmentationFields = {
    Field{"nextId", FieldType::Int, true, "Next available segment identity", {}, 1},
    Field{"segments", FieldType::Array, true, "Complete segment geometry in source order",
          {}, {}, {}, {}, maxRows, segmentFields},
};
constexpr std::array fields = {
    Field{"id", FieldType::Int, true, "Audio clip identity", {}, 1},
    Field{"hash", FieldType::Text, true, "Source BLAKE3 in lowercase hex", {}, {}, {}, 64},
    Field{"sampleRate", FieldType::Int, true, "Original sample rate", {}, lowestRate, highestRate},
    Field{"sourceFrames", FieldType::Int, true, "Entire source frame count", {}, 1},
    Field{"version", FieldType::Int, true, "Source analysis version", {}, 1, UINT32_MAX},
    Field{"configuration", FieldType::Int, true, "Source analysis configuration", {}, 0, UINT32_MAX},
    Field{"rows", FieldType::Array, true, "Complete replacement in original note order", {}, {}, {}, {}, maxRows, rowFields},
    Field{"segmentation", FieldType::Object, false, "Authored note geometry in the original source clock",
          {}, {}, {}, {}, {}, segmentationFields},
};
constexpr auto geometryFields = [] {
    auto result = fields;
    result.back().required = true;
    return result;
}();

std::uint64_t frame(const Payload& value) {
    const auto index = detail::signedInteger(value);
    if (index < 0) throw std::runtime_error("pitch edit requires a nonnegative integer");
    return static_cast<std::uint64_t>(index);
}

std::uint32_t uint32(const Payload& value) {
    const auto number = frame(value);
    if (number > UINT32_MAX) throw std::runtime_error("pitch edit identity exceeds its integer range");
    return static_cast<std::uint32_t>(number);
}

void validateIdentity(const ClipPitchEdits& edits) {
    const auto hex = [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); };
    if (edits.clipId <= 0 || edits.sourceHash.size() != 64 ||
        !std::all_of(edits.sourceHash.begin(), edits.sourceHash.end(), hex) ||
        edits.sampleRate < lowestRate || edits.sampleRate > highestRate || edits.analysisVersion == 0 ||
        edits.sourceFrames == 0 || edits.sourceFrames > maxSqlInt)
        throw std::runtime_error("invalid clip pitch source identity");
}

double finite(const Payload& value) {
    if (!value.is_number()) throw std::runtime_error("pitch edit control requires a number");
    const auto result = value.get<double>();
    if (!std::isfinite(result)) throw std::runtime_error("nonfinite pitch edit control");
    return result == 0 ? 0 : result;
}

Payload encodeRows(const std::vector<StoredPitchNoteEdit>& rows) {
    Payload payload = Payload::array();
    for (const auto& row : rows) {
        const auto& edit = row.edit;
        const auto muted = edit.gainDb == -std::numeric_limits<double>::infinity();
        auto value = Payload{
            {"note", edit.noteIndex}, {"sourceStart", row.sourceStartFrame}, {"sourceEnd", row.sourceEndFrame},
            {"start", edit.startFrame}, {"end", edit.endFrame}, {"pitch", edit.pitchShiftCents},
            {"straighten", edit.straighten}, {"tilt", edit.tiltCents}, {"tiltAnchor", edit.tiltGeometry.anchor},
            {"tiltStart", edit.tiltGeometry.startCents}, {"tiltEnd", edit.tiltGeometry.endCents},
            {"formant", edit.formantShiftCents}, {"gain", muted ? 0 : edit.gainDb}, {"muted", muted},
            {"drift", edit.drift}, {"preserveExpression", edit.preserveExpression}, {"transition", edit.transitionFrames},
        };
        if (edit.straightenRange)
            value["straightenRange"] = {{"start", edit.straightenRange->startFrame}, {"end", edit.straightenRange->endFrame}};
        payload.push_back(std::move(value));
    }
    return payload;
}

StoredPitchNoteEdit decodeRow(const Payload& value) {
    StoredPitchNoteEdit row;
    row.sourceStartFrame = frame(value.at("sourceStart"));
    row.sourceEndFrame = frame(value.at("sourceEnd"));
    const auto note = frame(value.at("note"));
    if (note > std::numeric_limits<std::size_t>::max()) throw std::runtime_error("pitch note index exceeds range");
    auto& edit = row.edit;
    edit.noteIndex = static_cast<std::size_t>(note);
    edit.startFrame = frame(value.at("start"));
    edit.endFrame = frame(value.at("end"));
    edit.pitchShiftCents = finite(value.at("pitch"));
    edit.straighten = finite(value.at("straighten"));
    if (value.contains("straightenRange")) {
        const auto& range = value.at("straightenRange");
        edit.straightenRange = audio::PitchStraightenRange{frame(range.at("start")), frame(range.at("end"))};
    }
    edit.tiltCents = finite(value.at("tilt"));
    edit.tiltGeometry = {finite(value.at("tiltAnchor")), finite(value.at("tiltStart")), finite(value.at("tiltEnd"))};
    edit.formantShiftCents = finite(value.at("formant"));
    edit.gainDb = finite(value.at("gain"));
    if (value.at("muted").get<bool>()) {
        if (edit.gainDb != 0) throw std::runtime_error("muted pitch edit requires zero stored gain");
        edit.gainDb = -std::numeric_limits<double>::infinity();
    }
    edit.drift = finite(value.at("drift"));
    edit.preserveExpression = value.at("preserveExpression").get<bool>();
    edit.transitionFrames = frame(value.at("transition"));
    return row;
}

Payload encodeSegmentation(const audio::PitchSegmentation& geometry) {
    Payload segments = Payload::array();
    for (const auto& span : geometry.segments) {
        segments.push_back({{"id", span.id}, {"start", span.startFrame}, {"end", span.endFrame}});
    }
    return {{"nextId", geometry.nextId}, {"segments", std::move(segments)}};
}

audio::PitchSegmentation decodeSegmentation(const Payload& payload) {
    audio::PitchSegmentation geometry;
    geometry.nextId = frame(payload.at("nextId"));
    const auto& spans = payload.at("segments");
    if (!spans.is_array() || spans.size() > maxRows) throw std::runtime_error("invalid clip pitch segment list");
    geometry.segments.reserve(spans.size());
    for (const auto& span : spans) {
        geometry.segments.push_back({frame(span.at("id")), frame(span.at("start")), frame(span.at("end"))});
    }
    return geometry;
}

bool matchesGeometry(const StoredPitchNoteEdit& row, const audio::PitchSegmentation& geometry) noexcept {
    if (row.edit.noteIndex >= geometry.segments.size()) return false;
    const auto& span = geometry.segments[row.edit.noteIndex];
    return row.sourceStartFrame == span.startFrame && row.sourceEndFrame == span.endFrame;
}

void validateRows(const ClipPitchEdits& edits) {
    if (edits.rows.size() > maxRows) throw std::runtime_error("too many pitch edit rows");
    if (edits.segmentation && !audio::validPitchSegmentation(*edits.segmentation, edits.sourceFrames))
        throw std::runtime_error("invalid clip pitch segmentation");
    std::uint64_t previousEnd = 0;
    std::optional<std::size_t> previousNote;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> destinations;
    destinations.reserve(edits.rows.size());
    for (const auto& row : edits.rows) {
        const auto& edit = row.edit;
        const auto original = audio::PitchNote{.startFrame=row.sourceStartFrame, .endFrame=row.sourceEndFrame, .curve={}};
        if (row.sourceStartFrame < previousEnd || (previousNote && edit.noteIndex <= *previousNote) ||
            edit.noteIndex >= edits.sourceFrames ||
            (edits.segmentation && !matchesGeometry(row, *edits.segmentation)) ||
            !audio::validPitchNoteEdit(original, edit, edits.sourceFrames, edits.sampleRate))
            throw std::runtime_error("invalid clip pitch note identity or edit");
        previousEnd = row.sourceEndFrame;
        previousNote = edit.noteIndex;
        destinations.emplace_back(edit.startFrame, edit.endFrame);
    }
    std::sort(destinations.begin(), destinations.end());
    if (std::adjacent_find(destinations.begin(), destinations.end(), [](const auto& left, const auto& right) {
        return left.second > right.first;
    }) != destinations.end()) throw std::runtime_error("clip pitch edit destinations overlap");
}

void decodeRows(ClipPitchEdits& edits, const Payload& payload) {
    if (!payload.is_array() || payload.size() > maxRows) throw std::runtime_error("invalid clip pitch row list");
    edits.rows.reserve(payload.size());
    for (const auto& value : payload) edits.rows.push_back(decodeRow(value));
    validateRows(edits);
}

std::vector<std::byte> encodeRecord(const ClipPitchEdits& edits) {
    auto rows = encodeRows(edits.rows);
    const auto record = edits.segmentation
        ? Payload{{"rows", std::move(rows)}, {"segmentation", encodeSegmentation(*edits.segmentation)}}
        : std::move(rows);
    auto bytes = encodePayload(record);
    if (bytes.size() > maxEditBytes) throw std::runtime_error("clip pitch edits exceed storage limit");
    return bytes;
}

// Preflight the definite CBOR envelope before giving project bytes to the
// recursive decoder. Key bytes and float words are skipped without decoding.
bool boundedRecord(std::span<const std::uint8_t> bytes) noexcept {
    std::array<std::size_t, 5> remaining{1};
    std::size_t depth = 0;
    while (!bytes.empty()) {
        if (remaining[depth] == 0) return false;
        --remaining[depth];
        const auto major = bytes.front() >> 5;
        const auto sizeCode = bytes.front() & 31;
        bytes = bytes.subspan(1);
        if (sizeCode > 27 || major == 2 || major == 6 || (depth == 0 && major != 4 && major != 5)) return false;

        auto value = static_cast<std::uint64_t>(sizeCode);
        if (sizeCode >= 24) {
            const auto width = std::size_t{1} << (sizeCode - 24);
            if (width > bytes.size()) return false;
            value = 0;
            for (const auto byte : bytes.first(width)) value = (value << 8) | byte;
            bytes = bytes.subspan(width);
        }
        if (major == 4) {
            if (depth == remaining.size() - 1 || value > maxRows) return false;
            remaining[++depth] = static_cast<std::size_t>(value);
        } else if (major == 5) {
            if (depth == remaining.size() - 1 || value > rowFields.size()) return false;
            remaining[++depth] = static_cast<std::size_t>(value) * 2;
        } else {
            if (major == 3) {
                if (value > std::string_view("preserveExpression").size() || value > bytes.size()) return false;
                bytes = bytes.subspan(static_cast<std::size_t>(value));
            } else if (major == 7 && sizeCode != 20 && sizeCode != 21 && sizeCode < 25) {
                return false;
            }
        }
        while (depth > 0 && remaining[depth] == 0) --depth;
    }
    return depth == 0 && remaining.front() == 0;
}

ClipPitchEdits decodeRecord(SQLite::Statement& row) {
    ClipPitchEdits edits;
    edits.clipId = row.getColumn(0).getInt64();
    edits.sourceHash = row.getColumn(1).getString();
    edits.sampleRate = uint32(row.getColumn(2).getInt64());
    edits.sourceFrames = frame(row.getColumn(3).getInt64());
    edits.analysisVersion = uint32(row.getColumn(4).getInt64());
    edits.analysisConfiguration = uint32(row.getColumn(5).getInt64());
    validateIdentity(edits);
    const auto blob = row.getColumn(6);
    const auto size = static_cast<std::size_t>(blob.getBytes());
    if (size == 0 || size > maxEditBytes || blob.getType() != SQLITE_BLOB)
        throw std::runtime_error("invalid clip pitch edit storage size");
    const std::span bytes(static_cast<const std::byte*>(blob.getBlob()), size);
    const std::span cbor(static_cast<const std::uint8_t*>(blob.getBlob()), size);
    if (!boundedRecord(cbor))
        throw std::runtime_error("invalid clip pitch edit record structure or nesting");
    const auto record = Payload::from_cbor(cbor.begin(), cbor.end());
    if (record.is_object()) edits.segmentation = decodeSegmentation(record.at("segmentation"));
    decodeRows(edits, record.is_object() ? record.at("rows") : record);
    if ((!edits.segmentation && edits.rows.empty()) || !std::ranges::equal(encodeRecord(edits), bytes))
        throw std::runtime_error("noncanonical clip pitch edit record");
    return edits;
}

constexpr auto selectRows =
    "SELECT clip_id, source_hash, sample_rate, source_frames, analysis_version, analysis_configuration, edits "
    "FROM clip_pitch_edits";

void requireSchema(OpContext& context) {
    if (!context.db.tableExists(table)) throw std::runtime_error("clip pitch edit schema is unavailable");
}

void requireCurrentSource(SQLite::Database& db, const ClipPitchEdits& edits) {
    SQLite::Statement target(db, "SELECT m.hash_blake3,m.sample_rate,m.frames,a.src_start_frames,a.src_len_frames "
        "FROM clips c JOIN audio_clips a ON a.clip_id=c.id JOIN media_files m ON m.id=a.media_id "
        "WHERE c.id=? AND c.kind='audio'");
    target.bind(1, edits.clipId);
    if (!target.executeStep()) throw std::runtime_error("pitch edit requires an existing sourced audio clip");
    const auto rate = target.getColumn(1);
    const auto frames = target.getColumn(2);
    const auto start = target.getColumn(3).getInt64();
    const auto length = target.getColumn(4).getInt64();
    if (target.getColumn(0).getString() != edits.sourceHash ||
        (!rate.isNull() && rate.getInt64() != static_cast<std::int64_t>(edits.sampleRate)) ||
        (!frames.isNull() && frames.getInt64() != static_cast<std::int64_t>(edits.sourceFrames)) ||
        start < 0 || length <= 0 || static_cast<std::uint64_t>(start) > edits.sourceFrames ||
        static_cast<std::uint64_t>(length) > edits.sourceFrames - static_cast<std::uint64_t>(start))
        throw std::runtime_error("clip pitch source changed after analysis");
}

bool buildInverse(OpContext& context, const Payload& payload, Payload& inverse, std::string& error) {
    if (audio::onAudioThread) { error = "audio callback"; return false; }
    try {
        requireSchema(context);
        const auto id = detail::signedInteger(payload.at("id"));
        inverse = {{"id", id}, {"_restore", detail::captureRow(context.db, table, clipKey, id).value_or(nullptr)}};
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

template<std::size_t MinimumSegments>
bool apply(OpContext& context, const Payload& payload, std::string& error);

constexpr std::array descriptors = {
    OpDescriptor{opType, "Replace an audio clip's pitch edit rows", Scope::Edit, EngineImpact::Snapshot,
                 fields, false, false, apply<0>, buildInverse, {}},
    OpDescriptor{"clip.splitPitchSegment", "Split a pitch segment using its resolved clip image", Scope::Edit,
                 EngineImpact::Snapshot, geometryFields, false, false, apply<2>, buildInverse, {}},
    OpDescriptor{"clip.mergePitchSegments", "Merge pitch segments using their resolved clip image", Scope::Edit,
                 EngineImpact::Snapshot, geometryFields, false, false, apply<1>, buildInverse, {}},
};

template<std::size_t MinimumSegments>
bool apply(OpContext& context, const Payload& payload, std::string& error) {
    if (audio::onAudioThread) { error = "audio callback"; return false; }
    try {
        requireSchema(context);
        const auto id = detail::signedInteger(payload.at("id"));
        if (context.replay && context.replay->asInverse) {
            const auto& restore = payload.at("_restore");
            if (!restore.is_null() && detail::signedInteger(restore.at(clipKey)) != id)
                throw std::runtime_error("captured pitch edits belong to a different clip");
            detail::eraseRowKey(context.db, table, {{clipKey, id}});
            if (!restore.is_null()) detail::restoreRow(context.db, table, restore);
            return true;
        }
        ClipPitchEdits edits{id, payload.at("hash").get<std::string>(), uint32(payload.at("sampleRate")),
            uint32(payload.at("version")), uint32(payload.at("configuration")), frame(payload.at("sourceFrames")), {}};
        validateIdentity(edits);
        requireCurrentSource(context.db, edits);
        if (payload.contains("segmentation")) edits.segmentation = decodeSegmentation(payload.at("segmentation"));
        if constexpr (MinimumSegments > 0) {
            if (!edits.segmentation || edits.segmentation->segments.size() < MinimumSegments)
                throw std::runtime_error("pitch geometry operation requires its resolved segment image");
        }
        decodeRows(edits, payload.at("rows"));
        if (!edits.segmentation && edits.rows.empty()) {
            detail::eraseRowKey(context.db, table, {{clipKey, id}});
            return true;
        }
        const auto bytes = encodeRecord(edits);
        SQLite::Statement insert(context.db,
            "INSERT INTO clip_pitch_edits(clip_id,source_hash,sample_rate,source_frames,analysis_version,analysis_configuration,edits) "
            "VALUES(?,?,?,?,?,?,?) ON CONFLICT(clip_id) DO UPDATE SET source_hash=excluded.source_hash, "
            "sample_rate=excluded.sample_rate,source_frames=excluded.source_frames,analysis_version=excluded.analysis_version, "
            "analysis_configuration=excluded.analysis_configuration,edits=excluded.edits");
        insert.bind(1, id);
        insert.bind(2, edits.sourceHash);
        insert.bind(3, static_cast<std::int64_t>(edits.sampleRate));
        insert.bind(4, static_cast<std::int64_t>(edits.sourceFrames));
        insert.bind(5, static_cast<std::int64_t>(edits.analysisVersion));
        insert.bind(6, static_cast<std::int64_t>(edits.analysisConfiguration));
        insert.bind(7, bytes.data(), static_cast<int>(bytes.size()));
        insert.exec();
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

ClipPitchEditsRead readOne(SQLite::Statement& row) {
    try {
        return {decodeRecord(row), {}};
    } catch (const std::exception& exception) {
        return {{}, exception.what(), row.getColumn(1).getString()};
    }
}

} // namespace

std::span<const OpDescriptor> pitchEditStoreOps() { return descriptors; }

namespace clip_edit {

ClipPitchEditsRead readClipPitchEdits(const Store& store, std::int64_t id) {
    if (audio::onAudioThread) return {{}, "audio callback"};
    try {
        auto& db = const_cast<SQLite::Database&>(store.db());
        if (!db.tableExists(table)) return {};
        SQLite::Statement row(db, std::string(selectRows) + " WHERE clip_id=?");
        row.bind(1, id);
        return row.executeStep() ? readOne(row) : ClipPitchEditsRead{};
    } catch (const std::exception& exception) {
        return {{}, exception.what()};
    }
}

ProjectPitchEditsRead readProjectPitchEdits(const Store& store) {
    if (audio::onAudioThread) return {{}, "audio callback"};
    ProjectPitchEditsRead result;
    try {
        auto& db = const_cast<SQLite::Database&>(store.db());
        if (!db.tableExists(table)) return result;
        result.clips.emplace();
        SQLite::Statement rows(db, std::string(selectRows) + " ORDER BY clip_id");
        while (rows.executeStep()) result.clips->emplace(rows.getColumn(0).getInt64(), readOne(rows));
        return result;
    } catch (const std::exception& exception) {
        return {{}, exception.what()};
    }
}

ClipPitchEditsResolve resolveClipPitchEdits(const ClipPitchEdits& edits, const audio::PitchAnalysis& analysis,
                                           std::string_view sourceHash, std::uint32_t version, std::uint32_t configuration) {
    if (audio::onAudioThread) return {{}, "audio callback"};
    if (edits.sourceHash != sourceHash || edits.sampleRate != analysis.sampleRate ||
        edits.sourceFrames != analysis.sourceFrames || edits.analysisVersion != version ||
        edits.analysisConfiguration != configuration) return {{}, "clip pitch analysis identity changed"};
    try {
        std::optional<audio::PitchAnalysis> adjusted;
        if (edits.segmentation) {
            auto draft = audio::draftPitchSegments(analysis, {}, &*edits.segmentation);
            if (!draft) return {{}, "clip pitch segmentation does not fit the analyzed notes"};
            adjusted = std::move(draft.draft->analysis);
        }
        const auto& effective = adjusted ? *adjusted : analysis;
        audio::PitchEditState state;
        state.notes.reserve(edits.rows.size());
        for (const auto& row : edits.rows) {
            if (row.edit.noteIndex >= effective.notes.size()) return {{}, "clip pitch note index changed"};
            const auto& note = effective.notes[row.edit.noteIndex];
            if (row.sourceStartFrame != note.startFrame || row.sourceEndFrame != note.endFrame)
                return {{}, "clip pitch note bounds changed"};
            state.notes.push_back(row.edit);
        }
        if (audio::validatePitchEdits(effective, state) != audio::PitchEditError::None)
            return {{}, "clip pitch edits do not fit the analyzed notes"};
        return {std::move(state), {}, std::move(adjusted)};
    } catch (const std::exception& exception) {
        return {{}, exception.what()};
    }
}

ClipPitchEditsResolve resolveClipPitchEdits(const ClipPitchEdits& edits, const media::ClipPitchAnalysis& source) {
    if (audio::onAudioThread) return {{}, "audio callback"};
    if (edits.clipId != source.clipId) return {{}, "pitch edits belong to a different clip"};
    return resolveClipPitchEdits(edits, source.analysis, source.source.hash, source.analysisVersion, source.analysisConfiguration);
}

ClipPitchEditRequest prepareClipPitchEdits(const media::ClipPitchAnalysis& source, const audio::PitchEditState& state,
                                          const audio::PitchSegmentation* geometry) {
    if (audio::onAudioThread) return {{}, "audio callback"};
    try {
        ClipPitchEdits edits{source.clipId, source.source.hash, source.source.sampleRate, source.analysisVersion,
                           source.analysisConfiguration, source.source.frames, {}};
        validateIdentity(edits);
        if (edits.sampleRate != source.analysis.sampleRate || edits.sourceFrames != source.analysis.sourceFrames ||
            state.notes.size() > maxRows || (geometry && geometry->segments.size() > maxRows))
            return {{}, "pitch edit state does not fit its source analysis"};
        std::optional<audio::PitchAnalysis> adjusted;
        if (geometry) {
            auto draft = audio::draftPitchSegments(source.analysis, state, geometry);
            if (!draft) return {{}, "pitch edit geometry does not fit its source analysis"};
            adjusted = std::move(draft.draft->analysis);
        } else if (audio::validatePitchEdits(source.analysis, state) != audio::PitchEditError::None) {
            return {{}, "pitch edit state does not fit its source analysis"};
        }
        const auto& effective = adjusted ? *adjusted : source.analysis;
        edits.rows.reserve(state.notes.size());
        for (const auto& edit : state.notes) {
            const auto& note = effective.notes[edit.noteIndex];
            edits.rows.push_back({note.startFrame, note.endFrame, edit});
        }
        OpRequest request;
        request.opType = opType;
        request.label = "Edit clip pitch";
        request.targetKind = "clip";
        request.targetId = edits.clipId;
        request.payload = {{"id", edits.clipId}, {"hash", edits.sourceHash}, {"sampleRate", edits.sampleRate},
            {"sourceFrames", edits.sourceFrames}, {"version", edits.analysisVersion},
            {"configuration", edits.analysisConfiguration}, {"rows", encodeRows(edits.rows)}};
        if (geometry) request.payload["segmentation"] = encodeSegmentation(*geometry);
        return {std::move(request), {}};
    } catch (const std::exception& exception) {
        return {{}, exception.what()};
    }
}

} // namespace clip_edit
} // namespace adi
