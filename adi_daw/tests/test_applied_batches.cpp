// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/applied_batches.hpp"
#include "adi/digest.hpp"
#include "adi/history.hpp"
#include "adi/media/blake3.hpp"
#include "adi/store.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace adi;
using Bytes = std::vector<std::byte>;
using Batch = std::shared_ptr<const AppliedBatch>;
int checks = 0, failures = 0;
bool check(bool value, const std::string& message) {
    ++checks;
    if (!value) { ++failures; std::printf("FAIL: %s\n", message.c_str()); }
    return value;
}
void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}
Bytes blob(const SQLite::Column& column) {
    if (column.getBytes() == 0) return {};
    const auto* first = static_cast<const std::byte*>(column.getBlob());
    return {first, first + column.getBytes()};
}
Payload binary(const Bytes& bytes) {
    std::vector<std::uint8_t> values; values.reserve(bytes.size());
    for (auto value : bytes) values.push_back(std::to_integer<std::uint8_t>(value));
    return Payload::binary(std::move(values));
}
Payload tagged(int type, Payload value) { return {{"type", type}, {"value", std::move(value)}}; }
// Explicit storage tags make INTEGER 1 different from REAL 1.0. The public
// event carries ordinary native column maps, independently tagged only here.
Payload tagWireRow(const Payload& row) {
    require(row.is_object(), "event typed row is an ordinary column map");
    Payload result = Payload::object();
    for (auto it = row.begin(); it != row.end(); ++it) {
        const auto& value = it.value();
        if (value.is_null()) result[it.key()] = tagged(SQLITE_NULL, nullptr);
        else if (value.is_number_integer()) result[it.key()] = tagged(SQLITE_INTEGER, value.get<std::int64_t>());
        else if (value.is_number_float()) result[it.key()] = tagged(SQLITE_FLOAT, std::bit_cast<std::uint64_t>(value.get<double>()));
        else if (value.is_string()) result[it.key()] = tagged(SQLITE_TEXT, value);
        else if (value.is_binary()) result[it.key()] = tagged(SQLITE_BLOB, value);
        else throw std::runtime_error("event row contains a non-SQL value");
    }
    return result;
}
std::vector<Payload> typedQuery(SQLite::Database& db, const std::string& sql) {
    SQLite::Statement query(db, sql); std::vector<Payload> rows;
    while (query.executeStep()) {
        Payload row = Payload::object();
        for (int i = 0; i < query.getColumnCount(); ++i) {
            const auto cell = query.getColumn(i); Payload value;
            switch (cell.getType()) {
                case SQLITE_INTEGER: value = cell.getInt64(); break;
                case SQLITE_FLOAT: value = std::bit_cast<std::uint64_t>(cell.getDouble()); break;
                case SQLITE_TEXT: value = cell.getString(); break;
                case SQLITE_BLOB: value = binary(blob(cell)); break;
                case SQLITE_NULL: value = nullptr; break;
                default: throw std::runtime_error("unexpected SQLite storage class");
            }
            row[query.getColumnName(i)] = tagged(cell.getType(), std::move(value));
        }
        rows.push_back(std::move(row));
    }
    return rows;
}
std::vector<Payload> tagWireRows(const std::vector<Payload>& rows) {
    std::vector<Payload> result; result.reserve(rows.size());
    for (const auto& row : rows) result.push_back(tagWireRow(row));
    return result;
}
using State = std::map<std::string, std::vector<Payload>>;
State fullState(Store& store) {
    const std::array<std::pair<const char*, const char*>, 15> tables{{
        {"project", "id"}, {"tracks", "id"}, {"device_chains", "id"}, {"devices", "id"},
        {"plugin_refs", "id"}, {"plugin_params", "device_id,param_id"}, {"plugin_state", "device_id,stream_role"},
        {"state_blobs", "hash_blake3"}, {"ops", "seq"}, {"op_clocks", "seq"},
        {"op_clients", "client_id"}, {"op_branches", "id"}, {"history_snapshots", "id"},
        {"history_checkpoint", "id"}, {"session_state", "key"}}};
    State result;
    for (const auto& [table, ordering] : tables)
        result.emplace(table, typedQuery(store.db(), std::string("SELECT * FROM ") + table + " ORDER BY " + ordering));
    return result;
}
OpRequest request(const char* type, Payload payload) {
    OpRequest result; result.opType = type; result.payload = std::move(payload); result.label = "Applied batch fixture";
    return result;
}
OpRequest param(std::int64_t device, const char* id, double norm, const char* display = "") {
    return request("device.setParam", {{"dev", device}, {"param", id}, {"norm", norm}, {"real", nullptr}, {"display", display}});
}
OpRequest clearParam(std::int64_t device, const char* id) {
    return request("device.setParam", {{"dev", device}, {"param", id}, {"norm", nullptr}});
}
OpRequest state(std::int64_t device, const std::string& hash, const char* role = "component") {
    return request("device.loadState", {{"dev", device}, {"role", role}, {"hash", hash}, {"hint", "opaque fixture"}});
}
OpRequest clearState(std::int64_t device, const char* role = "component") {
    return request("device.loadState", {{"dev", device}, {"role", role}, {"hash", nullptr}});
}
OpRequest rename(const char* name, std::int64_t track = 2) {
    return request("track.rename", {{"id", track}, {"name", name}});
}
struct Project {
    test::TempDirectory folder;
    std::unique_ptr<Store> store;
    explicit Project(const char* name) : folder("applied_batches", name) {
        StoreError error{}; store = Store::createEmpty(folder.path() / "song.adi", error);
        require(bool(store), std::string("create: ") + toString(error));
        store->db().exec("INSERT INTO project(id,name) VALUES(1,'Applied batches');"
            "INSERT INTO tracks(id,kind,name) VALUES(1,'midi','Instrument'),(2,'audio','Foreign');"
            "INSERT INTO plugin_refs(id,format,uid,name) VALUES(1,'vst3','test.one','One'),(2,'clap','test.two','Two');"
            "INSERT INTO device_chains(id,track_id) VALUES(10,1);"
            "INSERT INTO devices(id,chain_id,plugin_ref_id,name) VALUES(-2,10,NULL,'No reference'),(100,10,1,'First'),(200,10,2,'Second');"
            "ALTER TABLE devices ADD COLUMN future_any ANY;"
            "ALTER TABLE devices ADD COLUMN future_blob BLOB DEFAULT X'00FF';"
            "ALTER TABLE plugin_refs ADD COLUMN future_any ANY;"
            "ALTER TABLE plugin_params ADD COLUMN future_any ANY;"
            "ALTER TABLE plugin_params ADD COLUMN future_blob BLOB DEFAULT X'';"
            "UPDATE devices SET future_any=1 WHERE id=100;"
            "UPDATE devices SET future_any=CAST(1 AS REAL) WHERE id=200;"
            "UPDATE plugin_refs SET future_any=CAST(1 AS REAL) WHERE id=1;"
            "UPDATE plugin_refs SET future_any=X'00FE' WHERE id=2");
    }
    CommitResult commit(const std::vector<OpRequest>& operations) {
        const auto result = OpJournal(*store).commit(operations);
        require(result.ok && result.seqs.size() == operations.size(), "fixture commit: " + result.error);
        return result;
    }
    CommitResult commit(const OpRequest& operation) { return commit(std::vector<OpRequest>{operation}); }
    std::string addBlob(const Bytes& bytes) {
        const auto hash = media::blake3Bytes(bytes); require(bool(hash), "fixture content hash");
        SQLite::Statement insert(store->db(), "INSERT OR IGNORE INTO state_blobs(hash_blake3,data,size_bytes) VALUES(?,?,?)");
        insert.bind(1, hash.hex); insert.bind(2, bytes.data(), static_cast<int>(bytes.size()));
        insert.bind(3, static_cast<std::int64_t>(bytes.size())); insert.exec(); return hash.hex;
    }
    void reopen() {
        const auto path = store->path(); store.reset(); StoreError error{};
        store = Store::open(path, error); require(bool(store), std::string("reopen: ") + toString(error));
    }
};
struct Observer : AppliedObserver {
    Store* readable = nullptr;
    std::vector<Batch> batches;
    std::vector<std::pair<AppliedSource, AppliedSourceInvalidation>> invalidations;
    std::function<void(const Batch&)> onCommit;
    std::function<void()> onInvalidation;
    void committed(Batch batch) override {
        require(bool(batch), "observer receives an owned batch");
        if (readable) {
            check(sqlite3_get_autocommit(readable->db().getHandle()) != 0,
                "callback runs after SQL transaction closes");
            check(OpJournal(*readable).headSeq() == batch->afterHead() && appliedSource(*readable).sameConnection(batch->source()),
                "callback observes final committed head on its exact connection domain");
        }
        batches.push_back(std::move(batch));
        if (onCommit) onCommit(batches.back());
    }
    void sourceInvalidated(const AppliedSource& source, AppliedSourceInvalidation reason) override {
        check(!source.alive(), "source is already invalid before invalidation callback");
        invalidations.emplace_back(source, reason);
        if (onInvalidation) onInvalidation();
    }
};
AppliedSubscription subscribe(const AppliedSource& source, const std::shared_ptr<AppliedObserver>& observer) {
    auto result = source.observe(observer); require(result.ok, "subscribe: " + result.error);
    return std::move(result.subscription);
}
struct Expected {
    std::string recorded, dispatched;
    Payload payload;
    std::int64_t seq = 0, txn = 0;
    bool inverse = false;
    std::shared_ptr<const void> runtimeProof{};
};
Expected forward(const OpRequest& operation, const CommitResult& result, std::size_t index = 0) {
    require(index < result.seqs.size(), "expected forward sequence exists");
    return {operation.opType, operation.opType, operation.payload, result.seqs[index], result.txnId, false, operation.runtimeProof};
}
Expected inverse(const OpRequest& original, const OpRequest& actual, const CommitResult& result, std::size_t index = 0) {
    require(index < result.seqs.size(), "expected inverse sequence exists");
    return {original.opType, actual.opType, actual.payload, result.seqs[index], result.txnId, true};
}
void expect(const Batch& batch, AppliedBatchKind kind, std::optional<std::int64_t> before,
            std::optional<std::int64_t> after, const std::vector<Expected>& expected, const std::string& label) {
    require(bool(batch), "batch exists for " + label);
    check(batch->kind() == kind && batch->beforeHead() == before && batch->afterHead() == after,
        label + ": kind and null-aware head boundaries");
    if (!check(batch->operations().size() == expected.size(), label + ": exact executed operation count")) return;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const auto& actual = batch->operations()[i]; const auto& want = expected[i];
        check(actual.recordedType == want.recorded && actual.dispatchedType == want.dispatched &&
            encodePayload(actual.payload) == encodePayload(want.payload) && actual.seq == want.seq &&
            actual.txnId == want.txn && actual.inverse == want.inverse &&
            actual.runtimeProof == (kind == AppliedBatchKind::Commit ? want.runtimeProof : std::shared_ptr<const void>{}),
            label + ": literal dispatched payload/provenance at " + std::to_string(i));
    }
}
Batch latest(const std::shared_ptr<Observer>& observer) {
    require(!observer->batches.empty(), "observer has a batch"); return observer->batches.back();
}

