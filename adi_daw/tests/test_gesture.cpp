// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/applied_batches.hpp"
#include "adi/digest.hpp"
#include "adi/engine/gesture_preview.hpp"
#include "adi/engine/snapshot.hpp"
#include "adi/gesture_capture_detail.hpp"
#include "adi/gesture_presentation.hpp"
#include "adi/gesture_producers.hpp"
#include "adi/history.hpp"
#include "adi/media/blake3.hpp"
#include "adi/op_transaction_resources.hpp"
#include "adi/store.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
using namespace adi;
namespace g = adi::gesture;
using Bytes = std::vector<std::byte>;
using Rows = std::vector<Payload>;
using SqlState = std::map<std::string, Rows>;
int checks = 0, failures = 0;
bool check(bool value, const std::string& message) {
    ++checks;
    if (!value) { ++failures; std::printf("FAIL: %s\n", message.c_str()); }
    return value;
}
void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}
Rows rows(const SQLite::Database& db, const std::string& sql, bool tagged = false) {
    SQLite::Statement query(db, sql); Rows result;
    while (query.executeStep()) {
        Payload row = Payload::object();
        for (int i = 0; i < query.getColumnCount(); ++i) {
            const auto cell = query.getColumn(i); Payload value;
            switch (cell.getType()) {
                case SQLITE_INTEGER: value = cell.getInt64(); break;
                case SQLITE_FLOAT:
                    if (tagged) value = std::bit_cast<std::uint64_t>(cell.getDouble());
                    else value = cell.getDouble();
                    break;
                case SQLITE_TEXT: value = cell.getString(); break;
                case SQLITE_NULL: value = nullptr; break;
                case SQLITE_BLOB: {
                    std::vector<std::uint8_t> bytes;
                    if (cell.getBytes() != 0) {
                        const auto* begin = static_cast<const std::uint8_t*>(cell.getBlob());
                        bytes.assign(begin, begin + cell.getBytes());
                    }
                    value = Payload::binary(std::move(bytes)); break;
                }
                default: throw std::runtime_error("unknown SQL storage class");
            }
            row[query.getColumnName(i)] = tagged ? Payload{{"kind", cell.getType()}, {"value", value}} : value;
        }
        result.push_back(std::move(row));
    }
    return result;
}
Payload one(const Store& store, const std::string& sql) {
    const auto found = rows(store.db(), sql); require(found.size() == 1, "fixture query returns one row");
    return found.front();
}
SqlState sqlState(const Store& store) {
    // Explicit SQL-kind tags and REAL bits make the oracle independent of JSON
    // cross-kind numeric equality and cover rollback bookkeeping as well.
    const std::array<std::pair<const char*, const char*>, 18> tables{{
        {"project", "id"}, {"tracks", "id"}, {"mixer_strip", "track_id"}, {"tempo_map", "id"},
        {"device_chains", "id"}, {"devices", "id"}, {"plugin_refs", "id"},
        {"plugin_params", "device_id,param_id"}, {"plugin_state", "device_id,stream_role"},
        {"state_blobs", "hash_blake3"}, {"ops", "seq"}, {"op_clocks", "seq"},
        {"op_clients", "client_id"}, {"op_branches", "id"}, {"history_snapshots", "id"},
        {"history_checkpoint", "id"}, {"session_state", "key"}, {"mixer_pan_settings", "track_id"}}};
    SqlState result;
    for (const auto& [table, order] : tables)
        result.emplace(table, rows(store.db(), std::string("SELECT * FROM ") + table + " ORDER BY " + order, true));
    return result;
}
struct Writes {
    SQLite::Database* db;
    int material = 0, control = 0, all = 0;
    explicit Writes(Store& store) : db(&store.db()) {
        sqlite3_update_hook(db->getHandle(), [](void* context, int, const char*, const char* table, sqlite3_int64) {
            auto& self = *static_cast<Writes*>(context); ++self.all;
            const std::string name(table);
            if (name == "ops" || name.starts_with("op_") || name.starts_with("history_") || name == "session_state")
                ++self.control;
            else ++self.material;
        }, this);
    }
    ~Writes() { if (db) sqlite3_update_hook(db->getHandle(), nullptr, nullptr); }
    Writes(const Writes&) = delete;
    Writes& operator=(const Writes&) = delete;
};
OpRequest operation(const char* type, Payload payload) {
    OpRequest request; request.opType = type; request.payload = std::move(payload);
    request.label = "Producer label must be replaced"; return request;
}
OpRequest rename(std::int64_t id, const std::string& name) {
    return operation("track.rename", {{"id", id}, {"name", name}});
}
OpRequest volume(double db) { return operation("mixer.setVolume", {{"id", 1}, {"db", db}}); }
OpRequest loadState(const std::string& hash) {
    return operation("device.loadState", {{"dev", 100}, {"role", "component"}, {"hash", hash}});
}
struct Project {
    test::TempDirectory folder;
    std::unique_ptr<Store> store;
    explicit Project(const char* name) : folder("gesture", name) {
        StoreError error{}; store = Store::createEmpty(folder.path() / "song.adi", error);
        require(bool(store), std::string("create Store: ") + toString(error));
        store->db().exec("INSERT INTO project(id,name) VALUES(1,'Gesture');"
            "INSERT INTO tracks(id,kind,name) VALUES(1,'audio','Origin'),(2,'audio','Foreign');"
            "INSERT INTO mixer_strip(track_id,volume_db,pan) VALUES(1,-6.0,0.25),(2,-12.0,0.0);"
            "INSERT INTO device_chains(id,track_id) VALUES(10,1);"
            "INSERT INTO devices(id,chain_id,name) VALUES(100,10,'Device');"
            "ALTER TABLE tracks ADD COLUMN future_any ANY;"
            "ALTER TABLE mixer_strip ADD COLUMN future_any ANY;"
            "ALTER TABLE plugin_params ADD COLUMN future_any ANY;"
            "ALTER TABLE plugin_params ADD COLUMN future_blob BLOB DEFAULT X'00FE';"
            "UPDATE tracks SET future_any=X'00FF' WHERE id=1;"
            "UPDATE mixer_strip SET future_any=1 WHERE track_id=1");
    }
    CommitResult commit(const std::vector<OpRequest>& requests) {
        auto result = OpJournal(*store).commit(requests); require(result.ok, "fixture commit: " + result.error); return result;
    }
    CommitResult commit(const OpRequest& request) { return commit(std::vector<OpRequest>{request}); }
};
struct Observer final : AppliedObserver {
    std::vector<std::shared_ptr<const AppliedBatch>> batches;
    std::function<void()> callback;
    void committed(std::shared_ptr<const AppliedBatch> batch) override {
        batches.push_back(std::move(batch)); if (callback) callback();
    }
    void sourceInvalidated(const AppliedSource&, AppliedSourceInvalidation) override {}
};
AppliedSubscription observe(Store& store, const std::shared_ptr<Observer>& observer) {
    auto result = appliedSource(store).observe(observer); require(result.ok, "subscribe: " + result.error);
    return std::move(result.subscription);
}
struct Delta final : g::Input {
    explicit Delta(int input) : value(input) {}
    int value;
};
struct FrozenOrigin final : g::Origin {
    std::string name;
    double volume = 0;
};
struct ProducerStats { int captures = 0, preparations = 0; std::vector<std::optional<std::int64_t>> bases; };
struct OriginProducer final : g::Producer {
    std::shared_ptr<ProducerStats> stats;
    explicit OriginProducer(std::shared_ptr<ProducerStats> counts) : stats(std::move(counts)) {}
    g::OriginResult captureOrigin(const Store& store, const g::Base&) override {
        ++stats->captures; auto origin = std::make_unique<FrozenOrigin>();
        origin->name = one(store, "SELECT name FROM tracks WHERE id=1").at("name").get<std::string>();
        origin->volume = one(store, "SELECT volume_db FROM mixer_strip WHERE track_id=1").at("volume_db").get<double>();
        return {{}, std::move(origin)};
    }
    std::vector<g::TargetClaim> targetClaims(const g::Origin&) const override {
        return {{g::ClaimKind::Structural, g::TargetDomain::Material, 0, {}, {}}};
    }
    g::ProposalResult prepare(const Store& store, const g::Base& base, const g::Origin& origin,
                              const g::Input& input) const override {
        ++stats->preparations; stats->bases.push_back(base.head);
        const auto* frozen = dynamic_cast<const FrozenOrigin*>(&origin);
        const auto* delta = dynamic_cast<const Delta*>(&input);
        if (!frozen || !delta || delta->value < 0)
            return {{g::Error::InvalidInput, "nonnegative absolute displacement required"}, {}};
        g::Proposal proposal;
        proposal.rowGuards.push_back({"tracks", {{"id", 1}}, one(store, "SELECT * FROM tracks WHERE id=1")});
        proposal.rowGuards.push_back({"mixer_strip", {{"track_id", 1}}, one(store, "SELECT * FROM mixer_strip WHERE track_id=1")});
        proposal.rowSetGuards.push_back({"plugin_params", {{"device_id", 100}}, rows(store.db(),
            "SELECT * FROM plugin_params WHERE device_id=100 ORDER BY device_id,param_id")});
        if (delta->value != 0) {
            proposal.requests.push_back(rename(1, frozen->name + "/" + std::to_string(delta->value)));
            proposal.requests.push_back(volume(frozen->volume + static_cast<double>(delta->value)));
        }
        return {{}, std::move(proposal)};
    }
};
struct Captured final : g::CapturedPresentation { SqlState material; };
struct Prepared final : g::PreparedPresentation { SqlState material; };
struct Receipt final : g::RestorationReceipt {
    g::PresentationState value = g::PresentationState::Current;
    std::string text;
    g::PresentationState state() const noexcept override { return value; }
    const std::string& message() const noexcept override { return text; }
};
struct Presentation final : g::Presentation {
    Store* readable;
    int adopts = 0, captures = 0, prepares = 0, publishes = 0, promotes = 0, clears = 0;
    SqlState base, published;
    bool failCapture = false, failPrepare = false, failPublish = false, throwPromote = false;
    bool deviceValues = false, bindImage = false;
    std::optional<OpPresentationImageGuard> baseImage;
    bool inspectSql = true;
    std::shared_ptr<Receipt> restoration = std::make_shared<Receipt>();
    std::function<void()> beforePublish, afterPrepare;
    explicit Presentation(Store& store) : readable(&store) {}
    bool supportsDeviceValues() const noexcept override { return deviceValues; }
    void closedSql(const char* stage) {
        if (inspectSql) check(sqlite3_get_autocommit(readable->db().getHandle()) != 0,
            std::string(stage) + " runs after scratch rollback");
    }
    g::PresentationStatus adoptCommitted(const Store& store, const g::Base& committed, const AppliedBatch*) override {
        ++adopts; closedSql("adoptCommitted"); base = sqlState(store);
        if (bindImage) baseImage = OpPresentationImageGuard{engine::capturePresentationImage(store)};
        check(committed.head == OpJournal(*readable).headSeq(), "adoption base names actual committed head");
        return {};
    }
    g::CaptureResult capture(const Store& store, const g::Base&, const g::OwnerToken&, g::Strategy,
                             std::span<const OpRequest>) override {
        ++captures;
        check(sqlite3_get_autocommit(store.db().getHandle()) == 0, "capture sees the owned scratch transaction");
        if (failCapture) return {{g::Error::CaptureFailed, "injected capture failure"}, {}, {}};
        auto value = std::make_unique<Captured>(); value->material = sqlState(store);
        return {{}, std::shared_ptr<const engine::Snapshot>(engine::SnapshotBuilder::fromStore(store)), std::move(value), baseImage};
    }
    g::PrepareResult prepare(std::unique_ptr<const g::CapturedPresentation> capture) override {
        ++prepares; closedSql("prepare");
        if (inspectSql) check(sqlState(*readable) == base, "heavy prepare sees exact typed committed SQL after rollback");
        if (afterPrepare) afterPrepare();
        if (failPrepare) return {{g::Error::PreparationFailed, "injected prepare failure"}, {}};
        const auto* owned = dynamic_cast<const Captured*>(capture.get()); require(owned, "owned typed capture");
        auto value = std::make_unique<Prepared>(); value->material = owned->material;
        return {{}, std::move(value)};
    }
    g::PresentationStatus publish(std::unique_ptr<g::PreparedPresentation> preparation) override {
        ++publishes; closedSql("publish"); if (beforePublish) beforePublish();
        if (failPublish) return {g::PresentationState::Refused, "injected publication failure", {}};
        const auto* owned = dynamic_cast<const Prepared*>(preparation.get()); require(owned, "owned typed preparation");
        published = owned->material; return {};
    }
    g::PresentationStatus promote(const Store& store, const g::Base&, const g::OwnerToken&, const AppliedBatch&) override {
        ++promotes; closedSql("promote");
        if (throwPromote) throw std::runtime_error("injected postcommit failure");
        base = sqlState(store); published.clear(); return {};
    }
    g::PresentationStatus clear(const g::OwnerToken&) override {
        ++clears; if (inspectSql) closedSql("clear");
        if (restoration->value == g::PresentationState::Current) published.clear();
        return {restoration->value, restoration->text, restoration};
    }
};
struct Owner {
    std::shared_ptr<Presentation> presentation;
    std::shared_ptr<g::Coordinator> coordinator;
    std::shared_ptr<ProducerStats> stats = std::make_shared<ProducerStats>();
    std::unique_ptr<g::Session> session;
    explicit Owner(Store& store, bool start = true) : presentation(std::make_shared<Presentation>(store)) {
        auto attached = g::Coordinator::attach(store, presentation);
        require(attached.status.ok() && attached.value, "attach: " + attached.status.message);
        coordinator = std::move(attached.value); if (start) begin();
    }
    void begin() {
        g::Spec spec; spec.label = "One exact gesture"; spec.actor = Actor::Agent; spec.actorDetail = "independent fixture";
        spec.selBefore = Payload{{"tracks", Payload::array({1, 2})}};
        auto begun = coordinator->begin(std::move(spec), std::make_unique<OriginProducer>(stats));
        require(begun.status.ok() && begun.session, "begin: " + begun.status.message); session = std::move(begun.session);
    }
    g::UpdateResult update(int value) { return session->update(std::make_unique<Delta>(value)); }
};
bool snapshotName(const std::shared_ptr<const g::Preview>& preview, const std::string& expected) {
    if (!preview || !preview->snapshot()) return false;
    const auto* track = preview->snapshot()->findTrack(1);
    return track && track->name == expected;
}

