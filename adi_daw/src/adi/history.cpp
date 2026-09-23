// SPDX-License-Identifier: GPL-3.0-or-later

#include "adi/history.hpp"

#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <algorithm>
#include <cstdio>

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

/// One op, by seq, as a move between points needs it.
std::optional<ChainOp> opAt(SQLite::Database& db, std::int64_t seq) {
    SQLite::Statement st(db,
        "SELECT seq, parent_seq, op_type, payload, inverse FROM ops WHERE seq = ?");
    st.bind(1, seq);
    if (!st.executeStep()) return std::nullopt;
    ChainOp o;
    o.seq = st.getColumn(0).getInt64();
    if (!st.getColumn(1).isNull()) o.parentSeq = st.getColumn(1).getInt64();
    o.opType = st.getColumn(2).getString();
    o.payload = blobOf(st.getColumn(3));
    o.inverse = blobOf(st.getColumn(4));
    o.hasInverse = !st.getColumn(4).isNull();
    return o;
}

/// `seq` and its ancestors, nearest first, ending at a root op. Empty for the
/// root itself. Bounded by the op count, so a corrupt parent cycle (which
/// `check` reports as history.cycle) ends in an error rather than a hang.
bool chainOf(SQLite::Database& db, std::optional<std::int64_t> seq,
             std::vector<ChainOp>& out, std::string& err) {
    out.clear();
    const auto limit = db.execAndGet("SELECT COUNT(*) FROM ops").getInt64();
    while (seq) {
        auto o = opAt(db, *seq);
        if (!o) { err = "op " + std::to_string(*seq) + " is not in the log"; return false; }
        if (static_cast<std::int64_t>(out.size()) > limit) {
            err = "the parent chain loops";
            return false;
        }
        seq = o->parentSeq;
        out.push_back(std::move(*o));
    }
    return true;
}

/// The end of the line through `from`: follow the highest-seq child, the rule
/// redo and the fork-preservation in OpJournal::commit both follow.
std::optional<std::int64_t> tipOf(SQLite::Database& db, std::optional<std::int64_t> from) {
    auto tip = from;
    for (auto next = childOf(db, tip); next; next = childOf(db, tip)) tip = next;
    return tip;
}

/// Rewinds from `from` to the common ancestor with `to`, then replays forward
/// to `to`. The caller holds the transaction and moves the head afterwards.
///
/// Op by op rather than transaction by transaction: both points are
/// transaction boundaries (a head only ever rests on one), so the path between
/// them is whole transactions, and inverting its ops newest-first is exactly
/// what undoing each transaction in turn would do.
bool moveBetween(OpContext& ctx, std::optional<std::int64_t> from,
                 std::optional<std::int64_t> to, int& applied, std::string& err) {
    std::vector<ChainOp> down, up;
    if (!chainOf(ctx.db, from, down, err) || !chainOf(ctx.db, to, up, err)) return false;

    std::vector<std::int64_t> onDown;
    onDown.reserve(down.size());
    for (const auto& o : down) onDown.push_back(o.seq);
    std::sort(onDown.begin(), onDown.end());

    // The common ancestor: the first op on `to`'s chain that is also on
    // `from`'s. None means the two lines meet only at the root.
    std::size_t k = 0;
    while (k < up.size() && !std::binary_search(onDown.begin(), onDown.end(), up[k].seq)) ++k;
    const std::optional<std::int64_t> meet =
        k < up.size() ? std::optional<std::int64_t>(up[k].seq) : std::nullopt;

    for (const auto& o : down) {
        if (meet && o.seq == *meet) break;
        if (!o.hasInverse) {
            err = "op " + std::to_string(o.seq) + " (" + o.opType + ") has no stored inverse";
            return false;
        }
        if (!applyOne(ctx, o.opType, o.inverse, /*asInverse=*/true, err)) {
            err = "rewinding " + o.opType + ": " + err;
            return false;
        }
        ++applied;
    }
    for (std::size_t i = k; i-- > 0;) {
        if (!applyOne(ctx, up[i].opType, up[i].payload, /*asInverse=*/false, err)) {
            err = "replaying " + up[i].opType + ": " + err;
            return false;
        }
        ++applied;
    }
    return true;
}