void firstTouchAndSequences() {
    Project project("first_touch"); const auto source = appliedSource(*project.store);
    require(source.alive() && source.sequence() == 0, "fresh connection domain starts at zero");
    const auto unobserved = project.commit(rename("Unobserved"));
    check(source.sequence() == 1, "successful publication advances sequence without any observers");
    auto observer = std::make_shared<Observer>(); observer->readable = project.store.get();
    auto token = subscribe(source, observer);
    std::vector<OpRequest> operations{param(100, "P", .25, "opening"), rename("Between"),
        param(200, "Q", .5), param(100, "P", .75, "final")};
    const std::shared_ptr<const void> opaque = std::make_shared<const std::string>("unknown opaque receipt");
    operations[0].runtimeProof = opaque; operations[3].runtimeProof = opaque;
    operations[1].runtimeProof = std::make_shared<const int>(77);
    const auto committed = project.commit(operations); const auto head = committed.seqs.back();
    expect(latest(observer), AppliedBatchKind::Commit, unobserved.seqs.back(), head,
        {forward(operations[0], committed, 0), forward(operations[1], committed, 1),
         forward(operations[2], committed, 2), forward(operations[3], committed, 3)}, "interleaved forward batch");
    const auto retained = latest(observer);
    check(retained->operations().size() == operations.size() && retained->operations()[0].runtimeProof == opaque &&
        retained->operations()[3].runtimeProof == opaque,
        "normal Commit carries unknown opaque proof by pointer identity without interpreting it");
    const auto logged = OpJournal(*project.store).recent(4);
    require(logged.size() == operations.size(), "all first-touch operations are in log");
    for (std::size_t i = 0; i < operations.size(); ++i)
        check(encodePayload(logged[operations.size() - 1 - i].payload) == encodePayload(operations[i].payload),
            "opaque runtime proof adds no field or byte to persisted operation payload");
    check(retained->sequence() == 2 && source.sequence() == 2 && token.lastDeliveredSequence() == 2 && token.active(),
        "batch/source/subscription agree on successful delivery sequence");
    check(retained->finalParameters().empty() && retained->deviceTargets().size() == 2 && retained->stateBlobs().empty(),
        "parameter-only batch has target definitions but no state reconciliation or invented resources");
    const auto count = OpJournal(*project.store).count();
    require(History(*project.store).undo().ok, "first-touch undo");
    expect(latest(observer), AppliedBatchKind::Undo, head, unobserved.seqs.back(),
        {inverse(operations[3], param(100, "P", .25, "opening"), committed, 3),
         inverse(operations[2], clearParam(200, "Q"), committed, 2),
         inverse(operations[1], rename("Unobserved"), committed, 1),
         inverse(operations[0], clearParam(100, "P"), committed, 0)}, "first-touch inverse order");
    check(typedQuery(project.store->db(), "SELECT * FROM plugin_params").empty() && OpJournal(*project.store).count() == count,
        "Undo emits opening values then clears while leaving no parameter rows or new history");
    token.reset(); observer->readable = nullptr; project.reopen();
    const auto reopened = appliedSource(*project.store); auto newObserver = std::make_shared<Observer>();
    newObserver->readable = project.store.get(); auto newToken = subscribe(reopened, newObserver);
    check(!source.alive() && !reopened.sameConnection(source) && !reopened.isReplacementOf(source) &&
        !source.isReplacementOf(reopened), "reopen creates a distinct domain without SaveAs replacement provenance");
    require(History(*project.store).redo().ok, "first-touch reopened redo");
    expect(latest(newObserver), AppliedBatchKind::Redo, unobserved.seqs.back(), head,
        {forward(operations[0], committed, 0), forward(operations[1], committed, 1),
         forward(operations[2], committed, 2), forward(operations[3], committed, 3)}, "reopened literal redo");
    require(History(*project.store).undo().ok, "second first-touch undo");
    expect(latest(newObserver), AppliedBatchKind::Undo, head, unobserved.seqs.back(),
        {inverse(operations[3], param(100, "P", .25, "opening"), committed, 3),
         inverse(operations[2], clearParam(200, "Q"), committed, 2),
         inverse(operations[1], rename("Unobserved"), committed, 1),
         inverse(operations[0], clearParam(100, "P"), committed, 0)}, "second literal undo");
    check(retained->sequence() == 2 && retained->operations().size() == 4 &&
        encodePayload(retained->operations()[0].payload) == encodePayload(operations[0].payload),
        "retained immutable event survives source destruction, reopen and later navigation");
    static_cast<void>(newToken);
}