void originTwentyUpdatesAndOneCommit() {
    Project project("origin"); const auto before = sqlState(*project.store);
    const auto digest = digestProject(*project.store).text;
    auto listener = std::make_shared<Observer>(); auto token = observe(*project.store, listener);
    Owner owner(*project.store); std::shared_ptr<const g::Preview> first;
    {
        Writes writes(*project.store);
        for (int i = 1; i <= 20; ++i) {
            const auto updated = owner.update(i);
            if (!check(updated.status.ok() && updated.preview, "absolute preview " + std::to_string(i))) continue;
            if (i == 1) first = updated.preview;
            check(updated.preview->requests().size() == 2 &&
                updated.preview->requests()[0].payload == Payload{{"id", 1}, {"name", "Origin/" + std::to_string(i)}} &&
                updated.preview->requests()[1].payload == Payload{{"id", 1}, {"db", -6.0 + i}},
                "each update derives literal requests from immutable origin, never accumulated preview");
            check(sqlState(*project.store) == before && digestProject(*project.store).text == digest,
                "preview leaves exact typed material and journal/control state unchanged");
            check(snapshotName(updated.preview, "Origin/" + std::to_string(i)),
                "owned Snapshot captures changed scratch material");
        }
        check(writes.material > 0 && writes.control == 0, "twenty previews execute rollback material work and zero journal/control writes");
    }
    check(owner.stats->captures == 1 && listener->batches.empty() && OpJournal(*project.store).count() == 0,
        "twenty previews capture origin once, append zero ops and publish no committed events");
    require(bool(first), "first preview retained");
    check(first->requests().size() == 2 && first->requests()[0].payload.at("name") == "Origin/1" && snapshotName(first, "Origin/1"),
        "retained earlier preview and Snapshot remain immutable across nineteen updates");
    const auto committed = owner.session->commit();
    check(committed.durable && committed.durable->ok && owner.session->state() == g::State::Committed,
        "gesture commit reports durable SQL success and closes lease");
    const auto log = OpJournal(*project.store).recent();
    check(log.size() == 2 && log[0].txnId == log[1].txnId && log[0].label == "One exact gesture" &&
        log[1].label == "One exact gesture" && log[1].actor == "agent" && log[1].actorDetail == "independent fixture",
        "two effects share exactly one frozen label, actor and transaction");
    check(listener->batches.size() == 1 && listener->batches.front()->operations().size() == 2,
        "one durable commit produces one complete applied batch");
    check(log.size() == 2 && log[0].inverse == std::optional<Payload>(Payload{{"id", 1}, {"db", -6.0}}) &&
        log[1].inverse == std::optional<Payload>(Payload{{"id", 1}, {"name", "Origin"}}),
        "inverse captures exact committed opener, never a scratch value");
    const auto undo = History(*project.store).undo();
    check(undo.ok && undo.opsApplied == 2 && undo.selBefore == Payload{{"tracks", Payload::array({1, 2})}} &&
        digestProject(*project.store).text == digest, "one Undo restores origin and frozen selection");
    check(History(*project.store).redo().ok && one(*project.store, "SELECT volume_db FROM mixer_strip WHERE track_id=1").at("volume_db") == 14.0,
        "one Redo restores the final absolute input");
    const auto count = OpJournal(*project.store).count();
    owner.session->commit(); owner.session.reset();
    check(OpJournal(*project.store).count() == count, "recommit and destruction never duplicate durable effects");
    static_cast<void>(token);
}

void foreignBaseAndFailureRetention() {
    Project project("foreign"); Owner owner(*project.store);
    require(owner.update(3).status.ok(), "initial preview");
    const auto originBase = owner.session->originBase();
    const auto prepares = owner.stats->preparations;
    const auto foreign = project.commit({rename(2, "Foreign committed"), volume(-2.0)});
    const auto before = sqlState(*project.store);
    check(owner.presentation->prepares == 1 && owner.stats->preparations == prepares,
        "foreign observer queues invalidation without scratch or presentation work");
    const auto drained = owner.coordinator->drain(10);
    check(drained.status.ok() && owner.session->currentBase().head == foreign.seqs.back() &&
        owner.session->originBase().headAtCapture == originBase.headAtCapture && owner.stats->captures == 1,
        "foreign reapply adopts current committed head without recapturing origin");
    require(bool(owner.session->preview()), "foreign reapply preview");
    check(owner.session->preview()->requests()[1].payload.at("db") == -3.0 && sqlState(*project.store) == before,
        "foreign reapply derives origin plus accepted input while preserving current SQL");
    const auto prior = owner.session->preview(); const auto invalid = owner.update(-1);
    check(!invalid.status.ok() && owner.session->preview() == prior,
        "invalid same-base input preserves last accepted preview");
    project.store->db().exec("CREATE TEMP TRIGGER refuse_gesture BEFORE UPDATE OF name ON tracks WHEN NEW.id=1 BEGIN SELECT RAISE(ABORT,'gesture fixture rejection'); END");
    const auto failed = owner.session->commit();
    check(!failed.durable || !failed.durable->ok, "apply rejection is not reported as durable success");
    check(sqlState(*project.store) == before && !History(*project.store).undo().ok,
        "refused commit retains exact committed state and navigation lease");
    project.store->db().exec("DROP TRIGGER refuse_gesture");
    const auto retried = owner.session->commit();
    check(retried.durable && retried.durable->ok, "retained accepted intent can commit after transient refusal");
    check(History(*project.store).undo().ok && one(*project.store, "SELECT volume_db FROM mixer_strip WHERE track_id=1").at("volume_db") == -2.0 &&
        one(*project.store, "SELECT name FROM tracks WHERE id=2").at("name") == "Foreign committed",
        "Undo restores the actual foreign opener and preserves unrelated foreign edit");
}

