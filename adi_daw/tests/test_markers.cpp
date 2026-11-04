// SPDX-License-Identifier: GPL-3.0-or-later
// N25: locator/cycle identity, literal text, undo and old-file compatibility.
#include "temp_directory.hpp"
#include "adi/digest.hpp"
#include "adi/engine/snapshot.hpp"
#include "adi/history.hpp"
#include "adi/ops.hpp"
#include "adi/store.hpp"
#include "adi/textproj_store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <array>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace {
using namespace adi;
using adi::engine::SnapshotBuilder;
int checks = 0, failures = 0;
void check(bool ok, const std::string& name) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL %s\n", name.c_str()); }
}
std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot read fixture");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
struct Fixture {
    test::TempDirectory temp;
    std::unique_ptr<Store> store;
    explicit Fixture(const char* token) : temp("markers", token) {
        StoreError error = StoreError::Ok;
        store = Store::create(temp.path() / "song.adi", error);
        if (!store) throw std::runtime_error(toString(error));
        store->db().exec("INSERT INTO tracks(id,kind,name) VALUES(1,'audio','Audio')");
    }
    SQLite::Database& db() { return store->db(); }
};
OpRequest request(const char* type, Payload payload) {
    OpRequest op;
    op.opType = type; op.payload = std::move(payload); op.label = type;
    return op;
}
bool edit(Store& store, const char* type, Payload payload) {
    const auto result = OpJournal(store).commit(request(type, std::move(payload)));
    if (!result.ok) std::printf("  %s: %s\n", type, result.error.c_str());
    return result.ok;
}
std::string state(const Store& store) { return digestProject(store).text; }
void roundTrip(Store& store, const char* type, Payload payload) {
    const auto before = state(store);
    check(edit(store, type, std::move(payload)), std::string(type) + " commits");
    const auto after = state(store);
    History history(store);
    check(history.undo().ok && state(store) == before, std::string(type) + " exact undo");
    check(history.redo().ok && state(store) == after, std::string(type) + " exact redo");
}