void stateOrderAndTypedResources() {
    Project project("state_resources");
    const Bytes a{std::byte{0x00}, std::byte{0x41}, std::byte{0xff}}, b{std::byte{0x42}, std::byte{0x00}};
    const auto hashA = project.addBlob(a), hashB = project.addBlob(b);
    auto observer = std::make_shared<Observer>(); observer->readable = project.store.get();
    auto token = subscribe(appliedSource(*project.store), observer);
    const std::vector<OpRequest> operations{state(100, hashA), param(100, "z", .35),
        state(200, hashA), state(100, hashB), param(100, "a", .6), clearState(-2),
        request("device.remove", {{"id", 200}})};
    CommitOptions options;
    options.beforeCommit = [](SQLite::Database& db, std::int64_t, std::string&) {
        db.exec("UPDATE plugin_params SET future_any=1 WHERE param_id='a';"
            "UPDATE plugin_params SET future_any=CAST(1 AS REAL),future_blob=X'005AFF' WHERE param_id='z';"
            "UPDATE devices SET future_blob=X'' WHERE id=100;"
            "UPDATE plugin_refs SET future_any='late hook definition' WHERE id=1");
        return true;
    };
    const auto committed = OpJournal(*project.store).commit(operations, options);
    require(committed.ok && committed.seqs.size() == operations.size(), "resource batch commit: " + committed.error);
    std::vector<Expected> execution;
    for (std::size_t i = 0; i < operations.size(); ++i) execution.push_back(forward(operations[i], committed, i));
    const auto batch = latest(observer);
    expect(batch, AppliedBatchKind::Commit, {}, committed.seqs.back(), execution, "mixed state execution order");
    const auto* capturedA = batch->stateBlob(hashA); const auto* capturedB = batch->stateBlob(hashB);
    check(batch->stateBlobs().size() == 2 && capturedA && capturedB && capturedA->bytes == a && capturedB->bytes == b &&
        batch->stateBlob(std::string(64, '0')) == nullptr,
        "actual loadState references own full bytes once per hash, including superseded chunks");
    require(batch->deviceTargets().size() == 3 && batch->finalParameters().size() == 3,
        "all state-touched targets and final parameter sets are present");
    const std::array<std::int64_t, 3> ids{-2, 100, 200};
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const auto& target = batch->deviceTargets()[i]; const auto& projection = batch->finalParameters()[i];
        check(target.deviceId == ids[i] && projection.deviceId == ids[i], "target/projection order uses signed device identity");
        const auto devices = typedQuery(project.store->db(), "SELECT * FROM devices WHERE id=" + std::to_string(ids[i]));
        const auto params = typedQuery(project.store->db(), "SELECT * FROM plugin_params WHERE device_id=" + std::to_string(ids[i]) + " ORDER BY param_id");
        check(projection.devicePresent == !devices.empty() && tagWireRows(projection.rows) == params,
            "final parameter projection captures complete final typed rows and explicit empty/absent sets");
        check(target.device.has_value() == !devices.empty() &&
            (!target.device || tagWireRow(*target.device) == devices.front()),
            "target captures complete final device definition after late hook work");
        if (ids[i] == 100) {
            const auto refs = typedQuery(project.store->db(), "SELECT * FROM plugin_refs WHERE id=1");
            require(refs.size() == 1, "expected plugin reference");
            check(target.pluginRef && tagWireRow(*target.pluginRef) == refs.front(),
                "target owns final complete plugin reference including unknown text/REAL/BLOB fields");
            require(projection.rows.size() == 2, "two final parameter rows");
            check(projection.rows[0].at("param_id") == "a" && projection.rows[1].at("param_id") == "z" &&
                projection.rows[0].at("future_any").is_number_integer() && projection.rows[1].at("future_any").is_number_float(),
                "parameter reconciliation sorts IDs and preserves equal-valued INTEGER versus REAL");
        } else check(!target.pluginRef, "missing or reference-less device has explicit absent plugin reference");
    }
    const auto capturedRows = tagWireRows(batch->finalParameters()[1].rows);
    project.store->db().exec("UPDATE plugin_params SET future_any='later writer';DELETE FROM plugin_state");
    project.store->db().exec("DELETE FROM state_blobs");
    check(batch->stateBlob(hashA) && batch->stateBlob(hashA)->bytes == a &&
        batch->stateBlob(hashB) && batch->stateBlob(hashB)->bytes == b &&
        tagWireRows(batch->finalParameters()[1].rows) == capturedRows,
        "delayed retained event owns chunk bytes and typed projection after source rows change/disappear");
    static_cast<void>(token);

    Project ordered("chunk_opener_order");
    const auto firstHash = ordered.addBlob(a), secondHash = ordered.addBlob(b);
    auto listener = std::make_shared<Observer>(); listener->readable = ordered.store.get();
    auto subscription = subscribe(appliedSource(*ordered.store), listener);
    const std::vector<OpRequest> chunks{state(100, firstHash), state(100, secondHash)};
    const auto result = ordered.commit(chunks);
    require(History(*ordered.store).undo().ok, "state opener undo");
    expect(latest(listener), AppliedBatchKind::Undo, result.seqs.back(), {},
        {inverse(chunks[1], state(100, firstHash), result, 1), inverse(chunks[0], clearState(100), result, 0)},
        "state Undo restores opener before explicit clear");
    check(latest(listener)->stateBlobs().size() == 1 && latest(listener)->stateBlob(firstHash) &&
        latest(listener)->finalParameters().size() == 1 && latest(listener)->finalParameters()[0].rows.empty(),
        "state inverse captures only actually referenced chunk and explicit empty parameter set");
    require(History(*ordered.store).redo().ok, "state opener redo");
    expect(latest(listener), AppliedBatchKind::Redo, {}, result.seqs.back(),
        {forward(chunks[0], result, 0), forward(chunks[1], result, 1)}, "state opener literal redo");
    static_cast<void>(subscription);
}