void previewRollbackAndPublicationGuard() {
    Project project("rollback"); Owner owner(*project.store); const auto before = sqlState(*project.store);
    owner.presentation->failCapture = true; auto result = owner.update(1);
    check(!result.status.ok() && sqlState(*project.store) == before && owner.presentation->prepares == 0,
        "capture refusal rolls back all scratch changes before heavy preparation");
    owner.presentation->failCapture = false; owner.presentation->failPrepare = true; result = owner.update(2);
    check(!result.status.ok() && sqlState(*project.store) == before && owner.presentation->publishes == 0,
        "heavy preparation refusal leaves committed state and publication untouched");
    owner.presentation->failPrepare = false;
    owner.presentation->afterPrepare = [&] { project.store->db().exec("UPDATE mixer_strip SET future_any=CAST(1 AS REAL) WHERE track_id=1"); };
    result = owner.update(3);
    check(!result.status.ok() && owner.presentation->publishes == 0 &&
        one(*project.store, "SELECT typeof(future_any) AS kind FROM mixer_strip WHERE track_id=1").at("kind") == "real",
        "same-head INTEGER-to-REAL edit during heavy preparation refuses before publication");
    owner.presentation->afterPrepare = {}; owner.presentation->base = sqlState(*project.store);
    require(owner.update(4).status.ok(), "fresh reprepare after outside edit");
    const auto old = owner.session->preview();
    project.store->db().exec("UPDATE mixer_strip SET pan=0.5 WHERE track_id=1");
    result = owner.update(-1);
    check(!result.status.ok() && owner.session->preview() != old,
        "early invalid input after same-head target change cannot advertise previous preview current");
    require(owner.update(5).status.ok(), "accepted input before null-input stale check");
    project.store->db().exec("UPDATE mixer_strip SET pan=0.75 WHERE track_id=1");
    const auto nullInput = owner.session->update(nullptr);
    check(!nullInput.status.ok() && !owner.session->preview(),
        "null input after same-head target change withdraws previous preview instead of preserving stale accepted guards");
    owner.session->cancel();
    check(one(*project.store, "SELECT pan FROM mixer_strip WHERE track_id=1").at("pan") == 0.75 &&
        OpJournal(*project.store).count() == 0, "cancel preserves direct foreign material and appends nothing");
}

CommitOptions headOptions(Store& store) {
    CommitOptions options; options.expectHead.emplace(OpJournal(store).headSeq()); return options;
}
void refusal(Store& store, const std::function<CommitResult()>& submit, const std::string& label,
             bool noWrites = true) {
    const auto before = sqlState(store); const auto digest = digestProject(store).text;
    const auto head = OpJournal(store).headSeq(); const auto count = OpJournal(store).count();
    Writes writes(store); const auto result = submit();
    check(!result.ok && !result.error.empty(), label + ": named refusal, got " + result.error);
    check(sqlState(store) == before && digestProject(store).text == digest && OpJournal(store).headSeq() == head &&
        OpJournal(store).count() == count, label + ": exact typed material/control/head rollback");
    if (noWrites) check(writes.all == 0, label + ": refusal precedes all writes (material=" +
        std::to_string(writes.material) + ", control=" + std::to_string(writes.control) + ")");
}
void capturedAdmissionAndTypedGuards() {
    Project project("guards"), other("other_source"); auto& store = *project.store;
    const auto source = appliedSource(store); auto options = headOptions(store);
    auto listener = std::make_shared<Observer>(); auto token = observe(store, listener);
    OpTransactionInputs inputs;
    inputs.rowGuards.push_back({"mixer_strip", {{"track_id", 1}}, one(store, "SELECT * FROM mixer_strip WHERE track_id=1")});
    inputs.rowSetGuards.push_back({"plugin_params", {{"device_id", 100}}, {}});
    const auto initial = sqlState(store);
    {
        Writes writes(store); const auto result = OpJournal(store).commitCaptured({}, source, options, inputs);
        check(result.ok && result.seqs.empty() && writes.all == 0 && sqlState(store) == initial,
            "NULL-head captured no-op validates targets and writes no resource/client/journal rows");
    }
    refusal(store, [&] { return OpJournal(store).commitCaptured({}, appliedSource(*other.store), options, inputs); },
        "empty packet rejects foreign connection identity");
    refusal(store, [&] { return OpJournal(store).commitCaptured({}, source, {}, inputs); },
        "captured packet requires engaged head even for NULL root");
    project.commit(rename(2, "Advanced"));
    refusal(store, [&] { return OpJournal(store).commitCaptured({}, source, options, inputs); },
        "empty packet refuses old NULL head after unrelated commit");
    const std::vector<OpRequest> headOnly{rename(1, "Stale head")};
    refusal(store, [&] { return OpJournal(store).commitCaptured(headOnly, source, options, inputs); },
        "nonempty packet refuses head-only change while all captured target rows remain identical");
    options = headOptions(store);
    {
        Writes writes(store); const auto result = OpJournal(store).commitCaptured({}, source, options, inputs);
        check(result.ok && writes.all == 0, "non-NULL-head semantic no-op writes nothing");
    }
    const auto notices = listener->batches.size();
    const std::vector<OpRequest> requests{rename(1, "Guarded")};
    // A separate SQLite connection creates a same-head edit, with no N47 receipt.
    {
        SQLite::Database external(store.path().string(), SQLite::OPEN_READWRITE);
        external.exec("UPDATE mixer_strip SET future_any=CAST(1 AS REAL) WHERE track_id=1");
    }
    refusal(store, [&] { return OpJournal(store).commitCaptured(requests, source, options, inputs); },
        "full-row guard distinguishes equal INTEGER and REAL from external same-head writer");
    refusal(store, [&] { return OpJournal(store).commitCaptured({}, source, options, inputs); },
        "semantic no-op still validates full typed row guard");
    inputs.rowGuards[0].expectedRow = one(store, "SELECT * FROM mixer_strip WHERE track_id=1");
    store.db().exec("INSERT INTO plugin_params(device_id,param_id,normalized_value,future_any) VALUES(100,'X',0.5,1)");
    refusal(store, [&] { return OpJournal(store).commitCaptured(requests, source, options, inputs); },
        "complete empty set guard detects an inserted row");
    inputs.rowSetGuards[0].expectedRows = rows(store.db(), "SELECT * FROM plugin_params WHERE device_id=100 ORDER BY device_id,param_id");
    store.db().exec("UPDATE plugin_params SET future_any=CAST(1 AS REAL) WHERE device_id=100 AND param_id='X'");
    refusal(store, [&] { return OpJournal(store).commitCaptured({}, source, options, inputs); },
        "complete nonempty set guard distinguishes INTEGER from REAL");
    inputs.rowSetGuards[0].expectedRows = rows(store.db(), "SELECT * FROM plugin_params WHERE device_id=100 ORDER BY device_id,param_id");
    store.db().exec("UPDATE plugin_params SET future_blob=X'' WHERE device_id=100 AND param_id='X'");
    refusal(store, [&] { return OpJournal(store).commitCaptured(requests, source, options, inputs); },
        "complete set guards preserve opaque extension BLOB including empty content");
    inputs.rowSetGuards[0].expectedRows = rows(store.db(), "SELECT * FROM plugin_params WHERE device_id=100 ORDER BY device_id,param_id");
    inputs.rowGuards.push_back({"plugin_params", {{"device_id", 100}, {"param_id", "Absent"}}, std::nullopt});
    store.db().exec("INSERT INTO plugin_params(device_id,param_id) VALUES(100,'Absent')");
    refusal(store, [&] { return OpJournal(store).commitCaptured({}, source, options, inputs); },
        "full composite-key absence guard refuses newly present target");
    check(listener->batches.size() == notices, "all same-head refusals and no-ops publish zero committed events");
    inputs = {};
    const std::vector<OpRowGuard> malformed{
        {"tracks; DELETE FROM tracks", {{"id", 1}}, std::nullopt},
        {"ops", {{"seq", 1}}, std::nullopt},
        {"tracks", Payload::object(), std::nullopt},
        {"tracks", {{"id", 1}, {"name", "Origin"}}, std::nullopt},
        {"tracks", {{"id", 1.0}}, std::nullopt},
        {"tracks", {{"id", 1}}, Payload{{"id", 1}}}};
    for (std::size_t i = 0; i < malformed.size(); ++i) {
        inputs.rowGuards = {malformed[i]};
        refusal(store, [&] { return OpJournal(store).commitCaptured(requests, source, options, inputs); },
            "malformed row/address admission " + std::to_string(i));
    }
    inputs = {};
    const auto parameterRows = rows(store.db(), "SELECT * FROM plugin_params WHERE device_id=100 ORDER BY device_id,param_id");
    const std::vector<OpRowSetGuard> malformedSets{
        {"plugin_params", {{"device_id", 100}, {"injected", "OR 1=1"}}, parameterRows},
        {"ops", Payload::object(), {}},
        {"plugin_params", {{"device_id", 100}}, {parameterRows.front(), parameterRows.front()}},
        {"plugin_params", {{"device_id", 100}}, {Payload{{"device_id", 100}, {"param_id", "X"}}}}};
    for (std::size_t i = 0; i < malformedSets.size(); ++i) {
        inputs.rowSetGuards = {malformedSets[i]};
        refusal(store, [&] { return OpJournal(store).commitCaptured({}, source, options, inputs); },
            "malformed complete-set admission " + std::to_string(i));
    }
    inputs = {};
    inputs.rowGuards = {{"tracks", {{"id", 1}}, one(store, "SELECT * FROM tracks WHERE id=1")}};
    store.db().exec("ALTER TABLE tracks ADD COLUMN newer_extension BLOB DEFAULT X''");
    refusal(store, [&] { return OpJournal(store).commitCaptured({}, source, options, inputs); },
        "full row guard refuses changed schema even when every older field matches");
    static_cast<void>(token);
}