/// Proleptic Gregorian date from days since 1970-01-01 (H. Hinnant's
/// civil_from_days). Arithmetic rather than gmtime/localtime: those read the
/// process's time zone, and nothing in History may read ambient state.
void civilFromDays(std::int64_t z, std::int64_t& y, int& m, int& d) {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const std::int64_t doe = z - era * 146097;
    const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::int64_t mp = (5 * doy + 2) / 153;
    d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    y = yoe + era * 400 + (m <= 2 ? 1 : 0);
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

        // Rewind to the common ancestor, replay forward to the target's head.
        // The branch we leave keeps its own head_seq, so its line stays
        // reachable; switching back is the same move in the other direction.
        OpContext ctx{store_, db};
        SQLite::Transaction txn(db);
        std::string err;
        if (!moveBetween(ctx, here, target, r.opsApplied, err)) {
            r.error = err;
            r.opsApplied = 0;
            return r;   // rolls back
        }
        db.exec("UPDATE op_branches SET is_current = 0 WHERE is_current = 1");
        SQLite::Statement up(db, "UPDATE op_branches SET is_current = 1 WHERE id = ?");
        up.bind(1, branchId);
        up.exec();
        txn.commit();
        r.ok = true;
        return r;
    } catch (const std::exception& e) {
        r.error = e.what();
        return r;
    }
}

// ---------------------------------------------------------------------------
// Snapshots -- ADR-0128
// ---------------------------------------------------------------------------

std::string History::defaultSnapshotName(const std::string& projectName,
                                         std::int64_t utcMicros, int utcOffsetMinutes) {
    constexpr std::int64_t kMicrosPerMinute = 60'000'000;
    // Floor division: a time before 1970 still lands on the right minute.
    std::int64_t minutes = utcMicros / kMicrosPerMinute;
    if (utcMicros % kMicrosPerMinute < 0) --minutes;
    minutes += utcOffsetMinutes;
    std::int64_t days = minutes / 1440;
    std::int64_t inDay = minutes % 1440;
    if (inDay < 0) { inDay += 1440; --days; }

    std::int64_t y = 0;
    int m = 0, d = 0;
    civilFromDays(days, y, m, d);
    char when[40];
    std::snprintf(when, sizeof when, "%04lld-%02d-%02d %02d:%02d",
                  static_cast<long long>(y), m, d, static_cast<int>(inDay / 60),
                  static_cast<int>(inDay % 60));
    return (projectName.empty() ? std::string("Untitled") : projectName) + " " + when;
}

History::SnapshotResult History::takeSnapshot(const std::string& name, std::int64_t createdUtc,
                                              int utcOffsetMinutes, bool automatic) {
    SnapshotResult r;
    if (store_.readOnly()) { r.error = "project is open read-only"; return r; }
    try {
        auto& db = store_.db();
        r.name = name;
        if (r.name.empty()) {
            SQLite::Statement pn(db, "SELECT name FROM project WHERE id = 1");
            r.name = defaultSnapshotName(pn.executeStep() ? pn.getColumn(0).getString()
                                                          : std::string{},
                                         createdUtc, utcOffsetMinutes);
        }
        SQLite::Statement st(db,
            "INSERT INTO history_snapshots(name, op_seq, branch_id, created_utc, auto) "
            "VALUES (?,?,?,?,?)");
        st.bind(1, r.name);
        if (const auto h = head()) st.bind(2, *h); else st.bind(2);
        st.bind(3, journal_.currentBranchId());
        st.bind(4, createdUtc);
        st.bind(5, automatic ? 1 : 0);
        st.exec();
        r.id = db.getLastInsertRowid();
        r.ok = true;
    } catch (const std::exception& e) {
        r.error = e.what();
    }
    return r;
}

