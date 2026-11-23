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
#include "adi/media/blake3.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/media/publish_file.hpp"
#include "adi/media/published_rollback.hpp"
#include "adi/op_transaction_resources.hpp"
#include "adi/row_capture.hpp"
#include "adi/row_ids.hpp"
#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
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

struct StageDir {
    fs::path path;
    explicit StageDir(const fs::path& parent) {
        static std::atomic<unsigned long long> serial{0};
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned attempt = 0U; attempt < 128U; ++attempt) {
            const fs::path candidate =
                parent / (".adi-paste-" + std::to_string(tick) + "-" +
                          std::to_string(serial.fetch_add(1ULL, std::memory_order_relaxed)));
            std::error_code ec;
            if (fs::create_directory(candidate, ec)) {
                path = candidate;
                return;
            }
            if (ec && ec != std::errc::file_exists) {
                throw fs::filesystem_error("create paste staging directory", candidate, ec);
            }
        }
        throw std::runtime_error("cannot reserve paste staging directory");
    }
    ~StageDir() {
        if (!path.empty()) {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    }
    StageDir(const StageDir&) = delete;
    StageDir& operator=(const StageDir&) = delete;
};

std::string safeAudioFilename(const std::string& original, std::int64_t id) {
    std::string name = original;
    for (char& c : name) {
        if (static_cast<unsigned char>(c) < 32U ||
            std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos) {
            c = '_';
        }
    }
    while (!name.empty() && (name.back() == '.' || name.back() == ' ')) {
        name.pop_back();
    }
    if (name.empty() || name == "." || name == ".." || name.size() > 180U) {
        name = "media-" + std::to_string(id) + ".bin";
    }
    std::string stem = name.substr(0U, name.find('.'));
    for (char& c : stem) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" ||
        (stem.size() == 4U && (stem.starts_with("COM") || stem.starts_with("LPT")) &&
         stem[3] >= '1' && stem[3] <= '9')) {
        name = "_" + name;
    }
    return name;
}

void ensureAudioDirectory(const fs::path& folder) {
    const fs::path audioDir = folder / "audio";
    std::error_code ec;
    const auto status = fs::symlink_status(audioDir, ec);
    if (fs::is_symlink(status)) {
        throw std::runtime_error("destination audio/ must not be a symlink");
    }
    if (ec && ec != std::errc::no_such_file_or_directory) {
        throw fs::filesystem_error("inspect destination audio/", audioDir, ec);
    }
    fs::create_directories(audioDir);
}

void copyFileVerified(const fs::path& src, const fs::path& dst, const std::string& expectedHash) {
    std::ifstream in(src, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open source media: " + media::pathUtf8(src));
    }
    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("cannot create staged media: " + media::pathUtf8(dst));
    }
    std::vector<char> buf(65536U);
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const auto got = in.gcount();
        if (got > 0) {
            out.write(buf.data(), got);
            if (!out) {
                throw std::runtime_error("write failed while staging media");
            }
        }
    }
    if (!in.eof()) {
        throw std::runtime_error("read failed while staging media");
    }
    out.close();
    if (!out) {
        throw std::runtime_error("close failed while staging media");
    }
    const auto actual = media::blake3File(dst);
    if (!actual || actual.hex != expectedHash) {
        throw std::runtime_error("staged media hash mismatch: " + media::pathUtf8(src));
    }
}

fs::path publishCopiedMedia(const fs::path& stagedFile,
                            const fs::path& destFolder,
                            const std::string& origName,
                            std::int64_t mediaId,
                            media::detail::PublishedRollback& rollback) {
    const std::string base = safeAudioFilename(origName, mediaId);
    for (unsigned attempt = 0U; attempt < 10000U; ++attempt) {
        const std::string fileName =
            attempt == 0U ? base
                          : (std::to_string(mediaId) + "-" + std::to_string(attempt) + "-" + base);
        const fs::path target = destFolder / "audio" / media::pathFromUtf8(fileName);
        rollback.files.push_back({target, std::nullopt});
        const auto pub =
            media::publishFile(stagedFile, target, {}, media::PublishPolicy::VerifiedCopyFallback);
        if (pub.status == media::PublishStatus::published) {
            rollback.files.back().identity = pub.identity;
            return target;
        }
        rollback.files.pop_back();
        if (pub.status != media::PublishStatus::exists) {
            throw std::runtime_error("publish media failed: " + media::pathUtf8(target) + ": " +
                                     pub.error);
        }
    }
    throw std::runtime_error("too many filename collisions for media: " + origName);
}