void catalogAndValues() {
    Fixture f("values");
    for (const char* name : {"marker.create", "marker.delete", "marker.move", "marker.rename",
                             "marker.setColor", "marker.resize", "marker.setNote", "marker.setPrompt"}) {
        const auto* op = OpRegistry::instance().find(name);
        check(op && op->scope == Scope::Edit && !op->ephemeral && op->buildInverse &&
                  op->engineImpact == EngineImpact::Snapshot, std::string(name) + " contract");
    }
    roundTrip(*f.store, "marker.create", {{"id", 30}, {"pos", 23063040}, {"name", "Verse"},
              {"color", 0}, {"note", "A note\nwith a second line"}, {"prompt", "echo $(literal) `text`"}});
    roundTrip(*f.store, "marker.create", {{"id", 10}, {"pos", 2000000001LL}, {"time_base", 1},
              {"length", 750000000}, {"name", "Linear loop"}});
    auto snapshot = SnapshotBuilder::fromStore(*f.store);
    check(snapshot->markers.size() == 2 && snapshot->markers[0]->id == 10,
          "id order does not pretend ticks and ns are comparable display positions");
    const auto* musical = snapshot->findMarker(30);
    const auto* linear = snapshot->findMarker(10);
    check(musical && musical->timeBase == 0 && musical->posTicks == 23063040 && !musical->posNs &&
              !musical->lengthTicks && !musical->lengthNs, "position marker units and nulls");
    check(musical && musical->name == "Verse" && musical->kind == "position" && musical->color == 0 &&
              musical->note == "A note\nwith a second line" && musical->prompt == "echo $(literal) `text`",
          "marker name, kind, palette zero and literal text are readable");
    check(linear && linear->timeBase == 1 && linear->posNs == 2000000001LL && !linear->posTicks &&
              linear->lengthNs == 750000000 && !linear->lengthTicks && !linear->color &&
              linear->kind == "cycle" && linear->note.empty() && linear->prompt.empty(), "linear cycle values");
    check(!snapshot->findMarker(999), "missing marker lookup");
    roundTrip(*f.store, "marker.move", {{"id", 30}, {"pos", -13}});
    roundTrip(*f.store, "marker.move", {{"id", 10}, {"pos", 1234567890123LL}});
    roundTrip(*f.store, "marker.rename", {{"id", 30}, {"name", "\xe6\xa9\x8b"}});
    roundTrip(*f.store, "marker.setColor", {{"id", 30}});
    check(f.db().execAndGet("SELECT color IS NULL FROM markers WHERE id=30").getInt() == 1,
          "omitted colour clears to NULL");
    roundTrip(*f.store, "marker.setColor", {{"id", 30}, {"color", 69}});
    roundTrip(*f.store, "marker.resize", {{"id", 10}, {"length", 999999999}});
    check(f.db().execAndGet("SELECT length_ns FROM markers WHERE id=10").getInt64() == 999999999,
          "cycle resizing retains nanosecond units");
    roundTrip(*f.store, "marker.setNote", {{"id", 10}, {"text", "New note"}});
    roundTrip(*f.store, "marker.setPrompt", {{"id", 10}, {"text", "Literal prompt"}});
    roundTrip(*f.store, "marker.setNote", {{"id", 30}, {"text", ""}});
    roundTrip(*f.store, "marker.setPrompt", {{"id", 30}, {"text", ""}});
    const auto text = textproj::projectStore(*f.store).text;
    check(text.find("999999999ns") != std::string::npos && text.find("New note") != std::string::npos &&
              text.find("Literal prompt") != std::string::npos, "projection includes linear length, note and prompt");
    roundTrip(*f.store, "marker.delete", {{"id", 10}});
    check(f.db().execAndGet("SELECT count(*) FROM marker_details WHERE marker_id=10").getInt() == 0,
          "delete cascades owned marker text");
    check(History(*f.store).undo().ok, "restore deleted marker for reopen");
    const auto beforeClose = state(*f.store);
    const auto path = f.store->path();
    f.store.reset();
    StoreError error = StoreError::Ok;
    f.store = Store::open(path, error);
    check(f.store && state(*f.store) == beforeClose, "marker metadata survives reopen");
    check(f.store && History(*f.store).redo().ok && !SnapshotBuilder::fromStore(*f.store)->findMarker(10),
          "captured text history replays after reopen");
}

void sharing() {
    Fixture f("sharing");
    check(edit(*f.store, "marker.create", {{"id", 1}, {"pos", 42}, {"length", 90}, {"note", "n"}, {"prompt", "p"}}), "sharing seed");
    check(edit(*f.store, "marker.create", {{"id", 2}, {"pos", 4}}), "sharing sibling");
    auto prior = SnapshotBuilder::fromStore(*f.store);
    const auto again = SnapshotBuilder::fromStore(*f.store, *prior);
    check(again->markers[0] == prior->markers[0] && again->markers[1] == prior->markers[1], "unchanged markers share nodes");
    // Change each stored field. Length columns may hold imported inactive data;
    // positions change base together to honour the schema's exclusive columns.
    for (const char* sql : {"UPDATE markers SET name='new' WHERE id=1",
             "UPDATE markers SET pos_ticks=43 WHERE id=1",
             "UPDATE markers SET length_ticks=91 WHERE id=1", "UPDATE markers SET length_ns=81 WHERE id=1",
             "UPDATE markers SET color=0 WHERE id=1", "UPDATE markers SET kind='chapter' WHERE id=1",
             "UPDATE markers SET time_base=1,pos_ticks=NULL,pos_ns=73 WHERE id=1",
             "UPDATE markers SET pos_ns=74 WHERE id=1", "UPDATE marker_details SET note='new note' WHERE marker_id=1",
             "UPDATE marker_details SET prompt='new prompt' WHERE marker_id=1"}) {
        f.db().exec(sql);
        auto next = SnapshotBuilder::fromStore(*f.store, *prior);
        check(next->markers[0] != prior->markers[0] && next->markers[1] == prior->markers[1] &&
                  next->tracks[0] == prior->tracks[0] && next->tempo == prior->tempo,
              std::string("sharing sees ") + sql);
        prior = std::move(next);
    }
    f.db().exec("UPDATE markers SET id=3 WHERE id=2");
    auto next = SnapshotBuilder::fromStore(*f.store, *prior);
    check(next->markers[0] == prior->markers[0] && next->markers[1] != prior->markers[1] &&
              next->markers[1]->id == 3, "marker identity changes are visible");
}

