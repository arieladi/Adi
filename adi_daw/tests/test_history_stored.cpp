// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/applied_batches.hpp"
#include "adi/check.hpp"
#include "adi/digest.hpp"
#include "adi/engine/param_ops.hpp"
#include "adi/engine/session.hpp"
#include "adi/history.hpp"
#include "adi/history_content.hpp"
#include "adi/history_roots.hpp"
#include "adi/media/blake3.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/store.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <miniz.h>
#include <stdexcept>
namespace {
using namespace adi;
constexpr auto prepared = History::StoredRestore::SessionReloadPrepared;
int checks = 0, failures = 0;
void check(bool value, const std::string &message) {
    ++checks;
    if (!value) {
        ++failures;
        std::printf("FAIL: %s\n", message.c_str());
    }
}
void require(bool value, const std::string &message) {
    if (!value)
        throw std::runtime_error(message);
}
OpRequest request(const char *type, Payload payload) {
    OpRequest op;
    op.opType = type;
    op.payload = std::move(payload);
    return op;
}
struct Project {
    test::TempDirectory folder{"history_stored", "project"};
    std::unique_ptr<Store> store;
    Project() {
        StoreError error{};
        store = Store::create(folder.path() / "song.adi", error);
        require(store != nullptr, toString(error));
    }
    CommitResult commit(const char *type, Payload payload) {
        auto result = OpJournal(*store).commit(request(type, std::move(payload)));
        require(result.ok, result.error);
        return result;
    }
    void name(const std::string &value) { commit("project.setName", {{"name", value}}); }
    std::string name() { return store->db().execAndGet("SELECT name FROM project").getString(); }
    std::string digest() { return digestProject(*store).text; }
};
UndoRetentionPolicy budget(std::int64_t n) {
    UndoRetentionPolicy result;
    result.maxTransactions = n;
    return result;
}
std::int64_t rows(Store &store, const char *table) {
    return store.db().execAndGet("SELECT count(*) FROM " + std::string(table)).getInt64();
}
void savedPoints() {
    Project p;
    History history(*p.store);
    const auto empty = p.digest();
    const auto origin = history.takeSnapshot("Origin", 0, 0);
    require(origin.ok, origin.error);
    std::map<std::int64_t, std::string> states;
    states[origin.id] = empty;
    for (int i = 1; i <= 14; ++i) {
        p.name(i == 1 || i == 3 ? "Repeated" : std::to_string(i));
        const auto point = history.takeSnapshot("Named " + std::to_string(i), i, 0);
        require(point.ok, point.error);
        states[point.id] = p.digest();
    }
    const auto before = p.digest();
    const auto cap = history.setUndoRetentionPolicy(budget(3));
    check(cap.ok && !cap.protectedHistory && cap.transactions == 2,
          "many named states do not pin the active cap: " + cap.error);
    check(p.digest() == before && history.readTimeline().steps.size() == 2,
          "capture leaves live domain unchanged and hides saved metadata");
    auto points = history.listSnapshots();
    std::optional<std::string> first, third;
    for (const auto &point : points) {
        if (point.name == "Named 1")
            first = point.storedStateHash;
        if (point.name == "Named 3")
            third = point.storedStateHash;
    }
    check(first && third && first == third,
          "identical named states reuse the same manifest identity");
    const auto objectCount = rows(*p.store, "history_state_rows");
    check(objectCount > 0 && objectCount < rows(*p.store, "history_state_manifests") * 10,
          "unchanged project rows are shared across stored points");
    for (const auto &point : points) {
        const auto result = history.revertToSnapshot(point.id, 100, prepared);
        check(result.ok && p.digest() == states.at(point.id),
              "every original and named state reverts exactly: " + point.name + " " + result.error);
        check(history.readTimeline().error.empty(),
              "each restored line has a valid metadata timeline");
    }
    check(checkProject(*p.store).clean(), "all stored point content and graph pass integrity");
    const auto count = rows(*p.store, "history_state_rows");
    const auto compact = history.compactUndo();
    check(compact.ok && rows(*p.store, "history_state_rows") == count,
          "no-op compaction does not duplicate saved content");
    const auto path = p.store->path();
    p.store.reset();
    StoreError error{};
    p.store = Store::open(path, error, true);
    require(p.store != nullptr, toString(error));
    const auto changes = p.store->db().execAndGet("SELECT total_changes()").getInt64();
    const auto view = History(*p.store).readTimeline();
    Project opened;
    History::ReplayBaseResult loaded;
    {
        SQLite::Transaction read(p.store->db(), SQLite::TransactionBehavior::DEFERRED);
        loaded = History::restoreStoredSnapshot(*p.store, points[3].id, *opened.store);
        read.commit();
    }
    check(loaded.ok && loaded.retainedSeqs.empty() && opened.digest() == states.at(points[3].id),
          "a stored snapshot opens from a read-only source into an independent project");
    check(view.error.empty() && view.snapshots.size() == states.size() &&
              p.store->db().execAndGet("SELECT total_changes()").getInt64() == changes,
          "read-only reopen lists every saved point without writes");
}
void forestReplay() {
    Project p;
    History history(*p.store);
    const auto origin = history.takeSnapshot("Original", 0, 0);
    require(origin.ok, origin.error);
    for (int i = 1; i <= 8; ++i)
        p.name(std::to_string(i));
    require(history.undo().ok && history.undo().ok, "rewind old branch");
    p.name("B1");
    p.name("B2");
    const auto expected = p.digest();
    const auto cap = history.setUndoRetentionPolicy(budget(4));
    check(cap.ok && cap.transactions == 3 && !cap.protectedHistory,
          "forest retains global complete-transaction budget");
    Project target;
    History::ReplayBaseResult restored;
    {
        SQLite::Transaction read(p.store->db());
        restored = History::restoreReplayBase(*p.store, *target.store);
        require(restored.ok, restored.error);
        const auto log = OpJournal(*p.store).recent(100);
        for (const auto seq : restored.retainedSeqs) {
            const auto op = std::find_if(log.begin(), log.end(),
                                         [&](const auto &entry) { return entry.seq == seq; });
            require(op != log.end(), "retained replay record");
            target.commit(op->opType.c_str(), op->payload);
        }
        read.commit();
    }
    check(restored.retainedSeqs == std::vector<std::int64_t>({9, 10}) &&
              target.digest() == expected,
          "replay uses current ancestry and excludes the other retained branch");
    const auto view = history.readTimeline();
    check(view.error.empty() && view.steps.size() == 3 && view.snapshots.front().storedStateHash,
          "forest timeline hides synthetic origin and all baseline metadata");
    const auto internal =
        request("history.root", {{"state_hash", *view.snapshots.front().storedStateHash}});
    const auto head = history.head();
    const auto count = OpJournal(*p.store).count();
    check(!OpJournal(*p.store).commit(internal).ok && history.head() == head &&
              OpJournal(*p.store).count() == count,
          "callers cannot submit internal history metadata");
    std::int64_t old = 0;
    for (const auto &branch : history.branches())
        if (!branch.isCurrent)
            old = branch.id;
    require(old != 0, "old branch retained");
    check(history.switchToBranch(old, prepared).ok && p.name() == "8" && history.undo().ok &&
              p.name() == "7" && !history.canUndo(),
          "independent old baseline replays its retained suffix and undoes one "
          "real step");
    check(history.switchToBranch(1, prepared).ok && p.name() == "B2" && p.digest() == expected,
          "return from old root reconstructs current branch exactly");
    check(history.revertToSnapshot(origin.id, 20, prepared).ok && !history.canUndo(),
          "later synthetic origin remains independently restorable");
    Project empty;
    const auto original = History::restoreReplayBase(*p.store, *empty.store);
    check(original.ok && original.retainedSeqs.empty() && original.baseSeq > 10 &&
              empty.digest() == p.digest(),
          "numeric-later origin restores without replaying earlier unrelated "
          "edit rows");
    check(checkProject(*p.store).clean(), "forest after repeated navigation passes integrity");
}
void redoGap() {
    Project p;
    History h(*p.store);
    for (int i = 1; i <= 8; ++i)
        p.name(std::to_string(i));
    for (int i = 0; i < 5; ++i)
        require(h.undo().ok, "rewind redo gap");
    const auto before = p.digest();
    const auto cap = h.setUndoRetentionPolicy(budget(2));
    check(cap.ok && !cap.protectedHistory && cap.transactions == 1 && p.digest() == before &&
              !h.canUndo(),
          "compaction at an undone head preserves its state while pruning old "
          "redo bridges");
    std::int64_t saved = 0;
    for (const auto &branch : h.branches())
        if (!branch.isCurrent)
            saved = branch.id;
    check(saved != 0 && h.switchToBranch(saved, prepared).ok && p.name() == "8" && h.undo().ok &&
              p.name() == "7",
          "an unheld redo tip is named before pruning and its retained tail "
          "stays openable");
}
struct Observer final : AppliedObserver {
    std::vector<std::shared_ptr<const AppliedBatch>> batches;
    void committed(std::shared_ptr<const AppliedBatch> batch) override {
        batches.push_back(std::move(batch));
    }
    void sourceInvalidated(const AppliedSource &, AppliedSourceInvalidation) override {}
};
struct Device final : device::DeviceInstance {
    device::DeviceIdentity id;
    std::vector<std::uint8_t> bytes;
    Device() {
        id.format = "vst3";
        id.uid = "history.state";
    }
    const device::DeviceIdentity &identity() const noexcept override { return id; }
    bool loaded() const noexcept override { return true; }
    void process(const engine::NodeIo &io) noexcept override { device::passThrough(io); }
    std::vector<std::string> stateRoles() const override { return {"chunk"}; }
    std::vector<std::uint8_t> saveState(const std::string &) const override { return bytes; }
    bool loadState(const std::string &role, const std::vector<std::uint8_t> &value) override {
        if (role != "chunk")
            return false;
        bytes = value;
        return true;
    }
};
std::string blob(Project &p, const std::vector<std::uint8_t> &bytes) {
    const auto hash = media::blake3Bytes(std::span<const std::byte>(
        reinterpret_cast<const std::byte *>(bytes.data()), bytes.size()));
    require(static_cast<bool>(hash), "hash state");
    SQLite::Statement insert(p.store->db(),
                             "INSERT INTO state_blobs(hash_blake3,data,size_bytes) VALUES(?,?,?)");
    insert.bind(1, hash.hex);
    insert.bind(2, bytes.data(), static_cast<int>(bytes.size()));
    insert.bind(3, static_cast<std::int64_t>(bytes.size()));
    insert.exec();
    return hash.hex;
}
struct ZipReader {
    std::ifstream in;
    mz_zip_archive zip{};
    bool ready = false;
    static size_t input(void *opaque, mz_uint64 offset, void *data, size_t n) {
        auto &stream = static_cast<ZipReader *>(opaque)->in;
        stream.clear();
        stream.seekg(static_cast<std::streamoff>(offset));
        stream.read(static_cast<char *>(data), static_cast<std::streamsize>(n));
        return static_cast<size_t>(stream.gcount());
    }
    static size_t output(void *opaque, mz_uint64, const void *data, size_t n) {
        auto &out = *static_cast<std::ofstream *>(opaque);
        out.write(static_cast<const char *>(data), static_cast<std::streamsize>(n));
        return out ? n : 0;
    }
    explicit ZipReader(const std::filesystem::path &path)
        : in(path, std::ios::binary | std::ios::ate) {
        if (!in)
            return;
        const auto size = in.tellg();
        if (size < 0)
            return;
        zip.m_pIO_opaque = this;
        zip.m_pRead = input;
        ready = mz_zip_reader_init(&zip, static_cast<mz_uint64>(size), 0) != 0;
    }
    ~ZipReader() {
        if (ready)
            mz_zip_reader_end(&zip);
    }
    bool extract(const std::filesystem::path &dir) {
        if (!ready)
            return false;
        for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip); ++i) {
            mz_zip_archive_file_stat stat{};
            if (!mz_zip_reader_file_stat(&zip, i, &stat) || stat.m_method != 0)
                return false;
            const auto path = dir / media::pathFromUtf8(stat.m_filename);
            std::filesystem::create_directories(path.parent_path());
            std::ofstream out(path, std::ios::binary);
            if (!mz_zip_reader_extract_to_callback(&zip, i, output, &out, 0))
                return false;
        }
        return true;
    }
};

