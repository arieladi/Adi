// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/blob.hpp"
#include "adi/controller_lanes.hpp"
#include "adi/engine/snapshot.hpp"
#include "adi/history.hpp"
#include "adi/store.hpp"
#include "adi/store_rows.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <cstdio>
#include <cstring>
#include <sqlite3.h>
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
constexpr std::uint64_t wideId = (std::uint64_t(1) << 63) + 27;
struct Project {
    test::TempDirectory folder{"controllers", "project"};
    std::unique_ptr<Store> store;
    Project() {
        StoreError error{};
        store = Store::create(folder.path() / "song.adi", error);
        if (!store)
            throw std::runtime_error(toString(error));
        store->db().exec(
            "INSERT INTO tracks(id,kind) VALUES(1,'midi'),(2,'midi'); INSERT INTO "
            "clips(id,track_id,kind,pos_ticks,length_ticks) VALUES(1,1,'midi',0,1000),(2,2,'midi',0,1000)");
    }
    controllers::ReadResult read() {
        return controllers::readLanes(*store, 1);
    }
    controllers::Lane lane() {
        auto value = read();
        if (!value.ok || value.lanes.size() != 1)
            throw std::runtime_error("missing controller lane: " + value.error);
        return value.lanes[0];
    }
    CommitResult write(controllers::Address address, std::vector<controllers::Point> points) {
        return OpJournal(*store).commit(controllers::writeRequest(address, points));
    }
};
controllers::Point point(std::uint64_t id, std::int64_t time, double value) {
    return {id, time, value, 0.25f, static_cast<std::uint8_t>(Curve::Bezier), 7};
}
void basic() {
    Project p;
    History h(*p.store);
    const controllers::Address location{100, 1, 3, 74};
    check(p.read().ok && p.read().lanes.empty(), "missing controller lanes read as empty without writes");
    check(p.write(location, {point(wideId, 200, 0.123456789012345), point(1, 0, 0.75)}).ok,
          "native controller write creates an addressed stream with explicit IDs");
    const auto original = p.lane();
    check(original.id == 100 && original.clipId == 1 && original.channel == 3 && original.controller == 74 &&
              original.points.size() == 2 && original.points[0].id == 1 && original.points[1].id == wideId,
          "controller address, full-width IDs and time order are exposed");
    check(original.points[1].value == 0.123456789012345 && original.points[1].tension == 0.25f &&
              original.points[1].curve == static_cast<std::uint8_t>(Curve::Bezier) &&
              original.points[1].flags == 7,
          "controller values, curve, tension and flags retain their typed precision");
    check(h.undo().ok && p.read().lanes.empty(),
          "undo of first controller write restores absence rather than creating an empty lane");
    check(h.redo().ok && p.lane() == original, "redo restores the same explicit row and point identities");
    check(p.write(location, {point(wideId, 100, 0.5)}).ok && p.lane().points[1].timeTicks == 100 &&
              p.lane().points[0].id == 1,
          "point write replaces only its explicit ID and retains unrelated points");
    check(h.undo().ok && p.lane() == original, "point-write inverse restores exact previous bytes");
    const std::vector<std::uint64_t> ids{wideId};
    check(OpJournal(*p.store).commit(controllers::deleteRequest(location, ids)).ok &&
              p.lane().points.size() == 1 && p.lane().points[0].id == 1,
          "point deletion removes only selected controller points");
    check(h.undo().ok && p.lane() == original, "point deletion undo restores exact bytes");
    const std::vector<std::uint64_t> allIds{1, wideId};
    check(OpJournal(*p.store).commit(controllers::deleteRequest(location, allIds)).ok &&
              p.lane().points.empty(),
          "deleting the last points retains an empty addressed lane");
    check(h.undo().ok && p.lane() == original, "undo restores a cleared lane exactly");
    const auto rows = rows::readModel(*p.store);
    check(rows.controllers.size() == 1 && rows.controllers[0] == original,
          "row model carries the same addressed controller data");
}
void slotsAndFailures() {
    Project p;
    const controllers::Address first{100, 1, 0, 1}, second{101, 1, 1, 1};
    check(p.write(first, {point(1, 0, 0.2)}).ok && p.write(second, {point(1, 0, 0.8)}).ok &&
              p.read().lanes.size() == 2,
          "same CC on different channels remains independently addressed");
    const auto before = controllers::readLanes(*p.store);
    check(!p.write({100, 1, 1, 1}, {point(2, 20, 0.5)}).ok &&
              !p.write({102, 1, 0, 1}, {point(2, 20, 0.5)}).ok &&
              !p.write({100, 2, 0, 1}, {point(2, 20, 0.5)}).ok &&
              controllers::readLanes(*p.store).lanes == before.lanes,
          "row-ID and slot collisions never replace another controller stream");
    check(!p.write({200, 1, 16, 1}, {point(2, 0, 0.5)}).ok &&
              !p.write({200, 1, 0, 128}, {point(2, 0, 0.5)}).ok && !p.write(first, {}).ok,
          "invalid channels, controller numbers and empty writes refuse");
    check(!p.write(first, {point(2, 0, 1.1)}).ok && !p.write(first, {point(2, 0, 0.5), point(2, 1, 0.4)}).ok,
          "invalid values and duplicate written IDs refuse atomically");
    const std::vector<std::uint64_t> missing{1, 999};
    check(!OpJournal(*p.store).commit(controllers::deleteRequest(first, missing)).ok &&
              controllers::readLanes(*p.store).lanes == before.lanes,
          "delete with one missing ID leaves all points and slots unchanged");
    const auto count = OpJournal(*p.store).count();
    p.store->db().exec("CREATE TRIGGER refuse_cc BEFORE UPDATE ON event_streams BEGIN SELECT "
                       "RAISE(ABORT,'planted CC write failure'); END");
    check(!p.write(first, {point(1, 10, 0.9)}).ok && OpJournal(*p.store).count() == count &&
              controllers::readLanes(*p.store).lanes == before.lanes,
          "late SQL write failure rolls back the stream edit and journal");
    p.store->db().exec("DROP TRIGGER refuse_cc");
    const auto path = p.store->path();
    p.store.reset();
    StoreError error{};
    p.store = Store::open(path, error, true);
    check(p.store && controllers::readLanes(*p.store).ok && !p.write(first, {point(1, 10, 0.9)}).ok,
          "readonly files expose controller lanes while refusing edits");
}
void futureAndInvalid() {
    Project p;
    std::vector<AutomationPoint> points(2);
    points[0].point_id = 1;
    points[0].time = 0;
    points[0].value = 0.25;
    points[1].point_id = wideId;
    points[1].time = 10;
    points[1].value = 0.75;
    StreamHeader header{static_cast<std::uint32_t>(FourCC::Controller), 7, 36, 2, StreamFlags::SortedByTime};
    std::vector<std::byte> bytes(sizeof header + 72 + 3, std::byte{0x6b});
    std::memcpy(bytes.data(), &header, sizeof header);
    for (std::size_t i = 0; i < points.size(); ++i) {
        std::memcpy(bytes.data() + sizeof header + i * 36, &points[i], sizeof(AutomationPoint));
        bytes[sizeof header + i * 36 + 32] = static_cast<std::byte>(i + 1);
    }
    SQLite::Statement insert(p.store->db(),
                             "INSERT INTO event_streams(id,clip_id,stream_kind,channel,controller,data) "
                             "VALUES(100,1,'cc',2,11,?)");
    insert.bind(1, bytes.data(), static_cast<int>(bytes.size()));
    insert.exec();
    check(p.lane().error.empty(), "future controller stride/version is readable through the known prefix");
    const controllers::Address location{100, 1, 2, 11};
    check(p.write(location, {point(wideId, -10, 0.8)}).ok &&
              p.lane().data[sizeof header + 32] == std::byte{2} && p.lane().data.back() == std::byte{0x6b},
          "known-field rewrite moves future tails with their points and retains the trailer");
    check(History(*p.store).undo().ok && p.lane().data == bytes,
          "future controller update undo is byte exact");
    check(!p.write(location, {point(3, 20, 0.5)}).ok && p.lane().data == bytes,
          "new points refuse a future layout whose additional fields are unknown");
    const std::vector<std::uint64_t> remove{wideId};
    check(OpJournal(*p.store).commit(controllers::deleteRequest(location, remove)).ok &&
              p.lane().points.size() == 1 && p.lane().data.size() == sizeof header + 36 + 3,
          "future point deletion retains the surviving record tail and trailer");
    check(History(*p.store).undo().ok && p.lane().data == bytes,
          "future point deletion undo restores the original layout");
    p.store->db().exec("UPDATE event_streams SET data=X'00'");
    const auto invalid = p.lane();
    check(!invalid.error.empty() && invalid.data == std::vector<std::byte>{std::byte{0}} &&
              !p.write(location, {point(1, 0, 0.5)}).ok,
          "malformed controller stream is visible as an error and refuses destructive rewriting");
    p.store->db().exec("UPDATE event_streams SET channel=NULL");
    check(!p.lane().channel && !p.lane().error.empty(),
          "legacy unbound address remains distinct from MIDI channel zero");
}
const engine::ClipNode &clip(const engine::Snapshot &snapshot, int track) {
    const auto *owner = snapshot.findTrack(track);
    if (!owner || owner->clips.empty())
        throw std::runtime_error("snapshot clip");
    return *owner->clips[0];
}
void snapshots() {
    Project p;
    const controllers::Address location{100, 1, 0, 1};
    if (!p.write(location, {point(1, 0, 0.2)}).ok)
        throw std::runtime_error("snapshot fixture");
    auto previous = engine::SnapshotBuilder::fromStore(*p.store);
    auto unchanged = engine::SnapshotBuilder::fromStore(*p.store, *previous);
    check(&clip(*previous, 1) == &clip(*unchanged, 1) && clip(*previous, 1).controllers.size() == 1,
          "unchanged addressed controller data reuses the immutable clip node");
    for (int field = 0; field < 5; ++field) {
        auto nextPoint = p.lane().points.front();
        if (field == 0)
            nextPoint.timeTicks += 1;
        if (field == 1)
            nextPoint.value = 0.6;
        if (field == 2)
            nextPoint.tension = -0.5f;
        if (field == 3)
            nextPoint.curve = 1;
        if (field == 4)
            nextPoint.flags = 8;
        const auto before = clip(*previous, 1).controllers;
        if (!p.write(location, {nextPoint}).ok)
            throw std::runtime_error("snapshot point write");
        auto next = engine::SnapshotBuilder::fromStore(*p.store, *previous);
        check(
            &clip(*previous, 1) != &clip(*next, 1) && &clip(*previous, 2) == &clip(*next, 2) &&
                clip(*previous, 1).controllers == before,
            "isolated controller point field copies its path while old and unrelated nodes remain unchanged");
        previous = std::move(next);
    }
    for (int field = 0; field < 2; ++field) {
        auto bytes = p.lane().data;
        if (field == 0)
            std::memcpy(bytes.data() + sizeof(StreamHeader) + offsetof(AutomationPoint, point_id), &wideId,
                        sizeof wideId);
        else {
            StreamHeader header{};
            std::memcpy(&header, bytes.data(), sizeof header);
            header.flags |= 0x10000;
            std::memcpy(bytes.data(), &header, sizeof header);
        }
        SQLite::Statement update(p.store->db(), "UPDATE event_streams SET data=? WHERE id=100");
        update.bind(1, bytes.data(), static_cast<int>(bytes.size()));
        update.exec();
        auto next = engine::SnapshotBuilder::fromStore(*p.store, *previous);
        check(&clip(*previous, 1) != &clip(*next, 1) && clip(*next, 1).controllers.size() == 1 &&
                  clip(*next, 1).controllers[0].error.empty(),
              "isolated point identity and unknown header flag participate in snapshot sharing");
        previous = std::move(next);
    }
    for (const char *update : {"UPDATE event_streams SET channel=1", "UPDATE event_streams SET controller=2",
                               "UPDATE event_streams SET id=101", "UPDATE event_streams SET data=X'00'"}) {
        p.store->db().exec(update);
        auto next = engine::SnapshotBuilder::fromStore(*p.store, *previous);
        check(&clip(*previous, 1) != &clip(*next, 1),
              "address/identity/raw-error change participates in snapshot sharing");
        previous = std::move(next);
    }
    p.store->db().exec("UPDATE event_streams SET clip_id=2");
    auto moved = engine::SnapshotBuilder::fromStore(*p.store, *previous);
    check(clip(*moved, 1).controllers.empty() && clip(*moved, 2).controllers.size() == 1 &&
              &clip(*moved, 1) != &clip(*previous, 1) && &clip(*moved, 2) != &clip(*previous, 2),
          "controller clip-owner change updates both immutable clip paths");
}
void consistentRead() {
    Project p;
    const controllers::Address location{100, 1, 0, 7};
    if (!p.write(location, {point(1, 0, 0.2)}).ok)
        throw std::runtime_error("consistent snapshot fixture");
    auto changed = p.lane().data;
    double nextValue = 0.8;
    std::memcpy(changed.data() + sizeof(StreamHeader) + offsetof(AutomationPoint, value), &nextValue,
                sizeof nextValue);
    SQLite::Database external(p.store->path().string(), SQLite::OPEN_READWRITE);
    struct Injection {
        SQLite::Database *db;
        const std::vector<std::byte> *bytes;
        bool fired = false;
        std::string error;
    };
    Injection injection{&external, &changed, false, {}};
    const auto callback = [](unsigned type, void *context, void *statement, void *) -> int {
        auto &state = *static_cast<Injection *>(context);
        if (type != SQLITE_TRACE_PROFILE || state.fired)
            return 0;
        const auto *sql = sqlite3_sql(static_cast<sqlite3_stmt *>(statement));
        if (!sql ||
            std::string_view(sql).find("SELECT id,clip_id,channel,controller,data FROM event_streams") != 0)
            return 0;
        state.fired = true;
        try {
            SQLite::Transaction transaction(*state.db);
            SQLite::Statement update(*state.db, "UPDATE event_streams SET data=? WHERE id=100");
            update.bind(1, state.bytes->data(), static_cast<int>(state.bytes->size()));
            update.exec();
            state.db->exec("UPDATE clips SET name='New revision' WHERE id=1");
            transaction.commit();
        } catch (const std::exception &error) {
            state.error = error.what();
        }
        return 0;
    };
    struct TraceGuard {
        sqlite3 *database;
        ~TraceGuard() {
            sqlite3_trace_v2(database, 0, nullptr, nullptr);
        }
    };
    TraceGuard trace{p.store->db().getHandle()};
    sqlite3_trace_v2(p.store->db().getHandle(), SQLITE_TRACE_PROFILE, callback, &injection);
    auto before = engine::SnapshotBuilder::fromStore(*p.store);
    sqlite3_trace_v2(p.store->db().getHandle(), 0, nullptr, nullptr);
    check(injection.fired && injection.error.empty() && clip(*before, 1).name.empty() &&
              clip(*before, 1).controllers.size() == 1 && !clip(*before, 1).controllers[0].points.empty() &&
              clip(*before, 1).controllers[0].points[0].value == 0.2,
          "controller data and clip metadata stay in one read snapshot across a concurrent committed edit");
    auto after = engine::SnapshotBuilder::fromStore(*p.store, *before);
    check(clip(*after, 1).name == "New revision" && clip(*after, 1).controllers[0].points[0].value == 0.8,
          "next snapshot sees both parts of the new committed revision");
}
void preparation() {
    Project p;
    const auto notes = writeStream<NoteRecord>(FourCC::Notes, {});
    if (!p.store->putEventStream(1, "notes", notes))
        throw std::runtime_error("preparation fixture");
    const std::vector<controllers::Point> input{point(0, 0, 0.3), point(0, 100, 0.8)};
    const auto changes = sqlite3_total_changes(p.store->db().getHandle());
    const auto prepared = controllers::prepareWrite(*p.store, 1, 4, 7, input);
    check(prepared.ok && prepared.address.id > 1 && prepared.points.size() == 2 &&
              prepared.points[0].id != 0 && prepared.points[1].id != prepared.points[0].id,
          "owner producer allocates explicit stream/point IDs without colliding with other stream kinds");
    check(sqlite3_total_changes(p.store->db().getHandle()) == changes && OpJournal(*p.store).count() == 0 &&
              p.read().lanes.empty(),
          "controller preparation reserves or writes no project rows");
    check(controllers::commitPrepared(*p.store, prepared).ok && p.lane().points.size() == 2 &&
              p.store->getEventStream(1, "notes") == std::optional<std::vector<std::byte>>(notes),
          "prepared controller write commits once and preserves the notes stream");
    const auto repeated = controllers::commitPrepared(*p.store, prepared);
    check(!repeated.ok && repeated.stale && OpJournal(*p.store).count() == 1,
          "a prepared root-head edit checks NULL explicitly and cannot commit twice");
    auto edit = p.lane().points;
    edit[0].value = 0.6;
    edit.push_back(point(0, 200, 0.9));
    const auto existing = controllers::prepareWrite(*p.store, 1, 4, 7, edit);
    check(existing.ok && existing.address.id == prepared.address.id &&
              existing.points.back().id > prepared.points[1].id,
          "existing slot reuses its row and allocates only requested new point IDs");
    if (!p.write(prepared.address, {point(wideId, 300, 0.2)}).ok)
        throw std::runtime_error("stale preparation fixture");
    const auto before = p.lane();
    const auto stale = controllers::commitPrepared(*p.store, existing);
    check(!stale.ok && stale.stale && p.lane() == before,
          "prepared edit refuses an intervening journal change without overwriting data");
    Project other;
    check(!controllers::commitPrepared(*other.store, prepared).ok && OpJournal(*other.store).count() == 0,
          "prepared controller edit refuses a different document");
    check(!controllers::prepareWrite(*p.store, 1, 16, 7, input).ok &&
              !controllers::prepareWrite(*p.store, 999, 4, 7, input).ok,
          "producer refuses invalid addresses and missing clips before committing");
    {
        SQLite::Transaction ownerRead(p.store->db(), SQLite::TransactionBehavior::DEFERRED);
        const auto nested = controllers::prepareWrite(*p.store, 1, 4, 7, input);
        check(nested.ok && sqlite3_get_autocommit(p.store->db().getHandle()) == 0,
              "preparation honors a caller-owned read transaction without committing it");
        ownerRead.commit();
    }
    Project reopened;
    const auto oldPrepared = controllers::prepareWrite(*reopened.store, 1, 0, 7, input);
    const auto samePath = reopened.store->path();
    reopened.store.reset();
    StoreError reopenError{};
    reopened.store = Store::open(samePath, reopenError);
    check(reopened.store && !controllers::commitPrepared(*reopened.store, oldPrepared).ok &&
              OpJournal(*reopened.store).count() == 0,
          "closing/reopening the same file invalidates its old prepared edit even at the same head");
    const auto path = p.store->path();
    p.store.reset();
    StoreError error{};
    p.store = Store::open(path, error, true);
    check(p.store && !controllers::prepareWrite(*p.store, 1, 4, 7, input).ok,
          "readonly project refuses the controller preparation command");
}
} // namespace
int main() {
    try {
        basic();
        slotsAndFailures();
        futureAndInvalid();
        snapshots();
        consistentRead();
        preparation();
    } catch (const std::exception &error) {
        check(false, error.what());
    }
    std::printf("controller lanes: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