void presentationImageGuardsUnrelatedMaterial() {
    Project project("presentation_image"); auto& store = *project.store;
    store.db().exec("INSERT INTO tempo_map(id,pos_ticks,bpm,curve,tension) VALUES(51,0,120.0,0,0.0)");
    Owner owner(store, false); owner.presentation->bindImage = true; owner.begin();
    const auto source = appliedSource(store); const auto head = OpJournal(store).headSeq();
    const auto targets = rows(store.db(), "SELECT * FROM tracks WHERE id=1", true);
    const auto mixer = rows(store.db(), "SELECT * FROM mixer_strip WHERE track_id=1", true);
    owner.presentation->afterPrepare = [&] {
        SQLite::Database external(store.path().string(), SQLite::OPEN_READWRITE);
        external.exec("UPDATE tempo_map SET bpm=137.5 WHERE id=51");
    };
    const auto stale = owner.update(3);
    check(!stale.status.ok() && stale.status.error == g::Error::StaleBase && owner.presentation->publishes == 0 &&
        !owner.session->preview(), "presentation image guard refuses unrelated same-head tempo mutation before publication");
    check(rows(store.db(), "SELECT * FROM tracks WHERE id=1", true) == targets &&
        rows(store.db(), "SELECT * FROM mixer_strip WHERE track_id=1", true) == mixer &&
        OpJournal(store).headSeq() == head && appliedSource(store).sameConnection(source) && source.sequence() == 0 &&
        one(store, "SELECT bpm FROM tempo_map WHERE id=51").at("bpm") == 137.5,
        "unrelated-material fixture leaves all producer target guards and source/head unchanged while preserving external tempo");
    owner.presentation->afterPrepare = {};
    require(owner.update(4).status.ok(), "fresh image can preview after unrelated change");
    auto offered = g::detail::offerCaptured(*owner.session);
    require(offered.status.ok() && offered.transaction, "image-guarded captured offer");
    check(offered.transaction->inputs().presentationImage.has_value(),
        "presentation-supplied committed image is sealed into exported transaction admission");
    store.db().exec("UPDATE tempo_map SET curve=1 WHERE id=51");
    refusal(store, [&] { return OpJournal(store).commit(offered.requests); },
        "actual captured commit refuses unrelated same-head material change with zero writes");
    check(offered.transaction->phase() == CapturedSubmissionPhase::Refused,
        "stale full-image commit remains a visible retained refusal");
    owner.session->cancel();
    OpTransactionInputs inputs;
    inputs.presentationImage = OpPresentationImageGuard{engine::capturePresentationImage(store)};
    store.db().exec("ALTER TABLE tempo_map ADD COLUMN future_image_cell ANY");
    refusal(store, [&] { return OpJournal(store).commitCaptured({}, source, headOptions(store), inputs); },
        "presentation image checks unrelated schema even for semantic no-op");
    store.db().exec("UPDATE tempo_map SET future_image_cell=1 WHERE id=51");
    inputs.presentationImage = OpPresentationImageGuard{engine::capturePresentationImage(store)};
    store.db().exec("UPDATE tempo_map SET future_image_cell=CAST(1 AS REAL) WHERE id=51");
    refusal(store, [&] { return OpJournal(store).commitCaptured({}, source, headOptions(store), inputs); },
        "presentation image distinguishes unrelated equal-valued INTEGER and REAL storage cells");
}

OpStateBlob resource(Bytes bytes) {
    const auto hash = media::blake3Bytes(bytes); require(bool(hash), "state hash");
    return {hash.hex, std::move(bytes)};
}
struct BlobOrigin final : g::Origin {};
struct BlobProducer final : g::Producer {
    OpStateBlob blob;
    explicit BlobProducer(OpStateBlob owned) : blob(std::move(owned)) {}
    g::OriginResult captureOrigin(const Store&, const g::Base&) override { return {{}, std::make_unique<BlobOrigin>()}; }
    std::vector<g::TargetClaim> targetClaims(const g::Origin&) const override {
        return {{g::ClaimKind::Structural, g::TargetDomain::Material, 0, {}, {}}};
    }
    g::ProposalResult prepare(const Store& store, const g::Base&, const g::Origin&, const g::Input&) const override {
        g::Proposal result; result.stateBlobs = {blob}; result.requests = {loadState(blob.hash)};
        result.rowGuards = {{"devices", {{"id", 100}}, one(store, "SELECT * FROM devices WHERE id=100")}};
        result.rowSetGuards = {{"plugin_state", {{"device_id", 100}}, rows(store.db(),
            "SELECT * FROM plugin_state WHERE device_id=100 ORDER BY device_id,stream_role")}};
        return {{}, std::move(result)};
    }
};
void typedResourcesAndRollback() {
    Project project("resources"); auto& store = *project.store; const auto source = appliedSource(store);
    const auto options = headOptions(store); const auto chunk = resource({std::byte{0}, std::byte{255}, std::byte{19}});
    const std::vector<OpRequest> requests{loadState(chunk.hash)};
    OpTransactionInputs inputs; inputs.stateBlobs = {chunk};
    refusal(store, [&] { return OpJournal(store).commitCaptured({}, source, options, inputs); }, "resources on empty packet refuse");
    auto bad = inputs; bad.stateBlobs.push_back(chunk);
    refusal(store, [&] { return OpJournal(store).commitCaptured(requests, source, options, bad); }, "duplicate resource hash refuses");
    bad = inputs; bad.stateBlobs[0].hash = "not-a-hash";
    refusal(store, [&] { return OpJournal(store).commitCaptured(requests, source, options, bad); }, "malformed resource hash refuses");
    bad = inputs; bad.stateBlobs[0].bytes.push_back(std::byte{42});
    refusal(store, [&] { return OpJournal(store).commitCaptured(requests, source, options, bad); }, "resource digest/content mismatch refuses");
    const std::vector<OpRequest> unreferenced{rename(1, "Unused")};
    refusal(store, [&] { return OpJournal(store).commitCaptured(unreferenced, source, options, inputs); }, "unreferenced resource refuses");
    auto aborted = options; aborted.beforeCommit = [](SQLite::Database&, std::int64_t, std::string& error) {
        error = "late intentional rollback"; return false;
    };
    auto listener = std::make_shared<Observer>(); auto token = observe(store, listener);
    refusal(store, [&] { return OpJournal(store).commitCaptured(requests, source, aborted, inputs); },
        "late abort rolls back installed resource and plugin/log/head rows", false);
    check(!store.getStateBlob(chunk.hash) && listener->batches.empty(), "late resource failure leaves neither bytes nor committed event");
    Owner owner(store, false); owner.presentation->deviceValues = true;
    g::Spec spec; spec.label = "State gesture"; spec.strategy = g::Strategy::LiveValues;
    auto begun = owner.coordinator->begin(spec, std::make_unique<BlobProducer>(chunk));
    require(begun.status.ok() && begun.session, "blob producer begin"); owner.session = std::move(begun.session);
    const auto before = sqlState(store);
    require(owner.update(1).status.ok(), "resource rollback preview");
    check(sqlState(store) == before && !store.getStateBlob(chunk.hash),
        "preview owns newly installed state bytes after rollback without durable blob side effects");
    auto offer = g::detail::offerCaptured(*owner.session);
    require(offer.status.ok() && offer.transaction, "resource captured offer");
    check(offer.transaction->inputs().stateBlobs.size() == 1 && offer.transaction->inputs().stateBlobs.front().bytes == chunk.bytes,
        "captured capability retains exact opaque resource bytes");
    owner.session->cancel();
    check(sqlState(store) == before && offer.transaction->phase() == CapturedSubmissionPhase::Cancelled,
        "cancel invalidates exported resource offer and writes no blob or journal data");
    refusal(store, [&] { return OpJournal(store).commit(offer.requests); }, "cancelled resource packet refuses");
    const auto success = OpJournal(store).commitCaptured(requests, source, options, inputs);
    check(success.ok && store.getStateBlob(chunk.hash) == std::optional<Bytes>(chunk.bytes) && listener->batches.size() == 1,
        "captured resource installs atomically before real handler and owned applied event");
    check(History(store).undo().ok && rows(store.db(), "SELECT * FROM plugin_state").empty(),
        "resource-backed state first-touch Undo clears the original absence");
    check(History(store).redo().ok && one(store, "SELECT state_hash FROM plugin_state WHERE device_id=100").at("state_hash") == chunk.hash,
        "resource-backed state Redo survives transient capability destruction");
    // An existing content-addressed row must agree on both bytes and declared size.
    store.db().exec("UPDATE state_blobs SET size_bytes=999");
    refusal(store, [&] { return OpJournal(store).commitCaptured(requests, source, headOptions(store), inputs); },
        "existing resource size inconsistency refuses before mutation");
    static_cast<void>(token);
}

