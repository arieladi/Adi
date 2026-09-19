// SPDX-License-Identifier: GPL-3.0-or-later

#include "adi/history.hpp"

#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <algorithm>

namespace adi {

namespace {

/// One op as undo/redo needs it.
struct ChainOp {
    std::int64_t seq = 0;
    std::optional<std::int64_t> parentSeq;
    std::string opType;
    std::vector<std::byte> payload;
    std::vector<std::byte> inverse;
    bool hasInverse = false;
};

std::vector<std::byte> blobOf(const SQLite::Column& c) {
    if (c.isNull()) return {};
    const auto* p = static_cast<const std::byte*>(c.getBlob());
    return {p, p + c.getBytes()};
}

/// Every non-ephemeral op in a transaction, in seq order.
std::vector<ChainOp> txnOps(SQLite::Database& db, std::int64_t txnId) {
    std::vector<ChainOp> out;
    SQLite::Statement st(db,
        "SELECT seq, parent_seq, op_type, payload, inverse FROM ops "
        "WHERE txn_id = ? AND tags <> 'ephemeral' ORDER BY seq ASC");
    st.bind(1, txnId);
    while (st.executeStep()) {
        ChainOp o;
        o.seq = st.getColumn(0).getInt64();
        if (!st.getColumn(1).isNull()) o.parentSeq = st.getColumn(1).getInt64();
        o.opType = st.getColumn(2).getString();
        o.payload = blobOf(st.getColumn(3));
        o.inverse = blobOf(st.getColumn(4));
        o.hasInverse = !st.getColumn(4).isNull();
        out.push_back(std::move(o));
    }
    return out;
}

/// The op that redo would move onto: the highest-seq non-ephemeral child of
/// `head`.
///
/// Highest-seq rather than "the only one", because after a fork there are
/// several children and the most recently created is the line the user is on.
/// The fork-preservation in OpJournal::commit follows the same rule, so the two
/// cannot disagree about which line is live.
std::optional<std::int64_t> childOf(SQLite::Database& db,
                                    std::optional<std::int64_t> head) {
    SQLite::Statement st(db,
        "SELECT seq FROM ops WHERE tags <> 'ephemeral' AND "
        "  ((? IS NULL AND parent_seq IS NULL) OR parent_seq = ?) "
        "ORDER BY seq DESC LIMIT 1");
    if (head) { st.bind(1, *head); st.bind(2, *head); }
    else      { st.bind(1);        st.bind(2); }
    if (!st.executeStep()) return std::nullopt;
    return st.getColumn(0).getInt64();
}

bool applyOne(OpContext& ctx, const std::string& opType,
              const std::vector<std::byte>& cbor, bool asInverse, std::string& err) {
    const auto* d = OpRegistry::instance().find(opType);
    if (!d) { err = "unknown op in log: " + opType; return false; }

    // Which op runs the payload. For a symmetric inverse it is the same op with
    // the previous values; for a paired one it is the declared opposite --
    // track.delete's inverse replays through track.create (OPS.md §6.2).
    const std::string_view targetName =
        (asInverse && !d->inverseOp.empty()) ? d->inverseOp : d->name;
    const auto* target = OpRegistry::instance().find(targetName);
    if (!target || !target->apply) {
        err = "no handler for " + std::string(targetName);
        return false;
    }

    auto payload = decodeFromLog(cbor, err);
    if (!payload) { err = "cannot decode payload for " + opType + ": " + err; return false; }
    return target->apply(ctx, *payload, err);
}

}  // namespace

History::History(Store& s) : store_(s), journal_(s) {}

std::optional<std::int64_t> History::head() const { return journal_.headSeq(); }

std::optional<History::Step> History::nextUndo() const {
    const auto h = head();
    if (!h) return std::nullopt;   // at the root
    try {
        auto& db = store_.db();
        SQLite::Statement st(db,
            "SELECT txn_id, label, actor, actor_detail FROM ops WHERE seq = ?");
        st.bind(1, *h);
        if (!st.executeStep()) return std::nullopt;

        Step s;
        s.txnId = st.getColumn(0).getInt64();
        s.label = st.getColumn(1).getString();
        s.actor = st.getColumn(2).getString();
        s.actorDetail = st.getColumn(3).getString();

        const auto ops = txnOps(db, s.txnId);
        if (ops.empty()) return std::nullopt;
        s.firstSeq = ops.front().seq;
        s.lastSeq = ops.back().seq;
        s.opCount = static_cast<int>(ops.size());
        // The label users see is the transaction's, which is the first op's.
        SQLite::Statement lb(db, "SELECT label FROM ops WHERE seq = ?");
        lb.bind(1, s.firstSeq);
        if (lb.executeStep()) s.label = lb.getColumn(0).getString();
        return s;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<History::Step> History::nextRedo() const {
    try {
        auto& db = store_.db();
        const auto child = childOf(db, head());
        if (!child) return std::nullopt;

        SQLite::Statement st(db,
            "SELECT txn_id, label, actor, actor_detail FROM ops WHERE seq = ?");
        st.bind(1, *child);
        if (!st.executeStep()) return std::nullopt;

        Step s;
        s.txnId = st.getColumn(0).getInt64();
        s.label = st.getColumn(1).getString();
        s.actor = st.getColumn(2).getString();
        s.actorDetail = st.getColumn(3).getString();

        const auto ops = txnOps(db, s.txnId);
        if (ops.empty()) return std::nullopt;
        s.firstSeq = ops.front().seq;
        s.lastSeq = ops.back().seq;
        s.opCount = static_cast<int>(ops.size());
        return s;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

History::Result History::undo() {
    Result r;
    if (store_.readOnly()) { r.error = "project is open read-only"; return r; }

    const auto step = nextUndo();
    if (!step) { r.error = "nothing to undo"; return r; }

    auto& db = store_.db();
    OpContext ctx{store_, db};
    try {
        // One transaction for the inverses AND the head move. A crash between
        // them would leave the project changed and the head disagreeing, and
        // the next undo would then act on the wrong transaction.
        SQLite::Transaction txn(db);

        const auto ops = txnOps(db, step->txnId);

        // Reverse order: within a transaction, later ops may depend on earlier
        // ones, so their inverses have to come off in the opposite order they
        // went on.
        for (auto it = ops.rbegin(); it != ops.rend(); ++it) {
            if (!it->hasInverse) {
                r.error = "op " + std::to_string(it->seq) + " (" + it->opType +
                          ") has no stored inverse";
                return r;   // rolls back
            }
            std::string err;
            if (!applyOne(ctx, it->opType, it->inverse, /*asInverse=*/true, err)) {
                r.error = "undoing " + it->opType + ": " + err;
                return r;
            }
            ++r.opsApplied;
        }

        // The selection hint belongs to the transaction, so it is on its first
        // op. Advisory: a caller that ignores it is still correct.
        SQLite::Statement sel(db, "SELECT sel_before FROM ops WHERE seq = ?");
        sel.bind(1, ops.front().seq);
        if (sel.executeStep() && !sel.getColumn(0).isNull()) {
            std::string err;
            r.selBefore = decodeFromLog(blobOf(sel.getColumn(0)), err);
        }

        if (!journal_.setHeadSeq(ops.front().parentSeq)) {
            r.error = "could not move the undo head";
            return r;
        }

        txn.commit();
        r.ok = true;
        r.txnId = step->txnId;
        return r;
    } catch (const std::exception& e) {
        r.error = e.what();
        r.opsApplied = 0;
        return r;
    }
}

History::Result History::redo() {
    Result r;
    if (store_.readOnly()) { r.error = "project is open read-only"; return r; }

    const auto step = nextRedo();
    if (!step) { r.error = "nothing to redo"; return r; }

    auto& db = store_.db();
    OpContext ctx{store_, db};
    try {
        SQLite::Transaction txn(db);

        const auto ops = txnOps(db, step->txnId);
        // Forward order, forward payloads -- redo replays what was originally
        // done, it does not invert anything.
        for (const auto& o : ops) {
            std::string err;
            if (!applyOne(ctx, o.opType, o.payload, /*asInverse=*/false, err)) {
                r.error = "redoing " + o.opType + ": " + err;
                return r;
            }
            ++r.opsApplied;
        }

        if (!journal_.setHeadSeq(ops.back().seq)) {
            r.error = "could not move the undo head";
            return r;
        }

        txn.commit();
        r.ok = true;
        r.txnId = step->txnId;
        return r;
    } catch (const std::exception& e) {
        r.error = e.what();
        r.opsApplied = 0;
        return r;
    }
}

std::vector<History::Branch> History::branches() const {
    std::vector<Branch> out;
    try {
        SQLite::Statement st(store_.db(),
            "SELECT id, name, head_seq, is_current, created_utc FROM op_branches "
            "ORDER BY id");
        while (st.executeStep()) {
            Branch b;
            b.id = st.getColumn(0).getInt64();
            b.name = st.getColumn(1).getString();
            if (!st.getColumn(2).isNull()) b.headSeq = st.getColumn(2).getInt64();
            b.isCurrent = st.getColumn(3).getInt() != 0;
            b.createdUtc = st.getColumn(4).getInt64();
            out.push_back(std::move(b));
        }
    } catch (const std::exception&) {
    }
    return out;
}

History::Result History::switchToBranch(std::int64_t branchId) {
    Result r;
    if (store_.readOnly()) { r.error = "project is open read-only"; return r; }

    auto& db = store_.db();
    try {
        SQLite::Statement look(db, "SELECT head_seq FROM op_branches WHERE id = ?");
        look.bind(1, branchId);
        if (!look.executeStep()) { r.error = "no such branch"; return r; }
        std::optional<std::int64_t> target;
        if (!look.getColumn(0).isNull()) target = look.getColumn(0).getInt64();

        const auto here = head();
        if (here == target) {
            SQLite::Transaction txn(db);
            db.exec("UPDATE op_branches SET is_current = 0 WHERE is_current = 1");
            SQLite::Statement up(db, "UPDATE op_branches SET is_current = 1 WHERE id = ?");
            up.bind(1, branchId);
            up.exec();
            txn.commit();
            r.ok = true;
            return r;
        }

        // Rewinding to the common ancestor and replaying forward is the correct
        // general move and is not implemented yet. Doing it wrong would corrupt
        // a project, so it refuses rather than approximating.
        r.error = "switching between diverged branches needs rewind-and-replay, "
                  "which is not implemented; only re-selecting a branch at the "
                  "current head works today";
        return r;
    } catch (const std::exception& e) {
        r.error = e.what();
        return r;
    }
}

}  // namespace adi
