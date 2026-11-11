// SPDX-License-Identifier: GPL-3.0-or-later
//
// The op catalogue. OPS.md §9 is the specification; this is what is built.
//
// Separate from ops.cpp because the machinery is finished and this is not: the
// catalogue is heading for 174 entries and the registry, codec and journal are
// not going to grow with it.
//
// Two rules govern every handler here, and both are easy to break by accident:
//
//   ADR-0021  An op never reads ambient state. Every value it acts on is in its
//             payload, INCLUDING the ids of objects it creates. The round-trip
//             corpus catches a violation and nothing else does.
//   OPS.md §6.1  The inverse is built from the state ABOUT TO BE OVERWRITTEN,
//             before apply, in the same transaction.

#include "adi/split_pan_ops.hpp"
#include "adi/clip_image.hpp"
#include "adi/row_capture.hpp"
#include "adi/clip_edit_ops.hpp"
#include "adi/blob.hpp"
#include "adi/automation_edits.hpp"
#include "adi/automation_lanes.hpp"
#include "adi/controller_lanes.hpp"
#include "adi/note_flags_ops.hpp"
#include "adi/builtin_catalog.hpp"
#include "adi/midi_capture.hpp"
#include "adi/controller_maps.hpp"
#include "adi/freeze_groups.hpp"
#include "adi/freeze_state.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/note_transforms.hpp"
#include "adi/note_diff_ops.hpp"
#include "adi/ops.hpp"
#include "adi/store.hpp"
#include "adi/cue_settings.hpp"
#include "adi/transport_settings.hpp"
#include "adi/marker_ops.hpp"
#include "adi/track_recording.hpp"
#include "adi/return_send_tap.hpp"
#include "adi/clip_property_ops.hpp"
#include "adi/summing_flavors.hpp"
#include "adi/track_image.hpp"
#include "adi/tuning_ops.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <limits>
#include <vector>