void branchesAndRevert() {
    Project project("branch_paths"); auto observer = std::make_shared<Observer>();
    observer->readable = project.store.get(); auto token = subscribe(appliedSource(*project.store), observer);
    const auto rootSnapshot = History(*project.store).takeSnapshot("Root", 10, 0);
    require(rootSnapshot.ok, "root snapshot");
    const auto commonOp = param(100, "P", .1); const auto common = project.commit(commonOp);
    std::vector<OpRequest> aOps{param(100, "P", .2), param(200, "Q", .3)};
    aOps[0].runtimeProof = std::make_shared<const int>(1);
    const auto a = project.commit(aOps); const auto aSnapshot = History(*project.store).takeSnapshot("A", 20, 0);
    require(aSnapshot.ok && History(*project.store).undo().ok, "create A and rewind to common");
    std::vector<OpRequest> bOps{param(100, "P", .4), rename("B")};
    bOps[0].runtimeProof = std::make_shared<const int>(2);
    const auto b = project.commit(bOps); const auto bBranch = OpJournal(*project.store).currentBranchId();
    const auto branches = History(*project.store).branches();
    const auto aBranch = std::find_if(branches.begin(), branches.end(), [&](const History::Branch& branch) {
        return !branch.isCurrent && branch.headSeq == std::optional<std::int64_t>(a.seqs.back());
    });
    require(aBranch != branches.end(), "abandoned A tip remains reachable");
    const auto count = OpJournal(*project.store).count(); const auto notices = observer->batches.size();
    require(History(*project.store).switchToBranch(aBranch->id).ok, "switch B to A");
    check(observer->batches.size() == notices + 1, "whole branch rewind/replay publishes one atomic batch");
    expect(latest(observer), AppliedBatchKind::BranchSwitch, b.seqs.back(), a.seqs.back(),
        {inverse(bOps[1], rename("Foreign"), b, 1), inverse(bOps[0], commonOp, b, 0),
         forward(aOps[0], a, 0), forward(aOps[1], a, 1)}, "both branch legs in actual execution order");
    require(History(*project.store).switchToBranch(bBranch).ok, "switch A back to B");
    expect(latest(observer), AppliedBatchKind::BranchSwitch, a.seqs.back(), b.seqs.back(),
        {inverse(aOps[1], clearParam(200, "Q"), a, 1), inverse(aOps[0], commonOp, a, 0),
         forward(bOps[0], b, 0), forward(bOps[1], b, 1)}, "reverse branch traversal");
    require(History(*project.store).revertToSnapshot(aSnapshot.id, 30).ok, "revert across branches");
    expect(latest(observer), AppliedBatchKind::SnapshotRevert, b.seqs.back(), a.seqs.back(),
        {inverse(bOps[1], rename("Foreign"), b, 1), inverse(bOps[0], commonOp, b, 0),
         forward(aOps[0], a, 0), forward(aOps[1], a, 1)}, "snapshot revert preserves both literal legs");
    require(History(*project.store).revertToSnapshot(rootSnapshot.id, 40).ok, "revert root");
    expect(latest(observer), AppliedBatchKind::SnapshotRevert, a.seqs.back(), {},
        {inverse(aOps[1], clearParam(200, "Q"), a, 1), inverse(aOps[0], commonOp, a, 0),
         inverse(commonOp, clearParam(100, "P"), common)}, "root revert includes actual null-head boundary");
    check(OpJournal(*project.store).count() == count, "navigation notifications append no editing operations");
    const auto unchanged = observer->batches.size(); const auto sequence = appliedSource(*project.store).sequence();
    check(History(*project.store).revertToSnapshot(rootSnapshot.id, 50).ok && observer->batches.size() == unchanged &&
        appliedSource(*project.store).sequence() == sequence, "already-at-snapshot no-op emits no invented material batch");
    const auto current = OpJournal(*project.store).currentBranchId();
    require(History(*project.store).switchToBranch(current).ok, "same-head branch metadata switch");
    expect(latest(observer), AppliedBatchKind::BranchSwitch, {}, {}, {}, "same-head metadata switch emits empty success batch");
    check(appliedSource(*project.store).sequence() == sequence + 1, "successful empty metadata transaction advances publication sequence");
    static_cast<void>(token);
}

