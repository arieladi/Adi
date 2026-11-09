// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/freeze_state.hpp"

#include "adi/history.hpp"
#include "adi/applied_batches_detail.hpp"
#include "adi/gesture_navigation_detail.hpp"

#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <algorithm>
#include <cstdio>
#include <stdexcept>

namespace adi {

namespace {

bool mutationAllowed(const Store& store, std::string& error) {
    if (detail::appliedDispatchActive(store)) {
        error = "reentrant history mutation during applied-batch delivery";
        return false;
    }
    if (!appliedSource(store).alive()) {
        error = "project is closed";
        return false;
    }
    return gesture::detail::mutationAllowed(store, error);
}

detail::PreparedAppliedDelivery finishApplied(detail::AppliedBatchBuilder& builder,
    const Store& store, std::optional<std::int64_t> after) {
    std::string error;
    auto result = builder.finish(store, after, error);
    if (!result) throw std::runtime_error("cannot capture applied history batch: " + error);
    return std::move(*result);
}

/// One op as undo/redo needs it.
struct ChainOp {
    std::int64_t seq = 0;
    std::int64_t txnId = 0;
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
        o.txnId = txnId;
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

bool applyOne(OpContext& ctx, const ChainOp& recorded, bool asInverse, std::string& err,
              detail::AppliedBatchBuilder* applied = nullptr) {
    const auto& opType = recorded.opType;
    const auto& cbor = asInverse ? recorded.inverse : recorded.payload;
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
    if (!target->apply(ctx, *payload, err)) return false;
    return applied == nullptr || applied->append(ctx.store, opType, targetName, *payload,
        recorded.seq, recorded.txnId, asInverse, err);
}

/// One op, by seq, as a move between points needs it.
std::optional<ChainOp> opAt(SQLite::Database& db, std::int64_t seq) {
    SQLite::Statement st(db,
        "SELECT seq, parent_seq, op_type, payload, inverse, txn_id FROM ops WHERE seq = ?");
    st.bind(1, seq);
    if (!st.executeStep()) return std::nullopt;
    ChainOp o;
    o.seq = st.getColumn(0).getInt64();
    if (!st.getColumn(1).isNull()) o.parentSeq = st.getColumn(1).getInt64();
    o.opType = st.getColumn(2).getString();
    o.payload = blobOf(st.getColumn(3));
    o.inverse = blobOf(st.getColumn(4));
    o.hasInverse = !st.getColumn(4).isNull();
    o.txnId = st.getColumn(5).getInt64();
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
                 std::optional<std::int64_t> to, int& applied, std::string& err,
                 detail::AppliedBatchBuilder* notice = nullptr) {
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
        if (!applyOne(ctx, o, /*asInverse=*/true, err, notice)) {
            err = "rewinding " + o.opType + ": " + err;
            return false;
        }
        ++applied;
    }
    for (std::size_t i = k; i-- > 0;) {
        if (!applyOne(ctx, up[i], /*asInverse=*/false, err, notice)) {
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
        SQLite::Statement base(db, "SELECT base_seq FROM history_checkpoint WHERE id=1");
        if (base.executeStep() && *h <= base.getColumn(0).getInt64())
            return std::nullopt;
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
    if (!mutationAllowed(store_, r.error)) return r;
    auto navigation = gesture::detail::beginNavigation(store_);
    if (!navigation.status.ok()) { r.error = navigation.status.message; return r; }
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
        detail::AppliedBatchBuilder applied(store_, AppliedBatchKind::Undo, head());

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
            if (!applyOne(ctx, *it, /*asInverse=*/true, err, &applied)) {
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

        if (!journal_.writeHeadSeq(ops.front().parentSeq)) {
            r.error = "could not move the undo head";
            return r;
        }

        freeze::validate(store_);
        auto delivery = finishApplied(applied, store_, ops.front().parentSeq);
        txn.commit();
        freeze::remember(store_);
        r.ok = true;
        r.txnId = step->txnId;
        delivery.publish();
        return r;
    } catch (const std::exception& e) {
        r.error = e.what();
        r.opsApplied = 0;
        return r;
    }
}

History::Result History::redo() {
    Result r;
    if (!mutationAllowed(store_, r.error)) return r;
    auto navigation = gesture::detail::beginNavigation(store_);
    if (!navigation.status.ok()) { r.error = navigation.status.message; return r; }
    if (store_.readOnly()) { r.error = "project is open read-only"; return r; }

    const auto step = nextRedo();
    if (!step) { r.error = "nothing to redo"; return r; }

    auto& db = store_.db();
    OpContext ctx{store_, db};
    try {
        SQLite::Transaction txn(db);
        detail::AppliedBatchBuilder applied(store_, AppliedBatchKind::Redo, head());

        const auto ops = txnOps(db, step->txnId);
        // Forward order, forward payloads -- redo replays what was originally
        // done, it does not invert anything.
        for (const auto& o : ops) {
            std::string err;
            if (!applyOne(ctx, o, /*asInverse=*/false, err, &applied)) {
                r.error = "redoing " + o.opType + ": " + err;
                return r;
            }
            ++r.opsApplied;
        }

        if (!journal_.writeHeadSeq(ops.back().seq)) {
            r.error = "could not move the undo head";
            return r;
        }

        freeze::validate(store_);
        auto delivery = finishApplied(applied, store_, ops.back().seq);
        txn.commit();
        freeze::remember(store_);
        r.ok = true;
        r.txnId = step->txnId;
        delivery.publish();
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
    if (!mutationAllowed(store_, r.error)) return r;
    auto navigation = gesture::detail::beginNavigation(store_);
    if (!navigation.status.ok()) { r.error = navigation.status.message; return r; }
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
            detail::AppliedBatchBuilder applied(store_, AppliedBatchKind::BranchSwitch, here);
            db.exec("UPDATE op_branches SET is_current = 0 WHERE is_current = 1");
            SQLite::Statement up(db, "UPDATE op_branches SET is_current = 1 WHERE id = ?");
            up.bind(1, branchId);
            up.exec();
            auto delivery = finishApplied(applied, store_, target);
            txn.commit();
            r.ok = true;
            delivery.publish();
            return r;
        }

        // Rewind to the common ancestor, replay forward to the target's head.
        // The branch we leave keeps its own head_seq, so its line stays
        // reachable; switching back is the same move in the other direction.
        OpContext ctx{store_, db};
        SQLite::Transaction txn(db);
        detail::AppliedBatchBuilder applied(store_, AppliedBatchKind::BranchSwitch, here);
        std::string err;
        if (!moveBetween(ctx, here, target, r.opsApplied, err, &applied)) {
            r.error = err;
            r.opsApplied = 0;
            return r;   // rolls back
        }
        db.exec("UPDATE op_branches SET is_current = 0 WHERE is_current = 1");
        SQLite::Statement up(db, "UPDATE op_branches SET is_current = 1 WHERE id = ?");
        up.bind(1, branchId);
        up.exec();
        freeze::validate(store_);
        auto delivery = finishApplied(applied, store_, target);
        txn.commit();
        freeze::remember(store_);
        r.ok = true;
        delivery.publish();
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

History::SnapshotResult History::takeSnapshot(const std::string &name, std::int64_t createdUtc,
                                              int utcOffsetMinutes, bool automatic,
                                              const HistorySnapshotPolicy &policy) {
    SnapshotResult r;
    if (!mutationAllowed(store_, r.error)) return r;
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
        SQLite::Transaction transaction(db);
        if (!history_detail::trimSnapshots(db, history_detail::snapshotLimit(policy), 1, std::nullopt,
                                           r.error))
            return r;
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
        transaction.commit();
        r.ok = true;
    } catch (const std::exception& e) {
        r.error = e.what();
    }
    return r;
}

History::Result History::removeSnapshot(std::int64_t id) {
    Result result;
    if (!mutationAllowed(store_, result.error)) return result;
    if (store_.readOnly()) {
        result.error = "project is open read-only";
        return result;
    }
    try {
        auto &db = store_.db();
        SQLite::Transaction transaction(db);
        SQLite::Statement remove(db, "DELETE FROM history_snapshots WHERE id=?");
        remove.bind(1, id);
        if (remove.exec() != 1) {
            result.error = "no such snapshot";
            return result;
        }
        history_detail::clearDailyReference(db, id);
        transaction.commit();
        result.ok = true;
    } catch (const std::exception &error) {
        result.error = error.what();
    }
    return result;
}

History::Result History::renameSnapshot(std::int64_t snapshotId, const std::string& newName) {
    Result r;
    if (!mutationAllowed(store_, r.error)) return r;
    if (store_.readOnly()) { r.error = "project is open read-only"; return r; }
    if (newName.empty()) { r.error = "a snapshot name cannot be empty"; return r; }
    try {
        SQLite::Statement st(store_.db(), "UPDATE history_snapshots SET name = ?,auto=0 WHERE id = ?");
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
    if (!mutationAllowed(store_, r.error)) return r;
    auto navigation = gesture::detail::beginNavigation(store_);
    if (!navigation.status.ok()) { r.error = navigation.status.message; return r; }
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
        detail::AppliedBatchBuilder applied(store_, AppliedBatchKind::SnapshotRevert, here);

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
        if (!moveBetween(ctx, here, target, r.opsApplied, err, &applied)) {
            r.error = err;
            r.opsApplied = 0;
            return r;   // rolls back, the preserved branch row included
        }
        if (!journal_.writeHeadSeq(target)) {
            r.error = "could not move the undo head";
            r.opsApplied = 0;
            return r;
        }
        freeze::validate(store_);
        auto delivery = finishApplied(applied, store_, target);
        txn.commit();
        freeze::remember(store_);
        r.ok = true;
        delivery.publish();
        return r;
    } catch (const std::exception& e) {
        r.error = e.what();
        r.opsApplied = 0;
        return r;
    }
}

}  // namespace adi

namespace adi {
namespace history_detail {
Payload checkpointAt(Store &store, std::int64_t seq) {
    auto &db = store.db();
    db.exec("SAVEPOINT adi_history_checkpoint");
    try {
        OpContext ctx{store, db};
        int applied = 0;
        std::string error;
        if (!moveBetween(ctx, OpJournal(store).headSeq(), seq, applied, error))
            throw std::runtime_error(error);
        auto state = checkpointState(store);
        db.exec("ROLLBACK TO adi_history_checkpoint");
        db.exec("RELEASE adi_history_checkpoint");
        return state;
    } catch (...) {
        db.exec("ROLLBACK TO adi_history_checkpoint");
        db.exec("RELEASE adi_history_checkpoint");
        throw;
    }
}
} // namespace history_detail
UndoCompactionResult History::setUndoRetentionPolicy(const UndoRetentionPolicy &policy) {
    UndoCompactionResult result;
    if (!mutationAllowed(store_, result.error)) return result;
    if (store_.readOnly()) {
        result.error = "project is open read-only";
        return result;
    }
    try {
        SQLite::Transaction transaction(store_.db());
        history_detail::writeUndoPolicy(store_.db(), policy);
        result = history_detail::compactUndo(store_, policy);
        if (result.ok)
            transaction.commit();
    } catch (const std::exception &error) {
        result.ok = false;
        result.error = error.what();
    }
    return result;
}
UndoCompactionResult History::compactUndo() {
    UndoCompactionResult result;
    if (!mutationAllowed(store_, result.error)) return result;
    if (store_.readOnly()) {
        result.error = "project is open read-only";
        return result;
    }
    try {
        SQLite::Transaction transaction(store_.db());
        result = history_detail::compactUndo(store_, history_detail::undoPolicy(store_.db()));
        if (result.ok)
            transaction.commit();
    } catch (const std::exception &error) {
        result.ok = false;
        result.error = error.what();
    }
    return result;
}
History::ReplayBaseResult History::restoreReplayBase(const Store &source, Store &target) {
    ReplayBaseResult result;
    if (!mutationAllowed(target, result.error)) return result;
    auto navigation = gesture::detail::beginNavigation(target);
    if (!navigation.status.ok()) { result.error = navigation.status.message; return result; }
    if (!appliedSource(source).alive()) { result.error = "source project is closed"; return result; }
    if (target.readOnly()) {
        result.error = "target is open read-only";
        return result;
    }
    try {
        if (&source == &target)
            throw std::runtime_error("replay target must be a different project");
        SQLite::Transaction transaction(target.db());
        detail::AppliedBatchBuilder applied(target, AppliedBatchKind::ReplayBaseReset,
                                             OpJournal(target).headSeq());
        std::optional<detail::PreparedAppliedDelivery> delivery;
        if (OpJournal(target).count() != 0)
            throw std::runtime_error("replay target journal is not empty");
        SQLite::Statement row(const_cast<SQLite::Database &>(source.db()),
                              "SELECT base_seq,state FROM history_checkpoint WHERE id=1");
        if (row.executeStep()) {
            std::string error;
            auto state = decodeFromLog(blobOf(row.getColumn(1)), error);
            if (!state)
                throw std::runtime_error("invalid history checkpoint: " + error);
            history_detail::installCheckpoint(target, *state);
            result.baseSeq = row.getColumn(0).getInt64();
            delivery = finishApplied(applied, target, OpJournal(target).headSeq());
        }
        transaction.commit();
        result.ok = true;
        if (delivery) delivery->publish();
    } catch (const std::exception &error) {
        result.error = error.what();
    }
    return result;
}
} // namespace adi
