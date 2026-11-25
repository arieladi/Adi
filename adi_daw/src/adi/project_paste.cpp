// SPDX-License-Identifier: AGPL-3.0-or-later
//
// PD-15: cross-project track paste — the engine half (ADR-0068, ADR-0111).

#include "adi/project_paste.hpp"

#include "adi/applied_batches.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/automation_lanes.hpp"
#include "adi/automation_range_rows.hpp"
#include "adi/clip_image.hpp"
#include "adi/device_copy.hpp"
#include "adi/edit_rows.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/op_transaction_resources.hpp"
#include "adi/row_capture.hpp"
#include "adi/row_ids.hpp"
#include "adi/store.hpp"
#include "adi/take_lane_ops.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace adi {

namespace {

namespace fs = std::filesystem;

struct DeferredRead {
    std::optional<SQLite::Transaction> tx;
    explicit DeferredRead(SQLite::Database& db) {
        if (sqlite3_get_autocommit(db.getHandle()) != 0) {
            tx.emplace(db, SQLite::TransactionBehavior::DEFERRED);
        }
    }
};

[[nodiscard]] bool isValidBlake3Hex(std::string_view hash) noexcept {
    if (hash.size() != 64U) {
        return false;
    }
    for (char c : hash) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!ok) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool tablePresent(SQLite::Database& db, std::string_view name) {
    SQLite::Statement q(
        db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?");
    q.bind(1, std::string(name));
    return q.executeStep();
}

using PluginRefKey = std::tuple<std::string, std::string, std::optional<std::int64_t>>;

struct TrackSnapshot {
    std::int64_t id = 0;
    std::optional<std::int64_t> parentId;
    std::int64_t indexInParent = 0;
    std::string kind;
    std::string name;
    std::int64_t color = 0;
    bool muted = false;
    bool soloed = false;
    bool soloDefeat = false;
    bool recordArmed = false;
    std::int64_t monitorMode = 0;
    std::string inputRef;
    std::int64_t timeBase = 0;
    bool locked = false;
    std::int64_t automationMode = 0;

    bool hasMixer = false;
    double volumeDb = 0.0;
    double pan = 0.0;
    std::int64_t panLaw = 0;
    double width = 1.0;
    double inputGainDb = 0.0;
    bool phaseInvert = false;
    std::int64_t delaySamples = 0;
    std::optional<std::int64_t> vcaGroupId;

    bool hasSplitPan = false;
    std::string panMode = "stereo";
    double panLeft = -1.0;
    double panRight = 1.0;

    bool hasRecordingOptions = false;
    bool recQuantEnabled = true;
    std::string recQuantGrid = "1/16";
    bool playQuantEnabled = false;
    std::string playQuantGrid = "1/16";
    double playForgivenessMs = 0.0;
    bool keepMonitoringLatency = true;

    bool hasGroupSumming = false;
    bool summingEnabled = false;
    std::string summingFlavor = "console9";
    double summingDriveDb = 0.0;

    std::optional<bool> returnSendTapPreFader;
};

TrackSnapshot readTrackSnapshot(SQLite::Database& db, std::int64_t trackId) {
    SQLite::Statement q(
        db,
        "SELECT parent_id, index_in_parent, kind, name, color, muted, soloed, solo_defeat, "
        "record_armed, monitor_mode, input_ref, time_base, locked, automation_mode "
        "FROM tracks WHERE id=?");
    q.bind(1, trackId);
    if (!q.executeStep()) {
        throw std::runtime_error("source track does not exist");
    }

    TrackSnapshot snap;
    snap.id = trackId;
    if (!q.getColumn(0).isNull()) {
        snap.parentId = q.getColumn(0).getInt64();
    }
    snap.indexInParent = q.getColumn(1).getInt64();
    snap.kind = q.getColumn(2).getString();
    snap.name = q.getColumn(3).getString();
    snap.color = q.getColumn(4).getInt64();
    snap.muted = q.getColumn(5).getInt() != 0;
    snap.soloed = q.getColumn(6).getInt() != 0;
    snap.soloDefeat = q.getColumn(7).getInt() != 0;
    snap.recordArmed = q.getColumn(8).getInt() != 0;
    snap.monitorMode = q.getColumn(9).getInt64();
    snap.inputRef = q.getColumn(10).isNull() ? std::string{} : q.getColumn(10).getString();
    snap.timeBase = q.getColumn(11).getInt64();
    snap.locked = q.getColumn(12).getInt() != 0;
    snap.automationMode = q.getColumn(13).getInt64();

    SQLite::Statement m(
        db,
        "SELECT volume_db, pan, pan_law, width, input_gain_db, phase_invert, delay_samples, "
        "vca_group_id FROM mixer_strip WHERE track_id=?");
    m.bind(1, trackId);
    if (m.executeStep()) {
        snap.hasMixer = true;
        snap.volumeDb = m.getColumn(0).getDouble();
        snap.pan = m.getColumn(1).getDouble();
        snap.panLaw = m.getColumn(2).getInt64();
        snap.width = m.getColumn(3).getDouble();
        snap.inputGainDb = m.getColumn(4).getDouble();
        snap.phaseInvert = m.getColumn(5).getInt() != 0;
        snap.delaySamples = m.getColumn(6).getInt64();
        if (!m.getColumn(7).isNull()) {
            snap.vcaGroupId = m.getColumn(7).getInt64();
        }
    }

    if (tablePresent(db, "mixer_pan_settings")) {
        SQLite::Statement sp(
            db, "SELECT mode, pan_left, pan_right FROM mixer_pan_settings WHERE track_id=?");
        sp.bind(1, trackId);
        if (sp.executeStep()) {
            snap.hasSplitPan = true;
            snap.panMode = sp.getColumn(0).getString();
            snap.panLeft = sp.getColumn(1).getDouble();
            snap.panRight = sp.getColumn(2).getDouble();
        }
    }

    if (tablePresent(db, "track_recording_options")) {
        SQLite::Statement ro(
            db,
            "SELECT record_enabled, record_grid, play_enabled, play_grid, "
            "play_forgiveness_ms, keep_monitoring_latency "
            "FROM track_recording_options WHERE track_id=?");
        ro.bind(1, trackId);
        if (ro.executeStep()) {
            snap.hasRecordingOptions = true;
            snap.recQuantEnabled = ro.getColumn(0).getInt() != 0;
            snap.recQuantGrid = ro.getColumn(1).getString();
            snap.playQuantEnabled = ro.getColumn(2).getInt() != 0;
            snap.playQuantGrid = ro.getColumn(3).getString();
            snap.playForgivenessMs = ro.getColumn(4).getDouble();
            snap.keepMonitoringLatency = ro.getColumn(5).getInt() != 0;
        }
    }

    if (snap.kind == "group" && tablePresent(db, "group_summing")) {
        SQLite::Statement gs(
            db, "SELECT enabled, flavor, drive_db FROM group_summing WHERE track_id=?");
        gs.bind(1, trackId);
        if (gs.executeStep()) {
            snap.hasGroupSumming = true;
            snap.summingEnabled = gs.getColumn(0).getInt() != 0;
            snap.summingFlavor = gs.getColumn(1).getString();
            snap.summingDriveDb = gs.getColumn(2).getDouble();
        }
    }

    if (snap.kind == "return" && tablePresent(db, "return_send_taps")) {
        SQLite::Statement rt(db, "SELECT pre_fader FROM return_send_taps WHERE track_id=?");
        rt.bind(1, trackId);
        if (rt.executeStep()) {
            snap.returnSendTapPreFader = rt.getColumn(0).getInt() != 0;
        }
    }

    return snap;
}

/// Collect `rootTrackId` and all descendant tracks in parent-before-child order.
void collectTrackSubtree(SQLite::Database& db,
                         std::int64_t trackId,
                         std::set<std::int64_t>& visited,
                         std::vector<TrackSnapshot>& out) {
    if (!visited.insert(trackId).second) {
        throw std::runtime_error("cycle detected in source track hierarchy");
    }
    TrackSnapshot snap = readTrackSnapshot(db, trackId);
    if (snap.kind == "master") {
        throw std::runtime_error("master track cannot be pasted into another project");
    }
    out.push_back(std::move(snap));

    SQLite::Statement childrenQ(
        db, "SELECT id FROM tracks WHERE parent_id = ? ORDER BY index_in_parent, id");
    childrenQ.bind(1, trackId);
    std::vector<std::int64_t> childIds;
    while (childrenQ.executeStep()) {
        childIds.push_back(childrenQ.getColumn(0).getInt64());
    }
    for (const std::int64_t childId : childIds) {
        collectTrackSubtree(db, childId, visited, out);
    }
}

Payload buildPluginRefRow(SQLite::Database& db, const Payload& values) {
    SQLite::Statement ddl(
        db, "SELECT sql FROM sqlite_master WHERE type='table' AND name='plugin_refs'");
    if (!ddl.executeStep()) {
        throw std::runtime_error("plug-in reference schema is missing");
    }
    SQLite::Database scratch(":memory:", SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
    scratch.exec(ddl.getColumn(0).getString());
    detail::restoreRow(scratch, "plugin_refs", values);
    const auto captured =
        detail::captureRow(scratch, "plugin_refs", "id", detail::signedInteger(values.at("id")));
    if (!captured.has_value()) {
        throw std::runtime_error("failed to capture plug-in reference row");
    }
    return *captured;
}

std::optional<std::int64_t> findMatchingDestReturn(SQLite::Database& destDb,
                                                   const std::string& returnName) {
    SQLite::Statement q(
        destDb,
        "SELECT id FROM tracks WHERE kind = 'return' AND parent_id IS NULL AND name = ? "
        "ORDER BY index_in_parent, id");
    q.bind(1, returnName);
    std::optional<std::int64_t> found;
    while (q.executeStep()) {
        if (found.has_value()) {
            return std::nullopt;  // ambiguous return name in destination
        }
        found = q.getColumn(0).getInt64();
    }
    return found;
}

std::optional<bool> readDestReturnTap(SQLite::Database& destDb,
                                      std::int64_t destReturnId,
                                      const std::map<std::int64_t, std::optional<bool>>& pastedReturnTaps) {
    if (const auto it = pastedReturnTaps.find(destReturnId); it != pastedReturnTaps.end()) {
        return it->second;
    }
    if (!tablePresent(destDb, "return_send_taps")) {
        return std::nullopt;
    }
    SQLite::Statement q(destDb, "SELECT pre_fader FROM return_send_taps WHERE track_id = ?");
    q.bind(1, destReturnId);
    if (q.executeStep()) {
        return q.getColumn(0).getInt() != 0;
    }
    return std::nullopt;
}

}  // namespace

ProjectPasteResult pasteTrackFromProject(Store& source,
                                         std::int64_t sourceTrackId,
                                         Store& dest,
                                         Actor actor,
                                         std::string_view actorDetail) {
    ProjectPasteResult result;

    try {
        if (audio::onAudioThread) {
            result.error = "cross-project paste belongs off the audio callback";
            return result;
        }
        if (dest.readOnly()) {
            result.error = "destination project is read-only";
            return result;
        }

        std::vector<OpRequest> requests;
        CommitOptions options;
        OpTransactionInputs txInputs;
        std::int64_t rootNewTrackId = 0;

        auto appendOp = [&](const char* opType, Payload payload, const char* targetKind,
                            std::int64_t targetId) {
            OpRequest req;
            req.opType = opType;
            req.payload = std::move(payload);
            req.actor = actor;
            req.actorDetail = std::string(actorDetail);
            req.label = "Paste Track";
            req.targetKind = targetKind;
            req.targetId = targetId;
            requests.push_back(std::move(req));
        };

        {
            DeferredRead readSrc(source.db());
            DeferredRead readDest(dest.db());

            options.expectHead.emplace(OpJournal(dest).headSeq());
            RowIds ids(dest.db());

            // 1. Collect source track (and all descendant tracks if it is a group).
            std::vector<TrackSnapshot> tracks;
            {
                std::set<std::int64_t> visited;
                collectTrackSubtree(source.db(), sourceTrackId, visited, tracks);
            }

            for (const auto& t : tracks) {
                result.trackIds[t.id] = ids.next("tracks");
            }
            rootNewTrackId = result.trackIds.at(sourceTrackId);

            // 2. Reference media in place by BLAKE3 hash (ADR-0068 d5).
            //    Files are NOT copied to dest's folder; empty/invalid hashes are
            //    never deduplicated and are refused explicitly.
            const fs::path srcFolder = media::projectFolder(source);
            std::map<std::string, std::int64_t> hashToDestMedia;
            {
                std::set<std::int64_t> srcMediaIds;
                for (const auto& t : tracks) {
                    SQLite::Statement mediaQuery(
                        source.db(),
                        "SELECT DISTINCT a.media_id "
                        "FROM clips c JOIN audio_clips a ON a.clip_id = c.id "
                        "WHERE c.track_id = ? ORDER BY a.media_id");
                    mediaQuery.bind(1, t.id);
                    while (mediaQuery.executeStep()) {
                        srcMediaIds.insert(mediaQuery.getColumn(0).getInt64());
                    }
                }

                for (const std::int64_t srcMediaId : srcMediaIds) {
                    Payload row = media::mediaRow(source.db(), srcMediaId);
                    const std::string hash = row.at("hash_blake3").get<std::string>();
                    if (!isValidBlake3Hex(hash)) {
                        throw std::runtime_error(
                            "source media row " + std::to_string(srcMediaId) +
                            " has empty or invalid BLAKE3 hash");
                    }

                    if (const auto localIt = hashToDestMedia.find(hash);
                        localIt != hashToDestMedia.end()) {
                        result.mediaIds[srcMediaId] = localIt->second;
                        ++result.reusedMediaCount;
                        continue;
                    }

                    SQLite::Statement existing(
                        dest.db(), "SELECT id FROM media_files WHERE hash_blake3 = ?");
                    existing.bind(1, hash);
                    if (existing.executeStep()) {
                        const std::int64_t existingId = existing.getColumn(0).getInt64();
                        hashToDestMedia[hash] = existingId;
                        result.mediaIds[srcMediaId] = existingId;
                        ++result.reusedMediaCount;
                        continue;
                    }

                    // Hash absent in dest: reference the source file in place via
                    // its resolved absolute path (ADR-0068 d5).
                    std::string resolveErr;
                    const fs::path resolvedFile =
                        media::resolveMedia(srcFolder, row, resolveErr);

                    fs::path absTarget;
                    bool isMissing = false;
                    if (!resolvedFile.empty()) {
                        std::error_code ec;
                        const fs::path canonical = fs::weakly_canonical(resolvedFile, ec);
                        absTarget = ec ? fs::absolute(resolvedFile).lexically_normal() : canonical;
                        isMissing = false;
                    } else {
                        // Refuse corrupted/unsafe files; preserve missing media with its
                        // resolved absolute path so dest can relink by hash (ADR-0032).
                        if (resolveErr.starts_with("hash mismatch") ||
                            resolveErr.starts_with("absolute rel_path") ||
                            resolveErr.starts_with("cannot hash") ||
                            resolveErr.starts_with("cannot inspect")) {
                            throw std::runtime_error(resolveErr);
                        }
                        if (!row.at("rel_path").is_null()) {
                            const fs::path rel =
                                media::pathFromUtf8(row.at("rel_path").get<std::string>());
                            absTarget = fs::absolute(srcFolder / rel).lexically_normal();
                        } else if (!row.at("abs_path_hint").is_null()) {
                            const fs::path hint =
                                media::pathFromUtf8(row.at("abs_path_hint").get<std::string>());
                            if (hint.is_absolute()) {
                                absTarget = hint.lexically_normal();
                            }
                        }
                        isMissing = true;
                    }

                    row["rel_path"] = nullptr;
                    row["abs_path_hint"] =
                        absTarget.empty() ? Payload(nullptr) : Payload(media::pathUtf8(absTarget));
                    row["missing"] = isMissing ? 1 : 0;

                    const std::int64_t newMediaId = ids.next("media_files");
                    appendOp("media.import", {{"id", newMediaId}, {"row", std::move(row)}},
                             "media", newMediaId);

                    hashToDestMedia[hash] = newMediaId;
                    result.mediaIds[srcMediaId] = newMediaId;
                    ++result.referencedMediaCount;
                }
            }

            // 3. Create every track in the subtree (parent before children) with
            //    mixer strip, split pan, recording options, group summing, return
            //    send tap, and take lanes.
            SQLite::Statement idxQ(
                dest.db(),
                "SELECT COALESCE(MAX(index_in_parent), -1) + 1 FROM tracks WHERE parent_id IS NULL");
            idxQ.executeStep();
            const std::int64_t nextTopLevelIndex = idxQ.getColumn(0).getInt64();

            std::map<std::int64_t, std::optional<bool>> pastedReturnTaps;

            for (const auto& track : tracks) {
                const std::int64_t newTrackId = result.trackIds.at(track.id);
                const bool isRoot = (track.id == sourceTrackId);
                const Payload parentPayload =
                    isRoot ? Payload(nullptr)
                           : Payload(result.trackIds.at(track.parentId.value()));
                const std::int64_t indexPayload = isRoot ? nextTopLevelIndex : track.indexInParent;

                appendOp("track.create",
                         {{"id", newTrackId},
                          {"kind", track.kind},
                          {"name", track.name},
                          {"parent", parentPayload},
                          {"index", indexPayload}},
                         "track", newTrackId);

                appendOp("track.setColor", {{"id", newTrackId}, {"color", track.color}}, "track",
                         newTrackId);
                appendOp("track.setMute", {{"id", newTrackId}, {"muted", track.muted}}, "track",
                         newTrackId);
                appendOp("track.setSolo", {{"id", newTrackId}, {"soloed", track.soloed}}, "track",
                         newTrackId);
                appendOp("track.setSoloDefeat", {{"id", newTrackId}, {"defeat", track.soloDefeat}},
                         "track", newTrackId);
                appendOp("track.setArm", {{"id", newTrackId}, {"armed", track.recordArmed}},
                         "track", newTrackId);
                appendOp("track.setMonitorMode", {{"id", newTrackId}, {"mode", track.monitorMode}},
                         "track", newTrackId);
                if (!track.inputRef.empty()) {
                    appendOp("track.setInput", {{"id", newTrackId}, {"input", track.inputRef}},
                             "track", newTrackId);
                }
                appendOp("track.setTimeBase", {{"id", newTrackId}, {"base", track.timeBase}},
                         "track", newTrackId);
                appendOp("track.setLocked", {{"id", newTrackId}, {"locked", track.locked}}, "track",
                         newTrackId);
                appendOp("track.setAutomationMode",
                         {{"id", newTrackId}, {"mode", track.automationMode}}, "track", newTrackId);

                if (track.hasMixer) {
                    appendOp("mixer.setVolume", {{"id", newTrackId}, {"db", track.volumeDb}},
                             "track", newTrackId);
                    appendOp("mixer.setPan", {{"id", newTrackId}, {"pan", track.pan}}, "track",
                             newTrackId);
                    appendOp("mixer.setWidth", {{"id", newTrackId}, {"width", track.width}},
                             "track", newTrackId);
                    appendOp("mixer.setInputGain", {{"id", newTrackId}, {"db", track.inputGainDb}},
                             "track", newTrackId);
                    appendOp("mixer.setPhaseInvert",
                             {{"id", newTrackId}, {"inverted", track.phaseInvert}}, "track",
                             newTrackId);
                    appendOp("mixer.setDelay", {{"id", newTrackId}, {"samples", track.delaySamples}},
                             "track", newTrackId);
                    if (track.panLaw != 0) {
                        result.warnings.push_back(
                            "track '" + track.name + "' has non-default mixer_strip.pan_law (" +
                            std::to_string(track.panLaw) +
                            ") which cannot be preserved yet (no mixer.setPanLaw op in ops_catalog)");
                    }
                    if (track.vcaGroupId.has_value()) {
                        result.warnings.push_back(
                            "track '" + track.name + "' has mixer_strip.vca_group_id (" +
                            std::to_string(*track.vcaGroupId) +
                            ") which was not pasted across projects");
                    }
                }

                if (track.hasSplitPan) {
                    appendOp("mixer.setPanMode", {{"id", newTrackId}, {"mode", track.panMode}},
                             "track", newTrackId);
                    appendOp("mixer.setPanLeft", {{"id", newTrackId}, {"pan", track.panLeft}},
                             "track", newTrackId);
                    appendOp("mixer.setPanRight", {{"id", newTrackId}, {"pan", track.panRight}},
                             "track", newTrackId);
                }

                if (track.hasRecordingOptions) {
                    appendOp("track.setRecordQuantize",
                             {{"id", newTrackId},
                              {"on", track.recQuantEnabled},
                              {"grid", track.recQuantGrid}},
                             "track", newTrackId);
                    appendOp("track.setPlayQuantize",
                             {{"id", newTrackId},
                              {"on", track.playQuantEnabled},
                              {"grid", track.playQuantGrid},
                              {"forgivenessMs", track.playForgivenessMs}},
                             "track", newTrackId);
                    appendOp("track.setKeepMonitoringLatency",
                             {{"id", newTrackId}, {"on", track.keepMonitoringLatency}}, "track",
                             newTrackId);
                }

                if (track.hasGroupSumming) {
                    appendOp("group.setSummingFlavor",
                             {{"id", newTrackId}, {"flavor", track.summingFlavor}}, "track",
                             newTrackId);
                    appendOp("group.setSummingDrive",
                             {{"id", newTrackId}, {"db", track.summingDriveDb}}, "track",
                             newTrackId);
                    appendOp("group.setSumming",
                             {{"id", newTrackId}, {"enabled", track.summingEnabled}}, "track",
                             newTrackId);
                }

                if (track.kind == "return") {
                    pastedReturnTaps[newTrackId] = track.returnSendTapPreFader;
                    if (track.returnSendTapPreFader.has_value()) {
                        appendOp("return.setSendTap",
                                 {{"id", newTrackId}, {"pre", *track.returnSendTapPreFader}},
                                 "track", newTrackId);
                    }
                }

                // Take lanes: lane.create inserts at ord=0 and shifts others down,
                // so iterate source take lanes in reverse ord to preserve exact order.
                if (track.kind == "audio" || track.kind == "midi" || track.kind == "instrument") {
                    SQLite::Statement lanesQ(
                        source.db(),
                        "SELECT id, name, muted FROM lanes "
                        "WHERE track_id = ? AND kind = 'take' ORDER BY ord DESC, id DESC");
                    lanesQ.bind(1, track.id);
                    while (lanesQ.executeStep()) {
                        const std::int64_t oldLaneId = lanesQ.getColumn(0).getInt64();
                        const std::string laneName = lanesQ.getColumn(1).getString();
                        const bool laneMuted = lanesQ.getColumn(2).getInt() != 0;
                        const std::int64_t newLaneId = ids.next("lanes");
                        result.takeLaneIds[oldLaneId] = newLaneId;

                        OpRequest laneReq = createTakeLane(dest, newLaneId, newTrackId, laneName);
                        laneReq.actor = actor;
                        laneReq.actorDetail = std::string(actorDetail);
                        laneReq.label = "Paste Track";
                        laneReq.targetKind = "lane";
                        laneReq.targetId = newLaneId;
                        requests.push_back(std::move(laneReq));

                        if (laneMuted) {
                            appendOp("lane.setMute", {{"id", newLaneId}, {"on", true}}, "lane",
                                     newLaneId);
                        }
                    }
                }
            }

            // 4. Copy track sends (routing kind='send' + routing_send_taps).
            //    Sends to a return inside the pasted subtree map directly; sends
            //    to an external return match by unique return name in dest;
            //    unmappable sends are reported in warnings rather than dropped silently.
            {
                const bool hasSendTapsTable = tablePresent(source.db(), "routing_send_taps");
                for (const auto& track : tracks) {
                    const std::int64_t newSrcTrackId = result.trackIds.at(track.id);
                    SQLite::Statement sendsQ(
                        source.db(),
                        "SELECT id, dst_kind, dst_id, gain_db, pan, pre_fader, enabled "
                        "FROM routing WHERE src_kind = 'track' AND src_id = ? AND kind = 'send' "
                        "ORDER BY ord, id");
                    sendsQ.bind(1, track.id);
                    while (sendsQ.executeStep()) {
                        const std::int64_t oldRouteId = sendsQ.getColumn(0).getInt64();
                        const std::string dstKind = sendsQ.getColumn(1).getString();
                        const std::int64_t oldDstId = sendsQ.getColumn(2).getInt64();
                        const double gainDb = sendsQ.getColumn(3).getDouble();
                        const double pan = sendsQ.getColumn(4).getDouble();
                        const bool preFader = sendsQ.getColumn(5).getInt() != 0;
                        const bool enabled = sendsQ.getColumn(6).getInt() != 0;

                        std::optional<std::int64_t> destDstId;
                        std::string dstDesc = "id=" + std::to_string(oldDstId);

                        if (dstKind == "track") {
                            if (const auto it = result.trackIds.find(oldDstId);
                                it != result.trackIds.end()) {
                                destDstId = it->second;
                            } else {
                                SQLite::Statement retQ(
                                    source.db(),
                                    "SELECT kind, name FROM tracks WHERE id = ?");
                                retQ.bind(1, oldDstId);
                                if (retQ.executeStep() &&
                                    retQ.getColumn(0).getString() == "return") {
                                    const std::string retName = retQ.getColumn(1).getString();
                                    dstDesc = "return '" + retName + "'";
                                    destDstId = findMatchingDestReturn(dest.db(), retName);
                                }
                            }
                        }

                        if (!destDstId.has_value()) {
                            result.warnings.push_back(
                                "send from track '" + track.name + "' to " + dstDesc +
                                " could not be mapped to a destination return track");
                            continue;
                        }

                        const std::int64_t newRouteId = ids.next("routing");
                        result.routingIds[oldRouteId] = newRouteId;

                        appendOp("routing.connect",
                                 {{"id", newRouteId},
                                  {"srcKind", "track"},
                                  {"src", newSrcTrackId},
                                  {"dstKind", "track"},
                                  {"dst", *destDstId},
                                  {"kind", "send"}},
                                 "routing", newRouteId);
                        appendOp("routing.setGain", {{"id", newRouteId}, {"db", gainDb}},
                                 "routing", newRouteId);
                        appendOp("routing.setPan", {{"id", newRouteId}, {"pan", pan}},
                                 "routing", newRouteId);
                        if (!enabled) {
                            appendOp("routing.setEnabled",
                                     {{"id", newRouteId}, {"enabled", false}}, "routing",
                                     newRouteId);
                        }

                        std::optional<std::string> explicitTap;
                        if (hasSendTapsTable) {
                            SQLite::Statement tapQ(
                                source.db(),
                                "SELECT tap FROM routing_send_taps WHERE routing_id = ?");
                            tapQ.bind(1, oldRouteId);
                            if (tapQ.executeStep()) {
                                explicitTap = tapQ.getColumn(0).getString();
                            }
                        }

                        if (explicitTap.has_value()) {
                            appendOp("routing.setSendTap",
                                     {{"id", newRouteId}, {"tap", *explicitTap}}, "routing",
                                     newRouteId);
                        } else if (preFader) {
                            const auto destReturnTap =
                                readDestReturnTap(dest.db(), *destDstId, pastedReturnTaps);
                            if (destReturnTap.has_value() && !*destReturnTap) {
                                result.warnings.push_back(
                                    "send from track '" + track.name + "' to " + dstDesc +
                                    " kept destination return's post-fader tap policy");
                            } else {
                                appendOp("routing.setPreFader",
                                         {{"id", newRouteId}, {"pre", true}}, "routing",
                                         newRouteId);
                            }
                        }
                    }
                }
            }

            // 5. Copy top-level device chains and devices via readDeviceCopy().
            std::map<PluginRefKey, std::int64_t> destPluginRefs;
            std::set<std::string> stagedStateBlobHashes;

            for (const auto& track : tracks) {
                const std::int64_t newTrackId = result.trackIds.at(track.id);
                SQLite::Statement chainsQ(
                    source.db(),
                    "SELECT id, ord, name FROM device_chains "
                    "WHERE track_id = ? AND parent_device_id IS NULL ORDER BY ord, id");
                chainsQ.bind(1, track.id);

                struct SrcChain {
                    std::int64_t id = 0;
                    std::int64_t ord = 0;
                    std::string name;
                };
                std::vector<SrcChain> srcChains;
                while (chainsQ.executeStep()) {
                    srcChains.push_back(
                        {chainsQ.getColumn(0).getInt64(), chainsQ.getColumn(1).getInt64(),
                         chainsQ.getColumn(2).getString()});
                }

                for (const auto& chain : srcChains) {
                    const std::int64_t newChainId = ids.next("device_chains");
                    result.chainIds[chain.id] = newChainId;
                    appendOp("chain.create",
                             {{"id", newChainId},
                              {"track", newTrackId},
                              {"ord", chain.ord},
                              {"name", chain.name}},
                             "chain", newChainId);

                    SQLite::Statement devsQ(
                        source.db(),
                        "SELECT id, ord FROM devices WHERE chain_id = ? ORDER BY ord, id");
                    devsQ.bind(1, chain.id);

                    std::vector<std::pair<std::int64_t, std::int64_t>> srcDevs;
                    while (devsQ.executeStep()) {
                        srcDevs.emplace_back(devsQ.getColumn(0).getInt64(),
                                             devsQ.getColumn(1).getInt64());
                    }

                    for (const auto& [srcDevId, ord] : srcDevs) {
                        const DeviceCopy copy = readDeviceCopy(source, srcDevId);
                        if (!copy.ok) {
                            throw std::runtime_error(
                                copy.error.empty() ? "failed to read source device" : copy.error);
                        }
                        if (copy.hasNestedChains) {
                            throw std::runtime_error(
                                "nested rack chains are not supported in cross-project paste");
                        }

                        std::optional<std::int64_t> destRefId;
                        if (copy.reference.has_value()) {
                            const auto& ref = *copy.reference;
                            const PluginRefKey key{ref.format, ref.uid, ref.shellId};
                            if (const auto it = destPluginRefs.find(key);
                                it != destPluginRefs.end()) {
                                destRefId = it->second;
                            } else {
                                SQLite::Statement refQ(
                                    dest.db(),
                                    "SELECT id FROM plugin_refs "
                                    "WHERE format = ? AND uid = ? AND shell_id IS ?");
                                refQ.bind(1, ref.format);
                                refQ.bind(2, ref.uid);
                                if (ref.shellId.has_value()) {
                                    refQ.bind(3, *ref.shellId);
                                } else {
                                    refQ.bind(3);
                                }
                                if (refQ.executeStep()) {
                                    destRefId = refQ.getColumn(0).getInt64();
                                } else {
                                    const std::int64_t newRefId = ids.next("plugin_refs");
                                    const Payload refRow = buildPluginRefRow(
                                        dest.db(),
                                        {{"id", newRefId},
                                         {"format", ref.format},
                                         {"uid", ref.uid},
                                         {"vendor", ref.vendor},
                                         {"name", ref.name},
                                         {"version", ref.version},
                                         {"subtype", ref.subtype},
                                         {"path_hint", ref.pathHint},
                                         {"is_shell", ref.isShell ? 1 : 0},
                                         {"shell_id",
                                          ref.shellId ? Payload(*ref.shellId)
                                                      : Payload(nullptr)}});
                                    appendOp("pluginRef.register",
                                             {{"id", newRefId}, {"row", refRow}}, "plugin",
                                             newRefId);
                                    destRefId = newRefId;
                                }
                                destPluginRefs[key] = *destRefId;
                            }
                        }

                        const std::int64_t newDevId = ids.next("devices");
                        result.deviceIds[srcDevId] = newDevId;

                        Payload insertPayload = copy.insert;
                        insertPayload["id"] = newDevId;
                        insertPayload["chain"] = newChainId;
                        insertPayload["ord"] = ord;
                        insertPayload["ref"] =
                            destRefId ? Payload(*destRefId) : Payload(nullptr);
                        if (insertPayload.contains("remoteHost")) {
                            insertPayload["remoteHost"] = nullptr;
                        }

                        Payload stateEntries = Payload::array();
                        if (insertPayload.contains("state") &&
                            insertPayload.at("state").is_array()) {
                            stateEntries = std::move(insertPayload["state"]);
                            insertPayload.erase("state");
                        }

                        appendOp("device.insert", std::move(insertPayload), "device", newDevId);

                        for (const auto& st : stateEntries) {
                            const std::string role = st.at("role").get<std::string>();
                            const std::string hash = st.at("hash").get<std::string>();
                            const std::string hint = st.value("hint", std::string{});

                            if (stagedStateBlobHashes.contains(hash) ||
                                dest.getStateBlob(hash).has_value()) {
                                ++result.reusedStateBlobCount;
                            } else {
                                const auto bytes = source.getStateBlob(hash);
                                if (!bytes.has_value()) {
                                    throw std::runtime_error("missing source state_blob: " + hash);
                                }
                                txInputs.stateBlobs.push_back(OpStateBlob{hash, *bytes});
                                stagedStateBlobHashes.insert(hash);
                                ++result.copiedStateBlobCount;
                            }

                            appendOp("device.loadState",
                                     {{"dev", newDevId},
                                      {"role", role},
                                      {"hash", hash},
                                      {"hint", hint}},
                                     "device", newDevId);
                        }
                    }
                }
            }

            // 5b. Copy remaining routing touching the pasted subtree now that both
            //     result.trackIds and result.deviceIds are populated:
            //     - kind='main' (origin='user'): copy when destination track is inside
            //       the pasted subtree; warn on external user main routes.
            //     - kind='sidechain': copy when source track and destination device are
            //       both inside the pasted subtree, carrying routing_key_taps via keyTap;
            //       warn on every external or non-device sidechain touching the subtree.
            //     - kind='vca' and any other non-send route touching the subtree: warn.
            {
                const bool hasKeyTapsTable = tablePresent(source.db(), "routing_key_taps");
                std::set<std::int64_t> seenRouteIds;

                // 5b.1 User Main outputs from pasted tracks.
                for (const auto& track : tracks) {
                    const std::int64_t newSrcTrackId = result.trackIds.at(track.id);
                    SQLite::Statement mainQ(
                        source.db(),
                        "SELECT id, dst_kind, dst_id, gain_db, pan, enabled, origin "
                        "FROM routing WHERE src_kind = 'track' AND src_id = ? AND kind = 'main' "
                        "ORDER BY ord, id");
                    mainQ.bind(1, track.id);
                    while (mainQ.executeStep()) {
                        const std::int64_t oldRouteId = mainQ.getColumn(0).getInt64();
                        const std::string dstKind = mainQ.getColumn(1).getString();
                        const std::int64_t oldDstId = mainQ.getColumn(2).getInt64();
                        const double gainDb = mainQ.getColumn(3).getDouble();
                        const double pan = mainQ.getColumn(4).getDouble();
                        const bool enabled = mainQ.getColumn(5).getInt() != 0;
                        const std::string origin =
                            mainQ.getColumn(6).isNull() ? std::string("user")
                                                        : mainQ.getColumn(6).getString();
                        seenRouteIds.insert(oldRouteId);

                        if (origin != "user") {
                            continue;
                        }
                        if (dstKind == "track") {
                            if (const auto dstIt = result.trackIds.find(oldDstId);
                                dstIt != result.trackIds.end()) {
                                const std::int64_t newRouteId = ids.next("routing");
                                result.routingIds[oldRouteId] = newRouteId;
                                appendOp("routing.connect",
                                         {{"id", newRouteId},
                                          {"srcKind", "track"},
                                          {"src", newSrcTrackId},
                                          {"dstKind", "track"},
                                          {"dst", dstIt->second},
                                          {"kind", "main"}},
                                         "routing", newRouteId);
                                if (gainDb != 0.0) {
                                    appendOp("routing.setGain",
                                             {{"id", newRouteId}, {"db", gainDb}}, "routing",
                                             newRouteId);
                                }
                                if (pan != 0.0) {
                                    appendOp("routing.setPan", {{"id", newRouteId}, {"pan", pan}},
                                             "routing", newRouteId);
                                }
                                if (!enabled) {
                                    appendOp("routing.setEnabled",
                                             {{"id", newRouteId}, {"enabled", false}}, "routing",
                                             newRouteId);
                                }
                                continue;
                            }
                        }
                        result.warnings.push_back(
                            "user main route (id=" + std::to_string(oldRouteId) +
                            ") from track '" + track.name + "' to " + dstKind + ":" +
                            std::to_string(oldDstId) +
                            " targets outside pasted subtree and was not pasted");
                    }
                }

                // 5b.2 All other non-send routing rows touching any pasted track or device
                //      (sidechain, external incoming main, vca, etc.).
                SQLite::Statement otherQ(
                    source.db(),
                    "SELECT id, src_kind, src_id, dst_kind, dst_id, kind, gain_db, pan, enabled, "
                    "origin FROM routing WHERE kind != 'send' ORDER BY ord, id");
                while (otherQ.executeStep()) {
                    const std::int64_t oldRouteId = otherQ.getColumn(0).getInt64();
                    if (seenRouteIds.contains(oldRouteId)) {
                        continue;
                    }
                    const std::string srcKind = otherQ.getColumn(1).getString();
                    const std::int64_t oldSrcId = otherQ.getColumn(2).getInt64();
                    const std::string dstKind = otherQ.getColumn(3).getString();
                    const std::int64_t oldDstId = otherQ.getColumn(4).getInt64();
                    const std::string kind = otherQ.getColumn(5).getString();
                    const double gainDb = otherQ.getColumn(6).getDouble();
                    const double pan = otherQ.getColumn(7).getDouble();
                    const bool enabled = otherQ.getColumn(8).getInt() != 0;
                    const std::string origin =
                        otherQ.getColumn(9).isNull() ? std::string("user")
                                                     : otherQ.getColumn(9).getString();

                    const bool srcInSubtree =
                        (srcKind == "track" && result.trackIds.contains(oldSrcId)) ||
                        (srcKind == "device" && result.deviceIds.contains(oldSrcId));
                    const bool dstInSubtree =
                        (dstKind == "track" && result.trackIds.contains(oldDstId)) ||
                        (dstKind == "device" && result.deviceIds.contains(oldDstId));
                    if (!srcInSubtree && !dstInSubtree) {
                        continue;
                    }
                    seenRouteIds.insert(oldRouteId);

                    if (kind == "sidechain") {
                        if (srcKind == "track" && dstKind == "device" &&
                            result.trackIds.contains(oldSrcId) &&
                            result.deviceIds.contains(oldDstId)) {
                            const std::int64_t newRouteId = ids.next("routing");
                            result.routingIds[oldRouteId] = newRouteId;

                            Payload connectPayload{
                                {"id", newRouteId},
                                {"srcKind", "track"},
                                {"src", result.trackIds.at(oldSrcId)},
                                {"dstKind", "device"},
                                {"dst", result.deviceIds.at(oldDstId)},
                                {"kind", "sidechain"},
                            };
                            if (hasKeyTapsTable) {
                                SQLite::Statement tapQ(
                                    source.db(),
                                    "SELECT tap FROM routing_key_taps WHERE routing_id = ?");
                                tapQ.bind(1, oldRouteId);
                                if (tapQ.executeStep()) {
                                    connectPayload["keyTap"] = tapQ.getColumn(0).getString();
                                }
                            }
                            appendOp("routing.connect", std::move(connectPayload), "routing",
                                     newRouteId);
                            if (gainDb != 0.0) {
                                appendOp("routing.setGain", {{"id", newRouteId}, {"db", gainDb}},
                                         "routing", newRouteId);
                            }
                            if (pan != 0.0) {
                                appendOp("routing.setPan", {{"id", newRouteId}, {"pan", pan}},
                                         "routing", newRouteId);
                            }
                            if (!enabled) {
                                appendOp("routing.setEnabled",
                                         {{"id", newRouteId}, {"enabled", false}}, "routing",
                                         newRouteId);
                            }
                        } else {
                            result.warnings.push_back(
                                "sidechain route (id=" + std::to_string(oldRouteId) + ", " +
                                srcKind + ":" + std::to_string(oldSrcId) + " -> " + dstKind + ":" +
                                std::to_string(oldDstId) +
                                ") crosses outside pasted subtree and was not pasted");
                        }
                    } else if (kind == "main") {
                        if (origin == "user") {
                            result.warnings.push_back(
                                "main route (id=" + std::to_string(oldRouteId) + ", " + srcKind +
                                ":" + std::to_string(oldSrcId) + " -> " + dstKind + ":" +
                                std::to_string(oldDstId) +
                                ") crosses outside pasted subtree and was not pasted");
                        }
                    } else {
                        result.warnings.push_back(
                            "routing row (id=" + std::to_string(oldRouteId) + ", kind='" + kind +
                            "', " + srcKind + ":" + std::to_string(oldSrcId) + " -> " + dstKind +
                            ":" + std::to_string(oldDstId) + ") was not pasted across projects");
                    }
                }
            }

            // 6. Copy clips (audio & MIDI) across all pasted tracks, preserving
            //    take-lane assignments and internal alias links.
            {
                struct SrcClip {
                    std::int64_t id = 0;
                    std::int64_t trackId = 0;
                    std::optional<std::int64_t> aliasOf;
                };
                std::vector<SrcClip> allClips;
                for (const auto& track : tracks) {
                    SQLite::Statement clipsQ(
                        source.db(),
                        "SELECT id, alias_of FROM clips WHERE track_id = ? ORDER BY id");
                    clipsQ.bind(1, track.id);
                    while (clipsQ.executeStep()) {
                        SrcClip sc;
                        sc.id = clipsQ.getColumn(0).getInt64();
                        sc.trackId = track.id;
                        if (!clipsQ.getColumn(1).isNull()) {
                            sc.aliasOf = clipsQ.getColumn(1).getInt64();
                        }
                        allClips.push_back(sc);
                    }
                }

                // Pre-allocate destination clip ids so alias_of targets are known.
                std::map<std::int64_t, SrcClip> clipById;
                for (const auto& sc : allClips) {
                    result.clipIds[sc.id] = ids.next("clips");
                    clipById[sc.id] = sc;
                }

                // Order clips so every internal alias master precedes its aliases.
                std::vector<SrcClip> orderedClips;
                orderedClips.reserve(allClips.size());
                std::set<std::int64_t> emitted;
                std::set<std::int64_t> visiting;
                auto visitClip = [&](auto&& self, std::int64_t cid) -> void {
                    if (emitted.contains(cid)) {
                        return;
                    }
                    if (!visiting.insert(cid).second) {
                        throw std::runtime_error("cycle detected in clip alias_of chain");
                    }
                    const auto& sc = clipById.at(cid);
                    if (sc.aliasOf.has_value() && clipById.contains(*sc.aliasOf)) {
                        self(self, *sc.aliasOf);
                    }
                    visiting.erase(cid);
                    emitted.insert(cid);
                    orderedClips.push_back(sc);
                };
                for (const auto& sc : allClips) {
                    visitClip(visitClip, sc.id);
                }

                OpContext srcCtx{source, source.db()};
                for (const auto& sc : orderedClips) {
                    Payload img = clip_edit::captureImage(srcCtx, sc.id);
                    const std::int64_t newClipId = result.clipIds.at(sc.id);
                    const std::int64_t destTrackId = result.trackIds.at(sc.trackId);

                    img["id"] = newClipId;
                    img["row"]["id"] = newClipId;
                    img["row"]["track_id"] = destTrackId;

                    // Preserve take-lane assignment if the clip belongs to a pasted take lane.
                    if (!img["row"]["lane_id"].is_null()) {
                        const std::int64_t oldLaneId =
                            detail::signedInteger(img["row"]["lane_id"]);
                        if (const auto lit = result.takeLaneIds.find(oldLaneId);
                            lit != result.takeLaneIds.end()) {
                            img["row"]["lane_id"] = lit->second;
                        } else {
                            img["row"]["lane_id"] = nullptr;
                        }
                    }

                    // Preserve internal alias links; warn & clear links outside the pasted subtree.
                    if (sc.aliasOf.has_value()) {
                        if (const auto ait = result.clipIds.find(*sc.aliasOf);
                            ait != result.clipIds.end()) {
                            img["row"]["alias_of"] = ait->second;
                        } else {
                            img["row"]["alias_of"] = nullptr;
                            result.warnings.push_back(
                                "clip " + std::to_string(sc.id) + " alias_of external clip " +
                                std::to_string(*sc.aliasOf) + " was unlinked on cross-project paste");
                        }
                    } else {
                        img["row"]["alias_of"] = nullptr;
                    }
                    img["alias_links"] = Payload::array();

                    auto& children = img["children"];
                    // Clip automation streams are copied in Step 7 after lanes are remapped.
                    children.erase("automation_data");
                    children.erase("automation_segment_ranges");

                    for (const auto& child : clip_edit::childTables()) {
                        if (!children.contains(child.table)) {
                            continue;
                        }
                        for (auto& row : children[child.table]) {
                            row["clip_id"] = newClipId;
                            if (std::string_view(child.table) == "audio_clips") {
                                const std::int64_t oldMedia =
                                    detail::signedInteger(row.at("media_id"));
                                row["media_id"] = result.mediaIds.at(oldMedia);
                            }
                            if (std::string_view(child.key) == "id") {
                                row["id"] = ids.next(child.table);
                            }
                        }
                    }

                    edit::RowEdits edits(dest.db());
                    clip_edit::createImage(edits, img);
                    appendOp("clip.duplicate",
                             {{"id", newClipId}, {"changes", edits.changes()}}, "clip", newClipId);
                }
            }

            // 7. Copy track, device, clip, and send-routing automation lanes +
            //    automation_data + curve ranges.
            {
                struct SrcLane {
                    std::int64_t oldId = 0;
                    automation::LaneSpec spec;
                };
                std::vector<SrcLane> srcLanes;

                auto collectLanes = [&](const char* ownerKind, std::int64_t oldOwnerId,
                                        std::int64_t newOwnerId) {
                    SQLite::Statement lq(
                        source.db(),
                        "SELECT id, param_ref, param_name, time_base, value_domain, unit, "
                        "default_value, min_value, max_value, enabled, visible, height "
                        "FROM automation_lanes WHERE owner_kind = ? AND owner_id = ? ORDER BY id");
                    lq.bind(1, ownerKind);
                    lq.bind(2, oldOwnerId);
                    while (lq.executeStep()) {
                        SrcLane sl;
                        sl.oldId = lq.getColumn(0).getInt64();
                        sl.spec.id = ids.next("automation_lanes");
                        sl.spec.ownerKind = ownerKind;
                        sl.spec.ownerId = newOwnerId;
                        sl.spec.paramRef = lq.getColumn(1).getString();
                        sl.spec.paramName = lq.getColumn(2).getString();
                        sl.spec.timeBase = lq.getColumn(3).getInt64();
                        sl.spec.valueDomain = lq.getColumn(4).getString();
                        sl.spec.unit = lq.getColumn(5).getString();
                        sl.spec.defaultValue = lq.getColumn(6).getDouble();
                        if (!lq.getColumn(7).isNull()) {
                            sl.spec.minValue = lq.getColumn(7).getDouble();
                        }
                        if (!lq.getColumn(8).isNull()) {
                            sl.spec.maxValue = lq.getColumn(8).getDouble();
                        }
                        sl.spec.enabled = lq.getColumn(9).getInt() != 0;
                        sl.spec.visible = lq.getColumn(10).getInt() != 0;
                        sl.spec.height = lq.getColumn(11).getInt64();
                        result.laneIds[sl.oldId] = sl.spec.id;
                        srcLanes.push_back(std::move(sl));
                    }
                };

                for (const auto& [oldTrackId, newTrackId] : result.trackIds) {
                    collectLanes("track", oldTrackId, newTrackId);
                }
                for (const auto& [oldDevId, newDevId] : result.deviceIds) {
                    collectLanes("device", oldDevId, newDevId);
                }
                for (const auto& [oldClipId, newClipId] : result.clipIds) {
                    collectLanes("clip", oldClipId, newClipId);
                }
                for (const auto& [oldRouteId, newRouteId] : result.routingIds) {
                    collectLanes("routing", oldRouteId, newRouteId);
                }

                for (const auto& sl : srcLanes) {
                    OpRequest createLaneReq = automation::createLane(sl.spec);
                    createLaneReq.actor = actor;
                    createLaneReq.actorDetail = std::string(actorDetail);
                    createLaneReq.label = "Paste Track";
                    requests.push_back(std::move(createLaneReq));

                    SQLite::Statement dq(
                        source.db(),
                        "SELECT id, clip_id, data FROM automation_data "
                        "WHERE lane_id = ? ORDER BY id");
                    dq.bind(1, sl.oldId);
                    while (dq.executeStep()) {
                        const std::int64_t oldDataId = dq.getColumn(0).getInt64();
                        Payload destClip = nullptr;
                        if (!dq.getColumn(1).isNull()) {
                            const std::int64_t oldClipId = dq.getColumn(1).getInt64();
                            const auto cit = result.clipIds.find(oldClipId);
                            if (cit == result.clipIds.end()) {
                                continue;
                            }
                            destClip = cit->second;
                        }

                        const std::int64_t newDataId = ids.next("automation_data");
                        const auto col = dq.getColumn(2);
                        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(col.getBytes()));
                        if (!bytes.empty()) {
                            std::memcpy(bytes.data(), col.getBlob(), bytes.size());
                        }

                        Payload ranges = automation::range_rows::capture(source.db(), oldDataId);
                        for (auto& rangeRow : ranges) {
                            rangeRow["id"] = ids.next("automation_segment_ranges");
                            rangeRow["data_id"] = newDataId;
                            rangeRow["clip_id"] = destClip;
                        }

                        appendOp("automation.restoreData",
                                 {{"id", newDataId},
                                  {"lane", sl.spec.id},
                                  {"clip", destClip},
                                  {"state",
                                   {{"present", true},
                                    {"bytes", Payload::binary(std::move(bytes))},
                                    {"ranges", std::move(ranges)}}}},
                                 "automation_data", newDataId);
                    }
                }
            }
        }

        // 8. Commit all ops as ONE atomic undoable transaction on dest.
        CommitResult commitRes;
        if (txInputs.stateBlobs.empty()) {
            commitRes = OpJournal(dest).commit(requests, options);
        } else {
            commitRes =
                OpJournal(dest).commitCaptured(requests, appliedSource(dest), options, txInputs);
        }

        if (!commitRes.ok) {
            result.error = commitRes.error.empty() ? "paste transaction failed" : commitRes.error;
            return result;
        }

        result.ok = true;
        result.txnId = commitRes.txnId;
        result.trackId = rootNewTrackId;
        return result;
    } catch (const std::exception& ex) {
        result.error = ex.what();
        return result;
    }
}

}  // namespace adi