void capturedOfferProtocol() {
    Project project("offers"); auto& store = *project.store; Owner owner(store);
    require(owner.update(5).status.ok(), "offered preview");
    auto offer = g::detail::offerCaptured(*owner.session);
    require(offer.status.ok() && offer.transaction && offer.requests.size() == 2, "complete captured offer");
    check(offer.transaction->phase() == CapturedSubmissionPhase::Offered &&
        offer.requests[0].capturedTransaction == offer.transaction && offer.requests[1].capturedTransaction == offer.transaction,
        "every exported effect carries one sealed complete-transaction capability");
    check(offer.transaction->requests().size() == 2 && !offer.transaction->requests()[0].capturedTransaction &&
        !offer.transaction->requests()[1].capturedTransaction, "sealed owned packet avoids self-referential capability cycle");
    std::vector<std::vector<OpRequest>> malformed;
    auto packet = offer.requests; packet.pop_back(); malformed.push_back(packet);
    packet = offer.requests; std::reverse(packet.begin(), packet.end()); malformed.push_back(packet);
    packet = offer.requests; packet[0].payload["name"] = "tampered"; malformed.push_back(packet);
    packet = offer.requests; packet[0].label = "wrong gesture"; malformed.push_back(packet);
    packet = offer.requests; packet[0].selBefore = Payload{{"tracks", Payload::array({2})}}; malformed.push_back(packet);
    packet = offer.requests; packet[0].targetId = 200; malformed.push_back(packet);
    packet = offer.requests; packet[1].capturedTransaction.reset(); malformed.push_back(packet);
    packet = offer.requests; packet.push_back(rename(2, "raw suffix")); malformed.push_back(packet);
    packet = offer.requests; packet[1].actor = Actor::Script; malformed.push_back(packet);
    for (std::size_t i = 0; i < malformed.size(); ++i) {
        refusal(store, [&] { return OpJournal(store).commit(malformed[i]); }, "tampered captured group " + std::to_string(i));
        check(offer.transaction->phase() == CapturedSubmissionPhase::Offered,
            "invalid outer packet never poisons authentic offered capability");
    }
    const auto reoffered = g::detail::offerCaptured(*owner.session);
    check(reoffered.status.ok() && reoffered.requests.size() == 2 && OpJournal(store).count() == 0,
        "dropped output vector can be offered again without losing retained intent");
    auto options = headOptions(store); options.beforeCommit = [](SQLite::Database&, std::int64_t, std::string& error) {
        error = "authentic submit rollback"; return false;
    };
    const auto active = g::detail::offerCaptured(*owner.session);
    require(active.status.ok() && active.transaction, "active offer before refusal");
    refusal(store, [&] { return OpJournal(store).commit(active.requests, options); }, "authentic failure retains capability and bytes", false);
    check(active.transaction->phase() == CapturedSubmissionPhase::Refused && !active.transaction->lastRefusal().empty() &&
        !History(store).undo().ok, "refused authentic submission retains visible reason and navigation lease");
    refusal(store, [&] { return OpJournal(store).commit(active.requests); }, "refused capability requires explicit retry");
    auto retry = g::detail::retryCaptured(*owner.session);
    require(retry.status.ok() && retry.transaction, "explicit captured retry");
    check(retry.transaction != active.transaction && active.transaction->phase() == CapturedSubmissionPhase::Superseded,
        "explicit retry seals fresh admission and permanently supersedes old external packet");
    refusal(store, [&] { return OpJournal(store).commit(active.requests); }, "superseded packet refuses");
    auto observer = std::make_shared<Observer>(); observer->callback = [] { throw std::runtime_error("intentional observer failure"); };
    auto token = observe(store, observer);
    for (auto& request : retry.requests) { request.actor = Actor::User; request.actorDetail = "legacy submitter"; }
    const auto result = OpJournal(store).commit(retry.requests);
    check(result.ok && retry.transaction->phase() == CapturedSubmissionPhase::Durable &&
        retry.transaction->durableTxn() == result.txnId && token.state() == AppliedSubscriptionState::ObserverFailed,
        "permitted whole-group actor stamp and throwing observer preserve durable capability result");
    const auto count = OpJournal(store).count();
    refusal(store, [&] { return OpJournal(store).commit(retry.requests); }, "durable packet cannot submit twice");
    owner.coordinator->drain(20);
    check(owner.session->state() == g::State::Committed && OpJournal(store).count() == count,
        "coordinator acknowledges external durable success without resubmission");
    check(History(store).undo().ok && History(store).redo().ok && OpJournal(store).count() == count,
        "history replay needs no transient captured capability");
    const auto log = OpJournal(store).recent();
    check(log.size() == 2 && log[0].payload.size() == 2 && log[1].payload.size() == 2 &&
        log[1].actor == "user" && log[1].actorDetail == "legacy submitter",
        "captured transaction metadata never leaks into persistent payload");
}

void navigationAndRetainedCancellation() {
    Project project("navigation"), replaySource("replay_source"); auto& store = *project.store;
    const auto root = History(store).takeSnapshot("root", 1, 0); require(root.ok, "root snapshot");
    const auto initial = project.commit(rename(2, "History opener"));
    const auto current = History(store).takeSnapshot("current", 2, 0); require(current.ok, "current snapshot");
    const auto branch = OpJournal(store).currentBranchId();
    Owner owner(store); require(owner.update(2).status.ok(), "navigation preview");
    const auto before = sqlState(store); const auto source = appliedSource(store);
    {
        Writes writes(store);
        check(!History(store).undo().ok, "live gesture refuses Undo before applying an inverse");
        check(!History(store).redo().ok, "live gesture refuses Redo even without a redo candidate");
        check(!History(store).switchToBranch(branch).ok, "live gesture refuses same-head branch switch");
        check(!History(store).switchToBranch(999999).ok, "live gesture refuses nonexistent branch before lookup mutation");
        check(!History(store).revertToSnapshot(root.id, 3).ok && !History(store).revertToSnapshot(current.id, 3).ok,
            "live gesture refuses both moving and same-head snapshot reverts");
        check(!History::restoreReplayBase(*replaySource.store, store).ok,
            "live target refuses replay-base restore even when source has no checkpoint");
        check(!OpJournal(store).setHeadSeq(initial.seqs.back()), "public same-head setter respects live gesture lease");
        check(!store.close() && !store.saveAs(project.folder.path() / "blocked.adi").ok && source.alive(),
            "explicit close and SaveAs refuse while live lease preserves source identity");
        check(writes.all == 0 && sqlState(store) == before, "all navigation refusals precede SQL writes");
    }
    check(History(store).readTimeline().error.empty(), "history timeline reads remain available during gesture");
    {
        Project empty("empty_replay_target"); Owner lease(*empty.store);
        const auto unchanged = sqlState(*empty.store);
        check(!History::restoreReplayBase(*replaySource.store, *empty.store).ok && sqlState(*empty.store) == unchanged,
            "empty replay target lease refuses absent-checkpoint fast path before any material navigation");
    }
    {
        Project redo("real_redo"); redo.commit(rename(2, "Redo target"));
        require(History(*redo.store).undo().ok, "establish redo candidate"); Owner lease(*redo.store);
        const auto unchanged = sqlState(*redo.store);
        check(!History(*redo.store).redo().ok && sqlState(*redo.store) == unchanged,
            "live gesture refuses a real available Redo without applying its forward packet");
    }
    const auto named = History(store).takeSnapshot("allowed metadata", 4, 0);
    check(named.ok && History(store).renameSnapshot(named.id, "renamed metadata").ok && History(store).removeSnapshot(named.id).ok,
        "snapshot naming metadata remains independent of navigation fence");
    owner.presentation->restoration->value = g::PresentationState::Refused;
    owner.presentation->restoration->text = "runtime restoration intentionally refused";
    const auto cancelled = owner.session->cancel();
    check(cancelled.presentation.state == g::PresentationState::Refused && owner.session->state() == g::State::Cancelling &&
        !owner.update(3).status.ok() && !owner.session->reapply().status.ok(),
        "failed cancel enters sticky Cancelling and cannot resurrect accepted intent");
    const auto pending = sqlState(store); const auto commit = owner.session->commit();
    check((!commit.durable || !commit.durable->ok) && sqlState(store) == pending && !History(store).undo().ok,
        "Cancelling refuses commit and retains navigation lease without SQL changes");
    owner.session.reset();
    check(owner.coordinator->pendingRestorations() != 0 && !store.close(),
        "public handle destruction retains failed restoration entry and source lease");
    const auto count = OpJournal(store).count();
    const auto retryFailed = owner.coordinator->retryRestorations(10);
    check(retryFailed.pendingRestorations != 0 && OpJournal(store).count() == count,
        "abandoned restoration remains discoverable after another refused retry");
    owner.presentation->restoration->value = g::PresentationState::Current;
    const auto retried = owner.coordinator->retryRestorations(11);
    check(retried.pendingRestorations == 0 && owner.coordinator->pendingRestorations() == 0 &&
        History(store).undo().ok && OpJournal(store).count() == count,
        "completed abandoned restoration releases lease without submission");
    owner.presentation->inspectSql = false;
    const auto saved = store.saveAs(project.folder.path() / "after_cancel.adi");
    check(saved.ok && appliedSource(store).isReplacementOf(source), "successful cancel permits ordinary SaveAs replacement");
}

void noOpLifecycleAndDurableFailure() {
    for (bool nonNull : {false, true}) {
        Project project(nonNull ? "noop_nonnull" : "noop_null");
        if (nonNull) project.commit(rename(2, "Existing head"));
        Owner owner(*project.store); auto observer = std::make_shared<Observer>(); auto token = observe(*project.store, observer);
        const auto before = sqlState(*project.store);
        require(owner.update(0).status.ok(), "semantic no-op accepted");
        {
            Writes writes(*project.store); const auto result = owner.session->commit();
            check(result.status.ok() && result.semanticNoOp && writes.all == 0 && sqlState(*project.store) == before && observer->batches.empty(),
                nonNull ? "non-NULL-head gesture no-op closes without SQL or notification" : "NULL-head gesture no-op closes without SQL or notification");
        }
        check(owner.session->state() == g::State::Committed && g::detail::beginNavigation(*project.store).status.ok(),
            "accepted no-op releases navigation lease");
        static_cast<void>(token);
    }
    Project project("durable_failure"); Owner owner(*project.store);
    require(owner.update(7).status.ok(), "postcommit failure preview");
    auto observer = std::make_shared<Observer>();
    bool refusedReentry = false; const auto prepares = owner.stats->preparations;
    observer->callback = [&] { refusedReentry = !owner.update(8).status.ok(); };
    auto token = observe(*project.store, observer);
    owner.presentation->throwPromote = true;
    const auto result = owner.session->commit();
    check(result.durable && result.durable->ok && owner.session->state() == g::State::Committed &&
        result.presentation.state != g::PresentationState::Current,
        "postcommit presentation exception reports durable success separately from runtime failure");
    check(refusedReentry && owner.stats->preparations <= prepares + 1,
        "observer reentrancy is refused before evaluating a new gesture input");
    const auto count = OpJournal(*project.store).count();
    owner.session->commit(); owner.coordinator->drain(100);
    check(OpJournal(*project.store).count() == count && one(*project.store,
        "SELECT name FROM tracks WHERE id=1").at("name") == "Origin/7",
        "presentation failure and retries cannot duplicate or replace durable gesture");
    owner.presentation->throwPromote = false;
    static_cast<void>(token);
}