History::Result History::renameSnapshot(std::int64_t snapshotId, const std::string& newName) {
    Result r;
    if (store_.readOnly()) { r.error = "project is open read-only"; return r; }
    if (newName.empty()) { r.error = "a snapshot name cannot be empty"; return r; }
    try {
        SQLite::Statement st(store_.db(), "UPDATE history_snapshots SET name = ? WHERE id = ?");
        st.bind(1, newName);
        st.bind(2, snapshotId);
        if (st.exec() == 0) { r.error = "no such snapshot"; return r; }
        r.ok = true;
    } catch (const std::exception& e) {
        r.error = e.what();
    }
    return r;
}

std::vector<History::Snapshot> History::listSnapshots() const {
    std::vector<Snapshot> out;
    try {
        SQLite::Statement st(store_.db(),
            "SELECT id, name, op_seq, branch_id, created_utc, auto FROM history_snapshots "
            "ORDER BY created_utc, id");
        while (st.executeStep()) {
            Snapshot s;
            s.id = st.getColumn(0).getInt64();
            s.name = st.getColumn(1).getString();
            if (!st.getColumn(2).isNull()) s.opSeq = st.getColumn(2).getInt64();
            s.branchId = st.getColumn(3).getInt64();
            s.createdUtc = st.getColumn(4).getInt64();
            s.automatic = st.getColumn(5).getInt() != 0;
            out.push_back(std::move(s));
        }
    } catch (const std::exception&) {
        // A file older than 1.3 has no history_snapshots table: no snapshots.
    }
    return out;
}

History::Result History::revertToSnapshot(std::int64_t snapshotId, std::int64_t nowUtc) {
    Result r;
    if (store_.readOnly()) { r.error = "project is open read-only"; return r; }

    auto& db = store_.db();
    try {
        SQLite::Statement look(db, "SELECT name, op_seq FROM history_snapshots WHERE id = ?");
        look.bind(1, snapshotId);
        if (!look.executeStep()) { r.error = "no such snapshot"; return r; }
        const std::string name = look.getColumn(0).getString();
        std::optional<std::int64_t> target;
        if (!look.getColumn(1).isNull()) target = look.getColumn(1).getInt64();

        const auto here = head();
        if (here == target) { r.ok = true; return r; }   // already there

        OpContext ctx{store_, db};
        SQLite::Transaction txn(db);

        // ADR-0128 d2: nothing is lost by clicking. The line the head is on
        // may run past the head (undone work still redoable); its TIP is what
        // must stay reachable. It already is if another branch holds it, or if
        // it is where redo from the snapshot leads -- the next commit from there
        // forks it off exactly as it would after an undo (SPEC 8.2). Otherwise
        // the current branch's head is about to leave it, so name it now.
        const auto tip = tipOf(db, here);
        if (tip && tip != tipOf(db, target)) {
            SQLite::Statement held(db,
                "SELECT 1 FROM op_branches WHERE is_current = 0 AND head_seq = ?");
            held.bind(1, *tip);
            if (!held.executeStep()) {
                SQLite::Statement mk(db,
                    "INSERT INTO op_branches(name, head_seq, created_utc, is_current, "
                    "created_by) VALUES (?,?,?,0,'system')");
                mk.bind(1, "before revert to '" + name + "'");
                mk.bind(2, *tip);
                mk.bind(3, nowUtc);
                mk.exec();
            }
        }

        std::string err;
        if (!moveBetween(ctx, here, target, r.opsApplied, err)) {
            r.error = err;
            r.opsApplied = 0;
            return r;   // rolls back, the preserved branch row included
        }
        if (!journal_.setHeadSeq(target)) {
            r.error = "could not move the undo head";
            r.opsApplied = 0;
            return r;
        }
        txn.commit();
        r.ok = true;
        return r;
    } catch (const std::exception& e) {
        r.error = e.what();
        r.opsApplied = 0;
        return r;
    }
}

}  // namespace adi