void runtimeRestore() {
    Project p;
    p.store->db().exec(
        "INSERT INTO tracks(id,kind,name) VALUES(9,'master','Main'),(10,'audio','Device'); INSERT "
        "INTO plugin_refs(id,format,uid,name,subtype) "
        "VALUES(1,'vst3','history.state','State','effect'); INSERT INTO "
        "device_chains(id,track_id) VALUES(10,10); INSERT INTO "
        "devices(id,chain_id,ord,plugin_ref_id,name) VALUES(1,10,0,1,'State')");
    const std::vector<std::uint8_t> early{0, 255, 17}, late{4, 0, 128, 255};
    const auto first = blob(p, early);
    p.commit("device.loadState",
             {{"dev", 1}, {"role", "chunk"}, {"hash", first}, {"hint", "early"}});
    History h(*p.store);
    const auto point = h.takeSnapshot("Earlier plugin", 1, 0);
    require(point.ok, point.error);
    p.name("2");
    p.name("3");
    require(h.setUndoRetentionPolicy(budget(2)).ok, "first plugin cap");
    const auto second = blob(p, late);
    p.commit("device.loadState",
             {{"dev", 1}, {"role", "chunk"}, {"hash", second}, {"hint", "late"}});
    p.name("5");
    p.name("6");
    engine::DeviceLoader loader = [](const engine::DeviceRequest &,
                                     std::string &) -> std::unique_ptr<device::DeviceInstance> {
        return std::make_unique<Device>();
    };
    engine::Session old;
    require(old.load(*p.store, loader, {}), old.error());
    engine::ParamOps params;
    require(params.attachSession(old) == 1, "attach live stored-state receiver");
    const auto before = p.digest();
    const auto refused = h.revertToSnapshot(point.id, 2);
    check(!refused.ok && refused.sessionReloadRequired && p.digest() == before &&
              params.appliedDeliveryStatus().state != engine::AppliedDeliveryState::Invalidated,
          "default stored restore leaves live binding and SQL unchanged");
    const auto observer = std::make_shared<Observer>();
    auto subscription = appliedSource(*p.store).observe(observer);
    require(subscription.ok, subscription.error);
    const auto &batches = observer->batches;
    // No callback runs in this native owner-thread fixture. The caller has
    // fenced audio before opting in and constructs a fresh Session afterwards.
    const auto result = h.revertToSnapshot(point.id, 3, prepared);
    check(result.ok && result.sessionReloadRequired && rows(*p.store, "state_blobs") == 2,
          "stored restore retains immutable resources from both branches");
    check(params.appliedDeliveryStatus().state == engine::AppliedDeliveryState::Invalidated,
          "full-state reset invalidates old ParamOps delivery");
    check(batches.size() == 1 && batches.front()->kind() == AppliedBatchKind::ReplayBaseReset &&
              batches.front()->operations().empty(),
          "stored restore publishes one reset without fabricated edit records");
    engine::Session fresh;
    require(fresh.load(*p.store, loader, {}), fresh.error());
    const auto *entry = fresh.entryFor(1);
    check(entry && !entry->placeholder &&
              fresh.devices().deviceAt(entry->hostIndex).saveState("chunk") == early,
          "fresh Session rehydrates actual archived plugin bytes");
    std::int64_t newer = 0;
    for (const auto &branch : h.branches())
        if (!branch.isCurrent)
            newer = branch.id;
    require(newer != 0, "late branch preserved");
    check(h.switchToBranch(newer, prepared).ok && rows(*p.store, "state_blobs") == 2,
          "return to later branch keeps the shared resource pool");
    Project target;
    const auto base = History::restoreReplayBase(*p.store, *target.store);
    check(base.ok && rows(*target.store, "state_blobs") == 2,
          "replay baseline transfers resources needed by retained steps");
    SQLite::Statement corrupt(p.store->db(),
                              "UPDATE state_blobs SET data=X'FF',size_bytes=1 WHERE hash_blake3=?");
    corrupt.bind(1, first);
    corrupt.exec();
    const auto corrupted = p.digest();
    const auto currentHead = h.head();
    const auto refusedCollision = h.revertToSnapshot(point.id, 4, prepared);
    check(!refusedCollision.ok && p.digest() == corrupted && h.head() == currentHead,
          "an existing pool hash with different bytes refuses stored restore atomically");
}
void archivedMedia() {
    namespace fs = std::filesystem;
    Project p;
    History history(*p.store);
    fs::create_directories(p.folder.path() / "external");
    const auto source = p.folder.path() / "external/asset.bin";
    {
        std::ofstream output(source, std::ios::binary);
        output << "stored-only media bytes";
    }
    const auto imported = media::importMedia(*p.store, source, 10, 1);
    require(imported.ok, imported.error);
    const auto named = history.takeSnapshot("Media exists", 1, 0);
    require(named.ok, named.error);
    p.commit("media.unlink", {{"id", 10}});
    for (int i = 0; i < 6; ++i)
        p.name(std::to_string(i));
    require(history.setUndoRetentionPolicy(budget(2)).ok, "archived media compaction");
    check(rows(*p.store, "media_files") == 0 &&
              p.store->db()
                      .execAndGet("SELECT count(*) FROM ops WHERE op_type LIKE 'media.%'")
                      .getInt() == 0,
          "media fixture survives only in an archived state");
    const auto digest = p.digest();
    const auto hash = history.listSnapshots().front().storedStateHash;
    fs::create_directories(p.folder.path() / "copy/deep");
    const auto path = p.folder.path() / "copy/deep/Copy.adi";
    const auto copied = p.store->saveCopy(path);
    require(copied.ok, copied.error);
    check(p.digest() == digest && history.listSnapshots().front().storedStateHash == hash,
          "Save Copy relocation leaves original stored roots unchanged");
    StoreError error{};
    auto copy = Store::open(path, error);
    require(copy != nullptr, toString(error));
    const auto resolves = [&](Store &store) {
        std::string why;
        const auto resolved =
            media::resolveMedia(media::projectFolder(store), media::mediaRow(store.db(), 10), why);
        return !resolved.empty() && fs::is_regular_file(resolved);
    };
    check(History(*copy).revertToSnapshot(named.id, 2, prepared).ok && resolves(*copy),
          "copy restores and resolves media absent from current rows and all "
          "retained ops");
    copy.reset();
    const auto zip = p.folder.path() / "Stored.zip";
    const auto exported = p.store->collectAndExport(zip);
    require(exported.ok, exported.error);
    const auto extracted = p.folder.path() / "extracted";
    {
        ZipReader archive(zip);
        require(archive.extract(extracted), "extract stored-only archive");
    }
    fs::path extractedProject;
    for (const auto &file : fs::recursive_directory_iterator(extracted))
        if (file.path().extension() == ".adi")
            extractedProject = file.path();
    require(!extractedProject.empty(), "exported project exists");
    auto exportedStore = Store::open(extractedProject, error);
    require(exportedStore != nullptr, toString(error));
    fs::remove(source);
    check(History(*exportedStore).revertToSnapshot(named.id, 3, prepared).ok &&
              resolves(*exportedStore),
          "export contains and resolves archived-only media after its original "
          "is removed");
    exportedStore.reset();
    // Collection needs the source again, and must make stored-only bytes local.
    {
        std::ofstream output(source, std::ios::binary);
        output << "stored-only media bytes";
    }
    const auto collected = p.store->collectAllAndSave();
    require(collected.ok, collected.error);
    fs::remove(source);
    check(history.revertToSnapshot(named.id, 4, prepared).ok && resolves(*p.store),
          "Collect All includes immutable saved roots and rebases their media");
    check(checkProject(*p.store).clean(),
          "relocated archived roots retain consistent hashes and metadata");
}
void incompleteAnchor() {
    Project p;
    const auto batch = OpJournal(*p.store).commit(
        std::vector<OpRequest>{request("project.setName", {{"name", "one"}}),
                               request("project.setName", {{"name", "two"}})});
    require(batch.ok, batch.error);
    History h(*p.store);
    const auto point = h.takeSnapshot("Whole batch", 1, 0);
    require(point.ok, point.error);
    Project target;
    const auto targetDigest = target.digest();
    check(!History::restoreStoredSnapshot(*p.store, point.id, *target.store).ok &&
              target.digest() == targetDigest,
          "a snapshot still in stepwise history is not mistaken for an archived baseline");
    SQLite::Statement split(p.store->db(), "UPDATE history_snapshots SET op_seq=? WHERE id=?");
    split.bind(1, batch.seqs.front());
    split.bind(2, point.id);
    split.exec();
    for (int i = 3; i <= 7; ++i)
        p.name(std::to_string(i));
    const auto before = p.digest();
    const auto count = OpJournal(*p.store).count();
    const auto result = h.setUndoRetentionPolicy(budget(2));
    check(!result.ok && p.digest() == before && OpJournal(*p.store).count() == count &&
              rows(*p.store, "history_roots") == 0,
          "a corrupt snapshot splitting a transaction refuses compaction before graph changes");
}