using PluginRefKey = std::tuple<std::string, std::string, std::optional<std::int64_t>>;

struct TrackSnapshot {
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
    double width = 1.0;
    double inputGainDb = 0.0;
    bool phaseInvert = false;
    std::int64_t delaySamples = 0;
};

TrackSnapshot readTrackSnapshot(SQLite::Database& db, std::int64_t trackId) {
    SQLite::Statement q(db,
                        "SELECT kind, name, color, muted, soloed, solo_defeat, record_armed, "
                        "monitor_mode, input_ref, time_base, locked, automation_mode "
                        "FROM tracks WHERE id=?");
    q.bind(1, trackId);
    if (!q.executeStep()) {
        throw std::runtime_error("source track does not exist");
    }

    TrackSnapshot snap;
    snap.kind = q.getColumn(0).getString();
    snap.name = q.getColumn(1).getString();
    snap.color = q.getColumn(2).getInt64();
    snap.muted = q.getColumn(3).getInt() != 0;
    snap.soloed = q.getColumn(4).getInt() != 0;
    snap.soloDefeat = q.getColumn(5).getInt() != 0;
    snap.recordArmed = q.getColumn(6).getInt() != 0;
    snap.monitorMode = q.getColumn(7).getInt64();
    snap.inputRef = q.getColumn(8).isNull() ? std::string{} : q.getColumn(8).getString();
    snap.timeBase = q.getColumn(9).getInt64();
    snap.locked = q.getColumn(10).getInt() != 0;
    snap.automationMode = q.getColumn(11).getInt64();

    SQLite::Statement m(db,
                        "SELECT volume_db, pan, width, input_gain_db, phase_invert, delay_samples "
                        "FROM mixer_strip WHERE track_id=?");
    m.bind(1, trackId);
    if (m.executeStep()) {
        snap.hasMixer = true;
        snap.volumeDb = m.getColumn(0).getDouble();
        snap.pan = m.getColumn(1).getDouble();
        snap.width = m.getColumn(2).getDouble();
        snap.inputGainDb = m.getColumn(3).getDouble();
        snap.phaseInvert = m.getColumn(4).getInt() != 0;
        snap.delaySamples = m.getColumn(5).getInt64();
    }
    return snap;
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

}  // namespace

