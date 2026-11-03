// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/freeze_state.hpp"
#include "adi/engine/plan.hpp"
#include "adi/freeze_fingerprint.hpp"
#include "adi/row_capture.hpp"
#include "adi/store.hpp"
#include "adi/store_rows.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <set>
#include <sqlite3.h>
#include <stdexcept>
namespace adi::freeze {
bool validated(Store &store) noexcept {
    try {
        return store.frozenValidationChanges_ == sqlite3_total_changes64(store.db().getHandle()) &&
               store.frozenValidationDataVersion_ ==
                   store.db().execAndGet("PRAGMA data_version").getInt64() &&
               store.frozenValidationSchema_ == store.db().execAndGet("PRAGMA schema_version").getInt64() &&
               store.frozenValidationTempSchema_ ==
                   store.db().execAndGet("PRAGMA temp.schema_version").getInt64();
    } catch (...) {
        return false;
    }
}
void remember(Store &store) noexcept {
    try {
        store.frozenValidationChanges_ = sqlite3_total_changes64(store.db().getHandle());
        store.frozenValidationDataVersion_ = store.db().execAndGet("PRAGMA data_version").getInt64();
        store.frozenValidationSchema_ = store.db().execAndGet("PRAGMA schema_version").getInt64();
        store.frozenValidationTempSchema_ = store.db().execAndGet("PRAGMA temp.schema_version").getInt64();
    } catch (...) {
        store.frozenValidationChanges_ = -1;
        store.frozenValidationDataVersion_ = -1;
        store.frozenValidationSchema_ = -1;
        store.frozenValidationTempSchema_ = -1;
    }
}
std::vector<Payload> renders(SQLite::Database &db) {
    SQLite::Statement q(db, "SELECT * FROM frozen_renders ORDER BY track_id");
    std::vector<Payload> rows;
    while (q.executeStep())
        rows.push_back(detail::captureQueryRow(q));
    return rows;
}
Payload state(SQLite::Database &db, std::int64_t id) {
    const auto track = detail::captureRow(db, "tracks", "id", id);
    if (!track)
        throw std::runtime_error("freeze track is missing");
    return {{"armed", track->at("record_armed")},
            {"frozen", track->at("frozen")},
            {"media", track->at("freeze_media_id")},
            {"render", detail::captureRow(db, "frozen_renders", "track_id", id).value_or(Payload())}};
}
void validate(Store &store, bool source) {
    if (source && validated(store))
        return;
    if (!source) {
        if (store.db()
                .execAndGet(
                    "SELECT EXISTS(SELECT 1 FROM frozen_renders f JOIN tracks t ON t.id=f.track_id WHERE "
                    "t.record_armed<>0 OR t.frozen<>1 OR t.freeze_media_id IS NOT f.post_media_id)")
                .getInt() != 0)
            throw std::runtime_error("frozen track flags are inconsistent; unfreeze first");
        return;
    }
    for (const auto &row : renders(store.db())) {
        const auto id = detail::signedInteger(row.at("track_id"));
        const auto flags = state(store.db(), id);
        if (flags.at("armed") != 0 || flags.at("frozen") != 1 || flags.at("media") != row.at("post_media_id"))
            throw std::runtime_error("frozen track flags are inconsistent; unfreeze first");
        for (const auto *field : {"pre_media_id", "post_media_id"}) {
            SQLite::Statement media(store.db(),
                                    "SELECT frames,sample_rate,channels FROM media_files WHERE id=?");
            media.bind(1, detail::signedInteger(row.at(field)));
            if (!media.executeStep() || media.getColumn(0).isNull() || media.getColumn(1).isNull() ||
                media.getColumn(2).isNull() ||
                media.getColumn(0).getInt64() != detail::signedInteger(row.at("frames")) ||
                media.getColumn(1).getInt64() != detail::signedInteger(row.at("sample_rate")) ||
                media.getColumn(2).getInt64() != detail::signedInteger(row.at("channels")))
                throw std::runtime_error("frozen media dimensions disagree with capture provenance");
        }
        if (source) {
            const auto model = rows::readModel(store);
            const auto track = std::find_if(model.tracks.begin(), model.tracks.end(),
                                            [id](const auto &t) { return t.id == id; });
            if (track == model.tracks.end() ||
                (track->kind != "audio" && track->kind != "midi" && track->kind != "instrument"))
                throw std::runtime_error("whole-track cache requires a regular track");
            const auto plan = engine::planGraph(model);
            for (const auto &edge : plan.edges)
                if (plan.nodes.at(edge.to).trackId == id)
                    throw std::runtime_error(
                        "whole-track freeze cannot retain live incoming audio or sidechain routing");
            FingerprintSpec spec;
            spec.root = id;
            if (fingerprint(store, spec).hash != row.at("fingerprint").get<std::string>())
                throw std::runtime_error(
                    "frozen source changed; unfreeze before editing its material or devices");
        }
    }
}
bool liveControlBatch(std::span<const OpRequest> requests) {
    // These controls are outside every ordinary track's Pre/PostFX caches.
    static const std::set<std::string_view> live{
        "track.rename",   "track.setColor",       "track.setLocked", "track.setMute",
        "track.setSolo",  "track.setSoloDefeat",  "mixer.setVolume", "mixer.setPan",
        "mixer.setWidth", "mixer.setPhaseInvert", "mixer.setDelay",  "project.setName"};
    for (const auto &request : requests)
        if (!live.contains(request.opType))
            return false;
    return true;
}
namespace {
void shape(const Payload &value, std::int64_t id) {
    if (!value.is_object() || value.size() != 4 || !value.contains("armed") || !value.contains("frozen") ||
        !value.contains("media") || !value.contains("render"))
        throw std::runtime_error("invalid captured freeze state");
    for (const auto *field : {"armed", "frozen"}) {
        const auto n = detail::signedInteger(value.at(field));
        if (n != 0 && n != 1)
            throw std::runtime_error("invalid freeze switch");
    }
    if (!value.at("media").is_null() && detail::signedInteger(value.at("media")) <= 0)
        throw std::runtime_error("invalid freeze media id");
    const auto &row = value.at("render");
    if (row.is_null()) {
        if (value.at("frozen") == 0 && !value.at("media").is_null())
            throw std::runtime_error("unfreeze must clear cache flags");
        return;
    }
    static const std::set<std::string> fields{"track_id",    "pre_media_id", "post_media_id",
                                              "frames",      "sample_rate",  "channels",
                                              "fingerprint", "identities",   "original_armed"};
    if (!row.is_object() || row.size() != fields.size())
        throw std::runtime_error("invalid freeze cache metadata");
    for (auto it = row.begin(); it != row.end(); ++it)
        if (!fields.contains(it.key()))
            throw std::runtime_error("unknown freeze metadata field");
    if (detail::signedInteger(row.at("track_id")) != id || value.at("armed") != 0 ||
        value.at("frozen") != 1 || value.at("media") != row.at("post_media_id"))
        throw std::runtime_error("freeze cache flags disagree");
    for (const auto *field : {"pre_media_id", "post_media_id", "frames"})
        if (detail::signedInteger(row.at(field)) <= 0)
            throw std::runtime_error("invalid freeze stream");
    const auto rate = detail::signedInteger(row.at("sample_rate")),
               channels = detail::signedInteger(row.at("channels"));
    if (rate < 8000 || rate > 768000 || channels < 1 || channels > 64 ||
        detail::signedInteger(row.at("frames")) > (std::int64_t{1} << 50) ||
        !row.at("fingerprint").is_string() || row.at("fingerprint").get<std::string>().size() != 64 ||
        !row.at("identities").is_binary() || row.at("identities").get_binary().size() > 1024 * 1024)
        throw std::runtime_error("invalid freeze format or provenance");
    const auto arm = detail::signedInteger(row.at("original_armed"));
    if (arm != 0 && arm != 1)
        throw std::runtime_error("invalid original arm switch");
}
} // namespace
bool apply(OpContext &ctx, const Payload &p, std::string &error) {
    try {
        const auto id = detail::signedInteger(p.at("id"));
        shape(p.at("before"), id);
        shape(p.at("after"), id);
        if (state(ctx.db, id) != p.at("before"))
            throw std::runtime_error("captured freeze state is stale");
        SQLite::Statement remove(ctx.db, "DELETE FROM frozen_renders WHERE track_id=?");
        remove.bind(1, id);
        remove.exec();
        const auto &after = p.at("after");
        if (!after.at("render").is_null())
            detail::restoreRow(ctx.db, "frozen_renders", after.at("render"));
        SQLite::Statement flags(ctx.db,
                                "UPDATE tracks SET record_armed=?,frozen=?,freeze_media_id=? WHERE id=?");
        flags.bind(1, detail::signedInteger(after.at("armed")));
        flags.bind(2, detail::signedInteger(after.at("frozen")));
        if (after.at("media").is_null())
            flags.bind(3);
        else
            flags.bind(3, detail::signedInteger(after.at("media")));
        flags.bind(4, id);
        flags.exec();
        return true;
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
}
bool inverse(OpContext &, const Payload &p, Payload &output, std::string &) {
    output = {{"id", p.at("id")}, {"before", p.at("after")}, {"after", p.at("before")}};
    return true;
}
} // namespace adi::freeze
