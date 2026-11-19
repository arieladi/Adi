// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/check.hpp"
#include "adi/applied_batches.hpp"
#include "adi/media/blake3.hpp"
#include "adi/digest.hpp"
#include "adi/history.hpp"
#include "adi/store.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <string_view>
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
    check(p.store->db().execAndGet("SELECT count(*) FROM history_checkpoint_content c JOIN history_state_manifests m ON m.hash=c.state_hash WHERE c.id=1").getInt64()==1,"automatic compaction attaches its root to a real content manifest");
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
                          4 &&
                      OpJournal(*p.store).count() == 3,
                  "first batch compaction stops at the complete oldest transaction immediately");
    }
    check(h.compactUndo().baseSeq == 8 && OpJournal(*p.store).count() == 3,
          "multi-op transactions are never split by the retained boundary");
    check(h.undo().opsApplied == 2 && !h.canUndo() && p.name() == "8",
          "each retained batch remains one undo step");
    check(h.redo().ok && p.name() == "10", "batch redo returns to final state");
    Project target;
    target.store->db().exec("INSERT INTO session_state(key,value) VALUES('checkpoint-test','keep-ui')");
    const auto restored = History::restoreReplayBase(*p.store, *target.store);
    check(restored.ok && restored.baseSeq == 8 && target.name() == "8",
          "replay begins with checkpoint state, including edits no longer in ops");
    auto log = OpJournal(*p.store).recent(100);
    std::reverse(log.begin(), log.end());
    for (const auto &op : log)
        if (std::find(restored.retainedSeqs.begin(), restored.retainedSeqs.end(), op.seq) !=
            restored.retainedSeqs.end()) {
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
    constexpr auto prepared = History::StoredRestore::SessionReloadPrepared;
    Project p;
    History h(*p.store);
    p.edits(1, 2);
    const auto named = h.takeSnapshot("Pinned", 1, 0);
    p.edits(3, 7);
    const auto retained = h.setUndoRetentionPolicy(budget(2));
    check(named.ok && retained.ok && !retained.protectedHistory && retained.baseSeq == 6 &&
              retained.transactions == 1,
          "named snapshots become stored states without blocking the active undo cap");
    const auto before = digestProject(*p.store).text;
    const auto oldHead = h.head();
    const auto refused = h.revertToSnapshot(named.id, 2);
    check(!refused.ok && refused.sessionReloadRequired && h.head() == oldHead &&
              digestProject(*p.store).text == before,
          "cross-baseline default refuses before mutation and requests document reload");
    const auto restored = h.revertToSnapshot(named.id, 2, prepared);
    check(restored.ok && restored.sessionReloadRequired && p.name() == "2" && !h.canUndo(),
          "prepared stored snapshot restores its exact point without fabricated undo history");
    check(h.listSnapshots().front().storedStateHash.has_value() && checkProject(*p.store).clean(),
          "stored named point exposes a content identity and passes integrity");
    Project origin;
    History root(*origin.store);
    const auto emptyName = origin.name();
    const auto point = root.takeSnapshot("Before edits", 1, 0);
    origin.edits(1, 5);
    const auto capped = root.setUndoRetentionPolicy(budget(2));
    check(point.ok && capped.ok && !capped.protectedHistory && capped.transactions == 1 &&
              capped.baseSeq == 4,
          "a named original root no longer pins the edit prefix");
    check(root.revertToSnapshot(point.id, 2, prepared).ok && origin.name() == emptyName &&
              !root.canUndo(),
          "synthetic origin metadata restores the actual pre-edit state");
    check(checkProject(*origin.store).clean() && root.readTimeline().error.empty(),
          "a later synthetic origin sequence is a valid structural parent");
    Project fork;
    History tree(*fork.store);
    fork.edits(1, 6);
    check(tree.undo().ok && tree.undo().ok && tree.undo().ok, "fork fixture rewinds");
    check(fork.edit("B1").ok && fork.edit("B2").ok, "fork fixture creates second reachable line");
    std::int64_t old = 0;
    for (const auto &branch : tree.branches())
        if (!branch.isCurrent)
            old = branch.id;
    const auto cap = tree.setUndoRetentionPolicy(budget(2));
    check(cap.ok && !cap.protectedHistory && cap.baseSeq == 7 && cap.transactions == 1 && old != 0,
          "dead ends become stored states before their bridge is pruned");
    check(tree.switchToBranch(old, prepared).ok && fork.name() == "6" && !tree.canUndo(),
          "old dead end opens from its saved baseline after active history crosses the fork");
    const auto oldCap = tree.compactUndo();
    check(oldCap.ok && !oldCap.protectedHistory && oldCap.baseSeq == 6 &&
              checkProject(*fork.store).clean(),
          "switching to a stored dead end preserves cap and integrity");
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
    const auto deferred=failure.edit("3");
    check(deferred.ok && !deferred.retentionWarning.empty() && deferred.error.empty() &&
              failure.name()=="3" && OpJournal(*failure.store).count()==3 && !journal.compactUndo().ok,
          "checkpoint failure reports deferral while preserving the edit, state and log");
    failure.store->db().exec("DROP TRIGGER refuse_checkpoint");
    const auto retry=journal.compactUndo();const auto subsequent=failure.edit("4");
    check(retry.ok && retry.baseSeq==2 && subsequent.ok && subsequent.retentionWarning.empty(),
          "successful retry compacts deferred history and later commits clear the warning");
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
std::string internState(SQLite::Database& db,const Payload& state) {
    SQLite::Transaction transaction(db);const auto hash=history_detail::storeCheckpointContent(db,state);transaction.commit();return hash;
}
void contentAndBestEffort() {
    Project p;auto& db=p.store->db();
    const std::string extension="CREATE TABLE future_content(id INTEGER PRIMARY KEY,i INTEGER,r REAL,t TEXT,b BLOB,n TEXT) STRICT;";
    db.exec(extension+"INSERT INTO future_content VALUES(1,1,1.25,'1',X'',NULL),(2,2,-0.5,'α',X'0001FF','');");
    const auto digest=digestProject(*p.store).text;
    const auto initial=history_detail::checkpointState(*p.store);
    const auto hash=internState(db,initial);
    const auto rowCount=db.execAndGet("SELECT count(*) FROM history_state_rows").getInt64();
    const auto manifestCount=db.execAndGet("SELECT count(*) FROM history_state_manifests").getInt64();
    check(hash.size()==64 && rowCount>0 && manifestCount==1,"checkpoint content stores typed rows and one manifest by hash");
    const auto beforeRepeat=db.execAndGet("SELECT total_changes()").getInt64();
    check(internState(db,initial)==hash && db.execAndGet("SELECT count(*) FROM history_state_rows").getInt64()==rowCount && db.execAndGet("SELECT count(*) FROM history_state_manifests").getInt64()==manifestCount && db.execAndGet("SELECT total_changes()").getInt64()==beforeRepeat,"repeated checkpoint content reuses every immutable row and manifest");
    check(digestProject(*p.store).text==digest,"content-addressed history is excluded from the musical digest");
    Project target;target.store->db().exec(extension);{SQLite::Transaction restore(target.store->db());history_detail::installCheckpoint(*target.store,history_detail::readCheckpointContent(db,hash));restore.commit();}
    check(digestProject(*target.store).text==digest,"row-deduplicated checkpoint restores the same typed project");
    check(p.edit("Changed").ok,"content change fixture");
    const auto changed=internState(db,history_detail::checkpointState(*p.store));
    check(changed!=hash && db.execAndGet("SELECT count(*) FROM history_state_rows").getInt64()==rowCount+1 && db.execAndGet("SELECT count(*) FROM history_state_manifests").getInt64()==manifestCount+1,"a one-row edit adds exactly one content row and one manifest");
    db.exec("UPDATE project SET name='Untitled'");
    check(internState(db,history_detail::checkpointState(*p.store))==hash,"returning to an earlier state reuses its original content identity");
    Payload altered;std::string rowHash;
    for(const auto& table:initial.at("tables"))if(table.at("name")=="project"){
        altered=table.at("rows").at(0);const auto original=encodePayload(altered);rowHash=media::blake3Bytes(original).hex;
        const auto columns=table.at("columns").get<std::vector<std::string>>();const auto name=std::find(columns.begin(),columns.end(),"name");if(name==columns.end())throw std::runtime_error("project name column missing");
        altered.at(static_cast<std::size_t>(name-columns.begin()))=Payload::array({"text","Tampered valid row"});
    }
    const auto tampered=encodePayload(altered);{SQLite::Statement corrupt(db,"UPDATE history_state_rows SET data=? WHERE hash=?");corrupt.bind(1,tampered.data(),static_cast<int>(tampered.size()));corrupt.bind(2,rowHash);corrupt.exec();}
    bool refused=false;
    try{(void)history_detail::readCheckpointContent(db,hash);}catch(const std::exception&){refused=true;}
    check(refused,"corrupt content bytes cannot masquerade as a referenced row hash");
    refused=false;try{(void)internState(db,initial);}catch(const std::exception&){refused=true;}
    check(refused,"interning never trusts a corrupt pre-existing immutable object");

    Project failure;History history(*failure.store);check(history.setUndoRetentionPolicy(budget(2)).ok,"late-compaction failure policy");failure.edits(1,2);
    struct Observer final:AppliedObserver {unsigned deliveries=0;std::optional<std::int64_t> after;void committed(std::shared_ptr<const AppliedBatch> batch)override{++deliveries;after=batch->afterHead();}void sourceInvalidated(const AppliedSource&,AppliedSourceInvalidation)override{}};
    auto observer=std::make_shared<Observer>();auto subscription=appliedSource(*failure.store).observe(observer);check(subscription.ok,"observe the edit across optional retention failure");
    failure.store->db().exec("CREATE TRIGGER refuse_prune BEFORE DELETE ON ops WHEN OLD.seq=1 BEGIN SELECT RAISE(ABORT,'late prune failure'); END");
    bool hook=false;CommitOptions options;options.beforeCommit=[&](SQLite::Database& database,std::int64_t,std::string&){hook=database.execAndGet("SELECT name FROM project").getString()=="3";return true;};
    OpRequest request;request.opType="project.setName";request.payload={{"name","3"}};
    const auto edit=OpJournal(*failure.store).commit(std::span<const OpRequest>(&request,1),options);
    check(edit.ok && edit.error.empty() && edit.retentionWarning.find("late prune failure")!=std::string::npos && failure.name()=="3" && edit.seqs.size()==1,"late pruning failure preserves the complete actual edit and reports why");
    check(hook && observer->deliveries==1 && observer->after==history.head(),"caller hook and applied delivery still run after optional retention rollback");
    check(failure.store->db().execAndGet("SELECT count(*) FROM history_checkpoint").getInt64()==0 && failure.store->db().execAndGet("SELECT count(*) FROM history_state_rows").getInt64()==0 && failure.store->db().execAndGet("SELECT count(*) FROM history_state_manifests").getInt64()==0,"failed compaction rolls back its checkpoint, content objects and root retapping");
    check(OpJournal(*failure.store).count()==3 && failure.store->db().execAndGet("SELECT count(*) FROM op_clocks").getInt64()==3 && checkProject(*failure.store).clean(),"deferred compaction retains journal clocks and a valid undo tree");
    failure.store->db().exec("DROP TRIGGER refuse_prune");check(history.compactUndo().ok && history.canUndo(),"late-failure history remains compactable and undoable on retry");

    Project denied;
    sqlite3_set_authorizer(denied.store->db().getHandle(),+[](void*,int action,const char* operation,const char* name,const char*,const char*){
        return action==SQLITE_SAVEPOINT && operation && name && std::string_view(operation)=="BEGIN" && std::string_view(name)=="adi_optional_compaction"?SQLITE_DENY:SQLITE_OK;
    },nullptr);
    const auto deniedSavepoint=denied.edit("Still committed");sqlite3_set_authorizer(denied.store->db().getHandle(),nullptr,nullptr);
    check(deniedSavepoint.ok && !deniedSavepoint.retentionWarning.empty() && denied.name()=="Still committed","inability to start optional compaction cannot reject a valid edit");

    Project daily;History dailyHistory(*daily.store);check(dailyHistory.setUndoRetentionPolicy(budget(3)).ok,"daily non-anchor fixture policy");
    const auto automatic=dailyHistory.takeSnapshot("Daily",1,0,true);check(automatic.ok,"daily automatic root snapshot fixture");daily.edits(1,10);
    const auto retained=dailyHistory.compactUndo();check(retained.ok && !retained.protectedHistory && retained.baseSeq>0 && dailyHistory.listSnapshots().empty(),"automatic snapshots never block compaction and compacted daily metadata is removed");
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
    check(cap.ok && cap.baseSeq == 201 && cap.transactions == 1800 && OpJournal(*actual.store).count() == 1801,
          "default compaction triggers above 2000 and retains the 1800-step low-water budget");
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
        contentAndBestEffort();
        bytesAndDefault();
    } catch (const std::exception &error) {
        check(false, error.what());
    }
    std::printf("history compaction: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