void legacyCheckpoint() {
    Project p;
    p.name("1");
    p.name("2");
    const auto snapshot = History(*p.store).takeSnapshot("Legacy base", 1, 0);
    require(snapshot.ok, snapshot.error);
    p.name("3");
    p.name("4");
    auto state = history_detail::checkpointAt(*p.store, 2);
    state["schema_minor"] = 20;
    const auto bytes = encodePayload(state);
    {
        SQLite::Transaction transaction(p.store->db());
        p.store->db().exec("UPDATE ops SET parent_seq=NULL,inverse=NULL,sel_before=NULL WHERE "
                           "seq=2; DELETE FROM ops WHERE seq<2");
        SQLite::Statement insert(p.store->db(),
                                 "INSERT INTO history_checkpoint(id,base_seq,state) VALUES(1,2,?)");
        insert.bind(1, bytes.data(), static_cast<int>(bytes.size()));
        insert.exec();
        const auto hash = history_detail::storeCheckpointContent(p.store->db(), state);
        SQLite::Statement content(
            p.store->db(), "INSERT INTO history_checkpoint_content(id,state_hash) VALUES(1,?)");
        content.bind(1, hash);
        content.exec();
        transaction.commit();
    }
    const auto path = p.store->path();
    p.store.reset();
    {
        SQLite::Database db(path.string(), SQLite::OPEN_READWRITE);
        db.exec("DROP TABLE history_origin_root; DROP TABLE history_roots; PRAGMA "
                "user_version=1020; UPDATE adi_meta SET value='20' WHERE key='schema_minor'");
    }
    StoreError error{};
    p.store = Store::open(path, error);
    require(p.store != nullptr, toString(error));
    check(p.store->schemaMinor() == 21 && rows(*p.store, "history_roots") == 0,
          "real legacy checkpoint file upgrades additively without inventing archived states");
    for (int i = 5; i <= 9; ++i)
        p.name(std::to_string(i));
    History h(*p.store);
    const auto cap = h.setUndoRetentionPolicy(budget(2));
    check(cap.ok && !cap.protectedHistory && cap.baseSeq == 8 && cap.transactions == 1,
          "legacy checkpoint prefix converts to independent roots during the next cap");
    check(h.revertToSnapshot(snapshot.id, 2, prepared).ok && p.name() == "2" && !h.canUndo(),
          "legacy named base remains restorable after compaction passes it");
    const auto hash = h.listSnapshots().front().storedStateHash;
    require(hash.has_value(), "legacy point converted");
    const auto normalized = history_detail::readCheckpointContent(p.store->db(), *hash);
    check(normalized.at("schema_minor") == 21 && checkProject(*p.store).clean(),
          "restored legacy image is normalized and versioned for schema21 semantics");
}