ProjectPasteResult pasteTrackFromProject(Store& source,
                                         std::int64_t sourceTrackId,
                                         Store& dest,
                                         Actor actor,
                                         std::string_view actorDetail) {
    ProjectPasteResult result;
    media::detail::PublishedRollback rollback;

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
        std::int64_t newTrackId = 0;

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

            // 1. Read source track & validate.
            const TrackSnapshot track = readTrackSnapshot(source.db(), sourceTrackId);
            if (track.kind == "master") {
                result.error = "master track cannot be pasted into another project";
                return result;
            }

            // 2. Collect and publish missing media files by BLAKE3 hash.
            const fs::path srcFolder = media::projectFolder(source);
            const fs::path destFolder = media::projectFolder(dest);
            std::map<std::string, std::int64_t> hashToDestMedia;
            std::optional<StageDir> stageDir;

            {
                SQLite::Statement mediaQuery(
                    source.db(),
                    "SELECT DISTINCT a.media_id "
                    "FROM clips c JOIN audio_clips a ON a.clip_id = c.id "
                    "WHERE c.track_id = ? ORDER BY a.media_id");
                mediaQuery.bind(1, sourceTrackId);

                std::vector<std::int64_t> srcMediaIds;
                while (mediaQuery.executeStep()) {
                    srcMediaIds.push_back(mediaQuery.getColumn(0).getInt64());
                }

                for (const std::int64_t srcMediaId : srcMediaIds) {
                    Payload row = media::mediaRow(source.db(), srcMediaId);
                    const std::string hash = row.at("hash_blake3").get<std::string>();

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

                    // Hash absent in dest: resolve, verify, stage, and publish once.
                    std::string resolveErr;
                    const fs::path srcMediaFile =
                        media::resolveMedia(srcFolder, row, resolveErr);
                    if (srcMediaFile.empty()) {
                        throw std::runtime_error(resolveErr.empty()
                                                     ? "cannot resolve source media file"
                                                     : resolveErr);
                    }

                    ensureAudioDirectory(destFolder);
                    if (!stageDir.has_value()) {
                        stageDir.emplace(destFolder / "audio");
                    }

                    const std::int64_t newMediaId = ids.next("media_files");
                    const fs::path stagedPath =
                        stageDir->path / ("stage-" + std::to_string(newMediaId) + ".bin");
                    copyFileVerified(srcMediaFile, stagedPath, hash);

                    const std::string origName = row.at("orig_name").get<std::string>();
                    const fs::path published =
                        publishCopiedMedia(stagedPath, destFolder, origName, newMediaId, rollback);

                    row["rel_path"] = media::pathUtf8(published.lexically_relative(destFolder));
                    row["abs_path_hint"] = nullptr;
                    row["missing"] = 0;

                    appendOp("media.import", {{"id", newMediaId}, {"row", std::move(row)}},
                             "media", newMediaId);

                    hashToDestMedia[hash] = newMediaId;
                    result.mediaIds[srcMediaId] = newMediaId;
                    result.publishedMediaPaths.push_back(published);
                    ++result.copiedMediaCount;
                }
            }

            // 3. Create track and copy track + mixer properties.
            newTrackId = ids.next("tracks");
            SQLite::Statement idxQ(
                dest.db(),
                "SELECT COALESCE(MAX(index_in_parent), -1) + 1 FROM tracks WHERE parent_id IS NULL");
            idxQ.executeStep();
            const std::int64_t nextIndex = idxQ.getColumn(0).getInt64();

            appendOp("track.create",
                     {{"id", newTrackId},
                      {"kind", track.kind},
                      {"name", track.name},
                      {"parent", nullptr},
                      {"index", nextIndex}},
                     "track", newTrackId);

            appendOp("track.setColor", {{"id", newTrackId}, {"color", track.color}}, "track",
                     newTrackId);
            appendOp("track.setMute", {{"id", newTrackId}, {"muted", track.muted}}, "track",
                     newTrackId);
            appendOp("track.setSolo", {{"id", newTrackId}, {"soloed", track.soloed}}, "track",
                     newTrackId);
            appendOp("track.setSoloDefeat", {{"id", newTrackId}, {"defeat", track.soloDefeat}},
                     "track", newTrackId);
            appendOp("track.setArm", {{"id", newTrackId}, {"armed", track.recordArmed}}, "track",
                     newTrackId);
            appendOp("track.setMonitorMode", {{"id", newTrackId}, {"mode", track.monitorMode}},
                     "track", newTrackId);
            if (!track.inputRef.empty()) {
                appendOp("track.setInput", {{"id", newTrackId}, {"input", track.inputRef}}, "track",
                         newTrackId);
            }
            appendOp("track.setTimeBase", {{"id", newTrackId}, {"base", track.timeBase}}, "track",
                     newTrackId);
            appendOp("track.setLocked", {{"id", newTrackId}, {"locked", track.locked}}, "track",
                     newTrackId);
            appendOp("track.setAutomationMode",
                     {{"id", newTrackId}, {"mode", track.automationMode}}, "track", newTrackId);

            if (track.hasMixer) {
                appendOp("mixer.setVolume", {{"id", newTrackId}, {"db", track.volumeDb}}, "track",
                         newTrackId);
                appendOp("mixer.setPan", {{"id", newTrackId}, {"pan", track.pan}}, "track",
                         newTrackId);
                appendOp("mixer.setWidth", {{"id", newTrackId}, {"width", track.width}}, "track",
                         newTrackId);
                appendOp("mixer.setInputGain", {{"id", newTrackId}, {"db", track.inputGainDb}},
                         "track", newTrackId);
                appendOp("mixer.setPhaseInvert",
                         {{"id", newTrackId}, {"inverted", track.phaseInvert}}, "track",
                         newTrackId);
                appendOp("mixer.setDelay", {{"id", newTrackId}, {"samples", track.delaySamples}},
                         "track", newTrackId);
            }

            // 4. Copy top-level device chains and devices via readDeviceCopy().
            std::map<PluginRefKey, std::int64_t> destPluginRefs;
            std::set<std::string> stagedStateBlobHashes;

            SQLite::Statement chainsQ(
                source.db(),
                "SELECT id, ord, name FROM device_chains "
                "WHERE track_id = ? AND parent_device_id IS NULL ORDER BY ord, id");
            chainsQ.bind(1, sourceTrackId);

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
                        throw std::runtime_error(copy.error.empty() ? "failed to read source device"
                                                                    : copy.error);
                    }
                    if (copy.hasNestedChains) {
                        throw std::runtime_error(
                            "nested rack chains are not supported in cross-project paste");
                    }

                    std::optional<std::int64_t> destRefId;
                    if (copy.reference.has_value()) {
                        const auto& ref = *copy.reference;
                        const PluginRefKey key{ref.format, ref.uid, ref.shellId};
                        if (const auto it = destPluginRefs.find(key); it != destPluginRefs.end()) {
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
                                      ref.shellId ? Payload(*ref.shellId) : Payload(nullptr)}});
                                appendOp("pluginRef.register",
                                         {{"id", newRefId}, {"row", refRow}}, "plugin", newRefId);
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
                    insertPayload["ref"] = destRefId ? Payload(*destRefId) : Payload(nullptr);
                    if (insertPayload.contains("remoteHost")) {
                        insertPayload["remoteHost"] = nullptr;
                    }

                    Payload stateEntries = Payload::array();
                    if (insertPayload.contains("state") && insertPayload.at("state").is_array()) {
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
                                 {{"dev", newDevId}, {"role", role}, {"hash", hash},
                                  {"hint", hint}},
                                 "device", newDevId);
                    }
                }
            }

            // 5. Copy clips (audio & MIDI) via clip_edit::captureImage + clip.duplicate.
            {
                SQLite::Statement clipsQ(
                    source.db(), "SELECT id FROM clips WHERE track_id = ? ORDER BY id");
                clipsQ.bind(1, sourceTrackId);

                std::vector<std::int64_t> srcClipIds;
                while (clipsQ.executeStep()) {
                    srcClipIds.push_back(clipsQ.getColumn(0).getInt64());
                }

                OpContext srcCtx{source, source.db()};
                for (const std::int64_t srcClipId : srcClipIds) {
                    Payload img = clip_edit::captureImage(srcCtx, srcClipId);
                    const std::int64_t newClipId = ids.next("clips");
                    result.clipIds[srcClipId] = newClipId;

                    img["id"] = newClipId;
                    img["row"]["id"] = newClipId;
                    img["row"]["track_id"] = newTrackId;
                    img["row"]["lane_id"] = nullptr;
                    img["row"]["alias_of"] = nullptr;
                    img["alias_links"] = Payload::array();

                    auto& children = img["children"];
                    // Clip automation streams are copied in Step 6 after lanes are remapped.
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

            // 6. Copy track and device automation lanes + automation_data + curve ranges.
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

                collectLanes("track", sourceTrackId, newTrackId);
                for (const auto& [oldDevId, newDevId] : result.deviceIds) {
                    collectLanes("device", oldDevId, newDevId);
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

        // 7. Commit all ops as ONE atomic undoable transaction on dest.
        CommitResult commitRes;
        if (txInputs.stateBlobs.empty()) {
            commitRes = OpJournal(dest).commit(requests, options);
        } else {
            commitRes =
                OpJournal(dest).commitCaptured(requests, appliedSource(dest), options, txInputs);
        }

        if (!commitRes.ok) {
            const std::string rollbackWarn = rollback.rollback();
            result.error = commitRes.error.empty() ? "paste transaction failed" : commitRes.error;
            if (!rollbackWarn.empty()) {
                result.error += " (" + rollbackWarn + ")";
            }
            result.publishedMediaPaths.clear();
            return result;
        }

        rollback.committed = true;
        result.ok = true;
        result.txnId = commitRes.txnId;
        result.trackId = newTrackId;
        return result;
    } catch (const std::exception& ex) {
        const std::string rollbackWarn = rollback.rollback();
        result.error = ex.what();
        if (!rollbackWarn.empty()) {
            result.error += " (" + rollbackWarn + ")";
        }
        result.publishedMediaPaths.clear();
        return result;
    }
}

}  // namespace adi