namespace adi {
namespace {

// ---------------------------------------------------------------------------
// Scalar setters, from a table
// ---------------------------------------------------------------------------
//
// Most of P0 is "set one column on one row; the inverse is the old value". That
// is 24 ops whose handlers would be textually identical apart from two strings.
// Written out, one of them eventually gets a copy-paste error in the WHERE
// clause and silently edits the wrong row. Generated from a table, that failure
// mode does not exist -- and adding the 25th is one line.

struct ScalarSpec {
    const char* op;        // "track.setMute"
    const char* summary;
    const char* table;     // "tracks"
    const char* idCol;     // "id" -- mixer_strip keys on track_id
    const char* col;       // "muted"
    const char* key;       // payload key holding the new value
    FieldType type;
    EngineImpact impact;
    bool coalescable;
};

constexpr ScalarSpec kScalars[] = {
    // --- tracks ---------------------------------------------------------
    {"track.rename", "Rename a track", "tracks", "id", "name", "name",
     FieldType::Text, EngineImpact::None, false},
    {"track.setMute", "Mute or unmute a track", "tracks", "id", "muted", "muted",
     FieldType::Bool, EngineImpact::Snapshot, false},
    {"track.setSolo", "Solo or unsolo a track", "tracks", "id", "soloed", "soloed",
     FieldType::Bool, EngineImpact::Snapshot, false},
    {"track.setSoloDefeat", "Exempt a track from solo", "tracks", "id",
     "solo_defeat", "defeat", FieldType::Bool, EngineImpact::Snapshot, false},
    {"track.setArm", "Arm a track for recording", "tracks", "id", "record_armed",
     "armed", FieldType::Bool, EngineImpact::GraphRebuild, false},
    {"track.setMonitorMode", "Set input monitoring", "tracks", "id",
     "monitor_mode", "mode", FieldType::Int, EngineImpact::GraphRebuild, false},
    {"track.setInput", "Set a track's input source", "tracks", "id", "input_ref",
     "input", FieldType::Text, EngineImpact::GraphRebuild, false},
    {"track.setTimeBase", "Switch a track between musical and linear time",
     "tracks", "id", "time_base", "base", FieldType::Int, EngineImpact::Snapshot, false},
    {"track.setColor", "Recolour a track", "tracks", "id", "color", "color",
     FieldType::Int, EngineImpact::None, false},
    {"track.setLocked", "Lock or unlock a track", "tracks", "id", "locked",
     "locked", FieldType::Bool, EngineImpact::None, false},
    {"track.setAutomationMode", "Set the automation mode", "tracks", "id",
     "automation_mode", "mode", FieldType::Int, EngineImpact::None, false},
    {"track.reorder", "Move a track within its parent", "tracks", "id",
     "index_in_parent", "index", FieldType::Int, EngineImpact::Snapshot, false},

    // --- clips ----------------------------------------------------------
    // --- devices (OPS.md 9.7) -------------------------------------------
    // The three that ARE a single column. The other six are hand-written
    // below; a device that carries parameter values and opaque state is not
    // a function of one row.
    {"device.setEnabled", "Enable or bypass a device", "devices", "id",
     "enabled", "enabled", FieldType::Bool, EngineImpact::Snapshot, false},
    {"device.rename", "Rename a device", "devices", "id", "name", "name",
     FieldType::Text, EngineImpact::None, false},
    // GraphRebuild, not Snapshot: a latency change moves every compensated
    // node downstream of it, so the schedule is recomputed (ADR-0058,
    // ADR-0066). Reporting it as a snapshot would leave the old compensation
    // in force and shift audio that was aligned.
    {"device.setLatency", "Set a device's reported latency", "devices", "id",
     "latency_samples", "latency", FieldType::Int, EngineImpact::GraphRebuild, false},

    {"clip.setName", "Rename a clip", "clips", "id", "name", "name",
     FieldType::Text, EngineImpact::None, false},
    {"clip.setMute", "Mute or unmute a clip", "clips", "id", "muted", "muted",
     FieldType::Bool, EngineImpact::Snapshot, false},
    {"clip.setGain", "Set a clip's gain", "clips", "id", "gain_db", "gain",
     FieldType::Real, EngineImpact::Snapshot, true},
    {"clip.setColor", "Recolour a clip", "clips", "id", "color", "color",
     FieldType::Int, EngineImpact::None, false},

    // --- mixer (keyed on track_id, not id) --------------------------------
    {"mixer.setVolume", "Set a channel's fader", "mixer_strip", "track_id",
     "volume_db", "db", FieldType::Real, EngineImpact::Snapshot, true},
    {"mixer.setPan", "Set a channel's pan", "mixer_strip", "track_id", "pan",
     "pan", FieldType::Real, EngineImpact::Snapshot, true},
    {"mixer.setWidth", "Set a channel's stereo width", "mixer_strip", "track_id",
     "width", "width", FieldType::Real, EngineImpact::Snapshot, true},
    {"mixer.setInputGain", "Set a channel's input gain", "mixer_strip",
     "track_id", "input_gain_db", "db", FieldType::Real, EngineImpact::Snapshot, true},
    {"mixer.setPhaseInvert", "Invert a channel's phase", "mixer_strip",
     "track_id", "phase_invert", "inverted", FieldType::Bool,
     EngineImpact::Snapshot, false},
    // ADR-0172. Samples at the project's rate, positive late, negative early.
    // GraphRebuild, for device.setLatency's reason: the engine plays a delay
    // as latency, and a latency change moves the compensation downstream.
    {"mixer.setDelay", "Delay or advance a channel", "mixer_strip", "track_id",
     "delay_samples", "samples", FieldType::Int, EngineImpact::GraphRebuild, true},

    // --- routing ----------------------------------------------------------
    {"routing.setGain", "Set a connection's gain", "routing", "id", "gain_db",
     "db", FieldType::Real, EngineImpact::Snapshot, true},
    {"routing.setPan", "Set a connection's pan", "routing", "id", "pan", "pan",
     FieldType::Real, EngineImpact::Snapshot, true},
    {"routing.setEnabled", "Enable or disable a connection", "routing", "id",
     "enabled", "enabled", FieldType::Bool, EngineImpact::Snapshot, false},
    {"routing.setPreFader", "Move a send pre or post fader", "routing", "id",
     "pre_fader", "pre", FieldType::Bool, EngineImpact::GraphRebuild, false},
};

constexpr std::size_t kScalarCount = std::size(kScalars);

void bindValue(SQLite::Statement& st, int idx, const Payload& v, FieldType t) {
    if (v.is_null()) { st.bind(idx); return; }
    switch (t) {
        case FieldType::Bool: st.bind(idx, v.get<bool>() ? 1 : 0); break;
        case FieldType::Int:  st.bind(idx, v.get<std::int64_t>()); break;
        case FieldType::Real: st.bind(idx, v.get<double>()); break;
        default:              st.bind(idx, v.get<std::string>()); break;
    }
}

Payload readValue(const SQLite::Column& c, FieldType t) {
    if (c.isNull()) return nullptr;
    switch (t) {
        case FieldType::Bool: return c.getInt() != 0;
        case FieldType::Int:  return c.getInt64();
        case FieldType::Real: return c.getDouble();
        default:              return c.getString();
    }
}

template <std::size_t I>
bool scalarApply(OpContext& c, const Payload& p, std::string& err) {
    constexpr auto& S = kScalars[I];
    try {
        SQLite::Statement st(c.db, std::string("UPDATE ") + S.table + " SET " + S.col +
                                       " = ? WHERE " + S.idCol + " = ?");
        bindValue(st, 1, p.at(S.key), S.type);
        st.bind(2, p.at("id").get<std::int64_t>());
        if (st.exec() == 0) { err = std::string("no such row in ") + S.table; return false; }
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}

template <std::size_t I>
bool scalarInverse(OpContext& c, const Payload& p, Payload& inv, std::string& err) {
    constexpr auto& S = kScalars[I];
    try {
        if constexpr(std::string_view(S.op)=="routing.setPreFader")
            if(!allowSendEdgeTapEdit(c.db,p.at("id").get<std::int64_t>(),p.at("pre").get<bool>(),err))return false;

        SQLite::Statement st(c.db, std::string("SELECT ") + S.col + " FROM " + S.table +
                                       " WHERE " + S.idCol + " = ?");
        st.bind(1, p.at("id").get<std::int64_t>());
        if (!st.executeStep()) { err = std::string("no such row in ") + S.table; return false; }
        inv = Payload::object();
        inv["id"] = p.at("id");
        inv[S.key] = readValue(st.getColumn(0), S.type);
        return true;   // symmetric
    } catch (const std::exception& e) { err = e.what(); return false; }
}

// Field arrays for the generated ops. Two shapes cover all of them: the value
// is required, except where NULL is meaningful (a track with no parent is at
// the top level, and that is a real state rather than an omission).
template <std::size_t I>
constexpr std::array<Field, 2> scalarFields() {
    return {Field{"id", FieldType::Int, true},
            Field{kScalars[I].key, kScalars[I].type,
                  std::string_view(kScalars[I].col) != "parent_id" &&
                      std::string_view(kScalars[I].col) != "input_ref" &&
                      std::string_view(kScalars[I].col) != "color"}};
}

template <std::size_t I>
const std::array<Field, 2>& scalarFieldsStorage() {
    static const std::array<Field, 2> f = scalarFields<I>();
    return f;
}

template <std::size_t I>
constexpr OpDescriptor makeScalar() {
    return OpDescriptor{kScalars[I].op,
                        kScalars[I].summary,
                        Scope::Edit,
                        kScalars[I].impact,
                        {},
                        kScalars[I].coalescable,
                        false,
                        &scalarApply<I>,
                        &scalarInverse<I>,
                        ""};
}

template <std::size_t... Is>
void appendScalars(std::vector<OpDescriptor>& out, std::index_sequence<Is...>) {
    (
        [&] {
            auto d = makeScalar<Is>();
            d.fields = scalarFieldsStorage<Is>();
            out.push_back(d);
        }(),
        ...);
}

// ---------------------------------------------------------------------------
// ADR-0174: native analog summing on a group. The row is made on first use and
// an op that finds none reads the DDL's defaults, so "no row" and "a row of
// defaults" are the same state and the inverse of either restores it.
// ---------------------------------------------------------------------------

bool summingOnGroup(OpContext& c, std::int64_t id, std::string& err) {
    SQLite::Statement st(c.db, "SELECT kind FROM tracks WHERE id = ?");
    st.bind(1, id);
    if (!st.executeStep()) { err = "no such track"; return false; }
    const std::string kind = st.getColumn(0).getString();
    if (kind != "group" && kind != "master") {
        err = "summing is only on a group or master track";
        return false;
    }
    return true;
}

template <typename Bind>
bool summingSet(OpContext& c, const Payload& p, const char* col, Bind bind, std::string& err) {
    try {
        const auto id = p.at("id").get<std::int64_t>();
        if (!summingOnGroup(c, id, err)) return false;
        SQLite::Statement st(c.db, std::string("INSERT INTO group_summing(track_id, ") + col +
                                       ") VALUES (?, ?) ON CONFLICT(track_id) DO UPDATE SET " + col +
                                       " = excluded." + col);
        st.bind(1, id);
        bind(st);
        st.exec();
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}

bool summingInverse(OpContext& c, const Payload& p, const char* col, const char* key, FieldType t,
                    Payload dflt, Payload& inv, std::string& err) {
    try {
        const auto id = p.at("id").get<std::int64_t>();
        if (!summingOnGroup(c, id, err)) return false;
        SQLite::Statement st(c.db, std::string("SELECT ") + col + " FROM group_summing WHERE track_id = ?");
        st.bind(1, id);
        inv = Payload::object();
        inv["id"] = id;
        inv[key] = st.executeStep() ? readValue(st.getColumn(0), t) : std::move(dflt);
        return true;   // symmetric
    } catch (const std::exception& e) { err = e.what(); return false; }
}

bool summingEnabledApply(OpContext& c, const Payload& p, std::string& err) {
    const bool on = p.at("enabled").get<bool>();
    return summingSet(c, p, "enabled", [on](SQLite::Statement& st) { st.bind(2, on ? 1 : 0); }, err);
}
bool summingEnabledInverse(OpContext& c, const Payload& p, Payload& inv, std::string& err) {
    return summingInverse(c, p, "enabled", "enabled", FieldType::Bool, false, inv, err);
}
bool summingFlavorApply(OpContext& c, const Payload& p, std::string& err) {
    const std::string key = p.at("flavor").get<std::string>();
    if (findSummingFlavor(key) == nullptr) { err = "no summing flavour '" + key + "'"; return false; }
    return summingSet(c, p, "flavor", [&key](SQLite::Statement& st) { st.bind(2, key); }, err);
}
bool summingFlavorInverse(OpContext& c, const Payload& p, Payload& inv, std::string& err) {
    return summingInverse(c, p, "flavor", "flavor", FieldType::Text, std::string(kDefaultSummingFlavor),
                          inv, err);
}
bool summingDriveApply(OpContext& c, const Payload& p, std::string& err) {
    const double db = p.at("db").get<double>();
    if (!(db >= kSummingDriveMinDb && db <= kSummingDriveMaxDb)) {
        err = "summing drive must be between -12 and +24 dB";
        return false;
    }
    return summingSet(c, p, "drive_db", [db](SQLite::Statement& st) { st.bind(2, db); }, err);
}
bool summingDriveInverse(OpContext& c, const Payload& p, Payload& inv, std::string& err) {
    return summingInverse(c, p, "drive_db", "db", FieldType::Real, 0.0, inv, err);
}

// ---------------------------------------------------------------------------
// Hand-written: creation, deletion, and anything touching a blob
// ---------------------------------------------------------------------------

bool setProjectNameApply(OpContext& c, const Payload& p, std::string& err) {
    try {
        SQLite::Statement st(c.db, "UPDATE project SET name = ? WHERE id = 1");
        st.bind(1, p.at("name").get<std::string>());
        return st.exec() >= 0;
    } catch (const std::exception& e) { err = e.what(); return false; }
}
bool setProjectNameInverse(OpContext& c, const Payload&, Payload& inv, std::string& err) {
    try {
        SQLite::Statement st(c.db, "SELECT name FROM project WHERE id = 1");
        inv = Payload::object();
        inv["name"] = st.executeStep() ? st.getColumn(0).getString() : std::string{};
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}

// N38: win owns the native nullable tracks.role format/migration. Refuse a
// write on older schemas; readers can still show an unassigned role.
bool hasTrackRole(SQLite::Database& db) {
    SQLite::Statement columns(db,"PRAGMA table_info(tracks)");
    while(columns.executeStep())if(columns.getColumn(1).getString()=="role")return true;
    return false;
}
bool knownRole(const Payload& role) {
    return role.is_null() || (role.is_string() && (role=="kick" || role=="bass" || role=="vocal" || role=="reference"));
}
bool trackRoleValue(OpContext& c,const Payload& p,Payload& role,std::string& err) {
    if(!hasTrackRole(c.db)){err="tracks.role format migration has not landed";return false;}
    if(p.at("id").get<std::int64_t>()<=0){err="track role needs a positive id";return false;}
    role=p.value("role",Payload(nullptr));
    if(!knownRole(role)){err="unknown track role";return false;}
    SQLite::Statement row(c.db,"SELECT kind,role FROM tracks WHERE id=?");row.bind(1,p.at("id").get<std::int64_t>());
    if(!row.executeStep()){err="no such track";return false;}
    if(!role.is_null()) {
        const auto kind=row.getColumn(0).getString();
        if(kind!="audio" && kind!="midi" && kind!="instrument" && kind!="group" && kind!="return"){err="track kind cannot be an anchor role";return false;}
        if(role=="reference") {
            SQLite::Statement other(c.db,"SELECT 1 FROM tracks WHERE role='reference' AND id<>? LIMIT 1");other.bind(1,p.at("id").get<std::int64_t>());
            if(other.executeStep()){err="clear the existing reference role in the same batch first";return false;}
        }
    }
    return true;
}
bool trackRoleApply(OpContext& c,const Payload& p,std::string& err) {
    try {
        Payload role;if(!trackRoleValue(c,p,role,err))return false;
        SQLite::Statement update(c.db,"UPDATE tracks SET role=? WHERE id=?");
        if(role.is_null())update.bind(1);else update.bind(1,role.get<std::string>());
        update.bind(2,p.at("id").get<std::int64_t>());return update.exec()==1;
    }catch(const std::exception& exception){err=exception.what();return false;}
}
bool trackRoleInverse(OpContext& c,const Payload& p,Payload& inverse,std::string& err) {
    try {
        Payload role;if(!trackRoleValue(c,p,role,err))return false;
        SQLite::Statement row(c.db,"SELECT role FROM tracks WHERE id=?");row.bind(1,p.at("id").get<std::int64_t>());
        if(!row.executeStep()){err="no such track";return false;}
        Payload previous=nullptr;if(!row.getColumn(0).isNull())previous=row.getColumn(0).getString();
        if(!knownRole(previous)){err="unrecognized stored track role retained";return false;}
        inverse={{"id",p.at("id")},{"role",previous}};return true;
    }catch(const std::exception& exception){err=exception.what();return false;}
}

bool trackCreateApply(OpContext& c, const Payload& p, std::string& err) {
    try {
        if(p.contains("contents")) {
            detail::restoreTrack(c.db,p.at("id").get<std::int64_t>(),p.at("contents"));
            return true;
        }
        // The id comes from the payload, never from SQLite (ADR-0021 §7.3):
        // undo a create, redo it, and the object must return with the SAME id
        // or every later op referencing it points at nothing.
        SQLite::Statement st(c.db,
            "INSERT INTO tracks(id, kind, name, parent_id, index_in_parent) "
            "VALUES (?,?,?,?,?)");
        st.bind(1, p.at("id").get<std::int64_t>());
        st.bind(2, p.at("kind").get<std::string>());
        st.bind(3, p.value("name", std::string{}));
        if (p.contains("parent") && !p.at("parent").is_null())
            st.bind(4, p.at("parent").get<std::int64_t>());
        else
            st.bind(4);
        st.bind(5, p.value("index", std::int64_t{0}));
        st.exec();

        // Every track needs a mixer strip; creating it here rather than in a
        // second op keeps "a track exists" and "it can be mixed" from being two
        // states one undo apart.
        SQLite::Statement ms(c.db, "INSERT OR IGNORE INTO mixer_strip(track_id) VALUES (?)");
        ms.bind(1, p.at("id").get<std::int64_t>());
        ms.exec();
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}
bool trackCreateInverse(OpContext&, const Payload& p, Payload& inv, std::string& err) {
    if (!p.contains("id")) { err = "track.create payload has no id"; return false; }
    inv = Payload::object();
    inv["id"] = p.at("id");
    return true;   // paired: track.delete
}

bool trackDeleteApply(OpContext& c, const Payload& p, std::string& err) {
    try {
        detail::deleteTrack(c.db,p.at("id").get<std::int64_t>());
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}
bool trackDeleteInverse(OpContext& c, const Payload& p, Payload& inv, std::string& err) {
    // State capture (OPS.md §6.2/§6.3). Media is never captured -- deleting a
    // track unlinks media references, it does not delete audio -- so the inverse
    // of a 40-clip audio track is metadata, not gigabytes.
    try {
        SQLite::Statement st(c.db,
            "SELECT kind, name, parent_id, index_in_parent FROM tracks WHERE id = ?");
        st.bind(1, p.at("id").get<std::int64_t>());
        if (!st.executeStep()) { err = "no such track to capture"; return false; }
        inv = Payload::object();
        inv["id"] = p.at("id");
        inv["kind"] = st.getColumn(0).getString();
        inv["name"] = st.getColumn(1).getString();
        if (st.getColumn(2).isNull()) inv["parent"] = nullptr;
        else inv["parent"] = st.getColumn(2).getInt64();
        inv["index"] = st.getColumn(3).getInt64();
        inv["contents"] = detail::captureTrack(c.db,p.at("id").get<std::int64_t>());
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}

constexpr const char* audioColumns[] = {"media_id","src_start_frames","src_len_frames",
    "warp_enabled","warp_mode","warp_markers","transpose_semis","formant_shift","reverse","channel_mode"};
std::string audioColumnList() {
    std::string names;
    for (const auto* name:audioColumns) {if(!names.empty())names+=',';names+=name;}
    return names;
}
void restoreClipAudio(SQLite::Database& db, std::int64_t clip, const Payload& row) {
    if(!row.is_object() || row.size()!=10)throw std::runtime_error("invalid captured audio row");
    SQLite::Statement q(db,"INSERT INTO audio_clips(clip_id,"+audioColumnList()+") VALUES(?,?,?,?,?,?,?,?,?,?,?)");
    q.bind(1,clip);
    int column=2;
    for(const auto* name:audioColumns) {
        const auto& value=row.at(name);
        if(std::string_view(name)=="warp_markers") {
            if(value.is_null())q.bind(column);
            else {
                if(!value.is_array())throw std::runtime_error("invalid captured warp markers");
                std::vector<unsigned char> bytes;
                for(const auto& b:value) {
                    if(!b.is_number_integer() || b.get<std::int64_t>()<0 || b.get<std::int64_t>()>255)
                        throw std::runtime_error("invalid captured warp byte");
                    bytes.push_back(b.get<unsigned char>());
                }
                if(bytes.size()>static_cast<std::size_t>(std::numeric_limits<int>::max()))throw std::runtime_error("warp markers too large");
                const unsigned char empty=0;
                q.bind(column,bytes.empty()?&empty:bytes.data(),static_cast<int>(bytes.size()));
            }
        } else if(std::string_view(name)=="warp_mode")q.bind(column,value.get<std::string>());
        else if(std::string_view(name)=="transpose_semis" || std::string_view(name)=="formant_shift")q.bind(column,value.get<double>());
        else {
            if(!value.is_number_integer())throw std::runtime_error("invalid captured audio integer");
            q.bind(column,value.get<std::int64_t>());
        }
        ++column;
    }
    q.exec();
}
bool clipCreateApply(OpContext& c, const Payload& p, std::string& err) {
    try {
        if(p.contains("row")) {clip_edit::restoreDeleted(c,p);return true;}
        SQLite::Statement st(c.db,
            "INSERT INTO clips(id, track_id, kind, name, time_base, pos_ticks, "
            "length_ticks) VALUES (?,?,?,?,0,?,?)");
        st.bind(1, p.at("id").get<std::int64_t>());
        st.bind(2, p.at("track").get<std::int64_t>());
        st.bind(3, p.at("kind").get<std::string>());
        st.bind(4, p.value("name", std::string{}));
        st.bind(5, p.at("pos").get<std::int64_t>());
        st.bind(6, p.at("length").get<std::int64_t>());
        st.exec();
        if(p.contains("audio")) {
            if(p.at("kind")!="audio")throw std::runtime_error("captured audio needs an audio clip");
            restoreClipAudio(c.db,p.at("id").get<std::int64_t>(),p.at("audio"));
        }
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}
bool clipCreateInverse(OpContext&, const Payload& p, Payload& inv, std::string& err) {
    if (!p.contains("id")) { err = "clip.create payload has no id"; return false; }
    inv = Payload::object();
    inv["id"] = p.at("id");
    return true;
}

bool clipDeleteApply(OpContext& c, const Payload& p, std::string& err) {
    try {
        auto image=clip_edit::captureDeleted(c,detail::signedInteger(p.at("id")));
        edit::RowEdits deletion(c.db);clip_edit::deleteImage(deletion,image);
        return edit::applyRows(c,{{"changes",deletion.changes()}},err);
    } catch (const std::exception& e) { err = e.what(); return false; }
}
bool clipDeleteInverse(OpContext& c,const Payload& p,Payload& inv,std::string& err) {
    try {inv=clip_edit::captureDeleted(c,detail::signedInteger(p.at("id")));return true;}
    catch(const std::exception& exception){err=exception.what();return false;}
}

bool audioAttachApply(OpContext& c, const Payload& p, std::string& err) {
    try {
        SQLite::Statement clip(c.db, "SELECT kind FROM clips WHERE id=?");
        clip.bind(1,p.at("id").get<std::int64_t>());
        if (!clip.executeStep() || clip.getColumn(0).getString() != "audio") { err="audio attachment needs an audio clip"; return false; }
        const auto frames=p.at("frames").get<std::int64_t>();
        if(frames<=0) { err="audio attachment needs positive frames"; return false; }
        SQLite::Statement mediaBounds(c.db,"SELECT frames FROM media_files WHERE id=?");
        mediaBounds.bind(1,p.at("media").get<std::int64_t>());
        if(!mediaBounds.executeStep() || (!mediaBounds.getColumn(0).isNull() && frames>mediaBounds.getColumn(0).getInt64())) {
            err="audio attachment exceeds its media or media is missing";return false;
        }
        SQLite::Statement q(c.db,"INSERT INTO audio_clips(clip_id,media_id,src_len_frames) VALUES(?,?,?)");
        q.bind(1,p.at("id").get<std::int64_t>()); q.bind(2,p.at("media").get<std::int64_t>());q.bind(3,frames);q.exec();return true;
    } catch(const std::exception& e){err=e.what();return false;}
}
bool audioDetachApply(OpContext& c, const Payload& p, std::string& err) {
    try { SQLite::Statement q(c.db,"DELETE FROM audio_clips WHERE clip_id=?");q.bind(1,p.at("id").get<std::int64_t>());
        if(q.exec()!=1){err="no audio attachment";return false;} return true;
    } catch(const std::exception& e){err=e.what();return false;}
}
bool audioDetachInverse(OpContext& c,const Payload& p,Payload& inv,std::string& err) {
    try {
        // Refuse lossy removal: this simple pair owns only untouched attachments.
        SQLite::Statement q(c.db,"SELECT media_id,src_len_frames FROM audio_clips WHERE clip_id=? AND src_start_frames=0 AND warp_enabled=0 AND warp_mode='none' AND warp_markers IS NULL AND transpose_semis=0 AND formant_shift=0 AND reverse=0 AND channel_mode=0");
        q.bind(1,p.at("id").get<std::int64_t>());
        if(!q.executeStep()){err="audio attachment has edits or is missing";return false;}
        inv={{"id",p.at("id")},{"media",q.getColumn(0).getInt64()},{"frames",q.getColumn(1).getInt64()}};return true;
    } catch(const std::exception& e){err=e.what();return false;}
}

bool clipMoveApply(OpContext& c, const Payload& p, std::string& err) {
    try {
        SQLite::Statement st(c.db,
            "UPDATE clips SET pos_ticks = ?, track_id = ? WHERE id = ?");
        st.bind(1, p.at("to").get<std::int64_t>());
        st.bind(2, p.at("track").get<std::int64_t>());
        st.bind(3, p.at("id").get<std::int64_t>());
        if (st.exec() == 0) { err = "no such clip"; return false; }
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}
bool clipMoveInverse(OpContext& c, const Payload& p, Payload& inv, std::string& err) {
    try {
        SQLite::Statement st(c.db, "SELECT pos_ticks, track_id FROM clips WHERE id = ?");
        st.bind(1, p.at("id").get<std::int64_t>());
        if (!st.executeStep()) { err = "no such clip"; return false; }
        inv = Payload::object();
        inv["id"] = p.at("id");
        inv["to"] = st.getColumn(0).getInt64();
        inv["track"] = st.getColumn(1).getInt64();
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}

bool clipResizeApply(OpContext& c, const Payload& p, std::string& err) {
    try {
        SQLite::Statement st(c.db,
            "UPDATE clips SET pos_ticks = ?, length_ticks = ? WHERE id = ?");
        st.bind(1, p.at("pos").get<std::int64_t>());
        st.bind(2, p.at("length").get<std::int64_t>());
        st.bind(3, p.at("id").get<std::int64_t>());
        if (st.exec() == 0) { err = "no such clip"; return false; }
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}
bool clipResizeInverse(OpContext& c, const Payload& p, Payload& inv, std::string& err) {
    try {
        SQLite::Statement st(c.db, "SELECT pos_ticks, length_ticks FROM clips WHERE id = ?");
        st.bind(1, p.at("id").get<std::int64_t>());
        if (!st.executeStep()) { err = "no such clip"; return false; }
        inv = Payload::object();
        inv["id"] = p.at("id");
        inv["pos"] = st.getColumn(0).getInt64();
        inv["length"] = st.getColumn(1).getInt64();
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}

// The five historical note entry points delegate to the checked full-stride
// note-diff kernel. Their descriptor names and inverse pairs below remain stable
// because History resolves old journal inverses through the current catalogue.

// --- tempo and signature ------------------------------------------------------

bool tempoInsertApply(OpContext& c, const Payload& p, std::string& err) {
    try {
        const bool explicitId = p.contains("id");
        const bool explicitTension = p.contains("tension");
        std::string columns = "pos_ticks,bpm,curve", slots = "?,?,?";
        if (explicitId) { columns += ",id"; slots += ",?"; }
        if (explicitTension) { columns += ",tension"; slots += ",?"; }
        SQLite::Statement st(c.db, "INSERT INTO tempo_map(" + columns + ") VALUES(" + slots + ")");
        st.bind(1, p.at("pos").get<std::int64_t>());
        st.bind(2, p.at("bpm").get<double>());
        st.bind(3, p.value("curve", std::int64_t{0}));
        int parameter = 3;
        if (explicitId) st.bind(++parameter, p.at("id").get<std::int64_t>());
        if (explicitTension) st.bind(++parameter, p.at("tension").get<double>());
        st.exec();
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}
bool tempoInsertInverse(OpContext&, const Payload& p, Payload& inv, std::string& err) {
    if (!p.contains("pos")) { err = "tempo event needs a position"; return false; }
    inv = Payload::object();
    inv["pos"] = p.at("pos");
    return true;
}
bool tempoRemoveApply(OpContext& c, const Payload& p, std::string& err) {
    try {
        SQLite::Statement st(c.db, "DELETE FROM tempo_map WHERE pos_ticks = ?");
        st.bind(1, p.at("pos").get<std::int64_t>());
        if (st.exec() == 0) { err = "no tempo event at that position"; return false; }
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}
bool tempoRemoveInverse(OpContext& c, const Payload& p, Payload& inv, std::string& err) {
    try {
        SQLite::Statement st(c.db, "SELECT bpm, curve, id, tension FROM tempo_map WHERE pos_ticks = ?");
        st.bind(1, p.at("pos").get<std::int64_t>());
        if (!st.executeStep()) { err = "no tempo event to capture"; return false; }
        inv = Payload::object();
        inv["pos"] = p.at("pos");
        inv["bpm"] = st.getColumn(0).getDouble();
        inv["curve"] = st.getColumn(1).getInt64();
        inv["id"] = st.getColumn(2).getInt64();
        inv["tension"] = st.getColumn(3).getDouble();
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}

bool sigInsertApply(OpContext& c, const Payload& p, std::string& err) {
    try {
        SQLite::Statement st(c.db,
            "INSERT INTO time_signature_map(pos_ticks, numerator, denominator) "
            "VALUES (?,?,?)");
        st.bind(1, p.at("pos").get<std::int64_t>());
        st.bind(2, p.at("num").get<std::int64_t>());
        st.bind(3, p.at("den").get<std::int64_t>());
        st.exec();
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}
bool sigInsertInverse(OpContext&, const Payload& p, Payload& inv, std::string& err) {
    if (!p.contains("pos")) { err = "signature event needs a position"; return false; }
    inv = Payload::object();
    inv["pos"] = p.at("pos");
    return true;
}
bool sigRemoveApply(OpContext& c, const Payload& p, std::string& err) {
    try {
        SQLite::Statement st(c.db, "DELETE FROM time_signature_map WHERE pos_ticks = ?");
        st.bind(1, p.at("pos").get<std::int64_t>());
        if (st.exec() == 0) { err = "no signature at that position"; return false; }
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}
bool sigRemoveInverse(OpContext& c, const Payload& p, Payload& inv, std::string& err) {
    try {
        SQLite::Statement st(c.db,
            "SELECT numerator, denominator FROM time_signature_map WHERE pos_ticks = ?");
        st.bind(1, p.at("pos").get<std::int64_t>());
        if (!st.executeStep()) { err = "no signature to capture"; return false; }
        inv = Payload::object();
        inv["pos"] = p.at("pos");
        inv["num"] = st.getColumn(0).getInt64();
        inv["den"] = st.getColumn(1).getInt64();
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}

// --- routing ------------------------------------------------------------------

bool routingConnectApply(OpContext& c, const Payload& p, std::string& err) {
    try {
        if(p.contains("row")) {
            const auto& row=p.at("row");
            if(detail::signedInteger(row.at("id"))!=detail::signedInteger(p.at("id")))throw std::runtime_error("captured routing id differs");
            detail::restoreRow(c.db,"routing",row);return true;
        }
        bool pre=false;
        if(p.at("kind")=="send" && p.at("dstKind")=="track")
            if(const auto choice=explicitReturnSendTap(c.db,p.at("dst").get<std::int64_t>()))pre=*choice;

        SQLite::Statement st(c.db,
            "INSERT INTO routing(id, src_kind, src_id, dst_kind, dst_id, kind, pre_fader) "
            "VALUES (?,?,?,?,?,?,?)");
        st.bind(1, p.at("id").get<std::int64_t>());
        st.bind(2, p.at("srcKind").get<std::string>());
        st.bind(3, p.at("src").get<std::int64_t>());
        st.bind(4, p.at("dstKind").get<std::string>());
        st.bind(5, p.at("dst").get<std::int64_t>());
        st.bind(6, p.at("kind").get<std::string>());
        st.bind(7,pre?1:0);
        st.exec();
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}
bool routingConnectInverse(OpContext&, const Payload& p, Payload& inv, std::string& err) {
    if (!p.contains("id")) { err = "routing.connect needs an id"; return false; }
    inv = Payload::object();
    inv["id"] = p.at("id");
    return true;
}
bool routingDisconnectApply(OpContext& c, const Payload& p, std::string& err) {
    try {
        SQLite::Statement st(c.db, "DELETE FROM routing WHERE id = ?");
        st.bind(1, p.at("id").get<std::int64_t>());
        if (st.exec() == 0) { err = "no such connection"; return false; }
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}
bool routingDisconnectInverse(OpContext& c, const Payload& p, Payload& inv, std::string& err) {
    try {
        SQLite::Statement st(c.db,
            "SELECT src_kind, src_id, dst_kind, dst_id, kind FROM routing WHERE id = ?");
        st.bind(1, p.at("id").get<std::int64_t>());
        if (!st.executeStep()) { err = "no such connection to capture"; return false; }
        inv = Payload::object();
        inv["id"] = p.at("id");
        inv["srcKind"] = st.getColumn(0).getString();
        inv["src"] = st.getColumn(1).getInt64();
        inv["dstKind"] = st.getColumn(2).getString();
        inv["dst"] = st.getColumn(3).getInt64();
        inv["kind"] = st.getColumn(4).getString();
        inv["row"] = *detail::captureRow(c.db,"routing","id",p.at("id").get<std::int64_t>());
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}

// --- remarks (ADR-0131, OPS.md 9.12) ------------------------------------------
//
// The time a remark was written is in the payload (`created`), never read from
// a clock here: replayed a year later the row must come back identical (OPS.md
// 7.2). Likewise `author` is the payload's, not the submitting actor's -- a
// handler cannot see who submitted it, and must not (the replay corpus runs one
// log under two actors and demands the same project).

bool remarkAddApply(OpContext& c, const Payload& p, std::string& err) {
    try {
        SQLite::Statement st(c.db,
            "INSERT INTO remarks(id, target_kind, target_id, param_id, author, "
            "actor_detail, text, created_utc, resolved) VALUES (?,?,?,?,?,?,?,?,?)");
        st.bind(1, p.at("id").get<std::int64_t>());
        st.bind(2, p.at("kind").get<std::string>());
        st.bind(3, p.at("target").get<std::int64_t>());
        if (p.contains("param") && !p.at("param").is_null())
            st.bind(4, p.at("param").get<std::string>());
        else
            st.bind(4);
        st.bind(5, p.at("author").get<std::string>());
        st.bind(6, p.value("detail", std::string{}));
        st.bind(7, p.at("text").get<std::string>());
        st.bind(8, p.at("created").get<std::int64_t>());
        st.bind(9, p.value("resolved", false) ? 1 : 0);
        st.exec();
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}
bool remarkAddInverse(OpContext&, const Payload& p, Payload& inv, std::string& err) {
    if (!p.contains("id")) { err = "remark.add payload has no id"; return false; }
    inv = Payload::object();
    inv["id"] = p.at("id");
    return true;   // paired: remark.remove
}

bool remarkRemoveApply(OpContext& c, const Payload& p, std::string& err) {
    try {
        SQLite::Statement st(c.db, "DELETE FROM remarks WHERE id = ?");
        st.bind(1, p.at("id").get<std::int64_t>());
        if (st.exec() == 0) { err = "no such remark"; return false; }
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}
bool remarkRemoveInverse(OpContext& c, const Payload& p, Payload& inv, std::string& err) {
    // State capture: every column, so the remark.add that undoes this puts back
    // the same row -- author, time and resolved flag included.
    try {
        SQLite::Statement st(c.db,
            "SELECT target_kind, target_id, param_id, author, actor_detail, text, "
            "created_utc, resolved FROM remarks WHERE id = ?");
        st.bind(1, p.at("id").get<std::int64_t>());
        if (!st.executeStep()) { err = "no such remark to capture"; return false; }
        inv = Payload::object();
        inv["id"] = p.at("id");
        inv["kind"] = st.getColumn(0).getString();
        inv["target"] = st.getColumn(1).getInt64();
        if (st.getColumn(2).isNull()) inv["param"] = nullptr;
        else inv["param"] = st.getColumn(2).getString();
        inv["author"] = st.getColumn(3).getString();
        inv["detail"] = st.getColumn(4).getString();
        inv["text"] = st.getColumn(5).getString();
        inv["created"] = st.getColumn(6).getInt64();
        inv["resolved"] = st.getColumn(7).getInt() != 0;
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}

bool remarkEditApply(OpContext& c, const Payload& p, std::string& err) {
    try {
        SQLite::Statement st(c.db, "UPDATE remarks SET text = ? WHERE id = ?");
        st.bind(1, p.at("text").get<std::string>());
        st.bind(2, p.at("id").get<std::int64_t>());
        if (st.exec() == 0) { err = "no such remark"; return false; }
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}
bool remarkEditInverse(OpContext& c, const Payload& p, Payload& inv, std::string& err) {
    try {
        SQLite::Statement st(c.db, "SELECT text FROM remarks WHERE id = ?");
        st.bind(1, p.at("id").get<std::int64_t>());
        if (!st.executeStep()) { err = "no such remark"; return false; }
        inv = Payload::object();
        inv["id"] = p.at("id");
        inv["text"] = st.getColumn(0).getString();
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}

bool remarkResolveApply(OpContext& c, const Payload& p, std::string& err) {
    try {
        SQLite::Statement st(c.db, "UPDATE remarks SET resolved = ? WHERE id = ?");
        st.bind(1, p.at("resolved").get<bool>() ? 1 : 0);
        st.bind(2, p.at("id").get<std::int64_t>());
        if (st.exec() == 0) { err = "no such remark"; return false; }
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}
bool remarkResolveInverse(OpContext& c, const Payload& p, Payload& inv, std::string& err) {
    try {
        SQLite::Statement st(c.db, "SELECT resolved FROM remarks WHERE id = ?");
        st.bind(1, p.at("id").get<std::int64_t>());
        if (!st.executeStep()) { err = "no such remark"; return false; }
        inv = Payload::object();
        inv["id"] = p.at("id");
        inv["resolved"] = st.getColumn(0).getInt() != 0;
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}

// --- transport: the first real ephemeral ops ----------------------------------
//
// Play/stop/seek/record remain driver commands, with no undo (ADR-0027).
// Loop and click now materialise saved performance state in session_state. They still go through the registry -- same
// validation, same scopes, same attribution -- and still get a log row tagged
// `ephemeral`, so "what did the agent do at 14:32" stays answerable while Ctrl-Z
// lands on the last real edit.

bool transportNoop(OpContext&, const Payload&, std::string&) { return true; }

bool transportSetting(OpContext& c,const Payload& p,std::string& error,bool loop) {
    try {
        Payload value;
        if(loop) {
            const auto previous=readTransportSettings(c.store);
            const auto start=p.value("start",previous.loopStartTicks),end=p.value("end",previous.loopEndTicks);
            if(start<0 || end<=start){error="loop requires nonnegative start and end after start";return false;}
            value={{"on",p.at("on")},{"start",start},{"end",end}};
        }else value=p.at("on");
        SQLite::Statement set(c.db,"INSERT INTO session_state(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value");
        set.bind(1,loop?"transport.loop":"transport.metronome");set.bind(2,value.dump());set.exec();return true;
    }catch(const std::exception& e){error=e.what();return false;}
}
bool transportCountIn(OpContext &c, const Payload &p, std::string &error) {
    const auto bars = p.at("bars").get<std::int64_t>();
    if (bars != 0 && bars != 1 && bars != 2 && bars != 4) {
        error = "Count-In must be None, 1, 2 or 4 bars";
        return false;
    }
    SQLite::Statement set(
        c.store.db(),
        "INSERT OR REPLACE INTO session_state(key,value) VALUES('transport.countIn',?)");
    set.bind(1, Payload(bars).dump());
    set.exec();
    return true;
}
bool transportSongStart(OpContext &c, const Payload &p, std::string &error) {
    if (!p.at("on").get<bool>()) {
        c.store.db().exec("DELETE FROM session_state WHERE key='transport.songStart'");
        return true;
    }
    if (!p.contains("start") || p.at("start").get<std::int64_t>() < 0) {
        error = "Song start needs a nonnegative tick position";
        return false;
    }
    SQLite::Statement set(
        c.store.db(),
        "INSERT OR REPLACE INTO session_state(key,value) VALUES('transport.songStart',?)");
    set.bind(1, p.at("start").dump());
    set.exec();
    return true;
}
bool transportLoop(OpContext& c,const Payload& p,std::string& error){return transportSetting(c,p,error,true);}
bool transportMetronome(OpContext& c,const Payload& p,std::string& error){return transportSetting(c,p,error,false);}

// --- field tables ---------------------------------------------------------------

constexpr Field kFProjectName[] = {{"name", FieldType::Text, true}};
constexpr Field kFSummingEnabled[] = {{"id", FieldType::Int, true}, {"enabled", FieldType::Bool, true}};
constexpr Field kFSummingFlavor[] = {{"id", FieldType::Int, true}, {"flavor", FieldType::Text, true}};
constexpr Field kFSummingDrive[] = {{"id", FieldType::Int, true}, {"db", FieldType::Real, true}};
constexpr Field kFTrackRole[] = {{"id", FieldType::Int, true}, {"role", FieldType::Text, false}};
constexpr Field kFTrackCreate[] = {{"id", FieldType::Int, true},
                                   {"kind", FieldType::Text, true},
                                   {"name", FieldType::Text, false},
                                   {"parent", FieldType::Int, false},
                                   {"index", FieldType::Int, false},
                                   {"contents", FieldType::Object, false}};
constexpr Field kFId[] = {{"id", FieldType::Int, true}};
constexpr Field kFClipCreate[] = {{"id", FieldType::Int, true},
                                  {"track", FieldType::Int, true},
                                  {"kind", FieldType::Text, true},
                                  {"name", FieldType::Text, false},
                                  {"pos", FieldType::Int, true},
                                  {"length", FieldType::Int, true},
                                  {"audio", FieldType::Object, false}};
constexpr Field kFAudioAttach[] = {{"id",FieldType::Int,true},{"media",FieldType::Int,true},{"frames",FieldType::Int,true}};
constexpr Field kFClipMove[] = {{"id", FieldType::Int, true},
                                {"to", FieldType::Int, true},
                                {"track", FieldType::Int, true}};
constexpr Field kFClipResize[] = {{"id", FieldType::Int, true},
                                  {"pos", FieldType::Int, true},
                                  {"length", FieldType::Int, true}};
constexpr Field kFNoteFull[] = {{"clip", FieldType::Int, true},
                                {"note", FieldType::Int, true},
                                {"start", FieldType::Int, true},
                                {"dur", FieldType::Int, true},
                                {"key", FieldType::Int, true},
                                {"vel", FieldType::Int, false},
                                {"velOff", FieldType::Int, false},
                                {"chan", FieldType::Int, false},
                                {"prob", FieldType::Int, false},
                                {"cents", FieldType::Real, false}};
constexpr Field kFNoteRef[] = {{"clip", FieldType::Int, true},
                               {"note", FieldType::Int, true}};
constexpr Field kFNoteMove[] = {{"clip", FieldType::Int, true},
                                {"note", FieldType::Int, true},
                                {"start", FieldType::Int, true},
                                {"key", FieldType::Int, true}};
constexpr Field kFNoteResize[] = {{"clip", FieldType::Int, true},
                                  {"note", FieldType::Int, true},
                                  {"dur", FieldType::Int, true}};
constexpr Field kFNoteVel[] = {{"clip", FieldType::Int, true},
                               {"note", FieldType::Int, true},
                               {"vel", FieldType::Int, true}};
constexpr Field kFNoteMute[] = {{"clip",FieldType::Int,true},{"note",FieldType::Int,true},
                                {"muted",FieldType::Bool,true}};
constexpr Field kFTempoIns[] = {{"pos", FieldType::Int, true},
                                {"bpm", FieldType::Real, true},
                                {"curve", FieldType::Int, false},
                                {"id", FieldType::Int, false},
                                {"tension", FieldType::Real, false}};
constexpr Field kFPos[] = {{"pos", FieldType::Int, true}};
constexpr Field kFSigIns[] = {{"pos", FieldType::Int, true},
                              {"num", FieldType::Int, true},
                              {"den", FieldType::Int, true}};
constexpr Field kFRouting[] = {{"id", FieldType::Int, true},
                               {"srcKind", FieldType::Text, true},
                               {"src", FieldType::Int, true},
                               {"dstKind", FieldType::Text, true},
                               {"dst", FieldType::Int, true},
                               {"kind", FieldType::Text, true},
                               {"row", FieldType::Object, false}};
constexpr Field kFChainCreate[] = {{"id", FieldType::Int, true},
                                   {"track", FieldType::Int, false},
                                   {"device", FieldType::Int, false},
                                   {"ord", FieldType::Int, false},
                                   {"name", FieldType::Text, false}};
constexpr Field kFDeviceInsert[] = {{"id", FieldType::Int, true},
                                    {"chain", FieldType::Int, true},
                                    {"ord", FieldType::Int, false},
                                    {"ref", FieldType::Int, false},
                                    {"name", FieldType::Text, false},
                                    {"enabled", FieldType::Bool, false},
                                    {"rack", FieldType::Bool, false},
                                    {"preset", FieldType::Text, false},
                                    {"latency", FieldType::Int, false},
                                    {"always", FieldType::Bool, false},
                                    {"missing", FieldType::Bool, false},
                                    {"params", FieldType::Array, false},
                                    {"state", FieldType::Array, false},
                                    {"route", FieldType::Text, false},
                                    {"panel", FieldType::Array, false},
                                    {"rackKind", FieldType::Text, false},
                                    {"remoteHost", FieldType::Int, false}};
constexpr Field kFDeviceMove[] = {{"id", FieldType::Int, true},
                                  {"chain", FieldType::Int, true},
                                  {"ord", FieldType::Int, true}};
// `norm` is OPTIONAL-and-nullable, following the convention `track.setParent`
// set for `parent`: the validator treats an explicit null on an optional field
// as "clear this". A null `norm` MEANS the parameter has no stored row -- see
// the absence problem in the handler comment; it is what keeps this
// coalescable op's inverse symmetric.
//
// The handler then insists the KEY is present, which the validator cannot
// express. Omitting `norm` entirely and deleting the row would be a silent
// discard of a parameter value from a typo, and the closed-schema check only
// catches a misspelling, not an absence.
constexpr Field kFDeviceParam[] = {{"dev", FieldType::Int, true},
                                   {"param", FieldType::Text, true},
                                   {"norm", FieldType::Real, false},
                                   {"real", FieldType::Real, false},
                                   {"display", FieldType::Text, false}};
constexpr Field kFDeviceState[] = {{"dev", FieldType::Int, true},
                                   {"role", FieldType::Text, true},
                                   {"hash", FieldType::Text, false},
                                   {"hint", FieldType::Text, false}};
// ADR-0146, ADR-0149: `route` optional-and-nullable, `device.setParam`'s
// `norm` convention: null (or absent in an inverse) clears the row, which is
// Auto. The handler still insists the key is present.
// ADR-0154: `params` optional-and-nullable, the same convention: null (or
// absent in an inverse) is Live's default panel; an array, even empty, is a
// configured one. The handler insists the key is present.
constexpr Field kFDevicePanel[] = {{"dev", FieldType::Int, true},
                                   {"params", FieldType::Array, false}};
constexpr Field kFDeviceRoute[] = {{"dev", FieldType::Int, true},
                                   {"route", FieldType::Text, false}};
constexpr Field kFDevicePreset[] = {{"dev", FieldType::Int, true},
                                    {"preset", FieldType::Text, true}};
constexpr Field kFSetParent[] = {{"id", FieldType::Int, true},
                                 {"parent", FieldType::Int, false}};
constexpr Field kFRemarkAdd[] = {{"id", FieldType::Int, true},
                                 {"kind", FieldType::Text, true},
                                 {"target", FieldType::Int, true},
                                 {"param", FieldType::Text, false},
                                 {"author", FieldType::Text, true},
                                 {"detail", FieldType::Text, false},
                                 {"text", FieldType::Text, true},
                                 {"created", FieldType::Int, true},
                                 {"resolved", FieldType::Bool, false}};
constexpr Field kFRemarkEdit[] = {{"id", FieldType::Int, true},
                                  {"text", FieldType::Text, true}};
constexpr Field kFRemarkResolve[] = {{"id", FieldType::Int, true},
                                     {"resolved", FieldType::Bool, true}};
constexpr Field kFTransportSeek[] = {{"pos", FieldType::Int, true}};
constexpr Field kFTransportFlag[] = {{"on", FieldType::Bool, true}};
constexpr Field kFTransportCountIn[] = {{"bars", FieldType::Int, true}};
constexpr Field kFTransportSongStart[] = {{"on", FieldType::Bool, true},
                                          {"start", FieldType::Int, false}};
constexpr Field kFTransportLoop[] = {{"on", FieldType::Bool, true},
                                     {"start", FieldType::Int, false},
                                     {"end", FieldType::Int, false}};

constexpr Field kFMarkerCreate[]={{"id",FieldType::Int,true},{"pos",FieldType::Int,true},{"time_base",FieldType::Int,false},
    {"name",FieldType::Text,false},{"kind",FieldType::Text,false},{"length",FieldType::Int,false},{"color",FieldType::Int,false},
    {"note",FieldType::Text,false},{"prompt",FieldType::Text,false}};
constexpr Field kFClipMarker[] = {{"id",FieldType::Int,true},{"ticks",FieldType::Int,true}};
constexpr Field kFClipLoop[] = {{"id",FieldType::Int,true},{"on",FieldType::Bool,true},
    {"start",FieldType::Int,false},{"length",FieldType::Int,false}};
constexpr Field kFClipSignature[] = {{"id",FieldType::Int,true},{"num",FieldType::Int,false},{"den",FieldType::Int,false}};
constexpr Field kFClipScale[] = {{"id",FieldType::Int,true},{"root",FieldType::Int,false},
    {"scale",FieldType::Text,false},{"mask",FieldType::Int,false}};
constexpr Field kFMarkerId[]={{"id",FieldType::Int,true}};
constexpr Field kFMarkerMove[]={{"id",FieldType::Int,true},{"pos",FieldType::Int,true}};
constexpr Field kFMarkerName[]={{"id",FieldType::Int,true},{"name",FieldType::Text,true}};
constexpr Field kFMarkerColor[]={{"id",FieldType::Int,true},{"color",FieldType::Int,false}};
constexpr Field kFMarkerText[]={{"id",FieldType::Int,true},{"text",FieldType::Text,true}};
constexpr Field kFMarkerLength[]={{"id",FieldType::Int,true},{"length",FieldType::Int,true}};

constexpr Field kFCueFirst[]={{"first",FieldType::Int,true}};
constexpr Field kFCueDb[]={{"db",FieldType::Real,true}};
constexpr Field kFCueTrack[]={{"id",FieldType::Int,true},{"on",FieldType::Bool,true}};
constexpr Field kFCueReference[]={{"on",FieldType::Bool,true},{"id",FieldType::Int,false}};

// --- track.setParent: the first op that is not a function of one row -------
//
// ADR-0044 requires that parenting a track into a group route it to that
// group's bus IN THE SAME TRANSACTION, so there is no state in which a track is
// visually inside a group and still routed to the master. That makes this the
// first op in the catalogue that touches two tables, and it was a generated
// scalar until ADR-0065.
//
// The routing model it implements (ADR-0065, SPEC 6.1):
//
//   * NO main row      -> the default: route to my parent, or to the master if
//                         I have none. The common case stores nothing.
//   * an 'auto' row    -> a materialisation of that default; grouping keeps it
//                         pointing at the right place.
//   * a 'user' row     -> the user's own routing. Grouping NEVER touches it.
//
// Absence-means-default is what keeps this op from having to INVENT a routing
// row id. ADR-0021 7.3 says an id comes from the payload and never from SQLite,
// because undo-then-redo must return the same id or every later op referencing
// it points at nothing -- so an op that inserts a row needs that row's id in
// its payload. Making the default implicit means nothing is inserted, so
// nothing needs an id, and the payload stays `{id, parent}`.

namespace {

/// Where a track's audio goes by default: its parent, or the master.
/// `std::nullopt` means nowhere -- a project with no master and no parent,
/// which is legal and silent rather than an error.
std::optional<std::int64_t> defaultDestination(OpContext& c,
                                               std::optional<std::int64_t> parent) {
    if (parent) return parent;
    SQLite::Statement st(c.db,
        "SELECT id FROM tracks WHERE kind = 'master' ORDER BY id LIMIT 1");
    if (st.executeStep()) return st.getColumn(0).getInt64();
    return std::nullopt;
}

/// The track's own main output row, if it has one. Returns (rowId, dst, origin).
struct MainRoute {
    std::int64_t rowId = 0;
    std::int64_t dst = 0;
    std::string origin;
    bool found = false;
};

MainRoute findMainRoute(OpContext& c, std::int64_t trackId) {
    MainRoute r;
    SQLite::Statement st(c.db,
        "SELECT id, dst_id, origin FROM routing "
        "WHERE src_kind = 'track' AND src_id = ? AND kind = 'main' "
        "ORDER BY id LIMIT 1");
    st.bind(1, trackId);
    if (st.executeStep()) {
        r.rowId = st.getColumn(0).getInt64();
        r.dst = st.getColumn(1).getInt64();
        r.origin = st.getColumn(2).getString();
        r.found = true;
    }
    return r;
}

}  // namespace

bool trackSetParentApply(OpContext& c, const Payload& p, std::string& err) {
    try {
        const auto id = p.at("id").get<std::int64_t>();

        std::optional<std::int64_t> parent;
        if (p.contains("parent") && !p.at("parent").is_null())
            parent = p.at("parent").get<std::int64_t>();

        // A track parented into itself, or into its own descendant, is a cycle
        // in the track forest. `tracks` CHECKs only `id <> parent_id`, which
        // stops the one-element case and nothing else, so the walk is here.
        if (parent) {
            std::int64_t at = *parent;
            for (int steps = 0;; ++steps) {
                if (at == id) { err = "that would put the track inside itself"; return false; }
                SQLite::Statement up(c.db, "SELECT parent_id FROM tracks WHERE id = ?");
                up.bind(1, at);
                if (!up.executeStep()) break;               // parent does not exist
                if (up.getColumn(0).isNull()) break;        // reached a root
                at = up.getColumn(0).getInt64();
                if (steps > 10000) { err = "the track forest already has a cycle"; return false; }
            }
        }

        {
            SQLite::Statement st(c.db, "UPDATE tracks SET parent_id = ? WHERE id = ?");
            if (parent) st.bind(1, *parent); else st.bind(1);
            st.bind(2, id);
            if (st.exec() == 0) { err = "no such track"; return false; }
        }

        // The routing half. A 'user' row is the user's own decision and
        // survives every regroup -- that is the whole of routing.origin.
        const MainRoute route = findMainRoute(c, id);
        if (route.found && route.origin == "auto") {
            const auto dst = defaultDestination(c, parent);
            if (dst) {
                SQLite::Statement st(c.db, "UPDATE routing SET dst_id = ? WHERE id = ?");
                st.bind(1, *dst);
                st.bind(2, route.rowId);
                st.exec();
            } else {
                // Nowhere to route: the materialised default no longer names
                // anything. Dropped rather than left pointing at a stale track.
                SQLite::Statement st(c.db, "DELETE FROM routing WHERE id = ?");
                st.bind(1, route.rowId);
                st.exec();
            }
        }
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}

bool trackSetParentInverse(OpContext& c, const Payload& p, Payload& inv, std::string& err) {
    try {
        const auto id = p.at("id").get<std::int64_t>();

        SQLite::Statement st(c.db, "SELECT parent_id FROM tracks WHERE id = ?");
        st.bind(1, id);
        if (!st.executeStep()) { err = "no such track to capture"; return false; }

        inv = Payload::object();
        inv["id"] = id;
        if (st.getColumn(0).isNull()) inv["parent"] = nullptr;
        else inv["parent"] = st.getColumn(0).getInt64();

        // The routing row is NOT captured, and that is the point of ADR-0065.
        // Re-applying setParent with the old parent recomputes the same
        // destination from the same rule, so the inverse is symmetric and the
        // row id is never invented. A captured dst would also be wrong the
        // moment the old parent was itself moved in between.
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}


// --- devices: the nine ops of OPS.md 9.7 -----------------------------------
//
// Three of them are generated scalars in kScalars above. These six are not,
// and each is hand-written for a reason worth naming:
//
//   device.insert/remove  a device carries its parameter values and its opaque
//                         state, so the pair has to move all three together or
//                         undoing a delete returns a device at its defaults.
//   device.move           two columns, and reordering a chain is what it means.
//   device.setParam       the UPSERT, and the absence problem below.
//   device.loadState      content-addressed bytes (ADR-0038).
//   device.setPreset      a capture, because the name and the state change
//                         together and only the name lives in `devices`.
//
// THE ABSENCE PROBLEM, which is the one that would have been got wrong.
// `plugin_params` holds a row only for a parameter somebody has touched. So
// the inverse of "set cutoff to 0.8" is NOT always "set cutoff to 0.5" -- when
// no row existed before, the inverse is "there was no row", and re-applying it
// as a value leaves a row that was not there. ADR-0021's oracle compares
// BYTES, so that surfaces as a replay mismatch rather than as anything a user
// would notice, weeks later, in an unrelated branch.
//
// Hence `norm: null` means absence, and the op deletes the row. That keeps the
// inverse SYMMETRIC -- the same op with swapped arguments -- which OPS.md 9.7
// requires because setParam is coalescable and validate_ops.py check 6 refuses
// a coalescable op with a state-capture inverse.
//
// None of these allocates an id for a plugin_params or plugin_state row,
// because ADR-0057 removed those surrogate keys. Both tables are keyed on
// their natural key, so every write here is an UPSERT and ADR-0021 7.3 has
// nothing to say about it.

namespace {

/// Read `plugin_params` for one device into an array the insert path can
/// replay. Used only by inverse builders, which run before apply inside the
/// caller's transaction (OPS.md 6.1).
Payload captureParams(OpContext& c, std::int64_t deviceId) {
    Payload arr = Payload::array();
    SQLite::Statement st(c.db,
        "SELECT param_id, name, normalized_value, real_value, display, unit, flags "
        "FROM plugin_params WHERE device_id = ? ORDER BY param_id");
    st.bind(1, deviceId);
    while (st.executeStep()) {
        Payload row = Payload::object();
        row["param"]   = st.getColumn(0).getString();
        row["name"]    = st.getColumn(1).getString();
        row["norm"]    = st.getColumn(2).getDouble();
        if (st.getColumn(3).isNull()) row["real"] = nullptr;
        else                          row["real"] = st.getColumn(3).getDouble();
        row["display"] = st.getColumn(4).getString();
        row["unit"]    = st.getColumn(5).getString();
        row["flags"]   = st.getColumn(6).getInt64();
        arr.push_back(std::move(row));
    }
    return arr;
}

/// Same for `plugin_state`. Only hashes travel -- the bytes stay in
/// `state_blobs`, which is the whole of ADR-0038: twenty tweaks that end where
/// they started cost one blob, and an undo log that inlined them would be the
/// largest thing in the file.
Payload captureState(OpContext& c, std::int64_t deviceId) {
    Payload arr = Payload::array();
    SQLite::Statement st(c.db,
        "SELECT stream_role, state_hash, format_hint FROM plugin_state "
        "WHERE device_id = ? ORDER BY stream_role");
    st.bind(1, deviceId);
    while (st.executeStep()) {
        Payload row = Payload::object();
        row["role"] = st.getColumn(0).getString();
        row["hash"] = st.getColumn(1).getString();
        row["hint"] = st.getColumn(2).getString();
        arr.push_back(std::move(row));
    }
    return arr;
}

std::int64_t deviceInteger(const Payload& value,const char* key,std::int64_t fallback=0) {
    if(!value.contains(key))return fallback;
    const auto& number=value.at(key);
    if(!number.is_number_integer() ||
       (number.is_number_unsigned() && number.get<std::uint64_t>()>static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())))
        throw std::runtime_error(std::string("device integer outside signed 64-bit range: ")+key);
    return number.get<std::int64_t>();
}
void replayParams(OpContext& c, std::int64_t deviceId, const Payload& arr) {
    if (!arr.is_array()) return;
    for (const auto& row : arr) {
        SQLite::Statement st(c.db,
            "INSERT INTO plugin_params(device_id, param_id, name, normalized_value, "
            "real_value, display, unit, flags) VALUES (?,?,?,?,?,?,?,?)");
        st.bind(1, deviceId);
        st.bind(2, row.at("param").get<std::string>());
        st.bind(3, row.value("name", std::string{}));
        st.bind(4, row.value("norm", 0.0));
        if (row.contains("real") && !row.at("real").is_null())
            st.bind(5, row.at("real").get<double>());
        else
            st.bind(5);
        st.bind(6, row.value("display", std::string{}));
        st.bind(7, row.value("unit", std::string{}));
        st.bind(8, deviceInteger(row,"flags"));
        st.exec();
    }
}

void replayState(OpContext& c, std::int64_t deviceId, const Payload& arr) {
    if (!arr.is_array()) return;
    for (const auto& row : arr) {
        SQLite::Statement st(c.db,
            "INSERT INTO plugin_state(device_id, stream_role, state_hash, format_hint) "
            "VALUES (?,?,?,?)");
        st.bind(1, deviceId);
        st.bind(2, row.at("role").get<std::string>());
        st.bind(3, row.at("hash").get<std::string>());
        st.bind(4, row.value("hint", std::string{}));
        st.exec();
    }
}

}  // namespace

// --- chain.create / chain.delete -------------------------------------------
//
// Not among the nine device ops, and added anyway: a device's `chain_id` is
// NOT NULL and references `device_chains`, so with foreign keys on there is no
// way to reach `device.insert` through the op log without one of these. Nine
// ops that cannot be exercised by the round-trip corpus are nine ops whose
// freedom from ambient state is unproven, which is the only property that test
// can establish.
//
// The exactly-one-owner CHECK is the schema's (a chain hangs off a rack device
// or directly off a track, never both and never neither), so this does not
// restate it -- a payload with both set fails at the INSERT, loudly.

bool chainCreateApply(OpContext& c, const Payload& p, std::string& err) {
    try {
        SQLite::Statement st(c.db,
            "INSERT INTO device_chains(id, parent_device_id, track_id, ord, name) "
            "VALUES (?,?,?,?,?)");
        st.bind(1, p.at("id").get<std::int64_t>());
        if (p.contains("device") && !p.at("device").is_null())
            st.bind(2, p.at("device").get<std::int64_t>());
        else
            st.bind(2);
        if (p.contains("track") && !p.at("track").is_null())
            st.bind(3, p.at("track").get<std::int64_t>());
        else
            st.bind(3);
        st.bind(4, static_cast<std::int64_t>(p.value("ord", std::int64_t{0})));
        st.bind(5, p.value("name", std::string{}));
        st.exec();
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}

bool chainCreateInverse(OpContext&, const Payload& p, Payload& inv, std::string& err) {
    if (!p.contains("id")) { err = "chain.create payload has no id"; return false; }
    inv = Payload::object();
    inv["id"] = p.at("id");
    return true;   // paired: chain.delete
}

bool chainDeleteApply(OpContext& c, const Payload& p, std::string& err) {
    try {
        SQLite::Statement st(c.db, "DELETE FROM device_chains WHERE id = ?");
        st.bind(1, p.at("id").get<std::int64_t>());
        if (st.exec() == 0) { err = "no such chain"; return false; }
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}

bool chainDeleteInverse(OpContext& c, const Payload& p, Payload& inv, std::string& err) {
    try {
        const auto id = p.at("id").get<std::int64_t>();
        SQLite::Statement st(c.db,
            "SELECT parent_device_id, track_id, ord, name FROM device_chains WHERE id = ?");
        st.bind(1, id);
        if (!st.executeStep()) { err = "no such chain to capture"; return false; }
        inv = Payload::object();
        inv["id"] = id;
        if (st.getColumn(0).isNull()) inv["device"] = nullptr;
        else                          inv["device"] = st.getColumn(0).getInt64();
        if (st.getColumn(1).isNull()) inv["track"] = nullptr;
        else                          inv["track"] = st.getColumn(1).getInt64();
        inv["ord"]  = st.getColumn(2).getInt64();
        inv["name"] = st.getColumn(3).getString();
        // Devices in the chain cascade away with it. They are NOT captured
        // here: deleting a chain that still holds devices is a composite the
        // UI does not offer, and a capture that silently restored them would
        // make this look like an op it is not.
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}

// ADR-0154: the panel's rows, written and captured for device.setPanel and
// for device.insert / device.remove's undo.
namespace {

void writePanel(OpContext& c, std::int64_t dev, const Payload& params) {
    SQLite::Statement del(c.db, "DELETE FROM device_panels WHERE device_id = ?");
    del.bind(1, dev);
    del.exec();   // its parameter rows cascade
    if (params.is_null()) return;
    if (!params.is_array()) throw std::runtime_error("'params' must be an array or null");
    SQLite::Statement mk(c.db, "INSERT INTO device_panels(device_id) VALUES (?)");
    mk.bind(1, dev);
    mk.exec();
    SQLite::Statement row(c.db,
        "INSERT INTO device_panel_params(device_id, ord, param_id) VALUES (?,?,?)");
    std::int64_t ord = 0;
    for (const auto& id : params) {
        row.bind(1, dev);
        row.bind(2, ord++);
        row.bind(3, id.get<std::string>());
        row.exec();
        row.reset();
    }
}

Payload capturePanel(OpContext& c, std::int64_t dev) {
    SQLite::Statement has(c.db, "SELECT 1 FROM device_panels WHERE device_id = ?");
    has.bind(1, dev);
    if (!has.executeStep()) return nullptr;
    Payload out = Payload::array();
    SQLite::Statement st(c.db,
        "SELECT param_id FROM device_panel_params WHERE device_id = ? ORDER BY ord");
    st.bind(1, dev);
    while (st.executeStep()) out.push_back(st.getColumn(0).getString());
    return out;
}

}  // namespace

// --- device.insert / device.remove -----------------------------------------

bool deviceInsertApply(OpContext& c, const Payload& p, std::string& err) {
    try {
        const auto id = deviceInteger(p,"id");
        SQLite::Statement st(c.db,
            "INSERT INTO devices(id, chain_id, ord, plugin_ref_id, name, enabled, "
            "is_rack, preset_name, latency_samples, always_process, missing, rack_kind, remote_host_id) "
            "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)");
        st.bind(1, id);
        st.bind(2, deviceInteger(p,"chain"));
        st.bind(3, deviceInteger(p,"ord"));
        if (p.contains("ref") && !p.at("ref").is_null())
            st.bind(4, deviceInteger(p,"ref"));
        else
            st.bind(4);
        st.bind(5, p.value("name", std::string{}));
        st.bind(6, p.value("enabled", true) ? 1 : 0);
        st.bind(7, p.value("rack", false) ? 1 : 0);
        st.bind(8, p.value("preset", std::string{}));
        st.bind(9, deviceInteger(p,"latency"));
        st.bind(10, p.value("always", false) ? 1 : 0);
        // ADR-0011: a device whose plugin did not load is inserted MISSING
        // rather than not inserted. The flag travels so that undoing the
        // removal of a placeholder restores a placeholder, not a device that
        // claims to have loaded.
        st.bind(11, p.value("missing", false) ? 1 : 0);
        if(p.contains("rackKind") && !p.at("rackKind").is_null())st.bind(12,p.at("rackKind").get<std::string>());
        else st.bind(12);
        if(p.contains("remoteHost") && !p.at("remoteHost").is_null())st.bind(13,deviceInteger(p,"remoteHost"));
        else st.bind(13);
        st.exec();

        if (p.contains("params")) replayParams(c, id, p.at("params"));
        if (p.contains("state"))  replayState(c, id, p.at("state"));
        // ADR-0154: the panel travels with the device too.
        if (p.contains("panel") && !p.at("panel").is_null()) writePanel(c, id, p.at("panel"));
        // ADR-0149: the route travels with the device, so undoing a removal
        // plays it the way it played, and a new device takes the registry's
        // default through here, once.
        if (p.contains("route") && !p.at("route").is_null()) {
            SQLite::Statement r(c.db,
                "INSERT INTO device_expression_routes(device_id, route) VALUES (?,?)");
            r.bind(1, id);
            r.bind(2, p.at("route").get<std::string>());
            r.exec();
        }
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}

bool deviceInsertInverse(OpContext&, const Payload& p, Payload& inv, std::string& err) {
    if (!p.contains("id")) { err = "device.insert payload has no id"; return false; }
    inv = Payload::object();
    inv["id"] = p.at("id");
    return true;   // paired: device.remove
}

bool deviceRemoveApply(OpContext& c, const Payload& p, std::string& err) {
    try {
        // plugin_params and plugin_state cascade on device_id, so this one
        // statement takes the parameter values and the state references with
        // it. The inverse captured them first, which is the order OPS.md 6.1
        // requires and the reason it requires it.
        SQLite::Statement st(c.db, "DELETE FROM devices WHERE id = ?");
        st.bind(1, p.at("id").get<std::int64_t>());
        if (st.exec() == 0) { err = "no such device"; return false; }
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}

bool deviceRemoveInverse(OpContext& c, const Payload& p, Payload& inv, std::string& err) {
    try {
        const auto id = p.at("id").get<std::int64_t>();
        SQLite::Statement st(c.db,
            "SELECT chain_id, ord, plugin_ref_id, name, enabled, is_rack, "
            "preset_name, latency_samples, always_process, missing, rack_kind, remote_host_id "
            "FROM devices WHERE id = ?");
        st.bind(1, id);
        if (!st.executeStep()) { err = "no such device to capture"; return false; }

        inv = Payload::object();
        inv["id"]    = id;
        inv["chain"] = st.getColumn(0).getInt64();
        inv["ord"]   = st.getColumn(1).getInt64();
        if (st.getColumn(2).isNull()) inv["ref"] = nullptr;
        else                          inv["ref"] = st.getColumn(2).getInt64();
        inv["name"]    = st.getColumn(3).getString();
        inv["enabled"] = st.getColumn(4).getInt() != 0;
        inv["rack"]    = st.getColumn(5).getInt() != 0;
        inv["preset"]  = st.getColumn(6).getString();
        inv["latency"] = st.getColumn(7).getInt64();
        inv["always"]  = st.getColumn(8).getInt() != 0;
        inv["missing"] = st.getColumn(9).getInt() != 0;
        inv["rackKind"]=st.getColumn(10).isNull()?Payload{}:Payload(st.getColumn(10).getString());
        inv["remoteHost"]=st.getColumn(11).isNull()?Payload{}:Payload(st.getColumn(11).getInt64());

        // The part that makes undoing a delete actually restore the device
        // rather than a device with that name. Without these the plugin comes
        // back at its factory defaults and the user's sound is gone -- which
        // looks like the undo worked.
        inv["params"] = captureParams(c, id);
        inv["state"]  = captureState(c, id);
        // The panel and the route cascade with the device like the rows above
        // (ADR-0154, ADR-0149).
        {
            const Payload panel = capturePanel(c, id);
            if (!panel.is_null()) inv["panel"] = panel;
        }
        SQLite::Statement r(c.db,
            "SELECT route FROM device_expression_routes WHERE device_id = ?");
        r.bind(1, id);
        if (r.executeStep()) inv["route"] = r.getColumn(0).getString();
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}

// --- device.move ------------------------------------------------------------

bool deviceMoveApply(OpContext& c, const Payload& p, std::string& err) {
    try {
        SQLite::Statement st(c.db,
            "UPDATE devices SET chain_id = ?, ord = ? WHERE id = ?");
        st.bind(1, p.at("chain").get<std::int64_t>());
        st.bind(2, p.at("ord").get<std::int64_t>());
        st.bind(3, p.at("id").get<std::int64_t>());
        if (st.exec() == 0) { err = "no such device"; return false; }
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}

bool deviceMoveInverse(OpContext& c, const Payload& p, Payload& inv, std::string& err) {
    try {
        const auto id = p.at("id").get<std::int64_t>();
        SQLite::Statement st(c.db, "SELECT chain_id, ord FROM devices WHERE id = ?");
        st.bind(1, id);
        if (!st.executeStep()) { err = "no such device to capture"; return false; }
        inv = Payload::object();
        inv["id"]    = id;
        inv["chain"] = st.getColumn(0).getInt64();
        inv["ord"]   = st.getColumn(1).getInt64();
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}

// --- device.setParam --------------------------------------------------------

bool deviceSetParamApply(OpContext& c, const Payload& p, std::string& err) {
    try {
        const auto dev   = p.at("dev").get<std::int64_t>();
        const auto param = p.at("param").get<std::string>();

        // Presence is required even though the validator cannot say so: a
        // payload that simply forgot `norm` must not read as "delete it".
        if (!p.contains("norm")) {
            err = "device.setParam needs a 'norm' key; use null to clear";
            return false;
        }

        // Absence. See the header comment: this is the branch that makes the
        // inverse byte-exact when the parameter had never been touched.
        if (p.at("norm").is_null()) {
            SQLite::Statement del(c.db,
                "DELETE FROM plugin_params WHERE device_id = ? AND param_id = ?");
            del.bind(1, dev);
            del.bind(2, param);
            del.exec();
            return true;
        }

        // An UPSERT on the natural key, which is what ADR-0057 bought. `name`,
        // `unit` and `flags` are DECLARATION and are not touched here: they
        // come from reading the plugin, and a knob movement is not new
        // information about what the knob is called. On a first write they
        // take their column defaults.
        SQLite::Statement st(c.db,
            "INSERT INTO plugin_params(device_id, param_id, normalized_value, "
            "real_value, display) VALUES (?,?,?,?,?) "
            "ON CONFLICT(device_id, param_id) DO UPDATE SET "
            "normalized_value = excluded.normalized_value, "
            "real_value = excluded.real_value, "
            "display = excluded.display");
        st.bind(1, dev);
        st.bind(2, param);
        st.bind(3, p.at("norm").get<double>());
        // NULL rather than 0.0 when the device cannot give a real value. VST3
        // exposes one only as a display string, so this column is genuinely
        // absent for most VST3 parameters -- and a 0.0 there would read as
        // "this parameter is at zero Hz" (ADR-0057).
        if (p.contains("real") && !p.at("real").is_null())
            st.bind(4, p.at("real").get<double>());
        else
            st.bind(4);
        st.bind(5, p.value("display", std::string{}));
        st.exec();
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}

bool deviceSetParamInverse(OpContext& c, const Payload& p, Payload& inv, std::string& err) {
    try {
        const auto dev   = p.at("dev").get<std::int64_t>();
        const auto param = p.at("param").get<std::string>();

        inv = Payload::object();
        inv["dev"]   = dev;
        inv["param"] = param;

        SQLite::Statement st(c.db,
            "SELECT normalized_value, real_value, display FROM plugin_params "
            "WHERE device_id = ? AND param_id = ?");
        st.bind(1, dev);
        st.bind(2, param);
        if (!st.executeStep()) {
            // There was no row. The inverse of creating one is removing it,
            // and `norm: null` is how this op says that.
            inv["norm"] = nullptr;
            return true;
        }
        inv["norm"] = st.getColumn(0).getDouble();
        if (st.getColumn(1).isNull()) inv["real"] = nullptr;
        else                          inv["real"] = st.getColumn(1).getDouble();
        inv["display"] = st.getColumn(2).getString();
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}

// --- device.loadState -------------------------------------------------------

bool deviceLoadStateApply(OpContext& c, const Payload& p, std::string& err) {
    try {
        const auto dev  = p.at("dev").get<std::int64_t>();
        const auto role = p.at("role").get<std::string>();

        if (!p.contains("hash")) {
            err = "device.loadState needs a 'hash' key; use null to clear";
            return false;
        }
        if (p.at("hash").is_null()) {
            SQLite::Statement del(c.db,
                "DELETE FROM plugin_state WHERE device_id = ? AND stream_role = ?");
            del.bind(1, dev);
            del.bind(2, role);
            del.exec();
            return true;
        }

        // Only the hash travels. The bytes are in `state_blobs`, put there by
        // whoever read them out of the plugin -- the same treatment media gets
        // (ADR-0032), and the reason an undo log does not grow with the size
        // of a sampler's embedded content. The foreign key refuses a hash with
        // no blob behind it, so a payload that forgot to store the bytes fails
        // here rather than producing a device whose state is gone.
        SQLite::Statement st(c.db,
            "INSERT INTO plugin_state(device_id, stream_role, state_hash, format_hint) "
            "VALUES (?,?,?,?) "
            "ON CONFLICT(device_id, stream_role) DO UPDATE SET "
            "state_hash = excluded.state_hash, format_hint = excluded.format_hint");
        st.bind(1, dev);
        st.bind(2, role);
        st.bind(3, p.at("hash").get<std::string>());
        st.bind(4, p.value("hint", std::string{}));
        st.exec();
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}

bool deviceLoadStateInverse(OpContext& c, const Payload& p, Payload& inv, std::string& err) {
    try {
        const auto dev  = p.at("dev").get<std::int64_t>();
        const auto role = p.at("role").get<std::string>();

        inv = Payload::object();
        inv["dev"]  = dev;
        inv["role"] = role;

        SQLite::Statement st(c.db,
            "SELECT state_hash, format_hint FROM plugin_state "
            "WHERE device_id = ? AND stream_role = ?");
        st.bind(1, dev);
        st.bind(2, role);
        if (!st.executeStep()) { inv["hash"] = nullptr; return true; }
        inv["hash"] = st.getColumn(0).getString();
        inv["hint"] = st.getColumn(1).getString();
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}

// --- device.setPanel --------------------------------------------------------
//
// ADR-0154: the whole panel list at once. Adding, removing and reordering are
// all "the list is now this", which keeps the op symmetric and one gesture
// one op. Null returns the device to Live's default panel.

bool deviceSetPanelApply(OpContext& c, const Payload& p, std::string& err) {
    try {
        if (!p.contains("params")) {
            err = "device.setPanel needs a 'params' key; use null for Live's default panel";
            return false;
        }
        writePanel(c, p.at("dev").get<std::int64_t>(), p.at("params"));
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}

bool deviceSetPanelInverse(OpContext& c, const Payload& p, Payload& inv, std::string& err) {
    try {
        const auto dev = p.at("dev").get<std::int64_t>();
        inv = Payload::object();
        inv["dev"] = dev;
        inv["params"] = capturePanel(c, dev);
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}

// --- device.setExpressionRoute --------------------------------------------
//
// ADR-0146, ADR-0149: the route a device plays with. A string sets it, null
// clears it back to Auto. Symmetric: the inverse is the row as it was.

bool deviceSetRouteApply(OpContext& c, const Payload& p, std::string& err) {
    try {
        const auto dev = p.at("dev").get<std::int64_t>();
        if (!p.contains("route")) {
            err = "device.setExpressionRoute needs a 'route' key; use null for Auto";
            return false;
        }
        if (p.at("route").is_null()) {
            SQLite::Statement del(c.db, "DELETE FROM device_expression_routes WHERE device_id = ?");
            del.bind(1, dev);
            del.exec();
            return true;
        }
        SQLite::Statement st(c.db,
            "INSERT INTO device_expression_routes(device_id, route) VALUES (?,?) "
            "ON CONFLICT(device_id) DO UPDATE SET route = excluded.route");
        st.bind(1, dev);
        st.bind(2, p.at("route").get<std::string>());
        st.exec();
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}

bool deviceSetRouteInverse(OpContext& c, const Payload& p, Payload& inv, std::string& err) {
    try {
        const auto dev = p.at("dev").get<std::int64_t>();
        inv = Payload::object();
        inv["dev"] = dev;
        SQLite::Statement st(c.db, "SELECT route FROM device_expression_routes WHERE device_id = ?");
        st.bind(1, dev);
        if (st.executeStep()) inv["route"] = st.getColumn(0).getString();
        else                  inv["route"] = nullptr;
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}

// --- device.setPreset -------------------------------------------------------
//
// Only the NAME. A preset change is a name and a pile of opaque bytes, and the
// bytes are `device.loadState`'s job in the same transaction -- separating
// them is what lets twenty preset auditions share one blob each rather than
// duplicating them through the undo log.

bool deviceSetPresetApply(OpContext& c, const Payload& p, std::string& err) {
    try {
        SQLite::Statement st(c.db, "UPDATE devices SET preset_name = ? WHERE id = ?");
        st.bind(1, p.at("preset").get<std::string>());
        st.bind(2, p.at("dev").get<std::int64_t>());
        if (st.exec() == 0) { err = "no such device"; return false; }
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}

bool deviceSetPresetInverse(OpContext& c, const Payload& p, Payload& inv, std::string& err) {
    try {
        const auto dev = p.at("dev").get<std::int64_t>();
        SQLite::Statement st(c.db, "SELECT preset_name FROM devices WHERE id = ?");
        st.bind(1, dev);
        if (!st.executeStep()) { err = "no such device to capture"; return false; }
        inv = Payload::object();
        inv["dev"]    = dev;
        inv["preset"] = st.getColumn(0).getString();
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}

// ADR-0143: path-taking media APIs capture these replayable payloads.
constexpr Field kFFreeze[] = {
    {"id", FieldType::Int}, {"before", FieldType::Object}, {"after", FieldType::Object}};
constexpr Field kFMediaRow[] = {{"id", FieldType::Int}, {"row", FieldType::Object}};
constexpr Field kFMediaRelink[] = {{"id", FieldType::Int}, {"hash", FieldType::Text}, {"paths", FieldType::Object}};
constexpr Field kFControllerBind[] = {{"id", FieldType::Int}, {"binding", FieldType::Object, false}};
const OpDescriptor kHandWritten[] = {
    {"controller.bind", "Learn or replace a project controller binding", Scope::Edit, EngineImpact::None,
     kFControllerBind, false, false, controllerBindApply, controllerBindInverse, ""},
    {"controller.unbind", "Remove a project controller binding", Scope::Edit, EngineImpact::None, kFId, false,
     false, controllerUnbindApply, controllerUnbindInverse, "controller.bind"},
    {"media.import", "Import captured media metadata", Scope::Edit, EngineImpact::None, kFMediaRow, false,
     false, media::importApply, media::importInverse, "media.unlink"},
    {"media.unlink", "Unlink unreferenced media without deleting files", Scope::Edit, EngineImpact::None,
     kFId, false, false, media::unlinkApply, media::unlinkInverse, "media.import"},
    {"media.relink", "Relink the same media content", Scope::Edit, EngineImpact::Snapshot, kFMediaRelink,
     false, false, media::relinkApply, media::relinkInverse, ""},
    {"project.setName", "Rename the project", Scope::Edit, EngineImpact::None, kFProjectName, false, false,
     setProjectNameApply, setProjectNameInverse, ""},

    // ADR-0174: native analog summing on a group. Turning it on or choosing a
    // flavour adds or replaces nodes; the drive is a gain on nodes that exist.
    {"group.setSumming", "Turn a group's analog summing on or off", Scope::Edit, EngineImpact::GraphRebuild,
     kFSummingEnabled, false, false, summingEnabledApply, summingEnabledInverse, ""},
    {"group.setSummingFlavor", "Choose a group's summing console", Scope::Edit, EngineImpact::GraphRebuild,
     kFSummingFlavor, false, false, summingFlavorApply, summingFlavorInverse, ""},
    {"group.setSummingDrive", "Set how hard a group's summing is driven", Scope::Edit, EngineImpact::Snapshot,
     kFSummingDrive, true, false, summingDriveApply, summingDriveInverse, ""},

    {"group.setFreezeScope", "Set captured whole-group freeze scope", Scope::Edit, EngineImpact::GraphRebuild,
     kFFreeze, false, false, freeze::groupApply, freeze::groupInverse, ""},
    {"track.setFreeze", "Set captured track freeze state", Scope::Edit, EngineImpact::GraphRebuild, kFFreeze,
     false, false, freeze::apply, freeze::inverse, ""},
    {"track.create", "Create a track", Scope::Edit, EngineImpact::GraphRebuild,
     kFTrackCreate, false, false, trackCreateApply, trackCreateInverse, "track.delete"},
    {"track.setRole", "Set an anchor role", Scope::Edit, EngineImpact::GraphRebuild,
     kFTrackRole, false, false, trackRoleApply, trackRoleInverse, ""},
    {"track.delete", "Delete a track", Scope::Edit, EngineImpact::GraphRebuild,
     kFId, false, false, trackDeleteApply, trackDeleteInverse, "track.create"},
    {"track.setParent", "Move a track into or out of a group", Scope::Edit,
     EngineImpact::GraphRebuild, kFSetParent, false, false,
     trackSetParentApply, trackSetParentInverse, ""},

    {"clip.create", "Create a clip", Scope::Edit, EngineImpact::Snapshot, kFClipCreate, false, false,
     clipCreateApply, clipCreateInverse, "clip.delete"},
    {"clip.delete", "Delete a clip", Scope::Edit, EngineImpact::Snapshot, kFId, false, false, clipDeleteApply,
     clipDeleteInverse, "clip.create"},
    {"clip.attachAudio", "Attach audio to a clip", Scope::Edit, EngineImpact::GraphRebuild, kFAudioAttach,
     false, false, audioAttachApply, clipCreateInverse, "clip.detachAudio"},
    {"clip.detachAudio", "Detach unedited audio", Scope::Edit, EngineImpact::GraphRebuild, kFId, false, false,
     audioDetachApply, audioDetachInverse, "clip.attachAudio"},
    {"clip.move", "Move a clip in time, and optionally to another track", Scope::Edit, EngineImpact::Snapshot,
     kFClipMove, false, false, clipMoveApply, clipMoveInverse, ""},
    {"clip.resize", "Change a clip's start and length", Scope::Edit, EngineImpact::Snapshot, kFClipResize,
     false, false, clipResizeApply, clipResizeInverse, ""},

    {"note.insert", "Add a note to a clip", Scope::Edit, EngineImpact::Snapshot, kFNoteFull, false, false,
     notes::noteInsertApply, notes::noteInsertInverse, "note.delete"},
    {"note.delete", "Remove a note from a clip", Scope::Edit, EngineImpact::Snapshot, kFNoteRef, false, false,
     notes::noteDeleteApply, notes::noteDeleteInverse, "note.insert"},
    {"note.move", "Move a note in time or pitch", Scope::Edit, EngineImpact::Snapshot, kFNoteMove, false,
     false, notes::noteMoveApply, notes::noteMoveInverse, ""},
    {"note.resize", "Change a note's length", Scope::Edit, EngineImpact::Snapshot, kFNoteResize, false, false,
     notes::noteResizeApply, notes::noteResizeInverse, ""},
    {"note.setVelocity", "Set a note's velocity", Scope::Edit, EngineImpact::Snapshot, kFNoteVel, true, false,
     notes::noteVelocityApply, notes::noteVelocityInverse, ""},
    {"note.setMute", "Activate or deactivate a MIDI note", Scope::Edit, EngineImpact::Snapshot,
     kFNoteMute, false, false, note_flags::mute, note_flags::muteInverse, ""},

    {"project.insertTempoEvent", "Add a tempo change", Scope::Edit, EngineImpact::Snapshot, kFTempoIns, false,
     false, tempoInsertApply, tempoInsertInverse, "project.removeTempoEvent"},
    {"project.removeTempoEvent", "Remove a tempo change", Scope::Edit, EngineImpact::Snapshot, kFPos, false,
     false, tempoRemoveApply, tempoRemoveInverse, "project.insertTempoEvent"},
    {"project.insertTimeSignature", "Add a time signature change", Scope::Edit, EngineImpact::Snapshot,
     kFSigIns, false, false, sigInsertApply, sigInsertInverse, "project.removeTimeSignature"},
    {"project.removeTimeSignature", "Remove a time signature change", Scope::Edit, EngineImpact::Snapshot,
     kFPos, false, false, sigRemoveApply, sigRemoveInverse, "project.insertTimeSignature"},

    {"chain.create", "Create a device chain", Scope::Edit, EngineImpact::GraphRebuild, kFChainCreate, false,
     false, chainCreateApply, chainCreateInverse, "chain.delete"},
    {"chain.delete", "Delete a device chain", Scope::Edit, EngineImpact::GraphRebuild, kFId, false, false,
     chainDeleteApply, chainDeleteInverse, "chain.create"},

    {"device.insert", "Add a device to a chain", Scope::Edit, EngineImpact::GraphRebuild, kFDeviceInsert,
     false, false, deviceInsertApply, deviceInsertInverse, "device.remove"},
    {"device.remove", "Remove a device from a chain", Scope::Edit, EngineImpact::GraphRebuild, kFId, false,
     false, deviceRemoveApply, deviceRemoveInverse, "device.insert"},
    {"device.move", "Move a device within or between chains", Scope::Edit, EngineImpact::GraphRebuild,
     kFDeviceMove, false, false, deviceMoveApply, deviceMoveInverse, ""},
    // Coalescable: one knob gesture is a stream of these and undo should step
    // over the gesture, not through it (OPS.md 9.7, SPEC 7.3).
    {"device.setParam", "Set a device parameter", Scope::Edit, EngineImpact::Snapshot, kFDeviceParam, true,
     false, deviceSetParamApply, deviceSetParamInverse, ""},
    {"device.loadState", "Load opaque device state", Scope::Edit, EngineImpact::Snapshot, kFDeviceState,
     false, false, deviceLoadStateApply, deviceLoadStateInverse, ""},
    // ADR-0146, ADR-0149. Snapshot: the route is read by the device, not the
    // graph's shape.
    // ADR-0154. No engine impact: the panel is what the UI shows.
    {"device.setPanel", "Choose the parameters a plug-in's panel shows", Scope::Edit, EngineImpact::None,
     kFDevicePanel, false, false, deviceSetPanelApply, deviceSetPanelInverse, ""},
    {"device.setExpressionRoute", "Choose how per-note expression reaches a device", Scope::Edit,
     EngineImpact::Snapshot, kFDeviceRoute, false, false, deviceSetRouteApply, deviceSetRouteInverse, ""},
    {"device.setPreset", "Set a device's preset name", Scope::Edit, EngineImpact::Snapshot, kFDevicePreset,
     false, false, deviceSetPresetApply, deviceSetPresetInverse, ""},

    {"routing.connect", "Connect two points in the signal path", Scope::Edit, EngineImpact::GraphRebuild,
     kFRouting, false, false, routingConnectApply, routingConnectInverse, "routing.disconnect"},
    {"routing.disconnect", "Remove a connection", Scope::Edit, EngineImpact::GraphRebuild, kFId, false, false,
     routingDisconnectApply, routingDisconnectInverse, "routing.connect"},

    // ADR-0131. No engine impact: a remark is read by people and the agent,
    // never by the audio graph.
    {"remark.add", "Anchor a remark to a track, clip, device or parameter", Scope::Edit, EngineImpact::None,
     kFRemarkAdd, false, false, remarkAddApply, remarkAddInverse, "remark.remove"},
    {"remark.remove", "Remove a remark", Scope::Edit, EngineImpact::None, kFId, false, false,
     remarkRemoveApply, remarkRemoveInverse, "remark.add"},
    {"remark.edit", "Change a remark's text", Scope::Edit, EngineImpact::None, kFRemarkEdit, false, false,
     remarkEditApply, remarkEditInverse, ""},
    {"remark.resolve", "Mark a remark resolved, or reopen it", Scope::Edit, EngineImpact::None,
     kFRemarkResolve, false, false, remarkResolveApply, remarkResolveInverse, ""},

    {"marker.create","Create a locator or cycle marker",Scope::Edit,EngineImpact::Snapshot,kFMarkerCreate,false,false,marker::create,marker::createInverse,"marker.delete"},
    {"marker.delete","Delete a marker and its text",Scope::Edit,EngineImpact::Snapshot,kFMarkerId,false,false,marker::erase,marker::eraseInverse,"marker.create"},
    {"marker.move","Move a marker in its own time base",Scope::Edit,EngineImpact::Snapshot,kFMarkerMove,true,false,marker::move,marker::moveInverse,""},
    {"marker.rename","Rename a marker",Scope::Edit,EngineImpact::Snapshot,kFMarkerName,false,false,marker::rename,marker::renameInverse,""},
    {"marker.setColor","Set or clear a marker palette colour",Scope::Edit,EngineImpact::Snapshot,kFMarkerColor,false,false,marker::color,marker::rowInverse,""},
    {"marker.resize","Resize a cycle marker",Scope::Edit,EngineImpact::Snapshot,kFMarkerLength,false,false,marker::resize,marker::rowInverse,""},
    {"marker.setNote","Set a marker's note",Scope::Edit,EngineImpact::Snapshot,kFMarkerText,false,false,marker::note,marker::detailsInverse,""},
    {"marker.setPrompt","Set a marker's literal prompt",Scope::Edit,EngineImpact::Snapshot,kFMarkerText,false,false,marker::prompt,marker::detailsInverse,""},

    {"cue.setMode", "Use solo buttons as Cue", Scope::Transport, EngineImpact::GraphRebuild,
     kFTransportFlag,false,true,cueModeApply,nullptr,""},
    {"cue.setOutput", "Select the stereo Cue Out pair", Scope::Transport, EngineImpact::Snapshot,
     kFCueFirst,false,true,cueOutputApply,nullptr,""},
    {"cue.setVolume", "Set Cue Volume", Scope::Transport, EngineImpact::Snapshot,
     kFCueDb,false,true,cueVolumeApply,nullptr,""},
    {"cue.setTrack", "Cue a track without changing its solo", Scope::Transport, EngineImpact::GraphRebuild,
     kFCueTrack,false,true,cueTrackApply,nullptr,""},
    {"cue.setReference", "Select and audition the exclusive reference", Scope::Transport, EngineImpact::GraphRebuild,
     kFCueReference,false,true,cueReferenceApply,nullptr,""},
    {"mixer.setSoloInPlace", "Keep a soloed track's returns audible", Scope::Transport, EngineImpact::GraphRebuild,
     kFTransportFlag,false,true,soloInPlaceApply,nullptr,""},

    {"clip.setStartMarker","Set the clip source Start marker",Scope::Edit,EngineImpact::Snapshot,kFClipMarker,false,false,clip_properties::start,clip_properties::inverse,""},
    {"clip.setEndMarker","Set the clip source End marker",Scope::Edit,EngineImpact::Snapshot,kFClipMarker,false,false,clip_properties::end,clip_properties::inverse,""},
    {"clip.setLoop","Set the clip loop switch and window",Scope::Edit,EngineImpact::Snapshot,kFClipLoop,false,false,clip_properties::loop,clip_properties::inverse,""},
    {"clip.setTimeSignature","Set or inherit the clip signature",Scope::Edit,EngineImpact::Snapshot,kFClipSignature,false,false,clip_properties::signature,clip_properties::inverse,""},
    {"clip.setScale","Set or inherit the clip scale",Scope::Edit,EngineImpact::Snapshot,kFClipScale,false,false,clip_properties::scale,clip_properties::inverse,""},

    // Ephemeral: performance, not editing. No inverse, and undo skips them.
    {"transport.play", "Start playback", Scope::Transport, EngineImpact::None,
     {}, false, true, transportNoop, nullptr, ""},
    {"transport.stop", "Stop playback", Scope::Transport, EngineImpact::None,
     {}, false, true, transportNoop, nullptr, ""},
    {"transport.seek", "Move the playhead", Scope::Transport, EngineImpact::None,
     kFTransportSeek, false, true, transportNoop, nullptr, ""},
    {"transport.setLoop", "Set the loop region", Scope::Transport, EngineImpact::Snapshot,
     kFTransportLoop, false, true, transportLoop, nullptr, ""},
    {"transport.setCountIn", "Set the recording count-in", Scope::Transport, EngineImpact::Snapshot,
     kFTransportCountIn, false, true, transportCountIn, nullptr, ""},
    {"transport.setSongStart", "Set or clear the song start", Scope::Transport, EngineImpact::Snapshot,
     kFTransportSongStart, false, true, transportSongStart, nullptr, ""},
    {"transport.setRecord", "Arm or disarm the transport", Scope::Transport,
     EngineImpact::GraphRebuild, kFTransportFlag, false, true,
     transportNoop, nullptr, ""},
    {"transport.setMetronome", "Toggle the metronome", Scope::Transport,
     EngineImpact::Snapshot, kFTransportFlag, false, true,
     transportMetronome, nullptr, ""},
};

}  // namespace

std::span<const OpDescriptor> builtinOps() {
    static const std::vector<OpDescriptor> all = [] {
        std::vector<OpDescriptor> v(std::begin(kHandWritten), std::end(kHandWritten));
        const auto noteOps = notes::transformOps();
        v.insert(v.end(), noteOps.begin(), noteOps.end());
        const auto noteDiffOps = notes::diffOps();
        v.insert(v.end(), noteDiffOps.begin(), noteDiffOps.end());
        const auto controllerOps = controllers::laneOps();
        v.insert(v.end(), controllerOps.begin(), controllerOps.end());
        const auto captureOps = capture::midiOps();
        v.insert(v.end(), captureOps.begin(), captureOps.end());
        appendScalars(v, std::make_index_sequence<kScalarCount>{});
        const auto split=splitPanOps();v.insert(v.end(),split.begin(),split.end());
        const auto editing=clipEditOps();v.insert(v.end(),editing.begin(),editing.end());
        const auto builtins=builtinReferenceOps();v.insert(v.end(),builtins.begin(),builtins.end());
        const auto recording=trackRecordingOps();
        v.insert(v.end(),recording.begin(),recording.end());
        const auto taps=returnSendTapOps();v.insert(v.end(),taps.begin(),taps.end());
        const auto automation = automation::descriptors();
        v.insert(v.end(), automation.begin(), automation.end());
        const auto lanes = automation::laneDescriptors();
        v.insert(v.end(), lanes.begin(), lanes.end());
        const auto tuningOps=tunings::ops();
        v.insert(v.end(),tuningOps.begin(),tuningOps.end());
        return v;
    }();
    return all;
}

}  // namespace adi