void corruptionRollback() {
    Project p;
    History h(*p.store);
    p.name("saved");
    const auto snapshot = h.takeSnapshot("Saved", 1, 0);
    require(snapshot.ok, snapshot.error);
    for (int i = 0; i < 6; ++i)
        p.name(std::to_string(i));
    require(h.setUndoRetentionPolicy(budget(2)).ok, "corruption cap");
    const auto point = h.listSnapshots().front();
    require(point.storedStateHash.has_value(), "stored corruption point");
    Project empty;
    const auto emptyDigest = empty.digest();
    const auto absent = History::restoreStoredSnapshot(*p.store, 999999, *empty.store);
    check(!absent.ok && empty.digest() == emptyDigest && OpJournal(*empty.store).count() == 0,
          "missing snapshot cannot alter an open target");
    const auto before = p.digest();
    const auto head = h.head();
    const auto count = OpJournal(*p.store).count();
    p.store->db().exec("UPDATE history_state_manifests SET data=X'00' WHERE hash='" +
                       *point.storedStateHash + "'");
    const auto failed = h.revertToSnapshot(snapshot.id, 2, prepared);
    check(!failed.ok && p.digest() == before && h.head() == head &&
              OpJournal(*p.store).count() == count,
          "tampered stored content refuses restore with complete state/head "
          "rollback");
    check(!checkProject(*p.store).clean(),
          "integrity checking verifies stored state hashes beyond current "
          "baseline");
    Project failure;
    History owned(*failure.store);
    failure.name("named");
    const auto anchor = owned.takeSnapshot("Named", 1, 0);
    require(anchor.ok, anchor.error);
    failure.name("2");
    failure.name("3");
    failure.store->db().exec(
        "CREATE TRIGGER refuse_root BEFORE INSERT ON history_roots BEGIN SELECT "
        "RAISE(ABORT,'stored-root failure'); END");
    const auto changes = failure.digest();
    const auto rowsBefore = OpJournal(*failure.store).count();
    const auto cap = owned.setUndoRetentionPolicy(budget(2));
    check(!cap.ok && failure.digest() == changes &&
              OpJournal(*failure.store).count() == rowsBefore &&
              rows(*failure.store, "history_roots") == 0 &&
              rows(*failure.store, "history_state_rows") == 0,
          "failed stored-root publication rolls back capture objects, graph and "
          "pruning");
}

} // namespace
int main() {
    try {
        savedPoints();
        forestReplay();
        redoGap();
        runtimeRestore();
        archivedMedia();
        incompleteAnchor();
        legacyCheckpoint();
        corruptionRollback();
    } catch (const std::exception &error) {
        check(false, std::string("exception: ") + error.what());
    }
    std::printf("history stored: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