void sourceDestructionAndSqlOwnership() {
    Project project("source_death"); Owner owner(*project.store);
    require(owner.update(6).status.ok(), "source lifetime preview");
    auto preview = owner.session->preview(); const auto path = project.store->path();
    const auto source = appliedSource(*project.store); const auto original = digestProject(*project.store).text;
    owner.presentation->restoration->value = g::PresentationState::Refused;
    owner.presentation->restoration->text = "retain runtime cleanup after SQL death";
    owner.presentation->inspectSql = false; project.store.reset();
    check(!source.alive() && owner.session->state() == g::State::SourceLost && !owner.update(9).status.ok(),
        "forced Store destruction invalidates live gesture without dereferencing destroyed SQL");
    const auto commit = owner.session->commit();
    check(!commit.durable || !commit.durable->ok, "source-lost gesture cannot commit into a different connection");
    check(snapshotName(preview, "Origin/6"),
        "owned preview projection survives source destruction");
    check(owner.presentation->restoration->state() == g::PresentationState::Refused,
        "source destruction does not misreport refused runtime cleanup as restored");
    StoreError error{}; project.store = Store::open(path, error); require(bool(project.store), "source reopen");
    check(digestProject(*project.store).text == original && OpJournal(*project.store).count() == 0 &&
        !appliedSource(*project.store).sameConnection(source), "source destruction cancels without write and reopen has independent identity");
    owner.session.reset(); owner.coordinator.reset();
    Project sql("active_transaction"); Owner transactionOwner(*sql.store);
    sql.store->db().exec("BEGIN IMMEDIATE");
    const auto rejected = transactionOwner.update(1);
    check(!rejected.status.ok() && sqlite3_get_autocommit(sql.store->db().getHandle()) == 0 &&
        one(*sql.store, "SELECT name FROM tracks WHERE id=1").at("name") == "Origin",
        "caller-owned SQL transaction refuses preview without rolling back caller ownership");
    sql.store->db().exec("ROLLBACK");
    require(transactionOwner.update(2).status.ok(), "update after caller rollback");
    bool recursionRefused = false;
    transactionOwner.presentation->beforePublish = [&] { recursionRefused = !transactionOwner.update(3).status.ok(); };
    const auto updated = transactionOwner.update(4);
    check(updated.status.ok() && recursionRefused && updated.preview && updated.preview->requests()[0].payload.at("name") == "Origin/4",
        "presentation callback reentrancy cannot overwrite the accepted input or recursively publish");
    transactionOwner.presentation->beforePublish = {};
}

void tempoCanonicalAndLegacyPackets() {
    Project project("tempo"); auto& store = *project.store;
    store.db().exec("INSERT INTO tempo_map(id,pos_ticks,bpm,curve,tension) VALUES(8000000001,960,123.5,2,0.375)");
    const auto opening = rows(store.db(), "SELECT * FROM tempo_map ORDER BY id", true);
    Owner owner(store, false); g::Spec spec; spec.label = "Tap Tempo";
    auto begun = owner.coordinator->begin(spec, g::tempoProducer(960));
    require(begun.status.ok() && begun.session, "canonical existing tempo begin"); owner.session = std::move(begun.session);
    for (double bpm : {128.0, 141.25, 99.75}) {
        const auto preview = owner.session->update(std::make_unique<g::TempoInput>(bpm));
        require(preview.status.ok() && preview.preview, "canonical tempo preview");
        check(preview.preview->requests().size() == 2 &&
            preview.preview->requests()[0].opType == "project.removeTempoEvent" &&
            preview.preview->requests()[0].payload == Payload{{"pos", 960}} &&
            preview.preview->requests()[1].opType == "project.insertTempoEvent" &&
            preview.preview->requests()[1].payload == Payload{{"id", std::int64_t{8000000001}}, {"pos", 960},
                {"bpm", bpm}, {"curve", 2}, {"tension", 0.375}},
            "tempo replacement preserves sparse literal id, position, curve and tension in existing opcodes");
        check(rows(store.db(), "SELECT * FROM tempo_map ORDER BY id", true) == opening,
            "tempo preview leaves complete typed stored row untouched");
    }
    const auto result = owner.session->commit();
    check(result.durable && result.durable->ok && one(store, "SELECT * FROM tempo_map WHERE pos_ticks=960") ==
        Payload{{"id", std::int64_t{8000000001}}, {"pos_ticks", 960}, {"bpm", 99.75}, {"curve", 2}, {"tension", 0.375}},
        "canonical tempo replacement commits only the requested bpm");
    const auto log = OpJournal(store).recent();
    check(log.size() == 2 && log[1].inverse == Payload{{"id", std::int64_t{8000000001}}, {"pos", 960},
        {"bpm", 123.5}, {"curve", 2}, {"tension", 0.375}}, "tempo remove inverse captures literal id and nonzero tension");
    check(History(store).undo().ok && rows(store.db(), "SELECT * FROM tempo_map ORDER BY id", true) == opening,
        "tempo gesture Undo restores exact native canonical row");
    check(History(store).redo().ok && one(store, "SELECT id,tension,bpm FROM tempo_map WHERE pos_ticks=960") ==
        Payload{{"id", std::int64_t{8000000001}}, {"tension", 0.375}, {"bpm", 99.75}}, "tempo gesture Redo retains full identity");
    const auto legacy = operation("project.insertTempoEvent", {{"pos", 1920}, {"bpm", 110.0}, {"curve", 1}});
    project.commit(legacy);
    check(one(store, "SELECT bpm,curve,tension FROM tempo_map WHERE pos_ticks=1920") ==
        Payload{{"bpm", 110.0}, {"curve", 1}, {"tension", 0.0}}, "legacy tempo insertion without id/tension retains old defaults");
    check(History(store).undo().ok && History(store).redo().ok,
        "legacy tempo insertion remains undoable and replayable without new optional fields");
    project.commit(operation("project.removeTempoEvent", {{"pos", 1920}}));
    const auto oldInverse = encodePayload(Payload{{"pos", 1920}, {"bpm", 110.0}, {"curve", 1}});
    {
        SQLite::Statement update(store.db(), "UPDATE ops SET inverse=? WHERE seq=?");
        update.bind(1, oldInverse.data(), static_cast<int>(oldInverse.size()));
        update.bind(2, *OpJournal(store).headSeq()); update.exec();
    }
    check(History(store).undo().ok && one(store, "SELECT bpm,curve,tension FROM tempo_map WHERE pos_ticks=1920") ==
        Payload{{"bpm", 110.0}, {"curve", 1}, {"tension", 0.0}}, "historical remove inverse lacking id/tension still dispatches literally");
    Project extension("tempo_extension");
    extension.store->db().exec("INSERT INTO tempo_map(id,pos_ticks,bpm,curve,tension) VALUES(42,0,120.0,2,0.5);"
        "ALTER TABLE tempo_map ADD COLUMN future_any ANY");
    Owner extended(*extension.store, false);
    auto unsupported = extended.coordinator->begin(spec, g::tempoProducer(0));
    bool refused = !unsupported.status.ok();
    if (unsupported.session) refused = !unsupported.session->update(std::make_unique<g::TempoInput>(130.0)).status.ok();
    check(refused && OpJournal(*extension.store).count() == 0,
        "existing tempo producer explicitly refuses an extended noncanonical row instead of dropping unknown columns");
}

void maintenanceRetainsOwnedInput() {
    Project project("maintenance"); auto& store = *project.store;
    for (int i = 0; i < 4; ++i) project.commit(rename(2, "Before/" + std::to_string(i)));
    Owner owner(store, false); owner.presentation->deviceValues = true;
    const auto chunk = resource({std::byte{1}, std::byte{0}, std::byte{254}});
    g::Spec spec; spec.label = "State survives history maintenance"; spec.strategy = g::Strategy::LiveValues;
    auto begun = owner.coordinator->begin(spec, std::make_unique<BlobProducer>(chunk));
    require(begun.status.ok() && begun.session, "maintenance gesture begin"); owner.session = std::move(begun.session);
    require(owner.update(1).status.ok(), "maintenance resource preview");
    auto offered = g::detail::offerCaptured(*owner.session); require(offered.status.ok() && offered.transaction, "maintenance offer");
    const auto source = appliedSource(store); const auto head = OpJournal(store).headSeq();
    const auto material = rows(store.db(), "SELECT * FROM tracks ORDER BY id", true);
    auto observer = std::make_shared<Observer>(); auto token = observe(store, observer);
    UndoRetentionPolicy policy; policy.maxTransactions = 1;
    const auto retention = History(store).setUndoRetentionPolicy(policy); const auto compact = History(store).compactUndo();
    check(retention.ok && compact.ok && OpJournal(store).headSeq() == head && appliedSource(store).sameConnection(source) &&
        rows(store.db(), "SELECT * FROM tracks ORDER BY id", true) == material,
        "explicit retention/compaction maintenance is allowed while source and current material remain intact");
    check(observer->batches.empty() && offered.transaction->inputs().stateBlobs.front().bytes == chunk.bytes &&
        offered.transaction->phase() == CapturedSubmissionPhase::Offered && !store.getStateBlob(chunk.hash),
        "temporary checkpoint replay emits no fake event and preserves owned unpersisted resource/input");
    const auto committed = OpJournal(store).commit(offered.requests);
    check(committed.ok && store.getStateBlob(chunk.hash) == std::optional<Bytes>(chunk.bytes),
        "retained captured transaction survives permitted checkpoint maintenance");
    owner.coordinator->drain(100);
    check(History(store).undo().ok && rows(store.db(), "SELECT * FROM plugin_state").empty(),
        "resource gesture remains one correct Undo after retention maintenance");
    static_cast<void>(token);
}

