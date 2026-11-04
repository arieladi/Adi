// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/history_policy.hpp"
#include "adi/history.hpp"
#include "adi/ops.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <stdexcept>
namespace adi {
namespace history_detail {
std::int64_t snapshotLimit(const HistorySnapshotPolicy &policy) {
    const std::int64_t maximum = policy.collaboration ? 100000 : 10000;
    const auto limit = policy.maxSnapshots == 0 ? maximum : policy.maxSnapshots;
    if (limit < 1 || limit > maximum)
        throw std::runtime_error("snapshot budget exceeds the local/collaboration limit");
    return limit;
}
void clearDailyReference(SQLite::Database &db, std::int64_t id) {
    SQLite::Statement clear(
        db, "DELETE FROM adi_meta WHERE key GLOB 'history.daily.*' AND value=CAST(? AS TEXT)");
    clear.bind(1, id);
    clear.exec();
}
bool trimSnapshots(SQLite::Database &db, std::int64_t limit, std::int64_t additional,
                   std::optional<std::int64_t> protectedId, std::string &error) {
    auto count = db.execAndGet("SELECT COUNT(*) FROM history_snapshots").getInt64();
    while (count + additional > limit) {
        std::int64_t id = 0;
        {
            SQLite::Statement oldest(db, "SELECT id FROM history_snapshots WHERE auto=1 AND (? IS NULL OR "
                                         "id<>?) ORDER BY created_utc,id LIMIT 1");
            if (protectedId) {
                oldest.bind(1, *protectedId);
                oldest.bind(2, *protectedId);
            } else {
                oldest.bind(1);
                oldest.bind(2);
            }
            if (!oldest.executeStep()) {
                error = "snapshot limit reached; remove a named snapshot before creating another";
                return false;
            }
            id = oldest.getColumn(0).getInt64();
        }
        clearDailyReference(db, id);
        SQLite::Statement remove(db, "DELETE FROM history_snapshots WHERE id=? AND auto=1");
        remove.bind(1, id);
        if (remove.exec() != 1)
            throw std::runtime_error("automatic snapshot changed during retention");
        --count;
    }
    return true;
}
} // namespace history_detail
DailyHistorySnapshots::DailyHistorySnapshots(Store &store, HistorySnapshotPolicy policy)
    : store_(store), policy_(policy), observedHead_(OpJournal(store).headSeq()) {}
DailyHistorySnapshots::Result DailyHistorySnapshots::poll(std::int64_t utc, int offset) {
    Result result;
    if (store_.readOnly()) {
        result.status = Status::Failed;
        result.error = "project is open read-only";
        return result;
    }
    try {
        if (offset < -1440 || offset > 1440)
            throw std::runtime_error("invalid snapshot UTC offset");
        const auto limit = history_detail::snapshotLimit(policy_);
        auto &db = store_.db();
        SQLite::Transaction transaction(db);
        const auto head = OpJournal(store_).headSeq();
        if (head != observedHead_)
            pending_ = true;
        if (!pending_)
            return result;
        // Floor division, including pre-epoch times. Add the caller's offset
        // after dividing microseconds so the arithmetic cannot overflow.
        constexpr std::int64_t minute = 60000000;
        auto minutes = utc / minute;
        if (utc % minute < 0)
            --minutes;
        minutes += offset;
        auto day = minutes / 1440;
        if (minutes % 1440 < 0)
            --day;
        const auto key = "history.daily." + std::to_string(day);
        std::optional<std::int64_t> existing;
        {
            SQLite::Statement query(db, "SELECT h.id FROM adi_meta m JOIN history_snapshots h ON "
                                        "m.value=CAST(h.id AS TEXT) WHERE m.key=? AND h.auto=1");
            query.bind(1, key);
            if (query.executeStep())
                existing = query.getColumn(0).getInt64();
        }
        if (!history_detail::trimSnapshots(db, limit, existing ? 0 : 1, existing, result.error)) {
            result.status = Status::Blocked;
            return result;
        }
        const auto project = db.execAndGet("SELECT name FROM project WHERE id=1").getString();
        result.name = History::defaultSnapshotName(project, utc, offset);
        if (existing) {
            SQLite::Statement update(db, "UPDATE history_snapshots SET "
                                         "name=?,op_seq=?,branch_id=?,created_utc=? WHERE id=? AND auto=1");
            update.bind(1, result.name);
            if (head)
                update.bind(2, *head);
            else
                update.bind(2);
            update.bind(3, OpJournal(store_).currentBranchId());
            update.bind(4, utc);
            update.bind(5, *existing);
            if (update.exec() != 1)
                throw std::runtime_error("daily snapshot changed during update");
            result.snapshotId = *existing;
            result.status = Status::Updated;
        } else {
            SQLite::Statement insert(
                db,
                "INSERT INTO history_snapshots(name,op_seq,branch_id,created_utc,auto) VALUES(?,?,?,?,1)");
            insert.bind(1, result.name);
            if (head)
                insert.bind(2, *head);
            else
                insert.bind(2);
            insert.bind(3, OpJournal(store_).currentBranchId());
            insert.bind(4, utc);
            insert.exec();
            result.snapshotId = db.getLastInsertRowid();
            result.status = Status::Created;
            SQLite::Statement mark(db, "INSERT INTO adi_meta(key,value) VALUES(?,?) ON CONFLICT(key) DO "
                                       "UPDATE SET value=excluded.value");
            mark.bind(1, key);
            mark.bind(2, std::to_string(result.snapshotId));
            mark.exec();
        }
        transaction.commit();
        observedHead_ = head;
        pending_ = false;
    } catch (const std::exception &error) {
        result.status = Status::Failed;
        result.error = error.what();
    }
    return result;
}
} // namespace adi