void pairedInverseAndUnknownLogFields() {
    Project project("paired_inverse"); auto observer = std::make_shared<Observer>();
    observer->readable = project.store.get(); auto token = subscribe(appliedSource(*project.store), observer);
    const auto create = request("track.create", {{"id", 7}, {"kind", "midi"}, {"name", "Paired"}});
    const auto result = project.commit(create);
    require(History(*project.store).undo().ok, "paired track undo");
    expect(latest(observer), AppliedBatchKind::Undo, result.seqs.back(), {},
        {inverse(create, request("track.delete", {{"id", 7}}), result)}, "inverse names actual paired delete handler");
    require(History(*project.store).redo().ok, "paired track redo");
    expect(latest(observer), AppliedBatchKind::Redo, {}, result.seqs.back(), {forward(create, result)}, "paired redo restores original dispatch");
    const auto edit = param(100, "P", .5); const auto edited = project.commit(edit);
    require(History(*project.store).undo().ok, "prepare unknown stored fields fixture");
    auto extended = edit.payload;
    extended["future"] = {{"blob", binary({std::byte{0x00}, std::byte{0xff}})}, {"integer", 1}, {"real", 1.0},
        {"array", Payload::array({nullptr, "literal"})}};
    {
        const auto encoded = encodePayload(extended);
        SQLite::Statement update(project.store->db(), "UPDATE ops SET payload=? WHERE seq=?");
        update.bind(1, encoded.data(), static_cast<int>(encoded.size())); update.bind(2, edited.seqs.back());
        require(update.exec() == 1, "install literal future log payload");
    }
    require(History(*project.store).redo().ok, "permissive stored log replay");
    auto expected = forward(edit, edited); expected.payload = extended;
    expect(latest(observer), AppliedBatchKind::Redo, result.seqs.back(), edited.seqs.back(), {expected},
        "recorded future nested values remain literal owned event data");
    static_cast<void>(token);
}

void unchangedFailure(Project& project, const std::shared_ptr<Observer>& observer,
                      const std::function<bool()>& attempt, const std::string& label) {
    const auto before = fullState(*project.store); const auto digest = digestProject(*project.store).text;
    const auto source = appliedSource(*project.store); const auto sequence = source.sequence();
    const auto notices = observer->batches.size(); const bool accepted = attempt();
    const bool sameRows = fullState(*project.store) == before;
    const bool sameDigest = digestProject(*project.store).text == digest;
    if (!check(!accepted && sameRows && sameDigest && observer->batches.size() == notices &&
        source.sequence() == sequence && sqlite3_get_autocommit(project.store->db().getHandle()) != 0, label)) {
        std::printf("  accepted=%d sameRows=%d sameDigest=%d noticesBefore=%zu noticesAfter=%zu\n",
            accepted ? 1 : 0, sameRows ? 1 : 0, sameDigest ? 1 : 0, notices, observer->batches.size());
    }
}
void rollbackSilence() {
    Project project("rollback"); auto observer = std::make_shared<Observer>();
    observer->readable = project.store.get(); auto token = subscribe(appliedSource(*project.store), observer);
    const std::vector<OpRequest> operations{param(100, "P", .2), param(200, "Q", .4)};
    auto invalid = operations.front(); invalid.payload["typo"] = true;
    unchangedFailure(project, observer, [&] { return OpJournal(*project.store).commit(invalid).ok; },
        "schema refusal publishes nothing and preserves exact material/journal/control rows");
    unchangedFailure(project, observer, [&] { return OpJournal(*project.store).commit(state(100, std::string(64, '0'))).ok; },
        "missing referenced state blob cannot publish a successful applied batch");
    const std::array<const char*, 4> triggers{
        "CREATE TRIGGER fail_batch BEFORE INSERT ON plugin_params WHEN NEW.device_id=200 BEGIN SELECT RAISE(ABORT,'second handler'); END",
        "CREATE TRIGGER fail_batch BEFORE INSERT ON ops BEGIN SELECT RAISE(ABORT,'late log'); END",
        "CREATE TRIGGER fail_batch BEFORE INSERT ON op_clocks BEGIN SELECT RAISE(ABORT,'late clock'); END",
        "CREATE TRIGGER fail_batch BEFORE UPDATE ON op_branches BEGIN SELECT RAISE(ABORT,'late head'); END"};
    for (std::size_t i = 0; i < triggers.size(); ++i) {
        project.store->db().exec(triggers[i]);
        unchangedFailure(project, observer, [&] { return OpJournal(*project.store).commit(operations).ok; },
            "commit failure emits no successful prefix or publication sequence " + std::to_string(i));
        project.store->db().exec("DROP TRIGGER fail_batch");
    }
    for (const bool throwing : {false, true}) {
        CommitOptions options;
        options.beforeCommit = [throwing](SQLite::Database& db, std::int64_t, std::string& error) -> bool {
            db.exec("UPDATE tracks SET name='temporary hook write' WHERE id=2");
            if (throwing) throw std::runtime_error("hook exception");
            error = "hook refusal"; return false;
        };
        unchangedFailure(project, observer, [&] { return OpJournal(*project.store).commit(operations, options).ok; },
            throwing ? "throwing final hook has no durable writes or event" : "refusing final hook has no durable writes or event");
    }
    const auto notices = observer->batches.size();
    const auto sequence = appliedSource(*project.store).sequence();
    const auto scratchBefore = fullState(*project.store);
    {
        SQLite::Transaction scratch(project.store->db());
        OpContext context{*project.store, project.store->db()};
        const auto* descriptor = OpRegistry::instance().find("device.setParam");
        require(descriptor && descriptor->buildInverse && descriptor->apply, "scratch descriptor");
        Payload inversePayload; std::string error;
        require(descriptor->buildInverse(context, operations.front().payload, inversePayload, error) &&
            descriptor->apply(context, operations.front().payload, error), "scratch inverse/apply: " + error);
        check(!typedQuery(project.store->db(), "SELECT * FROM plugin_params").empty(),
            "rollback-only fixture really executes a material handler");
    }
    check(fullState(*project.store) == scratchBefore && observer->batches.size() == notices &&
        appliedSource(*project.store).sequence() == sequence,
        "rollback-only handler evaluation never publishes or advances source sequence");
    const std::vector<OpRequest> empty;
    check(OpJournal(*project.store).commit(empty).ok && observer->batches.size() == notices &&
        appliedSource(*project.store).sequence() == sequence, "empty journal call emits no fictitious applied transaction");
    project.commit(operations);
    const std::vector<OpRequest> next{param(100, "P", .5), param(200, "Q", .6)};
    const auto updated = project.commit(next);
    project.store->db().exec("CREATE TRIGGER fail_batch BEFORE UPDATE ON plugin_params WHEN NEW.device_id=100 BEGIN SELECT RAISE(ABORT,'inverse suffix'); END");
    unchangedFailure(project, observer, [&] { return History(*project.store).undo().ok; },
        "Undo suffix failure rolls back earlier inverse and emits no partial batch");
    project.store->db().exec("DROP TRIGGER fail_batch");
    require(History(*project.store).undo().ok, "healthy undo retry");
    project.store->db().exec("CREATE TRIGGER fail_batch BEFORE UPDATE ON plugin_params WHEN NEW.device_id=200 BEGIN SELECT RAISE(ABORT,'forward suffix'); END");
    unchangedFailure(project, observer, [&] { return History(*project.store).redo().ok; },
        "Redo suffix failure rolls back earlier forward and emits no partial batch");
    project.store->db().exec("DROP TRIGGER fail_batch");
    require(History(*project.store).redo().ok, "healthy redo retry");
    expect(latest(observer), AppliedBatchKind::Redo, updated.seqs.front() - 1, updated.seqs.back(),
        {forward(next[0], updated, 0), forward(next[1], updated, 1)}, "successful retry emits only its own complete batch");
    static_cast<void>(token);

    Project branch("branch_failure"); auto listener = std::make_shared<Observer>();
    listener->readable = branch.store.get(); auto registration = subscribe(appliedSource(*branch.store), listener);
    branch.commit(param(100, "P", .1)); const auto a = branch.commit(param(100, "P", .2));
    const auto snap = History(*branch.store).takeSnapshot("A", 10, 0);
    require(snap.ok && History(*branch.store).undo().ok, "branch failure fixture");
    branch.commit(param(100, "P", .3));
    const auto branches = History(*branch.store).branches();
    const auto target = std::find_if(branches.begin(), branches.end(), [&](const History::Branch& item) {
        return !item.isCurrent && item.headSeq == std::optional<std::int64_t>(a.seqs.back());
    });
    require(target != branches.end(), "failed switch target");
    branch.store->db().exec("CREATE TRIGGER fail_path BEFORE UPDATE ON plugin_params WHEN NEW.normalized_value=0.2 BEGIN SELECT RAISE(ABORT,'replay leg'); END");
    unchangedFailure(branch, listener, [&] { return History(*branch.store).switchToBranch(target->id).ok; },
        "branch rewind followed by failed replay emits nothing and preserves both branches");
    unchangedFailure(branch, listener, [&] { return History(*branch.store).revertToSnapshot(snap.id, 20).ok; },
        "snapshot revert rollback also restores automatically preserved branch metadata");
    branch.store->db().exec("DROP TRIGGER fail_path");
    check(History(*branch.store).revertToSnapshot(snap.id, 30).ok,
        "healthy navigation retry succeeds without a queued failed prefix");
    static_cast<void>(registration);
}

