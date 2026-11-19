// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/io_audit.hpp"
#include "adi/blob.hpp"
#include "adi/clip_image.hpp"
#include "adi/controller_lanes.hpp"
#include "adi/history.hpp"
#include "adi/row_ids.hpp"
#include "adi/store.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <cstdio>
#include <limits>
#include <stdexcept>
namespace {
using namespace adi;
int checks = 0, failures = 0;
void check(bool condition, const char *message) {
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL %s\n", message);
    }
}
template <class F> bool refuses(F &&action) {
    try {
        action();
        return false;
    } catch (const std::exception &) {
        return true;
    }
}
void run() {
    test::TempDirectory temp("row-ids", "allocation");
    StoreError error = StoreError::Ok;
    auto store = Store::createEmpty(temp.path() / "ids.adi", error);
    if (!store)
        throw std::runtime_error("create allocator store");
    auto &db = store->db();
    db.exec("CREATE TABLE allocator_a(id INTEGER PRIMARY KEY,value BLOB);"
            "CREATE TABLE allocator_b(id INTEGER PRIMARY KEY);"
            "CREATE TABLE allocator_negative(id INTEGER PRIMARY KEY);"
            "CREATE TABLE allocator_wide(id INTEGER PRIMARY KEY);"
            "CREATE TABLE allocator_gap(id INTEGER PRIMARY KEY);"
            "CREATE TABLE allocator_composite(id INTEGER,k INTEGER,PRIMARY KEY(id,k));"
            "CREATE TABLE allocator_text(id TEXT PRIMARY KEY);"
            "CREATE TABLE allocator_wrong_key(key INTEGER PRIMARY KEY);"
            "CREATE TABLE allocator_nonalias(id INT PRIMARY KEY);"
            "CREATE TABLE \"allocator_quoted\"\"name\"(id INTEGER PRIMARY KEY)");
    RowIds ids(db);
    const auto changes = db.getTotalChanges();
    check(ids.next("allocator_a") == 1, "empty table starts at positive one");
    check(ids.next("allocator_a") == 2, "same allocation context reserves unpublished identity");
    check(ids.next("allocator_b") == 1, "each table has a distinct namespace");
    check(db.getTotalChanges() == changes &&
              db.execAndGet("SELECT count(*) FROM allocator_a").getInt() == 0,
          "allocation and reservation never write project rows");
    ids.reserve("allocator_a", 400);
    check(ids.next("allocator_a") == 401, "explicit high reservation skips uncommitted batch IDs");
    ids.reserve("allocator_a", 12);
    ids.reserve("allocator_a", -10);
    ids.reserve("allocator_a", 0);
    check(ids.next("allocator_a") == 402,
          "low negative and zero reservations never lower positive high water");
    db.exec("INSERT INTO allocator_negative(id) VALUES(-8),(-1)");
    check(ids.next("allocator_negative") == 1,
          "new producers never derive negative identities from legacy negative rows");
    db.exec("INSERT INTO allocator_wide(id) VALUES(9007199254741023)");
    check(ids.next("allocator_wide") == INT64_C(9007199254741024),
          "native SQLite integer IDs remain exact above binary64 precision");
    check(ids.next("allocator_wide") == INT64_C(9007199254741025),
          "reserved native integers do not round through double");
    db.exec("INSERT INTO allocator_gap(id) VALUES(1),(3),(9223372036854775807)");
    check(refuses([&] { (void)ids.next("allocator_gap"); }),
          "ordinary producers refuse signed row-ID exhaustion");
    check(ids.next("allocator_gap", RowIdExhaustion::FirstPositiveGap) == 2,
          "controller-compatible exhaustion takes the first actual positive gap");
    check(ids.next("allocator_gap", RowIdExhaustion::FirstPositiveGap) == 4,
          "gap allocation does not repeat an unpublished identity");
    ids.reserve("allocator_gap", 5);
    check(ids.next("allocator_gap", RowIdExhaustion::FirstPositiveGap) == 6,
          "gap search honors explicit unpublished reservations");
    check(refuses([&] { (void)ids.next("allocator_gap"); }),
          "gap allocation never relaxes a later ordinary producer's exhaustion rule");
    ids.reserve("allocator_b", std::numeric_limits<std::int64_t>::max());
    check(refuses([&] { (void)ids.next("allocator_b"); }),
          "explicit signed maximum reservation refuses before addition");
    check(ids.next("allocator_b", RowIdExhaustion::FirstPositiveGap) == 2,
          "gap search skips identities allocated before a maximum reservation");
    check(ids.next("allocator_quoted\"name") == 1,
          "bound metadata and quoted identifiers support a literal embedded quote");
    for (const std::string table :
         {"allocator_composite", "allocator_text", "allocator_wrong_key", "allocator_nonalias",
          "missing", "", "allocator_a; DROP TABLE allocator_a; --"})
        check(refuses([&] { (void)ids.next(table); }),
              "unsupported missing or malformed table refuses without executable SQL");
    const std::string nul("allocator_a\0suffix", 18);
    check(refuses([&] { (void)ids.next(nul); }), "embedded NUL never aliases a real table");
    check(db.execAndGet("SELECT count(*) FROM allocator_a").getInt() == 0,
          "refused identifiers leave the real table untouched");
    db.exec("INSERT INTO allocator_a(id) VALUES(10),(1000)");
    RowIds later(db);
    check(later.next("allocator_a") == 1001,
          "a new snapshot context observes the actual current table maximum");
    const bool audioWas = audio::onAudioThread;
    audio::onAudioThread = true;
    const bool callback = refuses([&] { (void)ids.next("allocator_a"); });
    audio::onAudioThread = audioWas;
    check(callback, "even a cached namespace refuses allocation on the callback thread");
    clip_edit::CopyIds copy(db);
    copy.reserve("clips", 200);
    check(copy.next("clips") == 201 && copy.next("clips") == 202,
          "existing clip-copy API shares central reservation semantics");
    check(db.getTotalChanges() == changes + 8,
          "all allocator operations leave only explicit fixture inserts in SQLite");
}
void controllerExhaustion() {
    test::TempDirectory temp("row-ids", "controller-exhaustion");
    StoreError error = StoreError::Ok;
    auto store = Store::createEmpty(temp.path() / "controller.adi", error);
    if (!store)
        throw std::runtime_error("create controller allocator project");
    auto &db = store->db();
    db.exec("INSERT INTO project(id,name) VALUES(1,'Allocation');"
            "INSERT INTO tracks(id,kind,name) VALUES(1,'midi','MIDI');"
            "INSERT INTO clips(id,track_id,kind,time_base,pos_ticks,length_ticks) VALUES"
            "(1,1,'midi',0,0,5765760),(2,1,'midi',0,5765760,5765760)");
    const auto notes = writeStream<NoteRecord>(FourCC::Notes, {});
    SQLite::Statement insert(db, "INSERT INTO event_streams(id,clip_id,stream_kind,data) "
                                 "VALUES(9223372036854775807,2,'notes',?)");
    insert.bind(1, notes.data(), static_cast<int>(notes.size()));
    insert.exec();
    const std::vector<controllers::Point> input{{0, 0, 0.25, 0, 1, 0}, {0, 1000, 0.75, 0, 1, 0}};
    const auto changes = db.getTotalChanges();
    const auto prepared = controllers::prepareWrite(*store, 1, 0, 7, input);
    check(prepared.ok && prepared.address.id == 1 && db.getTotalChanges() == changes,
          "real controller producer retains its positive-gap exhaustion fallback without writes");
    const auto committed = controllers::commitPrepared(*store, prepared);
    check(committed.ok && db.execAndGet("SELECT count(*) FROM event_streams").getInt() == 2 &&
              store->getEventStream(2, "notes") == std::optional<std::vector<std::byte>>(notes),
          "gap-allocated controller commits beside a signed-maximum notes stream without changing "
          "it");
    check(History(*store).undo().ok &&
              db.execAndGet("SELECT id FROM event_streams").getInt64() == INT64_MAX &&
              History(*store).redo().ok &&
              db.execAndGet("SELECT count(*) FROM event_streams WHERE id=1").getInt() == 1,
          "Undo and Redo retain the captured positive-gap identity and old maximum identity");
}
} // namespace
int main() {
    try {
        run();
        controllerExhaustion();
    } catch (const std::exception &error) {
        ++failures;
        std::printf("EXCEPTION %s\n", error.what());
    }
    std::printf("row_ids: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
