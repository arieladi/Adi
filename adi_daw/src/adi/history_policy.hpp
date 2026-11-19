// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstdint>
#include <optional>
#include <string>
namespace SQLite {
class Database;
}
namespace adi {
class Store;
struct HistorySnapshotPolicy {
    bool collaboration = false;
    // Zero uses the director's maximum; a smaller project budget is allowed.
    std::int64_t maxSnapshots = 0;
};
class DailyHistorySnapshots {
  public:
    enum class Status { Unchanged, Created, Updated, Blocked, Failed };
    struct Result {
        Status status = Status::Unchanged;
        std::int64_t snapshotId = 0;
        std::string name, error;
    };
    // One service per open document, constructed BEFORE accepting edits. Poll
    // on successful edit/undo/redo and before close. No autosave timer/clock.
    explicit DailyHistorySnapshots(Store &, HistorySnapshotPolicy = {});
    Result poll(std::int64_t nowUtcMicros, int utcOffsetMinutes);

  private:
    Store &store_;
    HistorySnapshotPolicy policy_;
    std::optional<std::int64_t> observedHead_;
    bool pending_ = false;
};
namespace history_detail {
std::int64_t snapshotLimit(const HistorySnapshotPolicy &);
// Caller holds the metadata transaction. Never removes a named snapshot.
bool trimSnapshots(SQLite::Database &, std::int64_t limit, std::int64_t additional,
                   std::optional<std::int64_t> protectedId, std::string &error);
void clearDailyReference(SQLite::Database &, std::int64_t snapshotId);
} // namespace history_detail
} // namespace adi