void compactionAndReplayBase() {
    Project project("compaction"); auto observer = std::make_shared<Observer>();
    observer->readable = project.store.get(); auto token = subscribe(appliedSource(*project.store), observer);
    UndoRetentionPolicy policy; policy.maxTransactions = 1;
    require(History(*project.store).setUndoRetentionPolicy(policy).ok, "set compact test retention");
    check(observer->batches.empty(), "retention metadata alone does not invent an applied operation");
    std::vector<CommitResult> committed;
    for (int i = 1; i <= 5; ++i) {
        const auto edit = param(100, "P", static_cast<double>(i) / 10.0);
        const auto before = OpJournal(*project.store).headSeq(); const auto result = project.commit(edit);
        committed.push_back(result);
        expect(latest(observer), AppliedBatchKind::Commit, before, result.seqs.back(), {forward(edit, result)},
            "compacting commit carries its own original operation");
    }
    check(observer->batches.size() == 5 && appliedSource(*project.store).sequence() == 5 &&
        OpJournal(*project.store).count() < 5,
        "temporary checkpoint rewind/replay never notifies or advances source sequence");
    const auto oldBatch = observer->batches.front();
    check(oldBatch->operations().size() == 1 && oldBatch->operations()[0].seq == committed.front().seqs.front() &&
        oldBatch->operations()[0].payload.at("norm") == .1,
        "retained actual event survives pruning its original operation row");
    Project target("replay_target"); auto receiver = std::make_shared<Observer>();
    receiver->readable = target.store.get(); auto targetToken = subscribe(appliedSource(*target.store), receiver);
    History::ReplayBaseResult restored;
    {
        SQLite::Transaction read(project.store->db(), SQLite::TransactionBehavior::DEFERRED);
        restored = History::restoreReplayBase(*project.store, *target.store); read.commit();
    }
    require(restored.ok && restored.baseSeq > 0, "restore real compacted replay base: " + restored.error);
    require(receiver->batches.size() == 1, "one replay-base reset event");
    expect(latest(receiver), AppliedBatchKind::ReplayBaseReset, {}, {}, {}, "replay base has a distinct reset kind and no forged ops");
    check(latest(receiver)->stateBlobs().empty() && latest(receiver)->finalParameters().empty() &&
        latest(receiver)->deviceTargets().empty(), "replay-base reset asks consumers to rehydrate without a synthetic plugin sequence");
    target.commit(rename("Target now has history"));
    unchangedFailure(target, receiver, [&] { return History::restoreReplayBase(*project.store, *target.store).ok; },
        "refused replay-base reset emits no event and preserves target rows");
    static_cast<void>(token); static_cast<void>(targetToken);
}

