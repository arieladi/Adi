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

#include "adi/blob.hpp"
#include "adi/ops.hpp"
#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <string>
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

bool trackCreateApply(OpContext& c, const Payload& p, std::string& err) {
    try {
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
        SQLite::Statement st(c.db, "DELETE FROM tracks WHERE id = ?");
        st.bind(1, p.at("id").get<std::int64_t>());
        if (st.exec() == 0) { err = "no such track"; return false; }
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}
bool trackDeleteInverse(OpContext& c, const Payload& p, Payload& inv, std::string& err) {
    // State capture (OPS.md §6.2/§6.3). Media is never captured -- deleting a
    // track unlinks media_files rows, it does not delete audio -- so the inverse
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
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}

bool clipCreateApply(OpContext& c, const Payload& p, std::string& err) {
    try {
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
        SQLite::Statement st(c.db, "DELETE FROM clips WHERE id = ?");
        st.bind(1, p.at("id").get<std::int64_t>());
        if (st.exec() == 0) { err = "no such clip"; return false; }
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}
bool clipDeleteInverse(OpContext& c, const Payload& p, Payload& inv, std::string& err) {
    try {
        SQLite::Statement st(c.db,
            "SELECT track_id, kind, name, pos_ticks, length_ticks FROM clips WHERE id = ?");
        st.bind(1, p.at("id").get<std::int64_t>());
        if (!st.executeStep()) { err = "no such clip to capture"; return false; }
        inv = Payload::object();
        inv["id"] = p.at("id");
        inv["track"] = st.getColumn(0).getInt64();
        inv["kind"] = st.getColumn(1).getString();
        inv["name"] = st.getColumn(2).getString();
        inv["pos"] = st.getColumn(3).getInt64();
        inv["length"] = st.getColumn(4).getInt64();
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
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

// --- notes ------------------------------------------------------------------
//
// The first ops that touch a BLOB. ADR-0009 puts one notes blob per clip, so a
// note edit is read-modify-write of that clip's blob and nothing else -- which
// is exactly the granularity bound, exercised for the first time through the op
// system rather than only in a unit test.

std::vector<NoteRecord> readNotes(OpContext& c, std::int64_t clipId) {
    const auto blob = c.store.getEventStream(clipId, "notes");
    if (!blob) return {};
    StreamReader<NoteRecord> r(*blob, FourCC::Notes);
    if (!r.ok()) return {};
    const auto all = r.all();
    return all ? *all : std::vector<NoteRecord>{};
}

bool writeNotes(OpContext& c, std::int64_t clipId, std::vector<NoteRecord> notes) {
    // Kept sorted, because the stream header advertises SortedByTime and a
    // reader is entitled to believe it.
    std::sort(notes.begin(), notes.end(), [](const NoteRecord& a, const NoteRecord& b) {
        if (a.start_ticks != b.start_ticks) return a.start_ticks < b.start_ticks;
        if (a.key != b.key) return a.key < b.key;
        return a.note_id < b.note_id;
    });
    return c.store.putEventStream(clipId, "notes",
                                  writeStream<NoteRecord>(FourCC::Notes, notes));
}

NoteRecord noteFromPayload(const Payload& p) {
    NoteRecord n{};
    n.note_id = p.at("note").get<std::uint64_t>();
    n.start_ticks = p.at("start").get<std::int64_t>();
    n.dur_ticks = p.at("dur").get<std::int64_t>();
    n.key = static_cast<std::uint8_t>(p.at("key").get<int>());
    n.vel_on = static_cast<std::uint8_t>(p.value("vel", 100));
    n.vel_off = static_cast<std::uint8_t>(p.value("velOff", 64));
    n.channel = static_cast<std::uint8_t>(p.value("chan", 0));
    n.probability = static_cast<std::uint16_t>(p.value("prob", 10000));
    n.tuning_cents = p.value("cents", 0.0f);
    return n;
}

Payload noteToPayload(std::int64_t clipId, const NoteRecord& n) {
    Payload p = Payload::object();
    p["clip"] = clipId;
    p["note"] = n.note_id;
    p["start"] = n.start_ticks;
    p["dur"] = n.dur_ticks;
    p["key"] = static_cast<int>(n.key);
    p["vel"] = static_cast<int>(n.vel_on);
    p["velOff"] = static_cast<int>(n.vel_off);
    p["chan"] = static_cast<int>(n.channel);
    p["prob"] = static_cast<int>(n.probability);
    p["cents"] = n.tuning_cents;
    return p;
}

bool noteInsertApply(OpContext& c, const Payload& p, std::string& err) {
    const auto clip = p.at("clip").get<std::int64_t>();
    auto notes = readNotes(c, clip);
    const auto id = p.at("note").get<std::uint64_t>();
    if (std::any_of(notes.begin(), notes.end(),
                    [&](const NoteRecord& n) { return n.note_id == id; })) {
        err = "note id already exists in this clip";   // never silently reassign
        return false;
    }
    notes.push_back(noteFromPayload(p));
    if (!writeNotes(c, clip, std::move(notes))) { err = c.store.lastError(); return false; }
    return true;
}
bool noteInsertInverse(OpContext&, const Payload& p, Payload& inv, std::string& err) {
    if (!p.contains("clip") || !p.contains("note")) { err = "note.insert needs clip and note"; return false; }
    inv = Payload::object();
    inv["clip"] = p.at("clip");
    inv["note"] = p.at("note");
    return true;   // paired: note.delete
}

bool noteDeleteApply(OpContext& c, const Payload& p, std::string& err) {
    const auto clip = p.at("clip").get<std::int64_t>();
    const auto id = p.at("note").get<std::uint64_t>();
    auto notes = readNotes(c, clip);
    const auto before = notes.size();
    notes.erase(std::remove_if(notes.begin(), notes.end(),
                               [&](const NoteRecord& n) { return n.note_id == id; }),
                notes.end());
    if (notes.size() == before) { err = "no such note"; return false; }
    if (!writeNotes(c, clip, std::move(notes))) { err = c.store.lastError(); return false; }
    return true;
}
bool noteDeleteInverse(OpContext& c, const Payload& p, Payload& inv, std::string& err) {
    const auto clip = p.at("clip").get<std::int64_t>();
    const auto id = p.at("note").get<std::uint64_t>();
    for (const auto& n : readNotes(c, clip))
        if (n.note_id == id) { inv = noteToPayload(clip, n); return true; }
    err = "no such note to capture";
    return false;
}

/// One helper behind move, resize and setVelocity: find the note, mutate it,
/// write back. Their inverses are all "the note as it was", which is the same
/// capture in every case.
template <typename Mutate>
bool noteEdit(OpContext& c, const Payload& p, std::string& err, Mutate m) {
    const auto clip = p.at("clip").get<std::int64_t>();
    const auto id = p.at("note").get<std::uint64_t>();
    auto notes = readNotes(c, clip);
    auto it = std::find_if(notes.begin(), notes.end(),
                           [&](const NoteRecord& n) { return n.note_id == id; });
    if (it == notes.end()) { err = "no such note"; return false; }
    m(*it);
    if (!writeNotes(c, clip, std::move(notes))) { err = c.store.lastError(); return false; }
    return true;
}

bool noteMoveApply(OpContext& c, const Payload& p, std::string& err) {
    return noteEdit(c, p, err, [&](NoteRecord& n) {
        n.start_ticks = p.at("start").get<std::int64_t>();
        n.key = static_cast<std::uint8_t>(p.at("key").get<int>());
    });
}
bool noteMoveInverse(OpContext& c, const Payload& p, Payload& inv, std::string& err) {
    const auto clip = p.at("clip").get<std::int64_t>();
    const auto id = p.at("note").get<std::uint64_t>();
    for (const auto& n : readNotes(c, clip))
        if (n.note_id == id) {
            inv = Payload::object();
            inv["clip"] = clip;
            inv["note"] = p.at("note");
            inv["start"] = n.start_ticks;
            inv["key"] = static_cast<int>(n.key);
            return true;
        }
    err = "no such note";
    return false;
}

bool noteResizeApply(OpContext& c, const Payload& p, std::string& err) {
    const auto dur = p.at("dur").get<std::int64_t>();
    if (dur <= 0) { err = "a note's duration must be positive (SPEC 6.3.1)"; return false; }
    return noteEdit(c, p, err, [&](NoteRecord& n) { n.dur_ticks = dur; });
}
bool noteResizeInverse(OpContext& c, const Payload& p, Payload& inv, std::string& err) {
    const auto clip = p.at("clip").get<std::int64_t>();
    const auto id = p.at("note").get<std::uint64_t>();
    for (const auto& n : readNotes(c, clip))
        if (n.note_id == id) {
            inv = Payload::object();
            inv["clip"] = clip;
            inv["note"] = p.at("note");
            inv["dur"] = n.dur_ticks;
            return true;
        }
    err = "no such note";
    return false;
}

bool noteSetVelocityApply(OpContext& c, const Payload& p, std::string& err) {
    const auto v = p.at("vel").get<int>();
    if (v < 1 || v > 127) { err = "velocity must be 1..127 (SPEC 6.3.1)"; return false; }
    return noteEdit(c, p, err, [&](NoteRecord& n) {
        n.vel_on = static_cast<std::uint8_t>(v);
    });
}
bool noteSetVelocityInverse(OpContext& c, const Payload& p, Payload& inv, std::string& err) {
    const auto clip = p.at("clip").get<std::int64_t>();
    const auto id = p.at("note").get<std::uint64_t>();
    for (const auto& n : readNotes(c, clip))
        if (n.note_id == id) {
            inv = Payload::object();
            inv["clip"] = clip;
            inv["note"] = p.at("note");
            inv["vel"] = static_cast<int>(n.vel_on);
            return true;
        }
    err = "no such note";
    return false;
}

// --- tempo and signature ------------------------------------------------------

bool tempoInsertApply(OpContext& c, const Payload& p, std::string& err) {
    try {
        SQLite::Statement st(c.db,
            "INSERT INTO tempo_map(pos_ticks, bpm, curve) VALUES (?,?,?)");
        st.bind(1, p.at("pos").get<std::int64_t>());
        st.bind(2, p.at("bpm").get<double>());
        st.bind(3, p.value("curve", std::int64_t{0}));
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
        SQLite::Statement st(c.db, "SELECT bpm, curve FROM tempo_map WHERE pos_ticks = ?");
        st.bind(1, p.at("pos").get<std::int64_t>());
        if (!st.executeStep()) { err = "no tempo event to capture"; return false; }
        inv = Payload::object();
        inv["pos"] = p.at("pos");
        inv["bpm"] = st.getColumn(0).getDouble();
        inv["curve"] = st.getColumn(1).getInt64();
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
        SQLite::Statement st(c.db,
            "INSERT INTO routing(id, src_kind, src_id, dst_kind, dst_id, kind) "
            "VALUES (?,?,?,?,?,?)");
        st.bind(1, p.at("id").get<std::int64_t>());
        st.bind(2, p.at("srcKind").get<std::string>());
        st.bind(3, p.at("src").get<std::int64_t>());
        st.bind(4, p.at("dstKind").get<std::string>());
        st.bind(5, p.at("dst").get<std::int64_t>());
        st.bind(6, p.at("kind").get<std::string>());
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
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}

// --- transport: the first real ephemeral ops ----------------------------------
//
// They persist nothing (ADR-0027), so there is nothing to apply until an engine
// exists and nothing to undo ever. They still go through the registry -- same
// validation, same scopes, same attribution -- and still get a log row tagged
// `ephemeral`, so "what did the agent do at 14:32" stays answerable while Ctrl-Z
// lands on the last real edit.

bool transportNoop(OpContext&, const Payload&, std::string&) { return true; }

// --- field tables ---------------------------------------------------------------

constexpr Field kFProjectName[] = {{"name", FieldType::Text, true}};
constexpr Field kFTrackCreate[] = {{"id", FieldType::Int, true},
                                   {"kind", FieldType::Text, true},
                                   {"name", FieldType::Text, false},
                                   {"parent", FieldType::Int, false},
                                   {"index", FieldType::Int, false}};
constexpr Field kFId[] = {{"id", FieldType::Int, true}};
constexpr Field kFClipCreate[] = {{"id", FieldType::Int, true},
                                  {"track", FieldType::Int, true},
                                  {"kind", FieldType::Text, true},
                                  {"name", FieldType::Text, false},
                                  {"pos", FieldType::Int, true},
                                  {"length", FieldType::Int, true}};
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
constexpr Field kFTempoIns[] = {{"pos", FieldType::Int, true},
                                {"bpm", FieldType::Real, true},
                                {"curve", FieldType::Int, false}};
constexpr Field kFPos[] = {{"pos", FieldType::Int, true}};
constexpr Field kFSigIns[] = {{"pos", FieldType::Int, true},
                              {"num", FieldType::Int, true},
                              {"den", FieldType::Int, true}};
constexpr Field kFRouting[] = {{"id", FieldType::Int, true},
                               {"srcKind", FieldType::Text, true},
                               {"src", FieldType::Int, true},
                               {"dstKind", FieldType::Text, true},
                               {"dst", FieldType::Int, true},
                               {"kind", FieldType::Text, true}};
constexpr Field kFSetParent[] = {{"id", FieldType::Int, true},
                                 {"parent", FieldType::Int, false}};
constexpr Field kFTransportSeek[] = {{"pos", FieldType::Int, true}};
constexpr Field kFTransportFlag[] = {{"on", FieldType::Bool, true}};
constexpr Field kFTransportLoop[] = {{"on", FieldType::Bool, true},
                                     {"start", FieldType::Int, false},
                                     {"end", FieldType::Int, false}};

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

const OpDescriptor kHandWritten[] = {
    {"project.setName", "Rename the project", Scope::Edit, EngineImpact::None,
     kFProjectName, false, false, setProjectNameApply, setProjectNameInverse, ""},

    {"track.create", "Create a track", Scope::Edit, EngineImpact::GraphRebuild,
     kFTrackCreate, false, false, trackCreateApply, trackCreateInverse, "track.delete"},
    {"track.delete", "Delete a track", Scope::Edit, EngineImpact::GraphRebuild,
     kFId, false, false, trackDeleteApply, trackDeleteInverse, "track.create"},
    {"track.setParent", "Move a track into or out of a group", Scope::Edit,
     EngineImpact::GraphRebuild, kFSetParent, false, false,
     trackSetParentApply, trackSetParentInverse, ""},

    {"clip.create", "Create a clip", Scope::Edit, EngineImpact::Snapshot,
     kFClipCreate, false, false, clipCreateApply, clipCreateInverse, "clip.delete"},
    {"clip.delete", "Delete a clip", Scope::Edit, EngineImpact::Snapshot,
     kFId, false, false, clipDeleteApply, clipDeleteInverse, "clip.create"},
    {"clip.move", "Move a clip in time, and optionally to another track",
     Scope::Edit, EngineImpact::Snapshot, kFClipMove, false, false,
     clipMoveApply, clipMoveInverse, ""},
    {"clip.resize", "Change a clip's start and length", Scope::Edit,
     EngineImpact::Snapshot, kFClipResize, false, false,
     clipResizeApply, clipResizeInverse, ""},

    {"note.insert", "Add a note to a clip", Scope::Edit, EngineImpact::Snapshot,
     kFNoteFull, false, false, noteInsertApply, noteInsertInverse, "note.delete"},
    {"note.delete", "Remove a note from a clip", Scope::Edit, EngineImpact::Snapshot,
     kFNoteRef, false, false, noteDeleteApply, noteDeleteInverse, "note.insert"},
    {"note.move", "Move a note in time or pitch", Scope::Edit, EngineImpact::Snapshot,
     kFNoteMove, false, false, noteMoveApply, noteMoveInverse, ""},
    {"note.resize", "Change a note's length", Scope::Edit, EngineImpact::Snapshot,
     kFNoteResize, false, false, noteResizeApply, noteResizeInverse, ""},
    {"note.setVelocity", "Set a note's velocity", Scope::Edit, EngineImpact::Snapshot,
     kFNoteVel, true, false, noteSetVelocityApply, noteSetVelocityInverse, ""},

    {"project.insertTempoEvent", "Add a tempo change", Scope::Edit,
     EngineImpact::Snapshot, kFTempoIns, false, false,
     tempoInsertApply, tempoInsertInverse, "project.removeTempoEvent"},
    {"project.removeTempoEvent", "Remove a tempo change", Scope::Edit,
     EngineImpact::Snapshot, kFPos, false, false,
     tempoRemoveApply, tempoRemoveInverse, "project.insertTempoEvent"},
    {"project.insertTimeSignature", "Add a time signature change", Scope::Edit,
     EngineImpact::Snapshot, kFSigIns, false, false,
     sigInsertApply, sigInsertInverse, "project.removeTimeSignature"},
    {"project.removeTimeSignature", "Remove a time signature change", Scope::Edit,
     EngineImpact::Snapshot, kFPos, false, false,
     sigRemoveApply, sigRemoveInverse, "project.insertTimeSignature"},

    {"routing.connect", "Connect two points in the signal path", Scope::Edit,
     EngineImpact::GraphRebuild, kFRouting, false, false,
     routingConnectApply, routingConnectInverse, "routing.disconnect"},
    {"routing.disconnect", "Remove a connection", Scope::Edit,
     EngineImpact::GraphRebuild, kFId, false, false,
     routingDisconnectApply, routingDisconnectInverse, "routing.connect"},

    // Ephemeral: performance, not editing. No inverse, and undo skips them.
    {"transport.play", "Start playback", Scope::Transport, EngineImpact::None,
     {}, false, true, transportNoop, nullptr, ""},
    {"transport.stop", "Stop playback", Scope::Transport, EngineImpact::None,
     {}, false, true, transportNoop, nullptr, ""},
    {"transport.seek", "Move the playhead", Scope::Transport, EngineImpact::None,
     kFTransportSeek, false, true, transportNoop, nullptr, ""},
    {"transport.setLoop", "Set the loop region", Scope::Transport, EngineImpact::None,
     kFTransportLoop, false, true, transportNoop, nullptr, ""},
    {"transport.setRecord", "Arm or disarm the transport", Scope::Transport,
     EngineImpact::GraphRebuild, kFTransportFlag, false, true,
     transportNoop, nullptr, ""},
    {"transport.setMetronome", "Toggle the metronome", Scope::Transport,
     EngineImpact::Snapshot, kFTransportFlag, false, true,
     transportNoop, nullptr, ""},
};

}  // namespace

std::span<const OpDescriptor> builtinOps() {
    static const std::vector<OpDescriptor> all = [] {
        std::vector<OpDescriptor> v(std::begin(kHandWritten), std::end(kHandWritten));
        appendScalars(v, std::make_index_sequence<kScalarCount>{});
        return v;
    }();
    return all;
}

}  // namespace adi
