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

        // Invariant 2. An EMPTY schema is legal and meaningful: transport.play
        // takes no parameters, and an empty closed schema says exactly that --
        // "takes nothing, and any field is an error".
        //
        // This used to reject an empty schema on a write op, on the theory that
        // it meant someone forgot to declare one. It cannot tell the two apart
        // -- a std::span that was never set and one that is deliberately empty
        // are the same object -- so it was blocking correct ops while providing
        // a guarantee it could not actually make. And a genuinely forgotten
        // schema fails loudly on first use anyway, because validateSubmission
        // rejects every field as unknown.
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

OpRegistry::OpRegistry() {
    const auto builtin = builtinOps();
    ops_.assign(builtin.begin(), builtin.end());
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
    return commit(reqs, CommitOptions{});
}

CommitResult OpJournal::commit(std::span<const OpRequest> reqs, const CommitOptions& options) {
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

        // Where we are in the tree. New ops hang off this.
        std::optional<std::int64_t> head = headSeq();

        // ADR-0161: this Store's client, and the clock its ops continue from.
        // One more than every clock in the file, and than every seq, because
        // an op older than 1.6 reads as lamport = seq (SPEC 8.8).
        {
            SQLite::Statement client(db,
                "INSERT OR IGNORE INTO op_clients(client_id, label, first_seen_utc) VALUES (?,?,?)");
            client.bind(1, store_.clientId());
            client.bind(2, store_.clientLabel());
            client.bind(3, ts);
            client.exec();
        }
        std::int64_t lamport = db.execAndGet(
            "SELECT MAX(IFNULL((SELECT MAX(lamport) FROM op_clocks), 0),"
            "           IFNULL((SELECT MAX(seq) FROM ops), 0))").getInt64();

        // ADR-0153: the caller's expected head, checked HERE, inside the
        // transaction, so nothing can move it between the check and COMMIT.
        if (options.expectHead && head != *options.expectHead) {
            r.stale = true;
            r.headFound = head;
            r.error = "stale: the undo head moved";
            return r;   // Transaction dtor rolls back
        }

        // SPEC §8.2: "Undoing and then doing something new does not destroy the
        // branch you left; it forks."
        //
        // A fork is detectable here and nowhere else: if the head already has a
        // non-ephemeral child, we are committing from a position that is not the
        // tip, which means an undo happened and this work diverges from what
        // followed. Save the abandoned line as a branch BEFORE writing, or it
        // becomes unreachable the moment the new op takes its place in redo.
        {
            SQLite::Statement kids(db,
                "SELECT seq FROM ops WHERE tags <> 'ephemeral' AND "
                "  ((? IS NULL AND parent_seq IS NULL) OR parent_seq = ?) "
                "ORDER BY seq DESC LIMIT 1");
            if (head) { kids.bind(1, *head); kids.bind(2, *head); }
            else      { kids.bind(1);        kids.bind(2); }

            if (kids.executeStep()) {
                // Walk to the end of the abandoned line -- always the
                // highest-seq child, which is the same rule redo follows.
                std::int64_t tip = kids.getColumn(0).getInt64();
                for (;;) {
                    SQLite::Statement nxt(db,
                        "SELECT seq FROM ops WHERE parent_seq = ? AND tags <> 'ephemeral' "
                        "ORDER BY seq DESC LIMIT 1");
                    nxt.bind(1, tip);
                    if (!nxt.executeStep()) break;
                    tip = nxt.getColumn(0).getInt64();
                }
                SQLite::Statement mk(db,
                    "INSERT INTO op_branches(name, head_seq, created_utc, is_current, "
                    "created_by) VALUES (?,?,?,0,'system')");
                mk.bind(1, "forked at " + std::to_string(head ? *head : 0));
                mk.bind(2, tip);
                mk.bind(3, ts);
                mk.exec();
            }
        }

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

            // Ephemeral ops (transport, clip launching) are in the log for
            // audit and OUTSIDE the undo tree: parent_seq NULL, and they do not
            // move the head. OPS.md §10 -- undo skips them, the audit trail does
            // not. Keeping them out of the chain is what makes "skip" free
            // rather than a filter every walk has to remember.
            std::vector<std::byte> selBytes;
            const bool wantSel = (i == 0) && q.selBefore.has_value();
            if (wantSel) selBytes = encodePayload(*q.selBefore);

            SQLite::Statement st(db,
                "INSERT INTO ops(txn_id, parent_seq, ts_utc, actor, actor_detail, "
                "op_type, target_kind, target_id, payload, inverse, label, tags, "
                "sel_before) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)");
            st.bind(1, txnId);
            if (d->ephemeral || !head) st.bind(2); else st.bind(2, *head);
            st.bind(3, ts);
            st.bind(4, std::string(toString(q.actor)));
            st.bind(5, q.actorDetail);
            st.bind(6, q.opType);
            st.bind(7, q.targetKind);
            if (q.targetId) st.bind(8, *q.targetId); else st.bind(8);
            st.bindNoCopy(9, payloadBytes.data(), static_cast<int>(payloadBytes.size()));
            if (haveInverse)
                st.bindNoCopy(10, inverseBytes.data(), static_cast<int>(inverseBytes.size()));
            else
                st.bind(10);
            st.bind(11, q.label);
            st.bind(12, d->ephemeral ? "ephemeral" : "");
            if (wantSel)
                st.bindNoCopy(13, selBytes.data(), static_cast<int>(selBytes.size()));
            else
                st.bind(13);
            st.exec();
            const std::int64_t seq = db.getLastInsertRowid();
            r.seqs.push_back(seq);

            SQLite::Statement clock(db,
                "INSERT INTO op_clocks(seq, client_id, lamport) VALUES (?,?,?)");
            clock.bind(1, seq);
            clock.bind(2, store_.clientId());
            clock.bind(3, ++lamport);
            clock.exec();
            if (!d->ephemeral) head = seq;   // the chain advances; ephemeral does not
        }

        // The head move is in the SAME transaction as the mutations it
        // describes. Otherwise a crash between them leaves the project changed
        // and the head saying otherwise, which undo would then get wrong.
        if (!setHeadSeq(head)) {
            r.error = "could not advance the undo head";
            return r;
        }

        // ADR-0153: the caller's last word, still inside the transaction.
        if (options.beforeCommit) {
            std::string err;
            if (!options.beforeCommit(db, txnId, err)) {
                r.error = err.empty() ? std::string("the commit hook refused") : err;
                r.seqs.clear();
                return r;   // rolls back the ops, their rows and the hook's writes
            }
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
        // A file older than 1.6 opened read-only has no op_clocks (ADR-0161).
        const bool clocks = store_.schemaMinor() >= 6;
        SQLite::Statement st(store_.db(), clocks
            ? "SELECT o.seq, o.txn_id, o.ts_utc, o.actor, o.actor_detail, o.op_type, o.label, "
              "o.payload, o.inverse, o.parent_seq, o.tags, c.client_id, c.lamport "
              "FROM ops o LEFT JOIN op_clocks c ON c.seq = o.seq ORDER BY o.seq DESC LIMIT ?"
            : "SELECT seq, txn_id, ts_utc, actor, actor_detail, op_type, label, "
              "payload, inverse, parent_seq, tags FROM ops ORDER BY seq DESC LIMIT ?");
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
            if (!st.getColumn(9).isNull()) o.parentSeq = st.getColumn(9).getInt64();
            o.ephemeral = st.getColumn(10).getString() == "ephemeral";
            // An op without a clock row predates 1.6: lamport = seq (SPEC 8.8).
            o.lamport = o.seq;
            if (clocks && !st.getColumn(12).isNull()) {
                o.clientId = st.getColumn(11).getString();
                o.lamport = st.getColumn(12).getInt64();
            }
            out.push_back(std::move(o));
        }
    } catch (const std::exception&) {
    }
    return out;
}

std::int64_t OpJournal::currentBranchId() const {
    try {
        return store_.db()
            .execAndGet("SELECT id FROM op_branches WHERE is_current = 1")
            .getInt64();
    } catch (const std::exception&) {
        return 1;   // schema.sql seeds branch 1 on create
    }
}

std::optional<std::int64_t> OpJournal::headSeq() const {
    try {
        SQLite::Statement st(store_.db(),
            "SELECT head_seq FROM op_branches WHERE is_current = 1");
        if (!st.executeStep() || st.getColumn(0).isNull()) return std::nullopt;
        return st.getColumn(0).getInt64();
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

bool OpJournal::setHeadSeq(std::optional<std::int64_t> seq) {
    try {
        SQLite::Statement st(store_.db(),
            "UPDATE op_branches SET head_seq = ? WHERE is_current = 1");
        if (seq) st.bind(1, *seq); else st.bind(1);
        return st.exec() > 0;
    } catch (const std::exception&) {
        return false;
    }
}

std::int64_t OpJournal::count() const {
    try {
        return store_.db().execAndGet("SELECT COUNT(*) FROM ops").getInt64();
    } catch (const std::exception&) {
        return -1;
    }
}

}  // namespace adi