void throwingObserverAndRevocation() {
    Project project("observer_failure"); const auto source = appliedSource(*project.store);
    auto throwing = std::make_shared<Observer>(), healthy = std::make_shared<Observer>();
    throwing->readable = project.store.get(); healthy->readable = project.store.get();
    throwing->onCommit = [](const Batch&) { throw std::runtime_error("deliberate observer failure"); };
    auto failedToken = subscribe(source, throwing), healthyToken = subscribe(source, healthy);
    const auto first = project.commit(param(100, "P", .2));
    check(first.ok && first.seqs.size() == 1 && OpJournal(*project.store).headSeq() == std::optional<std::int64_t>(first.seqs.back()) &&
        failedToken.state() == AppliedSubscriptionState::ObserverFailed && failedToken.failureCount() == 1 &&
        !failedToken.error().empty() && healthy->batches.size() == 1,
        "throwing observer faults only its subscription and cannot falsify durable SQL success or skip healthy observers");
    project.commit(param(100, "P", .3));
    check(throwing->batches.size() == 1 && healthy->batches.size() == 2 && failedToken.failureCount() == 1 &&
        healthyToken.lastDeliveredSequence() == source.sequence(), "failed observer stays quiet while healthy subscription continues");
    throwing->onInvalidation = [] { throw std::runtime_error("deliberate invalidation failure"); };
    throwing->readable = nullptr; healthy->readable = nullptr;
    check(project.store->close(), "observer exception during invalidation does not falsify successful close");
    check(throwing->invalidations.size() == 1 && healthy->invalidations.size() == 1 && !source.alive() &&
        !failedToken.active() && !healthyToken.active(), "source invalidation reaches unrevoked faulted and healthy observers");
    failedToken.reset(); failedToken.reset(); healthyToken.reset();

    Project mutation("subscription_mutation"); const auto domain = appliedSource(*mutation.store);
    auto a = std::make_shared<Observer>(), b = std::make_shared<Observer>();
    auto c = std::make_shared<Observer>(), d = std::make_shared<Observer>();
    AppliedSubscription aToken, bToken, cToken, dToken;
    aToken = subscribe(domain, a); bToken = subscribe(domain, b); cToken = subscribe(domain, c);
    a->onCommit = [&](const Batch&) { aToken.reset(); bToken.reset(); dToken = subscribe(domain, d); };
    mutation.commit(rename("First"));
    check(a->batches.size() == 1 && b->batches.empty() && c->batches.size() == 1 && d->batches.empty() &&
        aToken.state() == AppliedSubscriptionState::Inactive && bToken.state() == AppliedSubscriptionState::Inactive,
        "self/other revocation skips not-yet-started callbacks and new subscriptions wait for next batch");
    mutation.commit(rename("Second"));
    check(a->batches.size() == 1 && b->batches.empty() && c->batches.size() == 2 && d->batches.size() == 1,
        "remaining observer order and next-batch registration remain usable after container mutation");
    auto moved = std::move(cToken);
    check(!cToken.active() && moved.active(), "subscription move leaves exactly one active revocation owner");
    cToken.reset(); mutation.commit(rename("Third"));
    check(c->batches.size() == 3 && d->batches.size() == 2, "resetting moved-from token does not revoke moved registration");
    auto expired = std::make_shared<Observer>(); auto expiredToken = subscribe(domain, expired); expired.reset();
    mutation.commit(rename("Fourth"));
    check(expiredToken.state() == AppliedSubscriptionState::ObserverExpired && !expiredToken.active(),
        "bus holds weak observer ownership and safely skips expired subscribers");
    static_cast<void>(moved);
}

struct DestructionObserver : Observer {
    explicit DestructionObserver(bool& destroyed) : destroyed_(destroyed) {}
    ~DestructionObserver() override { destroyed_ = true; }
    bool& destroyed_;
};
void callbackOwnershipAndReentrancy() {
    Project project("callback_ownership"); const auto source = appliedSource(*project.store);
    bool destroyed = false;
    auto self = std::make_shared<DestructionObserver>(destroyed);
    const std::weak_ptr<DestructionObserver> weak = self;
    AppliedSubscription selfToken = subscribe(source, self);
    self->onCommit = [&](const Batch&) {
        selfToken.reset(); self.reset();
        check(!destroyed, "dispatch retains observer during self-revocation and last external owner destruction");
    };
    project.commit(rename("Self gone"));
    check(destroyed && weak.expired(), "observer is released after its callback completes without a lingering bus owner");

    Project foreign("independent_callback_store");
    auto observer = std::make_shared<Observer>(); observer->readable = project.store.get();
    const auto snapshot = History(*project.store).takeSnapshot("Callback origin", 100, 0);
    require(snapshot.ok, "reentrancy snapshot fixture");
    DailyHistorySnapshots daily(*project.store);
    auto registration = subscribe(source, observer);
    bool attempted = false;
    observer->onCommit = [&](const Batch& batch) {
        if (attempted) return;
        attempted = true;
        const auto before = fullState(*project.store); const auto path = project.store->path();
        const auto sequence = source.sequence(); const auto branch = OpJournal(*project.store).currentBranchId();
        const std::vector<OpRequest> empty;
        check(!OpJournal(*project.store).commit(rename("Nested")).ok &&
            !OpJournal(*project.store).commit(empty).ok &&
            !OpJournal(*project.store).setHeadSeq(batch->afterHead()),
            "same-Store recursive journal writes refuse before empty and same-head paths");
        check(!History(*project.store).undo().ok && !History(*project.store).redo().ok &&
            !History(*project.store).switchToBranch(branch).ok &&
            !History(*project.store).revertToSnapshot(snapshot.id, 101).ok,
            "all recursive history navigation refuses during committed delivery");
        check(!History(*project.store).takeSnapshot("Nested", 102, 0).ok &&
            !History(*project.store).renameSnapshot(snapshot.id, "Nested").ok &&
            !History(*project.store).removeSnapshot(snapshot.id).ok &&
            !History(*project.store).compactUndo().ok &&
            !History(*project.store).setUndoRetentionPolicy(UndoRetentionPolicy{}).ok,
            "recursive history metadata/compaction mutations refuse as well");
        const auto polled = daily.poll(103, 0);
        check(polled.status == DailyHistorySnapshots::Status::Failed && !polled.error.empty(),
            "daily history poll reports refusal during applied-batch delivery");
        const auto destination = project.folder.path() / "nested-save.adi";
        check(!project.store->close() && !project.store->saveAs(destination).ok && !std::filesystem::exists(destination),
            "recursive close/SaveAs refuse before file creation, connection teardown or domain invalidation");
        check(fullState(*project.store) == before && project.store->path() == path && source.alive() &&
            source.sequence() == sequence && appliedSource(*project.store).sameConnection(source),
            "all rejected recursion leaves exact outer committed state and source intact");
        const auto other = OpJournal(*foreign.store).commit(rename("Independent"));
        check(other.ok && appliedSource(*foreign.store).sequence() == 1,
            "dispatch guard is connection-local and permits an independent Store commit");
    };
    const auto outer = project.commit(param(100, "P", .75));
    check(attempted && outer.ok && observer->batches.size() == 1 && registration.active(),
        "recursive refusals cannot turn the original committed transaction into failure");
    project.commit(param(100, "P", .5));
    check(observer->batches.size() == 2 && registration.active(), "dispatch guard releases after callback for subsequent commits");
}