void failuresAreAtomic() {
    Fixture f("invalid");
    check(edit(*f.store, "marker.create", {{"id", 1}, {"pos", 0}}), "invalid fixture seed");
    const auto before = state(*f.store);
    const auto count = OpJournal(*f.store).count();
    const std::array<OpRequest, 15> bad = {{
        request("marker.create", {{"id", 1}, {"pos", 2}}),
        request("marker.create", {{"id", 2}, {"pos", 2}, {"time_base", 2}}),
        request("marker.create", {{"id", 2}, {"pos", 2}, {"kind", "other"}}),
        request("marker.create", {{"id", 2}, {"pos", 2}, {"kind", "cycle"}}),
        request("marker.create", {{"id", 2}, {"pos", 2}, {"length", 0}}),
        request("marker.create", {{"id", 2}, {"pos", 2}, {"kind", "position"}, {"length", 4}}),
        request("marker.create", {{"id", 2}, {"pos", 2}, {"color", 70}}),
        request("marker.create", {{"id", 2}, {"pos", std::numeric_limits<std::uint64_t>::max()}}),
        request("marker.move", {{"id", 1}, {"pos", std::numeric_limits<std::uint64_t>::max()}}),
        request("marker.setColor", {{"id", 1}, {"color", -1}}),
        request("marker.resize", {{"id", 1}, {"length", 50}}),
        request("marker.setNote", {{"id", 999}, {"text", "missing"}}),
        request("marker.delete", {{"id", 999}}),
        request("marker.rename", {{"id", 1}, {"name", 5}}),
        request("marker.create", {{"id", 2}, {"pos", 0}, {"row", Payload::object()}})
    }};
    for (const auto& op : bad) {
        check(!OpJournal(*f.store).commit(op).ok && state(*f.store) == before &&
                  OpJournal(*f.store).count() == count, op.opType + " invalid edit is atomic");
    }
    const std::array batch = {request("marker.setNote", {{"id", 1}, {"text", "rolled back"}}),
                             request("marker.resize", {{"id", 1}, {"length", 0}})};
    check(!OpJournal(*f.store).commit(batch).ok && state(*f.store) == before &&
              OpJournal(*f.store).count() == count, "failed second op rolls back new details and journal");
}

