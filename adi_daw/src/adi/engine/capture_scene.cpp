// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/capture_scene.hpp"

#include "adi/clip_image.hpp"
#include "adi/engine/session.hpp"
#include "adi/row_capture.hpp"
#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>

#include <array>
#include <limits>
#include <stdexcept>

namespace adi::engine {
namespace {

constexpr auto label = "Capture and Insert Scene";

void append(clip_edit::PreparedEdit& result, const char* type, Payload payload,
            const char* targetKind, std::int64_t id) {
    OpRequest request;
    request.opType = type;
    request.payload = std::move(payload);
    request.label = label;
    request.targetKind = targetKind;
    request.targetId = id;
    result.requests.push_back(std::move(request));
}

void appendScene(SQLite::Database& db, clip_edit::PreparedEdit& result, std::int64_t id,
                 std::optional<std::int64_t> selected, std::string_view name) {
    SQLite::Statement rows(db, "SELECT id, ord FROM main.scenes ORDER BY ord");
    Payload order = Payload::array();
    bool inserted = false;
    auto highest = std::int64_t{-1};
    auto freeOrdinal = std::int64_t{0};
    const auto last = std::numeric_limits<std::int64_t>::max();
    while (rows.executeStep()) {
        const auto scene = rows.getColumn(0).getInt64();
        highest = rows.getColumn(1).getInt64();
        if (highest == freeOrdinal && freeOrdinal != last)
            ++freeOrdinal;
        order.push_back(scene);
        if (selected == scene) {
            order.push_back(id);
            inserted = true;
        }
    }
    if (selected && !inserted)
        throw std::runtime_error("selected Session scene is missing");
    if (!inserted)
        order.push_back(id);
    if (highest == last && freeOrdinal == last)
        throw std::runtime_error("no free Session scene order");
    append(result, "scene.create", {{"id", id}, {"ord", highest == last ? freeOrdinal : highest + 1},
        {"name", name}}, "scene", id);
    if (highest == last || order.back() != id)
        append(result, "scene.reorder", {{"scenes", std::move(order)}}, "scene", id);
}

bool storedFlag(const Payload& row, const char* field) {
    const auto value = adi::detail::signedInteger(row.at(field));
    if (value != 0 && value != 1)
        throw std::runtime_error("invalid captured Session flag " + std::string(field));
    return value == 1;
}

void appendLaunchSettings(clip_edit::PreparedEdit& result, const Payload& settings, std::int64_t clip) {
    if (settings.size() != 16)
        throw std::runtime_error("captured slot uses unrecognized launch settings");
    append(result, "session.setLaunchMode", {{"clip", clip}, {"mode", settings.at("launch_mode")}}, "clip", clip);
    append(result, "session.setLaunchQuant", {{"clip", clip}, {"quant", settings.at("launch_quant")}}, "clip", clip);
    append(result, "session.setLaunchSettings", {{"clip", clip}, {"legato", storedFlag(settings, "legato")},
        {"velocity", settings.at("velocity_amount")}, {"fade", storedFlag(settings, "clip_fade")},
        {"ram", storedFlag(settings, "ram_mode")}}, "clip", clip);
    Payload follow{{"clip", clip}, {"enabled", storedFlag(settings, "follow_enabled")},
        {"linked", storedFlag(settings, "follow_linked")}, {"loops", settings.at("follow_loop_count")},
        {"duration", settings.at("follow_duration_ticks")}, {"actionA", settings.at("follow_a")},
        {"actionB", settings.at("follow_b")}, {"chanceB", settings.at("follow_b_chance")}};
    if (!settings.at("follow_a_scene_id").is_null())
        follow["jumpA"] = settings.at("follow_a_scene_id");
    if (!settings.at("follow_b_scene_id").is_null())
        follow["jumpB"] = settings.at("follow_b_scene_id");
    append(result, "session.setFollowAction", std::move(follow), "clip", clip);
}

CapturedSceneTransfer captureTrack(OpContext& context, clip_edit::PreparedEdit& result,
                                   clip_edit::CopyIds& ids, const LaunchTrackStatus& track,
                                   std::int64_t newScene) {
    if (track.clip <= 0 || track.scene <= 0 || track.run == 0 || track.sourceTick < 0)
        throw std::runtime_error("invalid running Session slot identity");
    const auto source = adi::detail::captureRowKey(context.db, "clip_slots",
        {{"track_id", track.track}, {"scene_id", track.scene}});
    if (!source || source->at("clip_id") != track.clip)
        throw std::runtime_error("running Session slot changed after publication");
    if (source->size() != 4)
        throw std::runtime_error("captured slot uses unrecognized stored values");

    const auto image = clip_edit::captureImage(context, track.clip);
    const auto clip = ids.next("clips");
    edit::RowEdits clone(context.db);
    clip_edit::createImage(clone, clip_edit::cloneImage(image, clip, ids));
    append(result, "clip.duplicate", {{"id", clip}, {"changes", clone.changes()}}, "clip", clip);
    append(result, "session.setSlotClip", {{"track", track.track}, {"scene", newScene}, {"clip", clip}},
        "clip", clip);
    if (!storedFlag(*source, "has_stop"))
        append(result, "session.setStopButton", {{"track", track.track}, {"scene", newScene}, {"on", false}},
            "clip", clip);
    if (const auto settings = adi::detail::captureRow(context.db, "clip_launch", "clip_id", track.clip))
        appendLaunchSettings(result, *settings, clip);
    return {track.track, track.scene, track.clip, clip, track.sourceTick, track.run};
}

} // namespace

CaptureInsertedScene prepareCaptureAndInsertScene(Store& store, const SceneCaptureSnapshot& snapshot,
                                                   std::optional<std::int64_t> selectedScene,
                                                   std::string_view name) {
    CaptureInsertedScene result;
    try {
        if (store.readOnly())
            throw std::runtime_error("capturing a Session scene needs a writable project");
        const auto& header = snapshot.header;
        if (header.incarnation == 0 || header.incarnation != snapshot.currentIncarnation ||
            header.revision == 0 || header.trackCount > snapshot.tracks.size() ||
            header.endSample < 0 || header.endTick < 0)
            throw std::runtime_error("Session render status is stale or incomplete");
        if (!header.transportPlaying)
            throw std::runtime_error("capture needs currently running Session clips");

        auto& db = store.db();
        std::optional<SQLite::Transaction> read;
        if (sqlite3_get_autocommit(db.getHandle()) != 0)
            read.emplace(db, SQLite::TransactionBehavior::DEFERRED);
        result.source = appliedSource(store);
        result.sourceSequence = result.source.sequence();
        const auto head = OpJournal(store).headSeq();
        if (!result.source.alive() || result.sourceSequence != snapshot.publishedSequence ||
            head != snapshot.publishedHead)
            throw std::runtime_error("Session graph and current project head differ");
        if (const auto problem = store.essentialExtensionProblem(kSessionSchemaUserVersion); !problem.empty())
            throw std::runtime_error(problem);

        result.scene = RowIds(db).next("scenes");
        appendScene(db, result.edit, result.scene, selectedScene, name);
        clip_edit::CopyIds clipIds(db);
        OpContext context{store, db};
        auto previousTrack = std::int64_t{0};
        for (const auto& track : snapshot.tracks.first(header.trackCount)) {
            if (track.track <= previousTrack)
                throw std::runtime_error("Session render status has duplicate or unordered tracks");
            previousTrack = track.track;
            if (track.source == LaunchTrackSource::Session)
                result.transfers.push_back(captureTrack(context, result.edit, clipIds, track, result.scene));
        }
        if (result.transfers.empty())
            throw std::runtime_error("capture needs currently running Session clips");

        result.incarnation = header.incarnation;
        result.statusRevision = header.revision;
        result.edit.sourceFile = db.execAndGet("SELECT file FROM pragma_database_list WHERE name='main'").getString();
        result.edit.options.expectHead.emplace(head);
        if (read)
            read->commit();
        result.edit.ok = true;
    } catch (const std::exception& error) {
        result = {};
        result.edit.error = error.what();
    }
    return result;
}

CommitResult commitCaptureAndInsertScene(Store& store, const CaptureInsertedScene& prepared, Actor actor) {
    if (!prepared.edit.ok)
        return clip_edit::commitPrepared(store, prepared.edit, actor);
    const auto current = appliedSource(store);
    if (!current.alive() || !current.sameConnection(prepared.source) ||
        current.sequence() != prepared.sourceSequence) {
        CommitResult result;
        result.stale = true;
        result.error = "project changed after Session scene capture";
        return result;
    }
    return clip_edit::commitPrepared(store, prepared.edit, actor);
}

CaptureSceneCommitResult commitCaptureAndInsertScene(Store& store, Session& session,
                                                     const CaptureInsertedScene& prepared, Actor actor) {
    CaptureSceneCommitResult result;
    if (prepared.edit.ok && session.launchSourceEditReserved()) {
        result.durable.error = "Session scene capture needs the authored clip source";
        return result;
    }
    if (prepared.edit.ok && (session.launchIncarnation() != prepared.incarnation ||
                            session.realisedAppliedSequence() != prepared.sourceSequence)) {
        result.durable.stale = true;
        result.durable.error = "published Session changed after scene capture";
        return result;
    }
    result.durable = commitCaptureAndInsertScene(store, prepared, actor);
    if (!result.durable.ok)
        return result;
    if (!session.refresh(store)) {
        result.retargetError = session.error();
        return result;
    }
    if (prepared.transfers.size() > LaunchGestureQueue::capacity) {
        result.retargetError = "captured scene exceeds the driver's atomic handover capacity";
        return result;
    }

    std::array<LaunchRequest, LaunchGestureQueue::capacity> requests;
    std::size_t count = 0;
    for (const auto& transfer : prepared.transfers) {
        auto& request = requests[count++];
        request.kind = LaunchRequestKind::RetargetRunning;
        request.track = transfer.track;
        request.scene = prepared.scene;
        request.clip = transfer.newClip;
        request.incarnation = prepared.incarnation;
        request.expectedClip = transfer.oldClip;
        request.expectedRun = transfer.run;
    }
    result.retargetQueued = session.postLaunchGesture(std::span(requests).first(count));
    if (!result.retargetQueued)
        result.retargetError = "Session driver did not admit the captured scene handover";
    return result;
}

} // namespace adi::engine
