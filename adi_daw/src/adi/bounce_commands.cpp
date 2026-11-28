// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/bounce_commands.hpp"
#include "adi/edit_time.hpp"
#include "adi/engine/timeline_clip_sql.hpp"
#include "adi/freeze_commands.hpp"
#include "adi/row_capture.hpp"
#include "adi/row_ids.hpp"
#include "adi/store.hpp"
#include "adi/track_recording.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <cmath>
#include <limits>
#include <stdexcept>
namespace adi::clip_edit {
struct TrackBounce::Data {
    MaterialEdit material;
    std::uint64_t generation = 0;
    bool committed = false;
};
namespace {
void add(std::vector<OpRequest> &rows, const std::string &type, Payload payload) {
    OpRequest op;
    op.opType = type;
    op.payload = std::move(payload);
    op.label = "Bounce to New Track";
    rows.push_back(std::move(op));
}
bool exists(SQLite::Database &db, const char *table, std::int64_t id) {
    SQLite::Statement q(db, std::string("SELECT 1 FROM ") + table + " WHERE id=?");
    q.bind(1, id);
    return q.executeStep();
}
// track.create's captured image supports mixer fields without separate ops,
// including pan law and VCA membership. Read track defaults from the schema.
Payload bouncedTrackContents(SQLite::Database &db, const Payload &track, Payload mixer,
                             std::optional<Payload> splitPan) {
    const auto id = track.at("id").get<std::int64_t>();
    SQLite::Statement table(db, "SELECT sql FROM sqlite_master WHERE type='table' AND name='tracks'");
    if (!table.executeStep())
        throw std::runtime_error("bounce cannot read the track schema");
    SQLite::Database defaults(":memory:", SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
    defaults.exec("PRAGMA foreign_keys=OFF");
    defaults.exec(table.getColumn(0).getString());
    detail::restoreRow(defaults, "tracks", {{"id", id}, {"kind", "audio"}, {"name", track.at("name")},
        {"parent_id", track.at("parent")}, {"index_in_parent", track.at("index")},
        {"monitor_mode", kNewAudioTrackMonitorMode}});
    const auto root = detail::captureRow(defaults, "tracks", "id", id);
    if (!root)
        throw std::runtime_error("bounce cannot capture the new track");
    auto rows = Payload::array();
    // The undo image's canonical order is table name, then key.
    if (splitPan) {
        (*splitPan)["track_id"] = id;
        rows.push_back({{"table", "mixer_pan_settings"}, {"key", {{"track_id", id}}},
                        {"before", std::move(*splitPan)}, {"after", nullptr}});
    }
    mixer["track_id"] = id;
    mixer["input_gain_db"] = 0.; // The wet render contains the source's input gain.
    rows.push_back({{"table", "mixer_strip"}, {"key", {{"track_id", id}}},
                    {"before", std::move(mixer)}, {"after", nullptr}});
    rows.push_back({{"table", "tracks"}, {"key", {{"id", id}}}, {"before", *root}, {"after", nullptr}});
    return {{"version", 1}, {"root", id}, {"rows", std::move(rows)}};
}
void copySends(SQLite::Database &db, std::vector<OpRequest> &prefix, const TrackBounceOptions &options) {
    RowIds ids(db);
    if (options.routingId > 0)
        ids.reserve("routing", options.routingId);
    SQLite::Statement sends(db, "SELECT * FROM routing WHERE src_kind='track' AND src_id=? AND kind='send' ORDER BY ord,id");
    sends.bind(1, options.sourceTrack);
    while (sends.executeStep()) {
        auto row = detail::captureQueryRow(sends);
        const auto original = row.at("id").get<std::int64_t>();
        const auto id = ids.next("routing");
        auto tap = detail::captureRow(db, "routing_send_taps", "routing_id", original);
        row["id"] = id;
        row["src_id"] = options.newTrack;
        Payload route{{"id", id}, {"srcKind", "track"}, {"src", options.newTrack},
                      {"dstKind", row.at("dst_kind")}, {"dst", row.at("dst_id")}, {"kind", "send"},
                      {"row", std::move(row)}};
        if (tap) {
            (*tap)["routing_id"] = id;
            route["sendTapRow"] = std::move(*tap);
        }
        add(prefix, "routing.connect", std::move(route));
    }
}
} // namespace
bool TrackBounce::ready() const noexcept {
    return data_ && !data_->committed && data_->material.edit.ok && error_.empty();
}
TrackBounce prepareBounceToNewTrack(Store &store, const TrackBounceOptions &options,
                                    engine::DeviceLoader loader, render::Control control) {
    TrackBounce result;
    try {
        if (store.readOnly())
            throw std::runtime_error("bounce project is read-only");
        if (options.sourceTrack <= 0 || options.newTrack <= 0 || options.clipId <= 0 ||
            options.mediaId <= 0 || options.sourceTrack == options.newTrack)
            throw std::invalid_argument(
                "Bounce to New Track requires distinct positive allocated identities");
        const auto rate = options.job.session.sampleRate;
        if (!std::isfinite(rate) || rate < 8000 || rate > 768000 || std::floor(rate) != rate ||
            options.job.beginSample < 0 || options.job.endSample <= options.job.beginSample ||
            options.job.endSample > (std::int64_t{1} << 50))
            throw std::invalid_argument("invalid Bounce to New Track render range/rate");
        std::optional<std::int64_t> head;
        std::vector<OpRequest> prefix, silence;
        std::string name;
        {
            SQLite::Transaction capture(store.db(), SQLite::TransactionBehavior::DEFERRED);
            head = OpJournal(store).headSeq();
            if (exists(store.db(), "tracks", options.newTrack) || exists(store.db(), "clips", options.clipId))
                throw std::runtime_error("bounce target identity already exists");
            SQLite::Statement source(
                store.db(),
                "SELECT kind,name,parent_id,index_in_parent,color,locked,frozen FROM tracks WHERE id=?");
            source.bind(1, options.sourceTrack);
            if (!source.executeStep() ||
                (source.getColumn(0).getString() != "audio" && source.getColumn(0).getString() != "midi" &&
                 source.getColumn(0).getString() != "instrument" &&
                 source.getColumn(0).getString() != "group") ||
                source.getColumn(5).getInt() != 0 ||
                (!options.copyMixerSettings && source.getColumn(6).getInt() != 0))
                throw std::runtime_error(
                    "bounce source is missing, locked, frozen or not a regular track/group");
            name = options.name.empty() ? source.getColumn(1).getString() + " (Bounce)" : options.name;
            const Payload parent =
                source.getColumn(2).isNull() ? Payload(nullptr) : Payload(source.getColumn(2).getInt64());
            const auto oldIndex = source.getColumn(3).getInt64();
            if (oldIndex == std::numeric_limits<std::int64_t>::max())
                throw std::runtime_error("bounce sibling index overflows");
            const auto index = oldIndex + 1;
            SQLite::Statement siblings(store.db(), "SELECT id,index_in_parent FROM tracks WHERE parent_id IS "
                                                   "? AND index_in_parent>=? ORDER BY index_in_parent,id");
            if (parent.is_null())
                siblings.bind(1);
            else
                siblings.bind(1, parent.get<std::int64_t>());
            siblings.bind(2, index);
            while (siblings.executeStep()) {
                const auto previous = siblings.getColumn(1).getInt64();
                if (previous == std::numeric_limits<std::int64_t>::max())
                    throw std::runtime_error("bounce sibling index overflows");
                add(prefix, "track.reorder",
                    {{"id", siblings.getColumn(0).getInt64()}, {"index", previous + 1}});
            }
            Payload track{{"id", options.newTrack}, {"kind", "audio"}, {"name", name},
                          {"parent", parent}, {"index", index}};
            if (options.copyMixerSettings) {
                auto mixer = detail::captureRow(store.db(), "mixer_strip", "track_id", options.sourceTrack);
                if (mixer) {
                    auto splitPan = detail::captureRow(store.db(), "mixer_pan_settings", "track_id", options.sourceTrack);
                    track["contents"] = bouncedTrackContents(store.db(), track, std::move(*mixer), std::move(splitPan));
                }
            }
            add(prefix, "track.create", std::move(track));
            if (!source.getColumn(4).isNull()) {
                const auto color = source.getColumn(4).getInt64();
                if (color < 0 || color > 69)
                    throw std::runtime_error("bounce source color is not a Live palette index");
                add(prefix, "track.setColor", {{"id", options.newTrack}, {"color", color}});
            }
            SQLite::Statement route(store.db(), "SELECT dst_kind,dst_id FROM routing WHERE src_kind='track' "
                                                "AND src_id=? AND kind='main' AND origin='user' ORDER BY id");
            route.bind(1, options.sourceTrack);
            if (route.executeStep()) {
                if (options.routingId <= 0 || exists(store.db(), "routing", options.routingId))
                    throw std::runtime_error(
                        "bounce needs a free routing id for the source's explicit Main destination");
                add(prefix, "routing.connect",
                    {{"id", options.routingId},
                     {"srcKind", "track"},
                     {"src", options.newTrack},
                     {"dstKind", route.getColumn(0).getString()},
                     {"dst", route.getColumn(1).getInt64()},
                     {"kind", "main"}});
                if (route.executeStep())
                    throw std::runtime_error("bounce source has multiple explicit Main destinations");
            }
            if (options.copyMixerSettings)
                copySends(store.db(), prefix, options);
            if (options.muteOriginal) {
                // Frozen and group output is not the sum of clips on the root track.
                if (!options.copyMixerSettings || source.getColumn(0).getString() == "group" || source.getColumn(6).getInt() != 0) {
                    add(silence, "track.setMute", {{"id", options.sourceTrack}, {"muted", true}});
                } else {
                    const auto begin = edit::nanosFromSeconds(static_cast<double>(options.job.beginSample) / rate);
                    const auto end = edit::nanosFromSeconds(static_cast<double>(options.job.endSample) / rate);
                    edit::TimeMap time(store.db());
                    SQLite::Statement clips(store.db(), "SELECT id,time_base,pos_ticks,length_ticks,pos_ns,length_ns FROM clips "
                                                       "WHERE track_id=? AND lane_id IS NULL AND muted=0" +
                                                       engine::excludeSlotClips(store.db(), "clips.id") +
                                                       " ORDER BY id");
                    clips.bind(1, options.sourceTrack);
                    while (clips.executeStep()) {
                        const bool absolute = clips.getColumn(1).getInt() != 0;
                        const auto at = clips.getColumn(absolute ? 4 : 2).getInt64();
                        const auto last = edit::addTime(at, clips.getColumn(absolute ? 5 : 3).getInt64());
                        const auto clipBegin = absolute ? at : time.nanos(at);
                        const auto clipEnd = absolute ? last : time.nanos(last);
                        if (clipBegin >= end || clipEnd <= begin)
                            continue;
                        if (clipBegin < begin || clipEnd > end)
                            throw std::runtime_error("To mute the source after bouncing, include its whole overlapping clips.");
                        add(silence, "clip.setMute", {{"id", clips.getColumn(0).getInt64()}, {"muted", true}});
                    }
                }
            }
            capture.commit();
        }
        BounceOptions bounce;
        bounce.job = options.job;
        bounce.job.source = {options.sourceTrack,
                             options.copyMixerSettings ? engine::OfflineTap::PreFader : engine::OfflineTap::PostFader};
        bounce.job.regionId.reset();
        bounce.destination.track = options.newTrack;
        bounce.destination.clipId = options.clipId;
        bounce.destination.mediaId = options.mediaId;
        const auto placement = edit::nanosFromSeconds(static_cast<double>(options.job.beginSample) / rate);
        if (std::llround(static_cast<double>(placement) * rate / 1e9) != options.job.beginSample)
            throw std::runtime_error("bounce placement cannot preserve its source sample");
        bounce.destination.at = {1, placement};
        auto data = std::make_shared<TrackBounce::Data>();
        data->generation = options.documentGeneration;
        data->material = prepareBounce(store, bounce, std::move(loader), std::move(control));
        if (!data->material.edit.ok)
            throw std::runtime_error(data->material.edit.error);
        if (!data->material.edit.options.expectHead || *data->material.edit.options.expectHead != head)
            throw std::runtime_error("project changed before track-bounce capture");
        auto &requests = data->material.edit.requests;
        requests.insert(requests.begin(), prefix.begin(), prefix.end());
        add(requests, "clip.setName", {{"id", options.clipId}, {"name", name}});
        requests.insert(requests.end(), silence.begin(), silence.end());
        for (auto &op : requests)
            op.label = "Bounce to New Track";
        result.data_ = std::move(data);
    } catch (const std::exception &error) {
        result.error_ = error.what();
    }
    return result;
}
TrackBounce prepareBounceInPlace(Store &store, const InPlaceBounceOptions &options,
                                 engine::DeviceLoader loader, render::Control control) {
    TrackBounce result;
    try {
        const auto rate = options.job.session.sampleRate;
        if (store.readOnly() || options.sourceTrack <= 0 || options.clipId <= 0 || options.mediaId <= 0 ||
            options.job.beginSample != 0 || options.job.endSample <= 0 ||
            options.job.endSample > (std::int64_t{1} << 50) || !std::isfinite(rate) || rate < 8000 ||
            rate > 768000 || std::floor(rate) != rate)
            throw std::invalid_argument(
                "Bounce in Place needs a whole-track range from zero and positive allocated ids");
        const auto endNs = edit::nanosFromSeconds(static_cast<double>(options.job.endSample) / rate);
        if (std::llround(static_cast<double>(endNs) * rate / 1e9) != options.job.endSample)
            throw std::runtime_error("Bounce in Place range cannot preserve its final sample");
        std::optional<std::int64_t> head;
        std::vector<OpRequest> conversion, devicesAfterBounce;
        std::string name;
        {
            SQLite::Transaction capture(store.db(), SQLite::TransactionBehavior::DEFERRED);
            head = OpJournal(store).headSeq();
            if (exists(store.db(), "clips", options.clipId))
                throw std::runtime_error("Bounce in Place clip identity already exists");
            SQLite::Statement track(store.db(),
                                    "SELECT kind,name,locked,frozen,record_armed FROM tracks WHERE id=?");
            track.bind(1, options.sourceTrack);
            if (!track.executeStep() ||
                (track.getColumn(0).getString() != "audio" && track.getColumn(0).getString() != "midi" &&
                 track.getColumn(0).getString() != "instrument") ||
                track.getColumn(2).getInt() != 0 || track.getColumn(3).getInt() != 0)
                throw std::runtime_error(
                    "Bounce in Place source is missing, locked, frozen or not a regular track");
            name = options.name.empty() ? track.getColumn(1).getString() + " (Bounce)" : options.name;
            if (options.replaceDevices && track.getColumn(0).getString() != "audio") {
                auto edits = freeze::audioTrackConversion(store, options.sourceTrack);
                for (auto &request : edits)
                    conversion.push_back(std::move(request));
            }
            SQLite::Statement incoming(store.db(), "SELECT 1 FROM routing WHERE dst_kind='track' AND "
                                                   "dst_id=? AND kind IN ('main','send') LIMIT 1");
            incoming.bind(1, options.sourceTrack);
            if (incoming.executeStep())
                throw std::runtime_error("Bounce in Place cannot retain live incoming audio while replacing "
                                         "its baked mix; use Bounce to New Track");
            edit::TimeMap time(store.db());
            SQLite::Statement clips(
                store.db(),
                "SELECT time_base,pos_ticks,length_ticks,pos_ns,length_ns FROM clips "
                "WHERE track_id=? AND lane_id IS NULL" + engine::excludeSlotClips(store.db(), "clips.id"));
            clips.bind(1, options.sourceTrack);
            while (clips.executeStep()) {
                const auto base = clips.getColumn(0).getInt();
                const auto position = clips.getColumn(base ? 3 : 1).getInt64(),
                           length = clips.getColumn(base ? 4 : 2).getInt64();
                if (position < 0 || length <= 0)
                    throw std::runtime_error("Bounce in Place requires finite clip extents");
                const auto last = edit::addTime(position, length);
                const auto lastNs = base ? last : time.nanos(last);
                if (lastNs > endNs)
                    throw std::runtime_error(
                        "Bounce in Place range must include every clip and the requested tail; use the "
                        "range-bounce producer for material edits");
            }
            SQLite::Statement devices(store.db(),
                                      "SELECT d.id,d.enabled FROM devices d JOIN device_chains c ON c.id=d.chain_id "
                                      "WHERE c.track_id=? ORDER BY c.ord,c.id,d.ord,d.id");
            devices.bind(1, options.sourceTrack);
            while (devices.executeStep()) {
                const auto id = devices.getColumn(0).getInt64();
                if (options.replaceDevices)
                    add(devicesAfterBounce, "device.remove", {{"id", id}});
                else if (devices.getColumn(1).getInt() != 0)
                    add(devicesAfterBounce, "device.setEnabled", {{"id", id}, {"enabled", false}});
            }
            SQLite::Statement gain(store.db(), "SELECT input_gain_db FROM mixer_strip WHERE track_id=?");
            gain.bind(1, options.sourceTrack);
            if (gain.executeStep() && gain.getColumn(0).getDouble() != 0)
                add(devicesAfterBounce, "mixer.setInputGain", {{"id", options.sourceTrack}, {"db", 0.0}});
            if (track.getColumn(4).getInt() != 0)
                add(devicesAfterBounce, "track.setArm", {{"id", options.sourceTrack}, {"armed", false}});
            capture.commit();
        }
        BounceOptions bounce;
        bounce.job = options.job;
        bounce.job.source = {options.sourceTrack, engine::OfflineTap::PreFader};
        bounce.job.regionId.reset();
        bounce.destination.track = options.sourceTrack;
        bounce.destination.clipId = options.clipId;
        bounce.destination.mediaId = options.mediaId;
        bounce.destination.at = {1, 0};
        bounce.replace = RangeSelection{{options.sourceTrack}, {1, 0}, {1, endNs}};
        auto data = std::make_shared<TrackBounce::Data>();
        data->generation = options.documentGeneration;
        data->material = prepareBounce(store, bounce, std::move(loader), std::move(control));
        if (!data->material.edit.ok)
            throw std::runtime_error(data->material.edit.error);
        if (!data->material.edit.options.expectHead || *data->material.edit.options.expectHead != head)
            throw std::runtime_error("project changed before in-place capture");
        auto &requests = data->material.edit.requests;
        requests.insert(requests.begin(), conversion.begin(), conversion.end());
        requests.insert(requests.end(), devicesAfterBounce.begin(), devicesAfterBounce.end());
        add(requests, "clip.setName", {{"id", options.clipId}, {"name", name}});
        for (auto &op : requests)
            op.label = "Bounce in Place";
        result.data_ = std::move(data);
    } catch (const std::exception &error) {
        result.error_ = error.what();
    }
    return result;
}
CommitResult commitTrackBounce(Store &store, TrackBounce &bounce, std::uint64_t generation, Actor actor) {
    CommitResult result;
    if (!bounce.ready()) {
        result.error = bounce.error_.empty() ? "track bounce is absent or already committed" : bounce.error_;
        return result;
    }
    if (generation != bounce.data_->generation) {
        result.error = "track bounce belongs to a different document generation";
        return result;
    }
    result = commitMaterial(store, bounce.data_->material, actor);
    if (result.ok)
        bounce.data_->committed = true;
    return result;
}
} // namespace adi::clip_edit
