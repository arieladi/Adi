// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/pitch_flatten.hpp"

#include "adi/audio/io_audit.hpp"
#include "adi/audio/warp.hpp"
#include "adi/edit_rows.hpp"
#include "adi/media/analysis_cache.hpp"
#include "adi/media/blake3.hpp"
#include "adi/media/clip_pitch_render.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/pitch_edit_store.hpp"
#include "adi/row_capture.hpp"
#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <exception>
#include <stdexcept>
#include <utility>

namespace adi::clip_edit {
namespace {

PitchFlattenResult fail(PitchFlattenProblem problem, std::optional<std::string> detail = {}) {
    return {problem, {}, std::move(detail)};
}

std::string projectFile(SQLite::Database& db) {
    SQLite::Statement location(db, "SELECT file FROM pragma_database_list WHERE name='main'");
    if (!location.executeStep()) throw std::runtime_error("clip pitch flatten needs a project connection");
    const auto file = location.getColumn(0).getString();
    if (file.empty()) throw std::runtime_error("clip pitch flatten needs a saved project");
    return file;
}

struct CapturedClip {
    std::optional<std::int64_t> head;
    std::string file;
    Payload root, source, pitch, media;
    std::shared_ptr<const ClipPitchEdits> edits;
    std::uint64_t first, frames;
};

CapturedClip captureClip(Store& store, std::int64_t clipId) {
    auto& db = store.db();
    SQLite::Transaction read(db, SQLite::TransactionBehavior::DEFERRED);
    const auto head = OpJournal(store).headSeq();
    auto file = projectFile(db);
    auto root = edit::row(db, "clips", clipId);
    auto source = edit::row(db, "audio_clips", clipId);
    if (!root || root->at("kind") != "audio" || !root->at("alias_of").is_null() || !source)
        throw std::runtime_error("clip pitch flatten needs an independent audio clip");
    auto saved = readClipPitchEdits(store, clipId);
    if (!saved.error.empty()) throw std::runtime_error(saved.error);
    if (!saved.edits || saved.edits->rows.empty()) throw std::runtime_error("clip has no authored pitch edits to flatten");
    auto pitch = edit::row(db, "clip_pitch_edits", clipId);
    if (!pitch) throw std::runtime_error("captured pitch edit row is unavailable");

    const auto mediaId = detail::signedInteger(source->at("media_id"));
    SQLite::Statement embedded(db, "SELECT embedded FROM media_files WHERE id=?");
    embedded.bind(1, mediaId);
    if (!embedded.executeStep() || embedded.getColumn(0).getInt() != 0)
        throw std::runtime_error("extract embedded clip media before flattening pitch");
    auto media = adi::media::mediaRow(db, mediaId);
    if (media.at("hash_blake3") != saved.edits->sourceHash)
        throw std::runtime_error("authored pitch edits belong to a different source");
    const auto first = detail::signedInteger(source->at("src_start_frames"));
    const auto frames = detail::signedInteger(source->at("src_len_frames"));
    if (first < 0 || frames <= 0) throw std::runtime_error("invalid pitch flatten source window");
    const bool warp = source->at("warp_enabled") != 0;
    if (!warp && (source->at("formant_shift") != 0 ||
        audio::pitchedSampleRate(saved.edits->sampleRate, source->at("transpose_semis").get<double>()) <= 0))
        throw std::runtime_error("current clip source playback combination is unsupported");
    read.commit();
    return {head, std::move(file), std::move(*root), std::move(*source), std::move(*pitch), std::move(media),
            std::make_shared<const ClipPitchEdits>(std::move(*saved.edits)), static_cast<std::uint64_t>(first),
            static_cast<std::uint64_t>(frames)};
}

} // namespace

PitchFlattenResult preparePitchFlatten(Store& store, const PitchFlattenOptions& options) {
    if (audio::onAudioThread) return fail(PitchFlattenProblem::Realtime);
    if (options.stop.stop_requested()) return fail(PitchFlattenProblem::Cancelled);
    if (options.scope != PitchFlattenScope::WholeEvent)
        return fail(PitchFlattenProblem::UnsupportedScope, "pitch flatten requires explicit whole-event scope");
    return preparePitchFlatten(store, options, nullptr, &media::defaultAnalysisCache());
}

PitchFlattenResult preparePitchFlatten(Store& store, const PitchFlattenOptions& options,
                                      audio::DecodeCache* decoder, media::AnalysisCache* cache) {
    if (audio::onAudioThread) return fail(PitchFlattenProblem::Realtime);
    if (options.stop.stop_requested()) return fail(PitchFlattenProblem::Cancelled);
    if (options.scope != PitchFlattenScope::WholeEvent)
        return fail(PitchFlattenProblem::UnsupportedScope, "pitch flatten requires explicit whole-event scope");
    if (store.readOnly() || options.clipId <= 0 || options.mediaId <= 0)
        return fail(PitchFlattenProblem::Capture, "pitch flatten needs a writable project and allocated identities");

    try {
        auto captured = captureClip(store, options.clipId);
        std::string sourceError;
        const auto sourcePath = media::resolveMedia(media::projectFolder(store), captured.media, sourceError, options.stop);
        if (options.stop.stop_requested()) return fail(PitchFlattenProblem::Cancelled);
        if (sourcePath.empty()) return fail(PitchFlattenProblem::Capture, std::move(sourceError));
        // Keep the native aperture in forward physical order. Playback still
        // applies the clip's Reverse, Warp and Transpose to that same window.
        const media::ClipPitchRenderPlan render{sourcePath, {}, captured.edits,
                                               captured.first, captured.frames, false, {}};
        const auto rendered = media::prepareClipPitchRender(render, options.stop, decoder, cache);
        if (!rendered) {
            if (rendered.problem == media::ClipPitchRenderProblem::Cancelled) return fail(PitchFlattenProblem::Cancelled);
            return fail(PitchFlattenProblem::Render, rendered.detail.value_or("cannot render clip pitch"));
        }
        if (options.stop.stop_requested()) return fail(PitchFlattenProblem::Cancelled);
        if (rendered.prepared->rate != captured.edits->sampleRate || rendered.prepared->frames != captured.frames)
            return fail(PitchFlattenProblem::Render, "pitch render changed the clip's native source clock");

        auto& db = store.db();
        SQLite::Transaction read(db, SQLite::TransactionBehavior::DEFERRED);
        const auto oldMediaId = detail::signedInteger(captured.source.at("media_id"));
        if (OpJournal(store).headSeq() != captured.head || projectFile(db) != captured.file ||
            edit::row(db, "clips", options.clipId) != captured.root ||
            edit::row(db, "audio_clips", options.clipId) != captured.source ||
            edit::row(db, "clip_pitch_edits", options.clipId) != captured.pitch ||
            media::mediaRow(db, oldMediaId) != captured.media)
            return fail(PitchFlattenProblem::Stale, "clip changed during pitch render");

        MaterialEdit material;
        material.edit.sourceFile = captured.file;
        material.edit.options.expectHead.emplace(captured.head);
        material.edit.options.beforeCommit = [sourcePath, sourceHash = captured.edits->sourceHash, oldMediaId,
            mediaRow = std::move(captured.media), root = std::move(captured.root), clipId = options.clipId, stop = options.stop]
            (SQLite::Database& committingDb, std::int64_t, std::string& error) {
                if (stop.stop_requested()) { error = "pitch flatten cancelled"; return false; }
                if (edit::row(committingDb, "clips", clipId) != root || media::mediaRow(committingDb, oldMediaId) != mediaRow) {
                    error = "clip changed after pitch render";
                    return false;
                }
                const auto current = media::blake3File(sourcePath, stop);
                if (!current || current.hex != sourceHash) { error = "clip source changed after pitch render"; return false; }
                return true;
            };
        const auto imported = stageRenderedAudio(store, material, rendered.prepared->file,
                                                 options.mediaId, options.importedUtc, options.stop);
        if (options.stop.stop_requested()) return fail(PitchFlattenProblem::Cancelled);
        if (!imported.ok) return fail(PitchFlattenProblem::Material, imported.error);
        if (!imported.audio || imported.audio->sampleRate != rendered.prepared->rate ||
            imported.audio->channels != rendered.prepared->channels ||
            imported.audio->frames != static_cast<std::int64_t>(captured.frames))
            return fail(PitchFlattenProblem::Material, "staged pitch render dimensions changed");

        auto after = std::move(captured.source);
        after["media_id"] = imported.mediaId;
        after["src_start_frames"] = 0;
        edit::RowEdits changes(db);
        changes.set("audio_clips", options.clipId, std::move(after));
        changes.set("clip_pitch_edits", options.clipId, std::nullopt);
        OpRequest swap;
        swap.opType = "arranger.splice";
        swap.payload = {{"id", options.clipId}, {"changes", changes.changes()}};
        swap.targetKind = "clip";
        swap.targetId = options.clipId;
        material.edit.requests.push_back(std::move(swap));
        for (auto& op : material.edit.requests) op.label = "Flatten Clip Pitch";
        read.commit();
        material.edit.ok = true;
        return {PitchFlattenProblem::None, std::move(material), {}};
    } catch (const std::exception& error) {
        if (options.stop.stop_requested()) return fail(PitchFlattenProblem::Cancelled);
        return fail(PitchFlattenProblem::Capture, error.what());
    }
}

} // namespace adi::clip_edit
