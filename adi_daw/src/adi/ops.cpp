// SPDX-License-Identifier: GPL-3.0-or-later

#include "adi/ops.hpp"

#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <regex>
#include <stdexcept>

namespace adi {

// ---------------------------------------------------------------------------
// What a handler may touch
// ---------------------------------------------------------------------------

struct OpContext {
    Store& store;
    SQLite::Database& db;
};

namespace {

std::int64_t nowUtcMicros() {
    using namespace std::chrono;
    return duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
}

bool typeMatches(FieldType t, const Payload& v) {
    switch (t) {
        case FieldType::Int:    return v.is_number_integer();
        case FieldType::Real:   return v.is_number();          // an int is a valid real
        case FieldType::Text:   return v.is_string();
        case FieldType::Bool:   return v.is_boolean();
        case FieldType::Array:  return v.is_array();
        case FieldType::Object: return v.is_object();
    }
    return false;
}

std::string_view typeName(FieldType t) {
    switch (t) {
        case FieldType::Int:    return "int";
        case FieldType::Real:   return "real";
        case FieldType::Text:   return "text";
        case FieldType::Bool:   return "bool";
        case FieldType::Array:  return "array";
        case FieldType::Object: return "object";
    }
    return "?";
}

// --- handlers ---------------------------------------------------------------
//
// A deliberately small first tranche, chosen to exercise all three inverse
// shapes (OPS.md §6.2) rather than to be a lot of ops: symmetric, paired, and
// state capture. The rest of the 174 is mechanical once these are right.

bool setNameApply(OpContext& c, const Payload& p, std::string& err) {
    try {
        SQLite::Statement st(c.db, "UPDATE project SET name = ? WHERE id = 1");
        st.bind(1, p.at("name").get<std::string>());
        return st.exec() >= 0;
    } catch (const std::exception& e) { err = e.what(); return false; }
}

bool setNameInverse(OpContext& c, const Payload&, Payload& inv, std::string& err) {
    try {
        SQLite::Statement st(c.db, "SELECT name FROM project WHERE id = 1");
        inv = Payload::object();
        inv["name"] = st.executeStep() ? st.getColumn(0).getString() : std::string{};
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}

bool trackCreateApply(OpContext& c, const Payload& p, std::string& err) {
    try {
        // The id comes from the payload, never from SQLite. ADR-0021 §7.3:
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
    // track unlinks media_files rows, it does not delete audio -- so the
    // inverse of a 40-clip audio track is metadata, not gigabytes.
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

bool trackRenameApply(OpContext& c, const Payload& p, std::string& err) {
    try {
        SQLite::Statement st(c.db, "UPDATE tracks SET name = ? WHERE id = ?");
        st.bind(1, p.at("name").get<std::string>());
        st.bind(2, p.at("id").get<std::int64_t>());
        if (st.exec() == 0) { err = "no such track"; return false; }
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}

bool trackRenameInverse(OpContext& c, const Payload& p, Payload& inv, std::string& err) {
    try {
        SQLite::Statement st(c.db, "SELECT name FROM tracks WHERE id = ?");
        st.bind(1, p.at("id").get<std::int64_t>());
        if (!st.executeStep()) { err = "no such track"; return false; }
        inv = Payload::object();
        inv["id"] = p.at("id");
        inv["name"] = st.getColumn(0).getString();
        return true;   // symmetric
    } catch (const std::exception& e) { err = e.what(); return false; }
}

bool trackSetMuteApply(OpContext& c, const Payload& p, std::string& err) {
    try {
        SQLite::Statement st(c.db, "UPDATE tracks SET muted = ? WHERE id = ?");
        st.bind(1, p.at("muted").get<bool>() ? 1 : 0);
        st.bind(2, p.at("id").get<std::int64_t>());
        if (st.exec() == 0) { err = "no such track"; return false; }
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}

bool trackSetMuteInverse(OpContext& c, const Payload& p, Payload& inv, std::string& err) {
    try {
        SQLite::Statement st(c.db, "SELECT muted FROM tracks WHERE id = ?");
        st.bind(1, p.at("id").get<std::int64_t>());
        if (!st.executeStep()) { err = "no such track"; return false; }
        inv = Payload::object();
        inv["id"] = p.at("id");
        inv["muted"] = st.getColumn(0).getInt() != 0;
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
        inv["to"] = st.getColumn(0).getInt64();      // symmetric: swapped args
        inv["track"] = st.getColumn(1).getInt64();
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}

// --- field tables -----------------------------------------------------------

constexpr Field kSetName[]      = {{"name", FieldType::Text, true}};
constexpr Field kTrackCreate[]  = {{"id", FieldType::Int, true},
                                   {"kind", FieldType::Text, true},
                                   {"name", FieldType::Text, false},
                                   {"parent", FieldType::Int, false},
                                   {"index", FieldType::Int, false}};
constexpr Field kTrackId[]      = {{"id", FieldType::Int, true}};
constexpr Field kTrackRename[]  = {{"id", FieldType::Int, true},
                                   {"name", FieldType::Text, true}};
constexpr Field kTrackMute[]    = {{"id", FieldType::Int, true},
                                   {"muted", FieldType::Bool, true}};
constexpr Field kClipMove[]     = {{"id", FieldType::Int, true},
                                   {"to", FieldType::Int, true},
                                   {"track", FieldType::Int, true}};

const OpDescriptor kOps[] = {
    {"project.setName", "Rename the project", Scope::Edit, EngineImpact::None,
     kSetName, false, false, setNameApply, setNameInverse, ""},

    {"track.create", "Create a track", Scope::Edit, EngineImpact::GraphRebuild,
     kTrackCreate, false, false, trackCreateApply, trackCreateInverse, "track.delete"},

    {"track.delete", "Delete a track", Scope::Edit, EngineImpact::GraphRebuild,
     kTrackId, false, false, trackDeleteApply, trackDeleteInverse, "track.create"},

    {"track.rename", "Rename a track", Scope::Edit, EngineImpact::None,
     kTrackRename, false, false, trackRenameApply, trackRenameInverse, ""},

    {"track.setMute", "Mute or unmute a track", Scope::Edit, EngineImpact::Snapshot,
     kTrackMute, false, false, trackSetMuteApply, trackSetMuteInverse, ""},

    {"clip.move", "Move a clip in time, and optionally to another track",
     Scope::Edit, EngineImpact::Snapshot,
     kClipMove, false, false, clipMoveApply, clipMoveInverse, ""},
};

}  // namespace

// ---------------------------------------------------------------------------

std::string_view toString(Scope s) {
    switch (s) {
        case Scope::Read: return "read";
        case Scope::Edit: return "edit";
        case Scope::Transport: return "transport";
        case Scope::Session: return "session";
        case Scope::Hardware: return "hardware";
    }
    return "?";
}

std::string_view toString(EngineImpact e) {
    switch (e) {
        case EngineImpact::None: return "none";
        case EngineImpact::Snapshot: return "snapshot";
        case EngineImpact::GraphRebuild: return "graphRebuild";
        case EngineImpact::RequiresPause: return "requiresPause";
    }
    return "?";
}

std::string_view toString(Actor a) {
    switch (a) {
        case Actor::User: return "user";
        case Actor::Agent: return "agent";
        case Actor::Script: return "script";
        case Actor::Import: return "import";
        case Actor::Migration: return "migration";
        case Actor::Remote: return "remote";
    }
    return "?";
}

std::optional<Actor> actorFromString(std::string_view s) {
    if (s == "user") return Actor::User;
    if (s == "agent") return Actor::Agent;
    if (s == "script") return Actor::Script;
    if (s == "import") return Actor::Import;
    if (s == "migration") return Actor::Migration;
    if (s == "remote") return Actor::Remote;
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

std::vector<std::string> OpRegistry::selfCheck(std::span<const OpDescriptor> ops) {
    std::vector<std::string> bad;
    // OPS.md §3 invariant 4.
    static const std::regex nameRule(R"(^[a-z][a-zA-Z0-9]*\.[a-z][a-zA-Z0-9]*$)");

    for (std::size_t i = 0; i < ops.size(); ++i) {
        const auto& o = ops[i];
        const std::string n(o.name);

        if (!std::regex_match(n, nameRule))
            bad.push_back(n + ": name does not match domain.verb");

        // Invariant 1. The safe default is Read, and this is what stops a new
        // write op inheriting it by accident -- caught here rather than by a
        // client discovering it can edit.
        if (o.scope != Scope::Read) {
            if (!o.apply) bad.push_back(n + ": a write op with no apply handler");
            if (!o.buildInverse && !o.ephemeral)
                bad.push_back(n + ": a non-ephemeral write op with no inverse builder");
        }

        // Invariant 2.
        if (o.fields.empty() && o.scope != Scope::Read)
            bad.push_back(n + ": no payload schema (a closed schema may be empty "
                              "only for a read op)");
        for (const auto& f : o.fields)
            if (f.key.empty())
                bad.push_back(n + ": a field with an empty key");

        // Invariant 3.
        for (std::size_t j = i + 1; j < ops.size(); ++j)
            if (ops[j].name == o.name) bad.push_back(n + ": duplicate op name");

        // OPS.md §10: non-undoable ops live only in the ephemeral scopes.
        if (o.ephemeral && o.scope != Scope::Transport && o.scope != Scope::Session)
            bad.push_back(n + ": ephemeral outside transport/session scope");

        // OPS.md §6.4: coalescing keeps the FIRST op's inverse, so a
        // state-capture inverse would discard the intermediate states.
        if (o.coalescable && !o.inverseOp.empty())
            bad.push_back(n + ": coalescable ops must be symmetric, not paired");
    }

    // Every named inverse must exist, or undo walks into nothing.
    for (const auto& o : ops) {
        if (o.inverseOp.empty()) continue;
        const bool found = std::any_of(ops.begin(), ops.end(),
                                       [&](const OpDescriptor& x) { return x.name == o.inverseOp; });
        if (!found)
            bad.push_back(std::string(o.name) + ": names inverse '" +
                          std::string(o.inverseOp) + "', which is not registered");
    }
    return bad;
}

OpRegistry::OpRegistry() : ops_(std::begin(kOps), std::end(kOps)) {
    const auto bad = selfCheck(ops_);
    if (!bad.empty()) {
        std::string msg = "op registry is malformed and the process will not start:";
        for (const auto& b : bad) msg += "\n  - " + b;
        throw std::logic_error(msg);
    }
}

const OpRegistry& OpRegistry::instance() {
    static const OpRegistry r;
    return r;
}

const OpDescriptor* OpRegistry::find(std::string_view name) const {
    for (const auto& o : ops_)
        if (o.name == name) return &o;
    return nullptr;
}

std::span<const OpDescriptor> OpRegistry::all() const { return ops_; }

// ---------------------------------------------------------------------------
// Validation and codec
// ---------------------------------------------------------------------------

std::vector<ValidationIssue> validateSubmission(const OpDescriptor& d, const Payload& p) {
    std::vector<ValidationIssue> out;
    if (!p.is_object()) {
        out.push_back({"", "payload is not an object"});
        return out;
    }

    for (const auto& f : d.fields) {
        const std::string key(f.key);
        if (!p.contains(key)) {
            if (f.required) out.push_back({key, "required field missing"});
            continue;
        }
        const auto& v = p.at(key);
        if (v.is_null() && !f.required) continue;   // explicit null clears an optional
        if (!typeMatches(f.type, v))
            out.push_back({key, "expected " + std::string(typeName(f.type))});
        // Non-finite floats have no single encoding and no musical meaning.
        // Letting one through would put a NaN in a project file.
        if (v.is_number_float() && !std::isfinite(v.get<double>()))
            out.push_back({key, "value is not finite"});
    }

    // Closed schema. An unknown field from a CALLER is a typo or a version
    // mismatch, and accepting it silently means they believe they set something
    // they did not. (An unknown field read back from the LOG is a different
    // thing entirely and is preserved -- see decodeFromLog.)
    for (auto it = p.begin(); it != p.end(); ++it) {
        const bool known = std::any_of(d.fields.begin(), d.fields.end(),
                                       [&](const Field& f) { return f.key == it.key(); });
        if (!known) out.push_back({it.key(), "unknown field for " + std::string(d.name)});
    }
    return out;
}

std::vector<std::byte> encodePayload(const Payload& p) {
    const auto v = Payload::to_cbor(p);
    std::vector<std::byte> out(v.size());
    std::memcpy(out.data(), v.data(), v.size());
    return out;
}

std::optional<Payload> decodeFromLog(std::span<const std::byte> cbor, std::string& err) {
    try {
        // allow_exceptions=false would hide the reason; we want it in `err`.
        const auto* p = reinterpret_cast<const std::uint8_t*>(cbor.data());
        return Payload::from_cbor(std::vector<std::uint8_t>(p, p + cbor.size()));
    } catch (const std::exception& e) {
        err = e.what();
        return std::nullopt;
    }
}

// ---------------------------------------------------------------------------
// The journal
// ---------------------------------------------------------------------------

OpJournal::OpJournal(Store& s) : store_(s) {}

CommitResult OpJournal::commit(std::span<const OpRequest> reqs) {
    CommitResult r;
    if (reqs.empty()) { r.ok = true; return r; }
    if (store_.readOnly()) { r.error = "project is open read-only"; return r; }

    const auto& reg = OpRegistry::instance();

    // Validate everything BEFORE opening a transaction. A batch that cannot
    // succeed should not have touched the database at all.
    std::vector<const OpDescriptor*> descs;
    descs.reserve(reqs.size());
    for (const auto& q : reqs) {
        const auto* d = reg.find(q.opType);
        if (!d) { r.error = "unknown op: " + q.opType; return r; }
        if (!d->apply) { r.error = q.opType + " has no handler"; return r; }
        auto issues = validateSubmission(*d, q.payload);
        if (!issues.empty()) {
            r.issues = std::move(issues);
            r.error = "payload validation failed for " + q.opType;
            return r;
        }
        descs.push_back(d);
    }

    auto& db = store_.db();
    OpContext ctx{store_, db};

    try {
        // ADR-0003 / SPEC §3.5. One transaction covering BOTH the mutation and
        // its log row. A failure anywhere rolls back everything, so a
        // partially-applied batch cannot survive describing itself as complete.
        SQLite::Transaction txn(db);

        const std::int64_t txnId =
            db.execAndGet("SELECT IFNULL(MAX(txn_id), 0) + 1 FROM ops").getInt64();
        r.txnId = txnId;
        const std::int64_t ts = nowUtcMicros();

        for (std::size_t i = 0; i < reqs.size(); ++i) {
            const auto& q = reqs[i];
            const auto* d = descs[i];

            // The inverse is built from the state ABOUT TO BE OVERWRITTEN, so
            // it must run before apply (OPS.md §6.1). If it cannot produce a
            // correct inverse, the op does not commit.
            Payload inverse;
            bool haveInverse = false;
            if (d->buildInverse && !d->ephemeral) {
                std::string err;
                if (!d->buildInverse(ctx, q.payload, inverse, err)) {
                    r.error = q.opType + ": cannot build inverse: " + err;
                    return r;   // Transaction dtor rolls back
                }
                haveInverse = true;
            }

            std::string err;
            if (!d->apply(ctx, q.payload, err)) {
                r.error = q.opType + ": " + err;
                return r;
            }

            const auto payloadBytes = encodePayload(q.payload);
            std::vector<std::byte> inverseBytes;
            if (haveInverse) inverseBytes = encodePayload(inverse);

            SQLite::Statement st(db,
                "INSERT INTO ops(txn_id, ts_utc, actor, actor_detail, op_type, "
                "target_kind, target_id, payload, inverse, label, tags) "
                "VALUES (?,?,?,?,?,?,?,?,?,?,?)");
            st.bind(1, txnId);
            st.bind(2, ts);
            st.bind(3, std::string(toString(q.actor)));
            st.bind(4, q.actorDetail);
            st.bind(5, q.opType);
            st.bind(6, q.targetKind);
            if (q.targetId) st.bind(7, *q.targetId); else st.bind(7);
            st.bindNoCopy(8, payloadBytes.data(), static_cast<int>(payloadBytes.size()));
            if (haveInverse)
                st.bindNoCopy(9, inverseBytes.data(), static_cast<int>(inverseBytes.size()));
            else
                st.bind(9);
            st.bind(10, q.label);
            st.bind(11, d->ephemeral ? "ephemeral" : "");
            st.exec();
            r.seqs.push_back(db.getLastInsertRowid());
        }

        txn.commit();
        r.ok = true;
        return r;
    } catch (const std::exception& e) {
        r.error = e.what();
        r.seqs.clear();
        return r;
    }
}

std::vector<OpJournal::LoggedOp> OpJournal::recent(int limit) const {
    std::vector<LoggedOp> out;
    try {
        SQLite::Statement st(store_.db(),
            "SELECT seq, txn_id, ts_utc, actor, actor_detail, op_type, label, "
            "payload, inverse FROM ops ORDER BY seq DESC LIMIT ?");
        st.bind(1, limit);
        while (st.executeStep()) {
            LoggedOp o;
            o.seq = st.getColumn(0).getInt64();
            o.txnId = st.getColumn(1).getInt64();
            o.tsUtc = st.getColumn(2).getInt64();
            o.actor = st.getColumn(3).getString();
            o.actorDetail = st.getColumn(4).getString();
            o.opType = st.getColumn(5).getString();
            o.label = st.getColumn(6).getString();

            std::string err;
            const auto pc = st.getColumn(7);
            if (!pc.isNull()) {
                const auto* b = static_cast<const std::byte*>(pc.getBlob());
                if (auto d = decodeFromLog({b, static_cast<std::size_t>(pc.getBytes())}, err))
                    o.payload = std::move(*d);
            }
            const auto ic = st.getColumn(8);
            if (!ic.isNull()) {
                const auto* b = static_cast<const std::byte*>(ic.getBlob());
                if (auto d = decodeFromLog({b, static_cast<std::size_t>(ic.getBytes())}, err))
                    o.inverse = std::move(*d);
            }
            out.push_back(std::move(o));
        }
    } catch (const std::exception&) {
    }
    return out;
}

std::int64_t OpJournal::count() const {
    try {
        return store_.db().execAndGet("SELECT COUNT(*) FROM ops").getInt64();
    } catch (const std::exception&) {
        return -1;
    }
}

}  // namespace adi