void producerReservationsFenceNavigation() {
    Project project("producer_fence"); auto& store = *project.store;
    const auto source = appliedSource(store); auto registered = g::detail::registerProducer(store, 8);
    require(registered.status.ok() && registered.fence, "register bounded producer fence");
    check(registered.fence->reserve() == g::detail::ProducerFence::Reservation::BeforeNavigation,
        "queued callback reservation is visible before ring publication");
    check(!g::detail::beginNavigation(store).status.ok() && !History(store).undo().ok && !store.close(),
        "undrained Begin reservation fences navigation and close without an owner session");
    check(registered.fence->discardUnchanged(1).ok(), "classified unchanged callback releases exactly one reservation");
    {
        auto permit = g::detail::beginNavigation(store); require(permit.status.ok(), "navigation permit after acknowledgment");
        check(registered.fence->reserve() == g::detail::ProducerFence::Reservation::AfterNavigation,
            "callback admitted after navigation linearization remains queued for later ownership");
    }
    check(!g::detail::beginNavigation(store).status.ok(), "releasing navigation permit preserves later queued callback count");
    check(registered.fence->discardUnchanged(1).ok() && !registered.fence->discardUnchanged(1).ok(),
        "owner acknowledgment refuses reservation underflow");
    require(registered.fence->reserve() != g::detail::ProducerFence::Reservation::RejectedClosed, "overflow fixture reservation");
    registered.fence->publicationFailed();
    check(!g::detail::beginNavigation(store).status.ok(), "failed ring publication latches loss before releasing last count");
    check(registered.fence->acknowledgeLoss().ok() && g::detail::beginNavigation(store).status.ok(),
        "explicit owner loss recovery reopens navigation only after classified quiescent fixture");
    Owner owner(store, false);
    require(registered.fence->reserve() != g::detail::ProducerFence::Reservation::RejectedClosed, "pending Begin reservation");
    const std::vector<g::TargetClaim> claims{{g::ClaimKind::Structural, g::TargetDomain::Material, 0, {}, {}}};
    g::Spec spec; spec.label = "Pending callback";
    auto pending = g::detail::reservePending(*owner.coordinator, claims, owner.presentation, spec,
        std::make_unique<OriginProducer>(owner.stats), std::make_unique<Delta>(3));
    require(pending.status.ok() && pending.capture, "reserve owned callback input");
    check(registered.fence->acknowledge(*pending.capture, 1).ok() && !g::detail::beginNavigation(store).status.ok(),
        "pending capture lease is installed before callback reservation is acknowledged");
    auto adopted = g::detail::adoptPending(*owner.coordinator, *pending.capture);
    require(adopted.status.ok() && adopted.session, "adopt queued callback"); owner.session = std::move(adopted.session);
    check(!owner.session->originBase().headAtCapture && !owner.session->originBase().sequenceAtCapture &&
        owner.session->currentBase().head == OpJournal(store).headSeq(),
        "callback origin admits unknown original SQL head separately from current coherent base");
    owner.session->cancel(); pending.capture.reset(); owner.session.reset();
    owner.presentation->inspectSql = false;
    check(store.close() && !source.alive() &&
        registered.fence->reserve() == g::detail::ProducerFence::Reservation::RejectedClosed,
        "source invalidation closes stable producer token before retained callback memory dies");
}

void failedNavigationPermitDoesNotReleaseOwner() {
    Project project("permit_ownership"); auto& store = *project.store;
    auto registered = g::detail::registerProducer(store, 4);
    require(registered.status.ok() && registered.fence, "register permit ownership producer");
    {
        auto first = g::detail::beginNavigation(store);
        require(first.status.ok(), "first navigation permit acquired");
        {
            auto second = g::detail::beginNavigation(store);
            check(!second.status.ok(), "second navigation permit refuses while first owns navigation bit");
            auto movedFailure = std::move(second.permit);
            movedFailure = g::detail::NavigationPermit{};
            second.permit = g::detail::NavigationPermit{};
        }
        check(registered.fence->reserve() == g::detail::ProducerFence::Reservation::AfterNavigation,
            "failed permit preallocation, move, reset and destruction cannot release another permit's navigation bit");
    }
    check(!g::detail::beginNavigation(store).status.ok(),
        "actual permit owner release preserves callback reservation admitted while it was held");
    check(registered.fence->discardUnchanged(1).ok(), "acknowledge callback retained after real permit release");
    check(g::detail::beginNavigation(store).status.ok(),
        "navigation becomes available only after real owner release and callback acknowledgment");
}

void unrelatedProducerCannotClearCaptureFault() {
    for (bool publicationFailure : {false, true}) {
        Project project(publicationFailure ? "foreign_publication_fault" : "foreign_stable_fault");
        auto& store = *project.store;
        auto clean = g::detail::registerProducer(store, 4);
        auto faulted = g::detail::registerProducer(store, 4);
        require(clean.status.ok() && clean.fence && faulted.status.ok() && faulted.fence,
            "register independent clean and faulted producers");
        if (publicationFailure) {
            require(faulted.fence->reserve() != g::detail::ProducerFence::Reservation::RejectedClosed,
                "faulted producer reserves before failed publication");
            faulted.fence->publicationFailed();
        } else faulted.fence->captureFault();
        check(!g::detail::beginNavigation(store).status.ok(),
            "unresolved producer fault blocks navigation even with no outstanding reservation");
        static_cast<void>(clean.fence->acknowledgeLoss());
        check(!g::detail::beginNavigation(store).status.ok(),
            "clean producer recovery cannot clear another producer's unreconciled capture fault");
        static_cast<void>(clean.fence->acknowledgeLoss());
        check(!g::detail::beginNavigation(store).status.ok(),
            "repeated unrelated recovery cannot clear stable foreign fault without a new fault epoch");
        clean.fence.reset();
        check(!g::detail::beginNavigation(store).status.ok(),
            "unrelated producer detachment preserves unresolved foreign fault ownership");
        check(faulted.fence->acknowledgeLoss().ok() && g::detail::beginNavigation(store).status.ok(),
            "only explicit recovery of the faulted producer reopens navigation");
    }
    Project project("foreign_state_reset"); auto& store = *project.store; Owner owner(store, false);
    auto cleanState = g::detail::registerStateProducer(store, 4);
    auto faulted = g::detail::registerProducer(store, 4);
    require(cleanState.status.ok() && cleanState.fence && faulted.status.ok() && faulted.fence,
        "register clean state producer and independently faulted event producer");
    g::Spec spec; spec.label = "State cleanup recovery lease";
    auto pending = g::detail::reservePending(*owner.coordinator,
        {{g::ClaimKind::Structural, g::TargetDomain::Material, 0, {}, {}}}, owner.presentation, spec,
        std::make_unique<OriginProducer>(owner.stats), std::make_unique<Delta>(0));
    require(pending.status.ok() && pending.capture, "state reset owns its recovery entry");
    auto adopted = g::detail::adoptPending(*owner.coordinator, *pending.capture);
    require(adopted.status.ok() && adopted.session, "adopt state cleanup owner"); owner.session = std::move(adopted.session);
    faulted.fence->captureFault();
    check(cleanState.fence->resetAfterDetach(*pending.capture).ok(), "quiescent clean state producer can reset its own prefix");
    owner.session->cancel();
    check(owner.session->state() == g::State::Cancelled, "recovery entry can cancel while another producer remains faulted");
    owner.session.reset(); pending.capture.reset();
    check(!g::detail::beginNavigation(store).status.ok(),
        "clean state reset cannot erase foreign capture fault after its own recovery lease is released");
    cleanState.fence.reset();
    check(!g::detail::beginNavigation(store).status.ok(),
        "clean state producer detachment cannot erase stable foreign capture fault");
    check(faulted.fence->acknowledgeLoss().ok() && g::detail::beginNavigation(store).status.ok(),
        "foreign fault remains recoverable by its actual producer after unrelated state cleanup");
}

void sourceChangeFinalizationPreservesLateCapture() {
    Project project("source_finalization"); auto& store = *project.store;
    const auto source = appliedSource(store); const auto initial = sqlState(store);
    auto producer = g::detail::registerProducer(store, 4);
    require(producer.status.ok() && producer.fence, "register source-change producer");
    {
        auto change = g::detail::beginSourceChange(store);
        require(change.status.ok(), "begin exclusive source change");
        check(producer.fence->reserve() == g::detail::ProducerFence::Reservation::AfterNavigation,
            "callback arriving during source-copy window remains accepted and reserved");
        const auto refused = g::detail::finishSourceChange(change.permit);
        check(!refused.ok() && !refused.message.empty() && source.alive() &&
            appliedSource(store).sameConnection(source) && sqlState(store) == initial,
            "late callback makes source finalization refuse without changing connection or captured material");
    }
    check(!g::detail::beginSourceChange(store).status.ok() && !g::detail::beginNavigation(store).status.ok(),
        "failed source finalization releases only its permit and preserves pending capture count");
    check(producer.fence->discardUnchanged(1).ok(), "explicitly classify retained late callback before source-change retry");
    {
        auto retry = g::detail::beginSourceChange(store);
        require(retry.status.ok(), "source finalization retry after capture acknowledgment");
        check(g::detail::finishSourceChange(retry.permit).ok() &&
            producer.fence->reserve() == g::detail::ProducerFence::Reservation::RejectedClosed,
            "successful atomic finalization closes admission to subsequent callback capture");
    }
    project.store.reset();
    check(!source.alive(), "forced teardown invalidates source after direct finalization fixture");

    Project saved("saveas_fold"); const auto oldPath = saved.store->path();
    const auto oldSource = appliedSource(*saved.store);
    auto oldProducer = g::detail::registerProducer(*saved.store, 4);
    require(oldProducer.status.ok() && oldProducer.fence, "register SaveAs source producer");
    saved.commit(rename(2, "Durable before SaveAs"));
    const auto result = saved.store->saveAs(saved.folder.path() / "replacement.adi");
    check(result.ok && result.warning.empty() && appliedSource(*saved.store).isReplacementOf(oldSource) &&
        !oldSource.alive() && oldProducer.fence->reserve() == g::detail::ProducerFence::Reservation::RejectedClosed,
        "race-free SaveAs switches identity without a false close warning and closes old capture fence");
    check(!std::filesystem::exists(oldPath.string() + "-wal") && !std::filesystem::exists(oldPath.string() + "-shm"),
        "successful SaveAs physically checkpoints and removes old connection sidecars");
    {
        SQLite::Database previous(oldPath.string(), SQLite::OPEN_READONLY);
        check(rows(previous, "PRAGMA journal_mode").front().at("journal_mode") == "delete" &&
            rows(previous, "SELECT name FROM tracks WHERE id=2").front().at("name") == "Durable before SaveAs",
            "old SaveAs source is a standalone folded project containing its committed material");
    }
}

