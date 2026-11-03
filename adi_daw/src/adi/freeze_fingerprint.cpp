// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/freeze_fingerprint.hpp"
#include "adi/engine/plan.hpp"
#include "adi/media/blake3.hpp"
#include "adi/row_capture.hpp"
#include "adi/store.hpp"
#include "adi/store_rows.hpp"
#include "adi/track_image.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <stdexcept>
namespace adi::freeze {
namespace {
std::string sqlName(const std::string &name) {
    std::string value = "\"";
    for (char c : name) {
        value += c;
        if (c == '"')
            value += c;
    }
    return value + '"';
}
bool tableExists(SQLite::Database &db, const std::string &table) {
    SQLite::Statement q(db, "SELECT EXISTS(SELECT 1 FROM sqlite_master WHERE name=? UNION ALL SELECT 1 FROM "
                            "sqlite_temp_master WHERE name=?)");
    q.bind(1, table);
    q.bind(2, table);
    return q.executeStep() && q.getColumn(0).getInt() != 0;
}
Payload keyOf(SQLite::Database &db, const std::string &table, const Payload &row) {
    SQLite::Statement keys(db, "SELECT name FROM pragma_table_xinfo(?) WHERE pk>0 ORDER BY pk");
    keys.bind(1, table);
    Payload key = Payload::object();
    while (keys.executeStep())
        key[keys.getColumn(0).getString()] = row.at(keys.getColumn(0).getString());
    if (key.empty())
        throw std::runtime_error("freeze fingerprint requires keyed source rows: " + table);
    return key;
}
struct Row {
    std::string table;
    Payload key, value;
};
using Rows = std::map<std::pair<std::string, std::string>, Row>;
void add(Rows &rows, SQLite::Database &db, const std::string &table, const Payload &value) {
    auto key = keyOf(db, table, value);
    const auto identity = std::make_pair(table, key.dump());
    if (rows.contains(identity))
        return;
    if (rows.size() >= 100000)
        throw std::runtime_error("freeze fingerprint source row limit exceeded");
    rows.emplace(identity, Row{table, std::move(key), value});
}

void all(Rows &rows, SQLite::Database &db, const std::string &table) {
    if (!tableExists(db, table))
        return;
    SQLite::Statement q(db, "SELECT * FROM " + sqlName(table));
    while (q.executeStep())
        add(rows, db, table, detail::captureQueryRow(q));
}
std::int64_t id(const Payload &value, const char *column) {
    return value.contains(column) && !value.at(column).is_null() ? detail::signedInteger(value.at(column))
                                                                 : 0;
}
void erase(Payload &row, std::initializer_list<const char *> names) {
    for (const auto *name : names)
        row.erase(name);
}
void bind(SQLite::Statement &q, int index, const Payload &value) {
    if (value.is_null())
        q.bind(index);
    else if (value.is_string())
        q.bind(index, value.get<std::string>());
    else if (value.is_number_integer())
        q.bind(index, detail::signedInteger(value));
    else
        throw std::runtime_error("unsupported source asset key");
}
void parents(Rows &rows, SQLite::Database &db, const std::set<std::int64_t> &roots) {
    const std::set<std::string> scoped{
        "tracks",           "clips",           "devices",       "device_chains",       "lanes",
        "automation_lanes", "routing",         "ops",           "op_branches",         "history_snapshots",
        "agent_requests",   "op_clients",      "op_clocks",     "session_state",       "ui_view",
        "window_state",     "controller_maps", "device_panels", "device_panel_params", "frozen_renders",
        "state_blobs"};
    std::vector<Row> pending;
    for (const auto &[identity, row] : rows) {
        (void)identity;
        pending.push_back(row);
    }
    for (std::size_t at = 0; at < pending.size(); ++at) {
        const auto row = pending[at];
        SQLite::Statement foreign(
            db, "SELECT id,\"table\",\"from\",\"to\" FROM pragma_foreign_key_list(?) ORDER BY id,seq");
        foreign.bind(1, row.table);
        struct Foreign {
            std::string table;
            std::vector<std::string> from, to;
        };
        std::map<int, Foreign> keys;
        while (foreign.executeStep()) {
            auto &key = keys[foreign.getColumn(0).getInt()];
            key.table = foreign.getColumn(1).getString();
            std::transform(key.table.begin(), key.table.end(), key.table.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            key.from.push_back(foreign.getColumn(2).getString());
            key.to.push_back(foreign.getColumn(3).isNull() ? "" : foreign.getColumn(3).getString());
        }
        for (auto &[number, key] : keys) {
            (void)number;
            if (scoped.contains(key.table))
                continue;
            if (row.table == "tracks" && roots.contains(id(row.value, "id")) &&
                std::find(key.from.begin(), key.from.end(), "freeze_media_id") != key.from.end())
                continue;
            if (std::all_of(key.to.begin(), key.to.end(),
                            [](const auto &column) { return column.empty(); })) {
                key.to.clear();
                SQLite::Statement pk(db, "SELECT name FROM pragma_table_xinfo(?) WHERE pk>0 ORDER BY pk");
                pk.bind(1, key.table);
                while (pk.executeStep())
                    key.to.push_back(pk.getColumn(0).getString());
            }
            if (key.to.size() != key.from.size())
                throw std::runtime_error("freeze source foreign-key arity differs");
            std::string where;
            bool nullable = false;
            for (std::size_t i = 0; i < key.from.size(); ++i) {
                if (row.value.at(key.from[i]).is_null()) {
                    nullable = true;
                    break;
                }
                if (i)
                    where += " AND ";
                where += sqlName(key.to[i]) + " IS ?";
            }
            if (nullable)
                continue;
            std::string columns = "*";
            if (key.table == "media_files") {
                columns.clear();
                SQLite::Statement metadata(
                    db, "SELECT name FROM pragma_table_xinfo(?) WHERE hidden=0 ORDER BY cid");
                metadata.bind(1, key.table);
                while (metadata.executeStep()) {
                    const auto name = metadata.getColumn(0).getString();
                    if (name == "peaks")
                        continue;
                    if (!columns.empty())
                        columns += ',';
                    columns += sqlName(name);
                }
            }
            SQLite::Statement q(db, "SELECT " + columns + " FROM " + sqlName(key.table) + " WHERE " + where);
            for (std::size_t i = 0; i < key.from.size(); ++i)
                bind(q, static_cast<int>(i) + 1, row.value.at(key.from[i]));
            while (q.executeStep()) {
                auto value = detail::captureQueryRow(q);
                auto primary = keyOf(db, key.table, value);
                const auto identity = std::make_pair(key.table, primary.dump());
                if (rows.contains(identity))
                    continue;
                add(rows, db, key.table, value);
                pending.push_back(rows.at(identity));
            }
        }
    }
}
bool liveStrip(const std::string &parameter) {
    static const std::set<std::string> names{"volume",      "pan",          "width",       "mute", "delay",
                                             "track_delay", "phase_invert", "phaseInvert", "solo"};
    return names.contains(parameter);
}
} // namespace
Fingerprint fingerprint(Store &store, const FingerprintSpec &spec) {
    if (spec.root <= 0 || spec.prefixDevices.size() > 4096)
        throw std::runtime_error("invalid freeze source fingerprint scope");
    const auto model = rows::readModel(store);
    if (!model.problems.empty())
        throw std::runtime_error("cannot fingerprint an unresolved project");
    const auto found = std::find_if(model.tracks.begin(), model.tracks.end(),
                                    [&](const auto &track) { return track.id == spec.root; });
    if (found == model.tracks.end())
        throw std::runtime_error("freeze source track is missing");
    if (spec.subtree && found->kind != "group")
        throw std::runtime_error("freeze subtree root must be a group");
    std::set<std::int64_t> roots{spec.root};
    if (spec.subtree)
        for (bool changed = true; changed;) {
            changed = false;
            for (const auto &track : model.tracks)
                if (track.parentId && roots.contains(*track.parentId) && !roots.contains(track.id)) {
                    if (roots.size() >= 4096)
                        throw std::runtime_error("freeze subtree limit exceeded");
                    roots.insert(track.id);
                    changed = true;
                }
        }
    const auto plan = engine::planGraph(model);
    if (plan.cycle || !plan.problems.empty())
        throw std::runtime_error("cannot fingerprint unresolved routing");
    std::set<std::int64_t> selected = roots;
    if (!spec.prefixOnly)
        for (bool changed = true; changed;) {
            changed = false;
            for (const auto &edge : plan.edges)
                if (!edge.feedback && selected.contains(plan.nodes.at(edge.to).trackId) &&
                    !selected.contains(plan.nodes.at(edge.from).trackId)) {
                    selected.insert(plan.nodes.at(edge.from).trackId);
                    changed = true;
                }
            for (const auto &strip : model.strips)
                if (selected.contains(strip.trackId) && strip.vcaGroupId && strip.trackId != spec.root &&
                    !selected.contains(*strip.vcaGroupId)) {
                    selected.insert(*strip.vcaGroupId);
                    changed = true;
                }
            if (selected.size() > 4096)
                throw std::runtime_error("freeze dependency limit exceeded");
        }
    else
        for (const auto &edge : plan.edges)
            if (!edge.feedback && plan.nodes.at(edge.to).trackId == spec.root &&
                plan.nodes.at(edge.from).trackId != spec.root && edge.bus == engine::Bus::Main)
                throw std::runtime_error("source-prefix freeze requires a live mixed-input mode");
    Rows raw;
    std::vector<std::int64_t> capture{spec.root};
    for (const auto track : selected)
        if (track != spec.root)
            capture.push_back(track);
    for (const auto track : capture) {
        if (raw.contains({"tracks", Payload{{"id", track}}.dump()}))
            continue;
        const auto image = detail::captureTrackSource(store.db(), track);
        for (const auto &entry : image.at("rows"))
            add(raw, store.db(), entry.at("table").get<std::string>(), entry.at("before"));
    }
    std::set<std::int64_t> clips, devices, chains, lanes, routes, automation;
    for (const auto &[identity, row] : raw) {
        (void)identity;
        if (row.table == "clips" && selected.contains(id(row.value, "track_id")))
            clips.insert(id(row.value, "id"));
        if (row.table == "device_chains" && selected.contains(id(row.value, "track_id")))
            chains.insert(id(row.value, "id"));
        if (row.table == "lanes" && selected.contains(id(row.value, "track_id")))
            lanes.insert(id(row.value, "id"));
    }
    for (bool changed = true; changed;) {
        changed = false;
        for (const auto &[identity, row] : raw) {
            (void)identity;
            if (row.table == "devices" && chains.contains(id(row.value, "chain_id")) &&
                devices.insert(id(row.value, "id")).second)
                changed = true;
            if (row.table == "device_chains" && devices.contains(id(row.value, "parent_device_id")) &&
                chains.insert(id(row.value, "id")).second)
                changed = true;
        }
    }
    auto endpoint = [&](const Payload &value, const char *kind, const char *target) {
        const auto type = value.at(kind).get<std::string>();
        return type == "track" ? selected.contains(id(value, target))
                               : type == "device" && devices.contains(id(value, target));
    };
    std::set<std::int64_t> prefix(spec.prefixDevices.begin(), spec.prefixDevices.end());
    for (const auto device : prefix)
        if (device <= 0 || !devices.contains(device))
            throw std::runtime_error("cached prefix device is missing");
    for (const auto &[identity, row] : raw) {
        (void)identity;
        if (row.table != "routing")
            continue;
        const auto &value = row.value;
        if (!endpoint(value, "dst_kind", "dst_id"))
            continue;
        if (spec.prefixOnly && value.at("kind") == "sidechain") {
            if ((value.at("dst_kind") == "device" && prefix.contains(id(value, "dst_id"))) ||
                (value.at("dst_kind") == "track" && !prefix.empty()))
                throw std::runtime_error("a live sidechain drives the cached instrument prefix");
            continue;
        }
        routes.insert(id(value, "id"));
    }
    for (const auto &[identity, row] : raw) {
        (void)identity;
        if (row.table != "automation_lanes")
            continue;
        const auto &value = row.value;
        const auto owner = value.at("owner_kind").get<std::string>();
        const auto target = id(value, "owner_id");
        bool include = owner == "track"    ? selected.contains(target)
                       : owner == "device" ? devices.contains(target)
                       : owner == "clip"   ? clips.contains(target)
                                           : owner == "routing" && routes.contains(target);
        if (owner == "track" && target == spec.root && liveStrip(value.at("param_ref").get<std::string>()))
            include = false;
        if (include)
            automation.insert(id(value, "id"));
    }
    Rows source;
    for (const auto &[identity, row] : raw) {
        (void)identity;
        const auto &value = row.value;
        if (row.table == "tracks" && !selected.contains(id(value, "id")))
            continue;
        if (row.table == "clips" && !clips.contains(id(value, "id")))
            continue;
        if (row.table == "devices" && !devices.contains(id(value, "id")))
            continue;
        if (row.table == "device_chains" && !chains.contains(id(value, "id")))
            continue;
        if (row.table == "routing" && !routes.contains(id(value, "id")))
            continue;
        if (row.table == "automation_lanes" && !automation.contains(id(value, "id")))
            continue;
        if (value.contains("track_id") && !selected.contains(id(value, "track_id")) &&
            !value.at("track_id").is_null())
            continue;
        if (value.contains("clip_id") && !value.at("clip_id").is_null() &&
            !clips.contains(id(value, "clip_id")))
            continue;
        if (value.contains("device_id") && !devices.contains(id(value, "device_id")))
            continue;
        if (value.contains("lane_id") && !value.at("lane_id").is_null() &&
            !lanes.contains(id(value, "lane_id")) && !automation.contains(id(value, "lane_id")))
            continue;
        add(source, store.db(), row.table, value);
    }
    for (const auto *table :
         {"project", "tempo_map", "time_signature_map", "key_map", "key_map_tunings", "key_map_degrees"})
        all(source, store.db(), table);
    parents(source, store.db(), roots);
    Fingerprint result;
    result.tracks.assign(roots.begin(), roots.end());
    for (const auto track : selected)
        if (!roots.contains(track))
            result.dependencies.push_back(track);
    result.source = {{"version", 1},
                     {"root", spec.root},
                     {"subtree", spec.subtree},
                     {"prefix_only", spec.prefixOnly},
                     {"prefix_devices", spec.prefixDevices},
                     {"tracks", result.tracks},
                     {"dependencies", result.dependencies},
                     {"rows", Payload::array()}};
    for (const auto &[identity, entry] : source) {
        (void)identity;
        auto value = entry.value;
        erase(value, {"name", "color", "param_name", "visible", "height"});
        if (entry.table == "tracks" && roots.contains(id(value, "id"))) {
            erase(value, {"frozen", "freeze_media_id", "record_armed", "locked"});
            if (id(value, "id") == spec.root)
                erase(value, {"parent_id", "index_in_parent", "muted", "soloed", "solo_defeat"});
        }
        if (entry.table == "mixer_strip" && id(value, "track_id") == spec.root)
            for (const auto *field :
                 {"volume_db", "pan", "pan_law", "width", "phase_invert", "delay_samples", "vca_group_id"})
                value.erase(field);
        if (entry.table == "media_files")
            erase(value, {"orig_name", "rel_path", "abs_path_hint", "size_bytes", "missing", "imported_utc",
                          "peaks", "embedded"});
        if (entry.table == "project")
            erase(value, {"author", "notes", "length_ticks"});
        result.source["rows"].push_back(
            {{"table", entry.table}, {"key", entry.key}, {"value", std::move(value)}});
    }
    const auto bytes = encodePayload(result.source);
    if (bytes.size() > 64 * 1024 * 1024)
        throw std::runtime_error("freeze source fingerprint exceeds metadata limit");
    const auto digest = media::blake3Bytes(bytes);
    if (!digest)
        throw std::runtime_error("cannot hash freeze source state");
    result.hash = digest.hex;
    return result;
}
} // namespace adi::freeze
