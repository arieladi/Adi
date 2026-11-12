// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/mix_functions.hpp"
#include "adi/mix_function_edit.hpp"
#include "adi/history.hpp"
#include "adi/store.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <cstdio>
#include <limits>
#include <stdexcept>
namespace {
using namespace adi;
using namespace adi::mix;
int checks = 0, failures = 0;
void check(bool ok, const char *what) { ++checks; if (!ok) { ++failures; std::printf("FAIL %s\n", what); } }
struct Fixture {
    test::TempDirectory temporary{"mix_functions", "prepared"};
    std::unique_ptr<Store> store;
    Fixture() {
        StoreError error{}; store = Store::create(temporary.path() / "song.adi", error);
        if (!store) throw std::runtime_error(toString(error));
        auto &db = store->db();
        db.exec("INSERT INTO tracks(id,kind,name) VALUES(1,'audio','A'),(2,'audio','B'),(3,'audio','Empty')");
        db.exec("INSERT INTO device_chains(id,track_id,name) VALUES(10,1,'A'),(20,2,'B')");
        db.exec("INSERT INTO plugin_refs(id,format,uid,vendor,name,version,subtype) VALUES(1,'clap','org.adidaw.mix.eq','Other','Ordinary EQ','1','effect')");
        db.exec("INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name,enabled,is_rack,latency_samples) VALUES(10,10,100,1,'Ordinary',1,0,0)");
    }
    std::int64_t add(Function kind, std::int64_t track = 1) {
        auto prepared = prepareFunctionInsert(*store, kind, track, true);
        auto result = commitFunctionEdit(*store, prepared);
        check(prepared.ok && result.ok, "available native function inserts through the prepared journal batch");
        return prepared.deviceId;
    }
    std::string order(std::int64_t chain = 10) {
        return store->db().execAndGet("SELECT COALESCE(group_concat(id,','),'') FROM (SELECT id FROM devices WHERE chain_id=" +
            std::to_string(chain) + " ORDER BY ord,id)").getString();
    }
};
void catalogue() {
    check(functionCatalog().size() == 5, "exactly the five decided functions exist; no reserved Still or analyzer function");
    for (const auto &info : functionCatalog()) {
        check(functionInfo(info.kind) == &info && functionForReference("internal", info.uid) == &info,
              "stable internal UID round-trips to its function");
        check(!functionForReference("vst3", info.uid) && !functionForReference("clap", info.uid),
              "ordinary external plug-ins cannot become native functions by name or UID coincidence");
    }
    check(!functionForUid("org.adidaw.mix.still") && !functionInfo(static_cast<Function>(999)),
          "unknown catalogue values are refused");
}
void insertionAndUndo() {
    Fixture f;
    const auto compressor = f.add(Function::Compressor), gate = f.add(Function::Gate), limiter = f.add(Function::Limiter);
    const auto eq = f.add(Function::Eq), deesser = f.add(Function::Deesser);
    check(f.order() == "10," + std::to_string(eq) + "," + std::to_string(compressor) + "," + std::to_string(gate) + "," +
          std::to_string(deesser) + "," + std::to_string(limiter),
          "default native tail has EQ first, middle addition order, Limiter last, ordinary chain untouched");
    check(History(*f.store).undo().ok && f.store->db().execAndGet("SELECT count(*) FROM devices WHERE id=" + std::to_string(deesser)).getInt() == 0,
          "one undo removes the function and its order adjustments");
    check(History(*f.store).redo().ok && f.order().ends_with(std::to_string(deesser) + "," + std::to_string(limiter)),
          "redo restores the same authored identity and default-tail order");
    check(!prepareFunctionInsert(*f.store, Function::Eq, 1, true).ok,
          "duplicate native function on the owning channel is refused");
    check(f.add(Function::Eq, 2) != eq, "another channel has its own native function identity");
    OpRequest parameter; parameter.opType = "device.setParam";
    parameter.payload = {{"dev", limiter}, {"param", "input_gain"}, {"norm", .7}};
    check(OpJournal(*f.store).commit(parameter).ok, "function parameter uses the normal parameter op");
    OpRequest remove; remove.opType = "device.remove"; remove.payload = {{"id", limiter}};
    check(OpJournal(*f.store).commit(remove).ok && History(*f.store).undo().ok,
          "function removal uses ordinary device.remove and is undoable");
    check(f.store->db().execAndGet("SELECT normalized_value FROM plugin_params WHERE device_id=" + std::to_string(limiter)).getDouble() == .7,
          "undo removal restores stored parameter values rather than factory defaults");
}
void moves() {
    Fixture f;
    const auto limiter = f.add(Function::Limiter), eq = f.add(Function::Eq);
    const auto original = f.order();
    auto move = prepareFunctionMove(*f.store, limiter, 1, 10);
    check(move.ok && commitFunctionEdit(*f.store, move).ok && f.order() == std::to_string(limiter) + ",10," + std::to_string(eq),
          "a native tab moves before an ordinary device with its identity intact");
    check(History(*f.store).undo().ok && f.order() == original, "one undo restores the exact prior chain order");
    check(History(*f.store).redo().ok && f.order().starts_with(std::to_string(limiter) + ",10"), "redo restores the moved position");
    const auto before = f.store->db().execAndGet("SELECT count(*) FROM ops").getInt();
    const auto noop = prepareFunctionMove(*f.store, limiter, 1, limiter);
    check(noop.ok && noop.requests.empty() && commitFunctionEdit(*f.store, noop).ok &&
          f.store->db().execAndGet("SELECT count(*) FROM ops").getInt() == before,
          "dropping a tab on itself creates no mutation or undo step");
    check(!prepareFunctionMove(*f.store, 10, 1).ok, "ordinary plug-ins do not use the native-function drag path");
    check(!prepareFunctionMove(*f.store, limiter, 2).ok && !prepareFunctionMove(*f.store, limiter, 1, 999).ok,
          "cross-channel and missing-target drops are refused");
    const auto compressor = f.add(Function::Compressor);
    check(f.order() == std::to_string(limiter) + ",10," + std::to_string(eq) + "," + std::to_string(compressor),
          "adding another function does not pull a manually moved Limiter back to the tail");
}
void refusalsAndState() {
    Fixture f;
    const auto ops = f.store->db().execAndGet("SELECT count(*) FROM ops").getInt();
    check(!prepareFunctionInsert(*f.store, Function::Eq, 3, false).ok &&
          f.store->db().execAndGet("SELECT count(*) FROM device_chains WHERE track_id=3").getInt() == 0 &&
          f.store->db().execAndGet("SELECT count(*) FROM ops").getInt() == ops,
          "unavailable engines create neither authored devices, chains nor undo steps");
    check(!prepareFunctionInsert(*f.store, Function::Gate, 999, true).ok &&
          !prepareFunctionInsert(*f.store, static_cast<Function>(999), 1, true).ok,
          "missing channel and unknown function are named refusals");
    const auto prepared = prepareFunctionInsert(*f.store, Function::Limiter, 1, true);
    f.add(Function::Eq);
    const auto stale = commitFunctionEdit(*f.store, prepared);
    check(!stale.ok && stale.stale, "a concurrent journal edit rejects a stale insertion plan");
    Fixture other;
    check(!commitFunctionEdit(*other.store, prepareFunctionInsert(*f.store, Function::Gate, 1, true)).ok,
          "prepared edits cannot cross projects");
    Fixture legacy;
    legacy.add(Function::Limiter);
    check(History(*legacy.store).undo().ok && legacy.store->db().execAndGet("SELECT ord FROM devices WHERE id=10").getInt64() == 100,
          "undo preserves the exact legacy order value before normalization");
    StoreError error{};
    auto readonly = Store::open(f.temporary.path() / "song.adi", error, true);
    check(readonly && !prepareFunctionInsert(*readonly, Function::Gate, 1, true).ok,
          "read-only projects refuse function edits");
}
}
int main() {
    try { catalogue(); insertionAndUndo(); moves(); refusalsAndState(); }
    catch (const std::exception &e) { ++failures; std::printf("FAIL exception: %s\n", e.what()); }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