void stateWatermarkPrefixOwnership() {
    Project project("state_fence"); auto& store = *project.store; Owner owner(store, false);
    auto registered = g::detail::registerStateProducer(store, 4);
    require(registered.status.ok() && registered.fence, "register bounded state fence");
    require(registered.fence->beginSignal(), "first state signal admitted");
    const auto inFlight = registered.fence->watermarks();
    check(inFlight.reserved == 1 && inFlight.published == 0 && inFlight.consumed == 0 &&
        !g::detail::beginNavigation(store).status.ok(), "in-flight opaque signal fences navigation before SDK epoch publication");
    registered.fence->finishSignal();
    g::Spec spec; spec.label = "Owned state signal";
    auto pending = g::detail::reservePending(*owner.coordinator,
        {{g::ClaimKind::Structural, g::TargetDomain::Material, 0, {}, {}}}, owner.presentation, spec,
        std::make_unique<OriginProducer>(owner.stats), std::make_unique<Delta>(1));
    require(pending.status.ok() && pending.capture, "state signal retained owner");
    require(registered.fence->beginSignal(), "second signal admitted after snapshot");
    check(registered.fence->acknowledgePrefix(*pending.capture, 1).ok(), "owned stable published prefix can transfer to lease");
    const auto prefix = registered.fence->watermarks();
    check(prefix.reserved == 2 && prefix.published == 1 && prefix.consumed == 1 &&
        !g::detail::beginNavigation(store).status.ok(), "prefix acknowledgment cannot erase newer unpublished signal");
    check(!registered.fence->acknowledgePrefix(*pending.capture, 2).ok(), "unpublished future watermark cannot be acknowledged");
    registered.fence->finishSignal();
    check(registered.fence->acknowledgePrefix(*pending.capture, 2).ok() &&
        registered.fence->watermarks().consumed == 2 && !g::detail::beginNavigation(store).status.ok(),
        "complete prefix leaves pending owner lease responsible for accepted input");
    auto adopted = g::detail::adoptPending(*owner.coordinator, *pending.capture);
    require(adopted.status.ok() && adopted.session, "adopt state pending owner"); owner.session = std::move(adopted.session);
    owner.session->cancel(); owner.session.reset(); pending.capture.reset();
    check(g::detail::beginNavigation(store).status.ok(), "cancel and full prefix release permit navigation");
    owner.presentation->inspectSql = false; project.store.reset();
    check(registered.fence->watermarks().closed && !registered.fence->beginSignal(),
        "state fence retained after Store destruction is closed without a dangling SQL reference");
}

void scalarClaimsAndWrongThread() {
    Project project("claims"); auto& store = *project.store; Owner owner(store, false);
    g::Spec spec; spec.strategy = g::Strategy::LiveValues; spec.label = "Volume endpoint";
    auto first = owner.coordinator->begin(spec, g::scalarProducer({g::ScalarKind::TrackVolume, 1}));
    require(first.status.ok() && first.session, "first scalar claim");
    auto duplicate = owner.coordinator->begin(spec, g::scalarProducer({g::ScalarKind::TrackVolume, 1}));
    check(!duplicate.status.ok() && !duplicate.session, "overlapping scalar endpoint ownership is refused without implicit commit");
    auto disjoint = owner.coordinator->begin(spec, g::scalarProducer({g::ScalarKind::TrackPan, 1}));
    require(disjoint.status.ok() && disjoint.session, "disjoint pan endpoint");
    const auto before = sqlState(store);
    require(first.session->update(std::make_unique<g::ScalarInput>(-9.0)).status.ok(), "concrete volume preview");
    require(disjoint.session->update(std::make_unique<g::ScalarInput>(-0.5)).status.ok(), "concrete pan preview");
    check(sqlState(store) == before && OpJournal(store).count() == 0,
        "disjoint concrete scalar owners preview independently without SQL mutation or grouping");
    bool rejected = false;
    std::thread foreign([&] {
        const auto result = first.session->update(std::make_unique<g::ScalarInput>(-10.0));
        rejected = !result.status.ok() && result.status.error == g::Error::WrongThread;
    });
    foreign.join();
    check(rejected && sqlState(store) == before, "foreign-thread update refuses before SQL or presentation access");
    disjoint.session->cancel();
    check(!g::detail::beginNavigation(store).status.ok(), "cancelling one disjoint owner retains the other owner's lease");
    const auto committed = first.session->commit();
    check(committed.durable && committed.durable->ok && one(store, "SELECT volume_db,pan FROM mixer_strip WHERE track_id=1") ==
        Payload{{"volume_db", -9.0}, {"pan", 0.25}}, "concrete scalar commit changes only its owned endpoint");
    check(History(store).undo().ok && one(store, "SELECT volume_db,pan FROM mixer_strip WHERE track_id=1") ==
        Payload{{"volume_db", -6.0}, {"pan", 0.25}}, "concrete scalar inverse preserves cancelled disjoint endpoint");
    auto latch = owner.coordinator->begin(spec, g::trackSwitchProducer({g::TrackSwitchKind::Activation, 1, {1, 2}, false, false}));
    require(latch.status.ok() && latch.session, "momentary activation latch begin");
    for (int i = 0; i < 3; ++i) {
        const auto updated = latch.session->update(std::make_unique<g::TrackSwitchInput>(g::TrackSwitchValue::ToggleFromOrigin));
        require(updated.status.ok() && updated.preview, "activation latch preview");
        check(updated.preview->requests().size() == 2 &&
            updated.preview->requests()[0].payload.at("muted") == true && updated.preview->requests()[1].payload.at("muted") == true,
            "momentary latch repeated absolute ToggleFromOrigin never alternates an earlier preview");
    }
    const auto count = OpJournal(store).count(); latch.session->cancel();
    check(one(store, "SELECT muted FROM tracks WHERE id=1").at("muted") == 0 &&
        one(store, "SELECT muted FROM tracks WHERE id=2").at("muted") == 0 && OpJournal(store).count() == count,
        "momentary latch cancellation restores selected tracks without undo-recommit history");
}

void pendingClaimsRespectReservationOrder() {
    Project project("pending_claim_order"); auto& store = *project.store; Owner owner(store, false);
    g::Spec spec; spec.strategy = g::Strategy::LiveValues; spec.label = "FIFO scalar input";
    const std::vector<g::TargetClaim> claims{{g::ClaimKind::ScalarEndpoint, g::TargetDomain::Track, 1, "mixer.setVolume", {}}};
    auto first = g::detail::reservePending(*owner.coordinator, claims, owner.presentation, spec,
        g::scalarProducer({g::ScalarKind::TrackVolume, 1}), std::make_unique<g::ScalarInput>(-9.0));
    auto second = g::detail::reservePending(*owner.coordinator, claims, owner.presentation, spec,
        g::scalarProducer({g::ScalarKind::TrackVolume, 1}), std::make_unique<g::ScalarInput>(-12.0));
    require(first.status.ok() && first.capture && second.capture, "both same-endpoint inputs are retained before adoption");
    const auto before = sqlState(store);
    auto earlier = g::detail::adoptPending(*owner.coordinator, *first.capture);
    check(earlier.status.ok() && earlier.session,
        "later provisional same-endpoint claim cannot block earlier reserved input adoption");
    if (!earlier.session) return;
    auto blocked = g::detail::adoptPending(*owner.coordinator, *second.capture);
    check(!blocked.status.ok() && !blocked.session && sqlState(store) == before,
        "later same-endpoint input stays retained and blocked while earlier owner is active");
    earlier.session->cancel(); earlier.session.reset(); first.capture.reset();
    auto later = g::detail::adoptPending(*owner.coordinator, *second.capture);
    check(later.status.ok() && later.session,
        "later reserved input becomes adoptable after explicit resolution of earlier owner");
    if (later.session) {
        const auto updated = later.session->update(std::make_unique<g::ScalarInput>(-12.0));
        check(updated.status.ok() && updated.preview && updated.preview->requests().size() == 1 &&
            updated.preview->requests()[0].payload == Payload{{"id", 1}, {"db", -12.0}},
            "later input keeps its own absolute value after FIFO conflict resolution");
        later.session->cancel();
    }
    check(sqlState(store) == before, "provisional conflict resolution never auto-commits either scalar input");
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const std::vector<std::pair<const char*, void(*)()>> suites{
        {"origin, twenty previews and one commit", originTwentyUpdatesAndOneCommit},
        {"foreign base and retained failure", foreignBaseAndFailureRetention},
        {"rollback and final publication admission", previewRollbackAndPublicationGuard},
        {"captured source/head and exact typed row/set guards", capturedAdmissionAndTypedGuards},
        {"presentation image and unrelated same-head material", presentationImageGuardsUnrelatedMaterial},
        {"typed resources and rollback", typedResourcesAndRollback},
        {"sealed captured offer lifecycle", capturedOfferProtocol},
        {"navigation and retained cancellation", navigationAndRetainedCancellation},
        {"no-op lifecycle and durable presentation failure", noOpLifecycleAndDurableFailure},
        {"source destruction and SQL ownership", sourceDestructionAndSqlOwnership},
        {"canonical tempo replacement and legacy packets", tempoCanonicalAndLegacyPackets},
        {"maintenance preserves owned input/resources", maintenanceRetainsOwnedInput},
        {"callback reservations and navigation fence", producerReservationsFenceNavigation},
        {"failed navigation permit ownership", failedNavigationPermitDoesNotReleaseOwner},
        {"capture fault ownership across producers", unrelatedProducerCannotClearCaptureFault},
        {"atomic source finalization and late capture", sourceChangeFinalizationPreservesLateCapture},
        {"state signal prefix ownership", stateWatermarkPrefixOwnership},
        {"concrete scalar claims and momentary latch", scalarClaimsAndWrongThread},
        {"pending claim reservation order", pendingClaimsRespectReservationOrder}};
    for (const auto& [name, run] : suites) {
        std::printf("gesture: %s\n", name);
        try { run(); }
        catch (const std::exception& exception) { check(false, std::string("unexpected fixture exception: ") + name + ": " + exception.what()); }
    }
    std::printf("gesture: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