void captureStorageAndChildren() {
    Fixture f("capture");
    // Synthetic extension columns test captured history, not a native migration.
    f.db().exec("ALTER TABLE markers ADD COLUMN extra BLOB");
    f.db().exec("ALTER TABLE markers ADD COLUMN gain REAL DEFAULT 1.25");
    f.db().exec("ALTER TABLE markers ADD COLUMN \"quoted\"\"column\" TEXT");
    f.db().exec("ALTER TABLE marker_details ADD COLUMN extension TEXT");
    check(edit(*f.store, "marker.create", {{"id", 1}, {"pos", 2}, {"length", 6}, {"note", "n"}, {"prompt", "p"}}), "extension seed");
    f.db().exec("UPDATE markers SET extra=X'', \"quoted\"\"column\"='unknown' WHERE id=1");
    f.db().exec("UPDATE marker_details SET extension='future metadata' WHERE marker_id=1");
    roundTrip(*f.store, "marker.delete", {{"id", 1}});
    check(History(*f.store).undo().ok, "restore empty blob");
    check(f.db().execAndGet("SELECT typeof(extra)='blob' AND length(extra)=0 AND gain=1.25 AND "
          "\"quoted\"\"column\"='unknown' FROM markers WHERE id=1").getInt() == 1, "all SQLite storage types and quoted extension name survive");
    check(f.db().execAndGet("SELECT extension FROM marker_details WHERE marker_id=1").getString() == "future metadata",
          "delete undo preserves details extension");
    f.db().exec("UPDATE markers SET extra=X'0001FF' WHERE id=1");
    roundTrip(*f.store, "marker.delete", {{"id", 1}});
    check(History(*f.store).undo().ok && f.db().execAndGet("SELECT hex(extra) FROM markers WHERE id=1").getString() == "0001FF",
          "binary capture survives CBOR journal decoding");
    f.db().exec("CREATE TABLE marker_child(id INTEGER PRIMARY KEY, marker_id INTEGER REFERENCES markers(id) ON DELETE CASCADE)");
    f.db().exec("CREATE TABLE text_child(id INTEGER PRIMARY KEY, marker_id INTEGER REFERENCES marker_details(marker_id) ON DELETE CASCADE)");
    f.db().exec("INSERT INTO marker_child VALUES(1,1)");
    f.db().exec("INSERT INTO text_child VALUES(1,1)");
    roundTrip(*f.store, "marker.setColor", {{"id", 1}, {"color", 4}});
    roundTrip(*f.store, "marker.resize", {{"id", 1}, {"length", 9}});
    roundTrip(*f.store, "marker.setNote", {{"id", 1}, {"text", ""}});
    roundTrip(*f.store, "marker.setPrompt", {{"id", 1}, {"text", ""}});
    check(f.db().execAndGet("SELECT count(*) FROM marker_child").getInt() == 1 &&
              f.db().execAndGet("SELECT count(*) FROM text_child").getInt() == 1,
          "in-place inverse and clearing text never cascade unrelated child rows");
    check(f.db().execAndGet("SELECT extension FROM marker_details WHERE marker_id=1").getString() == "future metadata",
          "clearing both texts retains optional row's future fields");
}

void migrationAndReadOnly() {
    test::TempDirectory temp("markers", "old");
    const auto path = temp.path() / "old.adi";
    {
        SQLite::Database db(path.string(), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        db.exec(readFile(ADI_MARKER_HISTORY_FILE));
        db.exec("INSERT INTO project(id,name) VALUES(1,'Old project')");
        db.exec("INSERT INTO markers(id,time_base,pos_ns,length_ns,name,kind,color) VALUES(7,1,999,12,'Old marker','cycle',0)");
    }
    const auto bytes = readFile(path);
    StoreError error = StoreError::Ok;
    {
        auto store = Store::open(path, error, true);
        check(store && error == StoreError::Ok && store->schemaMinor() == 9 && store->readOnly(), "1.9 read-only open keeps version");
        if (!store) return;
        const auto snapshot = SnapshotBuilder::fromStore(*store);
        const auto* marker = snapshot->findMarker(7);
        check(marker && marker->name == "Old marker" && marker->posNs == 999 && marker->lengthNs == 12 &&
                  marker->color == 0 && marker->note.empty() && marker->prompt.empty(), "old read-only marker with TEMP metadata defaults");
        check(!OpJournal(*store).commit(request("marker.setNote", {{"id", 7}, {"text", "forbidden"}})).ok, "old read-only metadata edit refused");
    }
    check(readFile(path) == bytes, "read-only open did not write old file");
    {
        auto store = Store::open(path, error);
        check(store && error == StoreError::Ok && store->schemaMinor() == 10 && store->upgradedFromMinor() == 9,
              "1.9 writable open installs additive 1.10 migration");
        if (!store) return;
        check(store->db().execAndGet("SELECT count(*) FROM marker_details").getInt() == 0, "upgrade creates empty optional metadata");
        check(store->db().execAndGet("SELECT name='Old marker' AND color=0 AND length_ns=12 FROM markers WHERE id=7").getInt() == 1,
              "upgrade preserves existing marker columns");
        roundTrip(*store, "marker.setNote", {{"id", 7}, {"text", "after upgrade"}});
    }
    auto reopened = Store::open(path, error);
    check(reopened && SnapshotBuilder::fromStore(*reopened)->findMarker(7)->note == "after upgrade", "upgraded marker text persists");
}
} // namespace

int main() {
    try {
        catalogAndValues(); sharing(); failuresAreAtomic(); captureStorageAndChildren(); migrationAndReadOnly();
    } catch (const std::exception& error) { check(false, std::string("exception: ") + error.what()); }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
