// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/check.hpp"
#include "adi/digest.hpp"
#include "adi/history.hpp"
#include "adi/store.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <cstdio>
#include <stdexcept>
namespace {
using namespace adi;
int checks = 0, failures = 0;
void check(bool value, const char *message) {
    ++checks;
    if (!value) {
        ++failures;
        std::printf("FAIL: %s\n", message);
    }
}
struct Project {
    test::TempDirectory folder{"history_compaction", "project"};
    std::unique_ptr<Store> store;
    Project() {
        StoreError error{};
        store = Store::create(folder.path() / "song.adi", error);
        if (!store)
            throw std::runtime_error(toString(error));
    }
    CommitResult edit(const std::string &name) {
        OpRequest request;
        request.opType = "project.setName";
        request.payload = {{"name", name}};
        return OpJournal(*store).commit(request);
    }
    void edits(int first, int last) {
        for (int i = first; i <= last; ++i) {
            const auto result = edit(std::to_string(i));
            if (!result.ok)
                throw std::runtime_error(result.error);
        }
    }
    std::string name() {
        return store->db().execAndGet("SELECT name FROM project WHERE id=1").getString();
    }
};
UndoRetentionPolicy budget(std::int64_t steps) {
    UndoRetentionPolicy p;
    p.maxTransactions = steps;
    return p;
}
void linear() {
    Project p;
    History h(*p.store);
    check(h.setUndoRetentionPolicy(budget(3)).ok, "owner can lower the project undo budget");
    p.edits(1, 7);
    auto retained = h.compactUndo();
    check(retained.ok && retained.baseSeq == 4 && retained.transactions == 3 &&
              OpJournal(*p.store).count() == 4,
          "commits compact to three complete transactions plus the checkpoint root");
    check(p.name() == "7" && checkProject(*p.store).clean(),
          "checkpoint capture restores the live state and keeps integrity");
    int undone = 0;
    while (h.canUndo()) {
        if (!h.undo().ok)
            throw std::runtime_error("undo failed");
        ++undone;
    }
    check(undone == 3 && p.name() == "4" && h.head() == std::optional<std::int64_t>(4),
          "undo stops at retained state rather than offering an inverse-less root");
    check(!h.undo().ok && h.redo().ok && h.redo().ok && h.redo().ok && p.name() == "7",
          "redo round trip remains valid after compaction");
    check(!OpJournal(*p.store).setHeadSeq(std::nullopt) && !OpJournal(*p.store).setHeadSeq(3),
          "journal refuses a direct head move below its checkpoint root");
    const auto path = p.store->path();
    p.store.reset();
    StoreError error{};
    p.store = Store::open(path, error);
    check(p.store && History(*p.store).compactUndo().baseSeq == 4 && History(*p.store).canUndo(),
          "checkpoint and policy survive reopen");
}
void transactionsAndReplay() {
    Project p;
    History h(*p.store);
    check(h.setUndoRetentionPolicy(budget(2)).ok, "batched fixture policy");
    for (int i = 1; i <= 5; ++i) {
        std::vector<OpRequest> batch;
        for (int k = 1; k <= 2; ++k) {
            OpRequest request;
            request.opType = "project.setName";
            request.payload = {{"name", std::to_string(2 * (i - 1) + k)}};
            batch.push_back(request);
        }
        const auto result = OpJournal(*p.store).commit(batch);
        if (!result.ok)
            throw std::runtime_error(result.error);
        if (i == 3)
            check(p.store->db().execAndGet("SELECT base_seq FROM history_checkpoint WHERE id=1").getInt64() ==
                          2 &&
                      OpJournal(*p.store).count() == 5,
                  "first batch compaction stops at the complete oldest transaction immediately");
    }
    check(h.compactUndo().baseSeq == 6 && OpJournal(*p.store).count() == 5,
          "multi-op transactions are never split by the retained boundary");
    check(h.undo().opsApplied == 2 && h.undo().opsApplied == 2 && !h.canUndo() && p.name() == "6",
          "each retained batch remains one undo step");
    check(h.redo().ok && h.redo().ok && p.name() == "10", "batch redo returns to final state");
    Project target;
    target.store->db().exec("INSERT INTO session_state(key,value) VALUES('checkpoint-test','keep-ui')");
    const auto restored = History::restoreReplayBase(*p.store, *target.store);
    check(restored.ok && restored.baseSeq == 6 && target.name() == "6",
          "replay begins with checkpoint state, including edits no longer in ops");
    auto log = OpJournal(*p.store).recent(100);
    std::reverse(log.begin(), log.end());
    for (const auto &op : log)
        if (op.seq > restored.baseSeq) {
            OpRequest request;
            request.opType = op.opType;
            request.payload = op.payload;
            const auto result = OpJournal(*target.store).commit(request);
            if (!result.ok)
                throw std::runtime_error(result.error);
        }
    check(digestProject(*target.store).text == digestProject(*p.store).text,
          "checkpoint plus retained forward log reproduces the full project");
    check(target.store->db()
                  .execAndGet("SELECT value FROM session_state WHERE key='checkpoint-test'")
                  .getString() == "keep-ui",
          "replay base preserves target UI metadata");
    check(!History::restoreReplayBase(*p.store, *target.store).ok,
          "replay base refuses a nonempty target journal");
}
void richCheckpoint() {
    Project p;
    p.store->db().exec("INSERT INTO tracks(id,kind,name) VALUES(-20,'group','Parent'),(-10,'audio','Child')");
    p.store->db().exec("UPDATE tracks SET parent_id=-20,color=NULL WHERE id=-10");
    p.store->db().exec("INSERT INTO mixer_strip(track_id,pan,volume_db) VALUES(-10,0.123456789012345,-7.25)");
    p.store->db().exec("INSERT INTO state_blobs(hash_blake3,data,size_bytes) "
                       "VALUES('opaque',X'0001FF0002',5),('empty',X'',0)");
    History h(*p.store);
    check(h.setUndoRetentionPolicy(budget(2)).ok, "rich checkpoint fixture policy");
    p.edits(1, 4);
    Project target;
    const auto restored = History::restoreReplayBase(*p.store, *target.store);
    check(restored.ok && target.name() == "2", "rich checkpoint restores the actual retained state");
    check(target.store->db().execAndGet("SELECT parent_id FROM tracks WHERE id=-10").getInt64() == -20 &&
              target.store->db()
                      .execAndGet("SELECT COUNT(*) FROM tracks WHERE id=-10 AND color IS NULL")
                      .getInt() == 1,
          "negative IDs, hierarchy references and SQL NULL survive checkpoint round trip");
    check(target.store->db().execAndGet("SELECT pan FROM mixer_strip WHERE track_id=-10").getDouble() ==
              0.123456789012345,
          "floating-point state survives checkpoint without text conversion");
    check(target.store->db()
                      .execAndGet("SELECT hex(data) FROM state_blobs WHERE hash_blake3='opaque'")
                      .getString() == "0001FF0002" &&
              target.store->db()
                      .execAndGet("SELECT typeof(data) FROM state_blobs WHERE hash_blake3='empty'")
                      .getString() == "blob",
          "binary and zero-length BLOB remain BLOB rather than text or NULL");
    check(checkProject(*target.store).clean(), "restored rich checkpoint passes project integrity");
    p.store->db().exec("UPDATE history_checkpoint SET state=X'00'");
    check(!checkProject(*p.store).clean(), "integrity checking reports a corrupted checkpoint envelope");
}
void exactBytesAndLegacy() {
    Project p;
    History h(*p.store);
    p.edits(1, 3);
    const auto size = h.compactUndo().bytes;
    auto policy = budget(2000);
    policy.maxBytes = size;
    check(h.setUndoRetentionPolicy(policy).baseSeq == 0,
          "exact byte budget does not evict a transaction early");
    policy.maxBytes = size - 1;
    check(h.setUndoRetentionPolicy(policy).baseSeq == 1,
          "one byte above budget compacts the oldest whole transaction");
    policy.maxBytes = 1000000001;
    check(!h.setUndoRetentionPolicy(policy).ok, "byte budget cannot exceed 1000 decimal MB");
    Project legacy;
    History old(*legacy.store);
    legacy.store->db().exec("DROP TRIGGER media_never_embedded_insert");
    legacy.store->db().exec("INSERT INTO media_files(id,hash_blake3,embedded) VALUES(1,'legacy',1)");
    legacy.edits(1, 4);
    const auto result = old.setUndoRetentionPolicy(budget(2));
    check(result.ok && result.protectedHistory && result.baseSeq == 0 &&
              OpJournal(*legacy.store).count() == 4,
          "legacy embedded-media rows retain their replay log until extraction");
}
void anchors() {
    Project p;
    History h(*p.store);
    p.edits(1, 2);
    const auto named = h.takeSnapshot("Pinned", 1, 0);
    p.edits(3, 7);
    const auto retained = h.setUndoRetentionPolicy(budget(2));
    check(named.ok && retained.ok && retained.protectedHistory && retained.baseSeq == 2,
          "named snapshot limits compaction even above the budget");
    check(h.revertToSnapshot(named.id, 2).ok && p.name() == "2", "protected named point remains navigable");
    Project root;
    History rootHistory(*root.store);
    const auto point = rootHistory.takeSnapshot("Before edits", 1, 0);
    root.edits(1, 5);
    const auto blocked = rootHistory.setUndoRetentionPolicy(budget(2));
    check(point.ok && blocked.ok && blocked.protectedHistory && blocked.baseSeq == 0 &&
              OpJournal(*root.store).count() == 5,
          "a named root preserves the whole prefix");
    Project fork;
    History tree(*fork.store);
    fork.edits(1, 6);
    check(tree.undo().ok && tree.undo().ok && tree.undo().ok, "fork fixture rewinds");
    check(fork.edit("B1").ok && fork.edit("B2").ok, "fork fixture creates a second reachable line");
    const auto branchList = tree.branches();
    std::int64_t old = 0;
    for (const auto &branch : branchList)
        if (!branch.isCurrent)
            old = branch.id;
    const auto cap = tree.setUndoRetentionPolicy(budget(2));
    check(cap.ok && cap.protectedHistory && cap.baseSeq == 3 && old != 0,
          "reachable fork point prevents compaction crossing either branch");
    check(old != 0 && tree.switchToBranch(old).ok && fork.name() == "6",
          "old branch still replays after shared-prefix compaction");
    const auto oldBranchCap = tree.compactUndo();
    check(oldBranchCap.ok && oldBranchCap.protectedHistory && oldBranchCap.baseSeq == 3,
          "compaction after switching to the older branch also preserves the reachable fork");
    check(checkProject(*fork.store).clean(), "forked compacted tree passes integrity checks");
}
void collaborationAndFailures() {
    Project p;
    History h(*p.store);
    p.edits(1, 6);
    auto policy = budget(2);
    policy.collaboration = true;
    auto result = h.setUndoRetentionPolicy(policy);
    check(result.ok && result.protectedHistory && result.baseSeq == 0,
          "collaboration without an upload acknowledgement never compacts");
    policy.uploadedThroughSeq = 3;
    result = h.setUndoRetentionPolicy(policy);
    check(result.ok && result.protectedHistory && result.baseSeq == 3,
          "only the contiguous uploaded prefix can compact");
    policy.uploadedThroughSeq = 4;
    result = h.setUndoRetentionPolicy(policy);
    check(result.ok && !result.protectedHistory && result.baseSeq == 4 && h.canUndo(),
          "advancing upload acknowledgement allows the remaining safe compaction");
    Project failure;
    History journal(*failure.store);
    check(journal.setUndoRetentionPolicy(budget(2)).ok, "failure fixture policy");
    failure.edits(1, 2);
    failure.store->db().exec("CREATE TRIGGER refuse_checkpoint BEFORE INSERT ON history_checkpoint BEGIN "
                             "SELECT RAISE(ABORT,'planted checkpoint failure'); END");
    check(!failure.edit("3").ok && failure.name() == "2" && OpJournal(*failure.store).count() == 2 &&
              journal.compactUndo().baseSeq == 0,
          "checkpoint failure rolls back the whole edit, state and log together");
    failure.store->db().exec("DROP TRIGGER refuse_checkpoint");
    check(failure.edit("3").ok && journal.compactUndo().baseSeq == 1,
          "retry after checkpoint failure succeeds");
    Project late;
    History lateHistory(*late.store);
    if (!lateHistory.setUndoRetentionPolicy(budget(2)).ok)
        throw std::runtime_error("late hook policy");
    late.edits(1, 2);
    OpRequest request;
    request.opType = "project.setName";
    request.payload = {{"name", "3"}};
    CommitOptions options;
    options.beforeCommit = [](SQLite::Database &, std::int64_t, std::string &error) {
        error = "planted late hook failure";
        return false;
    };
    const auto rejected = OpJournal(*late.store).commit(std::span<const OpRequest>(&request, 1), options);
    check(!rejected.ok && late.name() == "2" && OpJournal(*late.store).count() == 2 &&
              lateHistory.compactUndo().baseSeq == 0,
          "final caller-hook rejection rolls back a successfully prepared checkpoint and edit");
    Project target;
    const auto before = digestProject(*target.store).text;
    const auto malformed = encodePayload({{"format", 1},
                                          {"schema_major", kSchemaMajor},
                                          {"schema_minor", kSchemaMinor},
                                          {"tables", Payload::array({{{"name", "not-a-project-table"},
                                                                      {"columns", Payload::array()},
                                                                      {"rows", Payload::array()}}})}});
    SQLite::Statement corrupt(failure.store->db(), "UPDATE history_checkpoint SET state=?");
    corrupt.bind(1, malformed.data(), static_cast<int>(malformed.size()));
    corrupt.exec();
    check(!History::restoreReplayBase(*failure.store, *target.store).ok &&
              digestProject(*target.store).text == before,
          "corrupt checkpoint cannot alter the replay target");
    auto invalid = budget(2001);
    check(!h.setUndoRetentionPolicy(invalid).ok, "project budget cannot exceed the director's maximum");
}
void bytesAndDefault() {
    Project p;
    History h(*p.store);
    auto policy = budget(2000);
    policy.maxBytes = 500;
    check(h.setUndoRetentionPolicy(policy).ok, "byte fixture policy");
    for (int i = 0; i < 6; ++i) {
        if (!p.edit(std::string(100, static_cast<char>('A' + i))).ok)
            throw std::runtime_error("byte edit");
    }
    const auto retained = h.compactUndo();
    check(retained.ok && retained.baseSeq > 0 && retained.bytes <= 500 && h.canUndo(),
          "byte cap can trigger before the transaction count limit");
    Project actual;
    actual.edits(1, 2001);
    History real(*actual.store);
    const auto cap = real.compactUndo();
    check(cap.ok && cap.baseSeq == 1 && cap.transactions == 2000 && OpJournal(*actual.store).count() == 2001,
          "default automatic compaction starts at the actual 2000-transaction boundary");
    const auto path = actual.store->path();
    actual.store.reset();
    StoreError error{};
    actual.store = Store::open(path, error, true);
    check(actual.store && !History(*actual.store).compactUndo().ok &&
              !History(*actual.store).setUndoRetentionPolicy(budget(2)).ok,
          "readonly projects refuse compaction and policy writes");
}
} // namespace
int main() {
    try {
        linear();
        transactionsAndReplay();
        richCheckpoint();
        exactBytesAndLegacy();
        anchors();
        collaborationAndFailures();
        bytesAndDefault();
    } catch (const std::exception &error) {
        check(false, error.what());
    }
    std::printf("history compaction: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
