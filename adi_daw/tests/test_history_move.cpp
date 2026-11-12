// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/applied_batches.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/check.hpp"
#include "adi/digest.hpp"
#include "adi/engine/param_ops.hpp"
#include "adi/engine/session.hpp"
#include "adi/gesture_navigation_detail.hpp"
#include "adi/history.hpp"
#include "adi/history_roots.hpp"
#include "adi/media/blake3.hpp"
#include "adi/op_transaction_resources_detail.hpp"
#include "adi/row_capture.hpp"
#include "adi/store.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <cstdio>
#include <map>
#include <sqlite3.h>
#include <stdexcept>
#include <string_view>
namespace {
using namespace adi;
int checks = 0, failures = 0;
void check(bool value, const char *message) {
    ++checks;
    if (!value) {
        ++failures;
        std::printf("FAIL %s\n", message);
    }
}
void require(bool value, const std::string &message) {
    if (!value)
        throw std::runtime_error(message);
}
OpRequest nameOp(const std::string &name) {
    OpRequest result;
    result.opType = "project.setName";
    result.payload = {{"name", name}};
    result.label = name;
    return result;
}
struct Project {
    test::TempDirectory folder{"history_move", "project"};
    std::unique_ptr<Store> store;
    Project() {
        StoreError error{};
        store = Store::create(folder.path() / "song.adi", error);
        require(bool(store), toString(error));
    }
    std::int64_t names(std::initializer_list<std::string> names) {
        std::vector<OpRequest> requests;
        for (const auto &name : names)
            requests.push_back(nameOp(name));
        const auto result = OpJournal(*store).commit(requests);
        require(result.ok, result.error);
        require(!result.seqs.empty(), "nonempty fixture step");
        return result.seqs.back();
    }
    std::string name() { return store->db().execAndGet("SELECT name FROM project").getString(); }
};
std::string quote(const std::string &value) {
    std::string out = "\"";
    for (char ch : value) {
        out += ch;
        if (ch == '"')
            out += '"';
    }
    return out + '"';
}
Payload allRows(Store &store) {
    Payload result = Payload::object();
    SQLite::Statement tables(store.db(), "SELECT name FROM sqlite_schema WHERE type='table' AND "
                                         "name NOT LIKE 'sqlite_%' ORDER BY name");
    while (tables.executeStep()) {
        const auto name = tables.getColumn(0).getString();
        SQLite::Statement rows(store.db(), "SELECT * FROM " + quote(name));
        std::vector<Payload> values;
        while (rows.executeStep())
            values.push_back(detail::captureQueryRow(rows));
        std::sort(values.begin(), values.end(),
                  [](const auto &a, const auto &b) { return a.dump() < b.dump(); });
        result[name] = std::move(values);
    }
    return result;
}
struct Observer final : AppliedObserver {
    Store &store;
    std::vector<std::shared_ptr<const AppliedBatch>> batches;
    bool afterCommit = true;
    bool reentryRefused = true;
    bool probeReentry = false;
    explicit Observer(Store &source) : store(source) {}
    void committed(std::shared_ptr<const AppliedBatch> batch) override {
        afterCommit = afterCommit && sqlite3_get_autocommit(store.db().getHandle()) != 0;
        if (probeReentry)
            reentryRefused = reentryRefused && !History(store).moveTo(batch->beforeHead()).ok;
        batches.push_back(std::move(batch));
    }
    void sourceInvalidated(const AppliedSource &, AppliedSourceInvalidation) override {}
};
struct Trace {
    SQLite::Database &db;
    int begins = 0, commits = 0;
    explicit Trace(SQLite::Database &source) : db(source) {
        sqlite3_trace_v2(
            db.getHandle(), SQLITE_TRACE_STMT,
            [](unsigned, void *opaque, void *, void *text) -> int {
                auto &self = *static_cast<Trace *>(opaque);
                if (text) {
                    const std::string_view sql(static_cast<const char *>(text));
                    if (sql.starts_with("BEGIN"))
                        ++self.begins;
                    if (sql.starts_with("COMMIT"))
                        ++self.commits;
                }
                return 0;
            },
            this);
    }
    ~Trace() { sqlite3_trace_v2(db.getHandle(), 0, nullptr, nullptr); }
};
void linear() {
    Project p;
    History h(*p.store);
    const auto origin = digestProject(*p.store).text;
    const auto a = p.names({"A"}), b = p.names({"B1", "B2"}), c = p.names({"C"});
    const auto immutable = allRows(*p.store);
    const auto count = OpJournal(*p.store).count();
    auto observer = std::make_shared<Observer>(*p.store);
    auto subscription = appliedSource(*p.store).observe(observer);
    require(subscription.ok, subscription.error);
    History::Result backward;
    int begins = 0, commits = 0;
    {
        Trace trace(p.store->db());
        backward = h.moveTo(a, 40);
        begins = trace.begins;
        commits = trace.commits;
    }
    check(backward.ok && backward.opsApplied == 3 && h.head() == a && p.name() == "A",
          "one move rewinds a later edit and both ops of a compound transaction");
    check(begins == 1 && commits == 1 && backward.txnId == 0,
          "multi-step navigation uses one SQL transaction without inventing an original txn ID");
    check(observer->batches.size() == 1 && observer->afterCommit,
          "one post-COMMIT applied delivery represents the whole navigation");
    bool ordered = false;
    if (observer->batches.size() == 1) {
        const auto &batch = *observer->batches.front();
        const auto ops = batch.operations();
        ordered = batch.kind() == AppliedBatchKind::HistoryMove && batch.beforeHead() == c &&
                  batch.afterHead() == a && ops.size() == 3;
        if (ops.size() == 3)
            ordered = ordered && ops[0].seq == c && ops[1].seq == b && ops[2].seq == b - 1 &&
                      ops[0].inverse && ops[1].inverse && ops[2].inverse;
    }
    check(ordered, "navigation event owns exact inverse records in newest-first dependency order");
    check(h.moveTo(c, 41).ok && h.head() == c && p.name() == "C",
          "one move replays the original forward chain");
    ordered = false;
    if (observer->batches.size() == 2) {
        const auto ops = observer->batches.back()->operations();
        ordered = ops.size() == 3;
        if (ops.size() == 3)
            ordered = ordered && ops[0].seq == b - 1 && ops[1].seq == b && ops[2].seq == c &&
                      !ops[0].inverse && !ops[1].inverse && !ops[2].inverse;
    }
    check(ordered, "forward event retains exact oldest-first recorded order");
    check(OpJournal(*p.store).count() == count &&
              detail::sameSqlValue(immutable.at("ops"), allRows(*p.store).at("ops")),
          "moving appends no journal ops and never rebuilds recorded inverses");
    const auto before = allRows(*p.store);
    const auto sequence = appliedSource(*p.store).sequence();
    const auto changes = p.store->db().getTotalChanges();
    const auto same = h.moveTo(c, 42);
    check(same.ok && same.opsApplied == 0 && detail::sameSqlValue(before, allRows(*p.store)) &&
              p.store->db().getTotalChanges() == changes &&
              appliedSource(*p.store).sequence() == sequence,
          "unchanged target writes no row and emits no receipt");
    check(h.moveTo(b, 43).ok && h.undo().ok && h.head() == a && p.name() == "A",
          "navigation lands only after whole transactions so ordinary Undo remains whole");
    check(h.redo().ok && h.head() == b && p.name() == "B2",
          "ordinary Redo retains the compound step after a move");
    check(h.moveTo(std::nullopt, 44).ok && !h.canUndo() && digestProject(*p.store).text == origin,
          "NULL moves to original project state without undoing creation");
    check(h.moveTo(c, 45).ok && p.name() == "C",
          "forward from original state restores every retained edit");
}
void branches() {
    Project p;
    History h(*p.store);
    const auto a = p.names({"A"}), b = p.names({"B1", "B2"}), c = p.names({"C"});
    require(h.moveTo(a).ok, "branch fixture rewind");
    const auto d = p.names({"D"});
    const auto before = h.branches().size();
    const auto count = OpJournal(*p.store).count();
    const auto result = h.moveTo(b, 123);
    check(result.ok && result.opsApplied == 3 && p.name() == "B2" && h.head() == b,
          "cross-branch move rewinds to common ancestor and replays a compound step atomically");
    bool held = false;
    for (const auto &branch : h.branches())
        if (!branch.isCurrent && branch.headSeq == d && branch.name == "before history move" &&
            branch.createdUtc == 123)
            held = true;
    check(held && h.branches().size() == before + 1,
          "departing unheld line receives one system branch with caller timestamp");
    check(h.moveTo(d, 124).ok && p.name() == "D" && h.moveTo(c, 125).ok && p.name() == "C",
          "both departed and destination lines stay reachable");
    const auto stable = h.branches().size();
    for (int i = 0; i < 8; ++i)
        require(h.moveTo(i % 2 ? c : d, 126 + i).ok, "repeat branch navigation");
    check(h.branches().size() == stable && OpJournal(*p.store).count() == count,
          "repeated navigation does not duplicate held tips or append synthetic ops");
    check(checkProject(*p.store).clean() && h.readTimeline().error.empty(),
          "all branches and their timeline remain structurally valid");
    // From an undone head, preserve its future tip rather than just this point.
    require(h.moveTo(d).ok && h.undo().ok, "leave an undone fork for preservation");
    require(h.moveTo(c).ok, "restore another fork after undone state");
    check(h.moveTo(d).ok && p.name() == "D",
          "undone future work stays addressable after leaving its line");
}
void undoneFuture() {
    Project p;
    History h(*p.store);
    (void)p.names({"A"});
    const auto d = p.names({"D"});
    require(h.undo().ok, "create older alternate branch");
    (void)p.names({"B"});
    const auto c = p.names({"C"});
    require(h.undo().ok && h.undo().ok, "leave current future undone and unheld");
    const auto count = OpJournal(*p.store).count();
    const auto moved = h.moveTo(d, 133);
    bool held = false;
    for (const auto &branch : h.branches())
        if (!branch.isCurrent && branch.headSeq == c && branch.createdUtc == 133)
            held = true;
    check(moved.ok && p.name() == "D" && held,
          "leaving an undone line preserves its future tip rather than just its current head");
    check(h.moveTo(c).ok && p.name() == "C" && OpJournal(*p.store).count() == count,
          "the previously undone future remains restorable without new journal work");
}
void refusals() {
    Project p;
    History h(*p.store);
    const auto a = p.names({"A"}), b = p.names({"B1", "B2"}), c = p.names({"C"});
    auto observer = std::make_shared<Observer>(*p.store);
    auto subscription = appliedSource(*p.store).observe(observer);
    require(subscription.ok, subscription.error);
    for (int which = 0; which < 4; ++which) {
        Project isolated;
        History navigation(*isolated.store);
        (void)isolated.names({"A"});
        const auto compound = isolated.names({"B1", "B2"}), tail = isolated.names({"C"});
        const auto invalid = which == 0 ? -1 : which == 1 ? 0 : which == 2 ? 999999 : compound - 1;
        const auto before = allRows(*isolated.store);
        const auto sequence = appliedSource(*isolated.store).sequence();
        const auto result = navigation.moveTo(invalid);
        check(!result.ok && !result.error.empty() && result.opsApplied == 0 &&
                  navigation.head() == tail &&
                  detail::sameSqlValue(before, allRows(*isolated.store)) &&
                  appliedSource(*isolated.store).sequence() == sequence,
              "missing nonpositive or middle-of-transaction target refuses without mutation");
    }
    p.store->db().exec("CREATE TEMP TRIGGER fail_history_move BEFORE UPDATE ON project WHEN "
                       "NEW.name='A' BEGIN SELECT RAISE(ABORT,'late navigation rollback'); END");
    const auto before = allRows(*p.store);
    const auto failed = h.moveTo(a);
    check(!failed.ok && failed.opsApplied == 0 && h.head() == c &&
              detail::sameSqlValue(before, allRows(*p.store)) && observer->batches.empty(),
          "late real SQL refusal rolls earlier inverses and head back with no partial event");
    p.store->db().exec("DROP TRIGGER fail_history_move");
    check(h.moveTo(a).ok && p.name() == "A",
          "authentic intent can retry after the SQL cause is removed");
    require(h.moveTo(c).ok, "restore for inverse corruption");
    const auto oldRow = detail::captureRow(p.store->db(), "ops", "seq", b - 1);
    require(oldRow.has_value(), "captured inverse fixture row");
    const auto old = *oldRow;
    p.store->db().exec("UPDATE ops SET inverse=NULL WHERE seq=" + std::to_string(b - 1));
    const auto bad = allRows(*p.store);
    const auto seq = appliedSource(*p.store).sequence();
    const auto missing = h.moveTo(a);
    check(!missing.ok && missing.opsApplied == 0 && detail::sameSqlValue(bad, allRows(*p.store)) &&
              appliedSource(*p.store).sequence() == seq,
          "missing inverse after earlier rewind operations causes exact complete rollback");
    detail::restoreRow(p.store->db(), "ops", old, "seq");
    observer->probeReentry = true;
    check(h.moveTo(a).ok && observer->reentryRefused,
          "history navigation during its own applied dispatch is refused");
    const auto path = p.store->path();
    subscription.subscription.reset();
    p.store.reset();
    StoreError error{};
    p.store = Store::open(path, error, true);
    require(bool(p.store), toString(error));
    const auto readonly = allRows(*p.store);
    const auto result = History(*p.store).moveTo(c);
    check(!result.ok && detail::sameSqlValue(readonly, allRows(*p.store)),
          "readonly navigation changes neither document nor history metadata");
}
struct StateProbe {
    std::vector<std::vector<std::uint8_t>> loads;
    unsigned factories = 0;
};
struct StateDevice final : device::DeviceInstance {
    device::DeviceIdentity id;
    std::vector<std::uint8_t> bytes;
    std::shared_ptr<StateProbe> probe;
    explicit StateDevice(std::shared_ptr<StateProbe> value) : probe(std::move(value)) {
        id.format = "vst3";
        id.uid = "history.move";
        ++probe->factories;
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
        probe->loads.push_back(value);
        return true;
    }
};
void liveDelivery() {
    Project p;
    p.store->db().exec(
        "INSERT INTO tracks(id,kind,name) VALUES(9,'master','Main'),(10,'audio','Device');"
        "INSERT INTO plugin_refs(id,format,uid,name,subtype) "
        "VALUES(1,'vst3','history.move','State','effect');"
        "INSERT INTO device_chains(id,track_id) VALUES(10,10);"
        "INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name) VALUES(1,10,0,1,'State')");
    const auto state = [&](const std::vector<std::uint8_t> &bytes) {
        const auto hash = media::blake3Bytes(std::as_bytes(std::span(bytes)));
        require(bool(hash), "hash runtime state");
        SQLite::Statement insert(
            p.store->db(), "INSERT INTO state_blobs(hash_blake3,data,size_bytes) VALUES(?,?,?)");
        insert.bind(1, hash.hex);
        insert.bind(2, bytes.data(), static_cast<int>(bytes.size()));
        insert.bind(3, static_cast<std::int64_t>(bytes.size()));
        insert.exec();
        OpRequest request;
        request.opType = "device.loadState";
        request.payload = {
            {"dev", 1}, {"role", "chunk"}, {"hash", hash.hex}, {"hint", "move state"}};
        const auto result = OpJournal(*p.store).commit(request);
        require(result.ok, result.error);
        require(!result.seqs.empty(), "runtime state head");
        return result.seqs.back();
    };
    const std::vector<std::uint8_t> early{0, 17, 255}, middle{4, 0, 128}, late{5, 9, 0, 255};
    const auto first = state(early);
    (void)state(middle);
    const auto last = state(late);
    auto probe = std::make_shared<StateProbe>();
    engine::DeviceLoader loader = [probe](const engine::DeviceRequest &, std::string &)
        -> std::unique_ptr<device::DeviceInstance> { return std::make_unique<StateDevice>(probe); };
    engine::Session session;
    require(session.load(*p.store, loader, {}), session.error());
    engine::ParamOps params;
    require(params.attachSession(session) == 1, "attach actual history runtime receiver");
    probe->loads.clear();
    const auto before = appliedSource(*p.store).sequence();
    const auto backward = History(*p.store).moveTo(first);
    check(backward.ok && backward.opsApplied == 2 &&
              appliedSource(*p.store).sequence() == before + 1,
          "actual attached runtime receives one multi-operation history receipt");
    std::vector<OpRequest> echoes;
    (void)params.drain(100, echoes);
    check(echoes.empty() &&
              params.appliedDeliveryStatus().state == engine::AppliedDeliveryState::Idle,
          "ordinary owner drain acknowledges the history receipt without a new edit");
    check(probe->loads == std::vector<std::vector<std::uint8_t>>{middle, early},
          "same-baseline runtime receives every inverse state exactly once and in order");
    const auto forward = History(*p.store).moveTo(last);
    (void)params.drain(200, echoes);
    check(echoes.empty() &&
              params.appliedDeliveryStatus().state == engine::AppliedDeliveryState::Idle,
          "forward owner drain acknowledges its receipt without journaling an echo");
    check(forward.ok &&
              probe->loads == std::vector<std::vector<std::uint8_t>>{middle, early, middle, late} &&
              probe->factories == 1,
          "forward replay restores original state order on the same live instance");
    const auto *entry = session.entryFor(1);
    check(entry && session.devices().deviceAt(entry->hostIndex).saveState("chunk") == late &&
              params.appliedDeliveryStatus().state != engine::AppliedDeliveryState::Invalidated,
          "final actual device bytes match SQL without a whole-session reset");
}
void forwardRollback() {
    Project p;
    History h(*p.store);
    (void)p.names({"A"});
    const auto b = p.names({"B1", "B2"});
    (void)p.names({"C"});
    require(h.undo().ok && h.undo().ok, "prepare alternate branch by ordinary Undo");
    const auto d = p.names({"D"});
    auto observer = std::make_shared<Observer>(*p.store);
    auto subscription = appliedSource(*p.store).observe(observer);
    require(subscription.ok, subscription.error);
    p.store->db().exec("CREATE TEMP TRIGGER fail_history_forward BEFORE UPDATE ON project WHEN "
                       "NEW.name='B2' BEGIN SELECT RAISE(ABORT,'late forward rollback'); END");
    const auto before = allRows(*p.store);
    const auto sequence = appliedSource(*p.store).sequence();
    const auto failed = h.moveTo(b, 711);
    check(!failed.ok && failed.opsApplied == 0 && h.head() == d && p.name() == "D" &&
              detail::sameSqlValue(before, allRows(*p.store)) &&
              appliedSource(*p.store).sequence() == sequence && observer->batches.empty(),
          "late forward failure rolls rewind replay preserved-branch rows and head back together");
    p.store->db().exec("DROP TRIGGER fail_history_forward");
    check(h.moveTo(b, 712).ok && h.head() == b && p.name() == "B2" && observer->batches.size() == 1,
          "cross-branch intent retries after the real forward failure is removed");
}
void captureAdmission() {
    Project p;
    History h(*p.store);
    const auto a = p.names({"A"});
    (void)p.names({"B"});
    auto producer = gesture::detail::registerProducer(*p.store, 4);
    require(producer.status.ok() && producer.fence, "register native capture fence");
    const auto reservation = producer.fence->reserve();
    check(reservation == gesture::detail::ProducerFence::Reservation::BeforeNavigation,
          "callback producer owns a real reservation before navigation");
    const auto before = allRows(*p.store);
    const auto sequence = appliedSource(*p.store).sequence();
    const auto refused = h.moveTo(a);
    check(!refused.ok && detail::sameSqlValue(before, allRows(*p.store)) &&
              appliedSource(*p.store).sequence() == sequence,
          "navigation cannot bypass unprocessed producer input or consume its reservation");
    const auto released = producer.fence->discardUnchanged(1);
    check(released.ok() && h.moveTo(a).ok && p.name() == "A",
          "explicit unchanged-input acknowledgement permits the same navigation intent");
    const auto ownerState = allRows(*p.store);
    const bool prior = audio::onAudioThread;
    audio::onAudioThread = true;
    const auto callback = h.moveTo(std::nullopt);
    audio::onAudioThread = prior;
    check(!callback.ok && detail::sameSqlValue(ownerState, allRows(*p.store)),
          "callback thread refuses history movement before SQL changes");
}
void invalidBoundaries() {
    Project p;
    History h(*p.store);
    (void)p.names({"A"});
    const auto b = p.names({"B1", "B2"});
    (void)p.names({"C"});
    OpRequest performance;
    performance.opType = "transport.play";
    performance.payload = Payload::object();
    const auto ephemeral = OpJournal(*p.store).commit(performance);
    require(ephemeral.ok && !ephemeral.seqs.empty(),
            "actual ephemeral transport fixture: " + ephemeral.error);
    const auto before = allRows(*p.store);
    const auto event = appliedSource(*p.store).sequence();
    const auto notEdit = h.moveTo(ephemeral.seqs.back());
    check(!notEdit.ok && detail::sameSqlValue(before, allRows(*p.store)) &&
              appliedSource(*p.store).sequence() == event,
          "a real ephemeral performance sequence cannot become an undo head");
    p.store->db().exec("UPDATE op_branches SET head_seq=" + std::to_string(b - 1) +
                       " WHERE is_current=1");
    const auto corrupt = allRows(*p.store);
    const auto invalid = h.moveTo(std::nullopt);
    check(!invalid.ok && invalid.opsApplied == 0 &&
              detail::sameSqlValue(corrupt, allRows(*p.store)),
          "an already corrupted interior head refuses rather than rewinding half a transaction");
}
void stored() {
    Project p;
    History h(*p.store);
    const auto original = digestProject(*p.store).text;
    const auto origin = h.takeSnapshot("Origin", 0, 0);
    require(origin.ok, origin.error);
    std::int64_t saved = 0, last = 0;
    std::string savedDigest, lastDigest;
    for (int i = 1; i <= 9; ++i) {
        last = p.names({std::to_string(i)});
        if (i == 3) {
            saved = last;
            savedDigest = digestProject(*p.store).text;
            require(h.takeSnapshot("Saved", i, 0).ok, "saved snapshot");
        }
    }
    lastDigest = digestProject(*p.store).text;
    UndoRetentionPolicy policy;
    policy.maxTransactions = 3;
    const auto cap = h.setUndoRetentionPolicy(policy);
    require(cap.ok, cap.error);
    auto observer = std::make_shared<Observer>(*p.store);
    auto subscription = appliedSource(*p.store).observe(observer);
    require(subscription.ok, subscription.error);
    const auto before = allRows(*p.store);
    const auto refused = h.moveTo(saved);
    check(!refused.ok && refused.sessionReloadRequired &&
              detail::sameSqlValue(before, allRows(*p.store)) && observer->batches.empty(),
          "cross-baseline navigation requires prepared runtime reload before any SQL change");
    // No audio callback runs here; owner has fenced its runtime before opting in.
    constexpr auto prepared = History::StoredRestore::SessionReloadPrepared;
    const auto restored = h.moveTo(saved, 100, prepared);
    check(restored.ok && restored.sessionReloadRequired &&
              digestProject(*p.store).text == savedDigest,
          "explicitly prepared navigation restores the actual archived target state");
    check(observer->batches.size() == 1 &&
              observer->batches.front()->kind() == AppliedBatchKind::ReplayBaseReset &&
              observer->batches.front()->operations().empty(),
          "stored transition emits one empty reset without inventing incremental operations");
    check(h.moveTo(last, 101, prepared).ok && digestProject(*p.store).text == lastDigest,
          "cross-root forward target replays only its own retained tail");
    check(h.moveTo(std::nullopt, 102, prepared).ok && digestProject(*p.store).text == original &&
              !h.canUndo(),
          "NULL canonicalizes to the retained stored origin rather than an invalid pre-checkpoint "
          "head");
    check(h.moveTo(last, 103, prepared).ok && checkProject(*p.store).clean() &&
              h.readTimeline().error.empty(),
          "navigation across all stored baselines retains a valid forest and timeline");
}
} // namespace
int main() {
    try {
        linear();
        branches();
        undoneFuture();
        refusals();
        stored();
        liveDelivery();
        forwardRollback();
        captureAdmission();
        invalidBoundaries();
    } catch (const std::exception &error) {
        ++failures;
        std::printf("EXCEPTION %s\n", error.what());
    }
    std::printf("history_move: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