void sourceCloseAndSaveAs() {
    Project project("source_lifetime"); auto observer = std::make_shared<Observer>();
    observer->readable = project.store.get(); const auto original = appliedSource(*project.store);
    auto token = subscribe(original, observer);
    project.commit(param(100, "P", .4)); const auto retained = latest(observer);
    const auto originalFile = original.sourceFile(); const auto before = fullState(*project.store);
    const auto existing = project.store->path();
    check(!project.store->saveAs(existing).ok && original.alive() && appliedSource(*project.store).sameConnection(original) &&
        !appliedSource(*project.store).isReplacementOf(original) && fullState(*project.store) == before && observer->invalidations.empty(),
        "failed SaveAs leaves source domain, subscriptions and exact project rows intact");
    {
        SQLite::Statement live(project.store->db(), "SELECT id FROM tracks");
        require(live.executeStep(), "live statement SaveAs fixture");
        const auto blocked = project.folder.path() / "statement-blocked.adi";
        check(!project.store->saveAs(blocked).ok && !std::filesystem::exists(blocked) &&
            original.alive() && observer->invalidations.empty(), "SaveAs statement refusal does not invalidate observers or publish a file");
    }
    const auto target = project.folder.path() / "renamed.adi";
    require(project.store->saveAs(target).ok, "successful SaveAs");
    const auto replacement = appliedSource(*project.store);
    check(!original.alive() && replacement.alive() && !replacement.sameConnection(original) &&
        replacement.isReplacementOf(original) && !original.isReplacementOf(replacement) &&
        !replacement.isReplacementOf(replacement) && !original.isReplacementOf(original) &&
        original.sourceFile() == originalFile && replacement.sourceFile() != originalFile &&
        observer->invalidations.size() == 1 && observer->invalidations[0].second == AppliedSourceInvalidation::Replaced &&
        token.state() == AppliedSubscriptionState::SourceInvalidated,
        "successful SaveAs invalidates old connection/subscriptions and installs a distinct source identity");
    check(!original.observe(observer).ok, "expired source cannot silently rebind an observer to the replacement database");
    project.commit(param(100, "P", .5));
    check(observer->batches.size() == 1 && replacement.sequence() == 1,
        "old subscription receives no replacement-source commit while unobserved new sequence advances");
    auto freshToken = subscribe(replacement, observer); project.commit(param(100, "P", .6));
    check(observer->batches.size() == 2 && latest(observer)->source().sameConnection(replacement) &&
        latest(observer)->sequence() == 2, "explicit new-source subscription resumes on current connection only");
    observer->readable = nullptr;
    require(project.store->close(), "explicit source close");
    check(observer->invalidations.size() == 2 && observer->invalidations.back().second == AppliedSourceInvalidation::Closed &&
        !replacement.alive() && freshToken.state() == AppliedSubscriptionState::SourceInvalidated,
        "close invalidates current domain before callback and connection release");
    check(project.store->close() && observer->invalidations.size() == 2,
        "repeated close is harmless and does not repeat invalidation");
    project.store.reset(); token.reset(); freshToken.reset();
    require(retained->operations().size() == 1, "retained event operation exists");
    check(retained->sequence() == 1 && retained->source().sourceFile() == originalFile &&
        retained->operations()[0].payload.at("norm") == .4,
        "retained batch keeps immutable source filename/sequence/payload after replacement and destruction");

    AppliedSource dead; auto absentObserver = std::make_shared<Observer>();
    check(!dead.alive() && !dead.observe(absentObserver).ok && !replacement.observe(absentObserver).ok,
        "default and closed source registration refuse without exposing dead Store pointers");
    Project destroyedProject("store_destroy_first");
    auto survivor = std::make_shared<Observer>(); const auto doomed = appliedSource(*destroyedProject.store);
    auto survivorToken = subscribe(doomed, survivor);
    destroyedProject.store.reset();
    check(!doomed.alive() && survivor->invalidations.size() == 1 &&
        survivorToken.state() == AppliedSubscriptionState::SourceInvalidated,
        "Store destruction invalidates passive subscribers before their later safe token destruction");
    survivorToken.reset();
}

void directReplacementLineage() {
    Project project("replacement_lineage"), unrelated("unrelated_lineage");
    const auto first = appliedSource(*project.store), other = appliedSource(*unrelated.store);
    const AppliedSource empty;
    check(!first.isReplacementOf(first) && !first.isReplacementOf(other) && !other.isReplacementOf(first) &&
        !first.isReplacementOf(empty) && !empty.isReplacementOf(first) && !empty.isReplacementOf(empty),
        "fresh/default/unrelated identities carry no invented replacement provenance");
    require(project.store->saveAs(project.folder.path() / "second.adi").ok, "first lineage SaveAs");
    const auto second = appliedSource(*project.store);
    check(second.isReplacementOf(first) && !first.isReplacementOf(second) && !second.isReplacementOf(other),
        "first successful SaveAs records only its exact direct predecessor identity");
    auto observer = std::make_shared<Observer>(); observer->readable = project.store.get();
    auto token = subscribe(second, observer);
    const auto before = fullState(*project.store); const auto secondPath = project.store->path();
    check(!project.store->saveAs(secondPath).ok && second.alive() && token.active() &&
        appliedSource(*project.store).sameConnection(second) && appliedSource(*project.store).isReplacementOf(first) &&
        !appliedSource(*project.store).isReplacementOf(second) && fullState(*project.store) == before &&
        observer->invalidations.empty(),
        "failed SaveAs preserves the old binding and its existing direct predecessor without adding a link");
    require(project.store->saveAs(project.folder.path() / "third.adi").ok, "second lineage SaveAs");
    const auto third = appliedSource(*project.store);
    check(third.isReplacementOf(second) && !third.isReplacementOf(first) &&
        !third.isReplacementOf(third) && !second.isReplacementOf(third) && !third.isReplacementOf(other) &&
        second.isReplacementOf(first) && observer->invalidations.size() == 1 &&
        token.state() == AppliedSubscriptionState::SourceInvalidated,
        "second SaveAs links only S2 to S1, while S1 retains its own S0 relation");
    observer->readable = nullptr; project.store.reset(); token.reset();
    check(!first.alive() && !second.alive() && !third.alive() &&
        third.isReplacementOf(second) && !third.isReplacementOf(first) && second.isReplacementOf(first),
        "copied immutable replacement identities remain queryable after Store and connections are destroyed");
}
} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const std::vector<std::pair<const char*, void(*)()>> suites{
        {"first touch and source sequences", firstTouchAndSequences},
        {"state order and typed resources", stateOrderAndTypedResources},
        {"branch and snapshot traversal", branchesAndRevert},
        {"paired inverse and unknown log fields", pairedInverseAndUnknownLogFields},
        {"rollback silence", rollbackSilence},
        {"compaction and replay base", compactionAndReplayBase},
        {"throwing observer and revocation", throwingObserverAndRevocation},
        {"callback ownership and reentrancy", callbackOwnershipAndReentrancy},
        {"source close and SaveAs", sourceCloseAndSaveAs},
        {"direct SaveAs replacement lineage", directReplacementLineage}};
    for (const auto& [name, run] : suites) {
        std::printf("applied batches: %s\n", name);
        try { run(); }
        catch (const std::exception& error) { check(false, std::string(name) + ": " + error.what()); }
    }
    std::printf("applied batches: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
