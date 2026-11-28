// SPDX-License-Identifier: GPL-3.0-or-later
//
// The nine device ops of OPS.md 9.7, plus the chain pair they need to exist.
//
// WHY THIS IS A SEPARATE SUITE FROM test_replay.cpp, which is the finding that
// produced it. The round-trip corpus applies the device ops and then undoes
// everything, and that ought to catch an inverse that restores the wrong
// thing. It does not, for one specific reason:
//
//     Undoing `device.insert` deletes the device, and `plugin_params` and
//     `plugin_state` CASCADE on `device_id`. So a setParam inverse that leaves
//     a spurious row behind has that row swept away moments later by an undo
//     further down the stack, and the final comparison against a blank
//     project passes.
//
// Planted and watched: making `device.setParam`'s inverse record 0.0 instead
// of absence leaves the corpus at 58 checks, 0 failures. The corpus is not
// wrong -- it tests replay determinism and it does -- but a cascade is a very
// effective way to hide a per-row defect, and every check here undoes ONE
// transaction and looks at the row rather than at the project.
//
// ADR-0057 is the decision these implement against.

#include "temp_directory.hpp"

#include "adi/digest.hpp"
#include "adi/history.hpp"
#include "adi/media/blake3.hpp"
#include "adi/ops.hpp"
#include "adi/rack_drums.hpp"
#include "adi/store.hpp"
#include "adi/track_image.hpp"
#include "adi/store_rows.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace adi;

int g_failures = 0;
int g_checks = 0;

void check(bool cond, const std::string& what) {
    ++g_checks;
    if (!cond) { ++g_failures; std::printf("  FAIL  %s\n", what.c_str()); }
}
void section(const char* s) { std::printf("[%s]\n", s); }

struct Scratch {
    adi::test::TempDirectory temp; // destroyed after store/file members
    fs::path dir;
    explicit Scratch(const char* name)
        : temp("device_ops", name), dir(temp.path()) {
    }
    fs::path operator/(const char* leaf) const { return dir / leaf; }
};

constexpr const char* kHash  = "b3aaaa0000000000000000000000aaaa";
constexpr const char* kHash2 = "b3bbbb0000000000000000000000bbbb";

/// A project with one track, one chain and one device already in it, so each
/// test starts where the interesting part begins.
std::unique_ptr<Store> project(const fs::path& p) {
    StoreError err = StoreError::Ok;
    auto st = Store::createEmpty(p, err);
    if (!st) return st;
    st->db().exec("INSERT INTO project(id,name) VALUES (1,'Devices')");
    for (const char* h : {kHash, kHash2}) {
        SQLite::Statement b(st->db(),
            "INSERT INTO state_blobs(hash_blake3, data, size_bytes) VALUES (?,?,?)");
        b.bind(1, h);
        const std::string bytes = std::string("chunk-") + h;
        b.bind(2, bytes.data(), static_cast<int>(bytes.size()));
        b.bind(3, static_cast<std::int64_t>(bytes.size()));
        b.exec();
    }
    return st;
}

/// One op, one transaction -- so `undo()` steps over exactly one of them.
bool run(Store& s, const char* type, Payload p, std::string* errOut = nullptr) {
    OpRequest r;
    r.opType = type;
    r.payload = std::move(p);
    r.label = type;
    OpJournal j(s);
    const std::vector<OpRequest> one{r};
    const auto res = j.commit(one);
    if (!res.ok && errOut != nullptr) *errOut = res.error;
    return res.ok;
}

bool seedChainAndDevice(Store& s) {
    return run(s, "track.create", {{"id", 1}, {"kind", "audio"}, {"name", "Bass"}})
        && run(s, "chain.create", {{"id", 5}, {"track", 1}, {"name", "FX"}})
        // ord 2, NOT 0. A device at position zero makes every "the ord was
        // restored" check pass against an inverse that captured nothing --
        // the assertion would be comparing a correct answer to a different
        // correct answer that happens to coincide.
        && run(s, "device.insert", {{"id", 9}, {"chain", 5}, {"ord", 2},
                                    {"name", "EQ"}});
}

/// The stored value of one parameter, or nullopt when there is NO ROW. The
/// distinction is the whole point of several checks below, so it is in the
/// return type rather than in a sentinel.
struct Stored {
    double norm = 0.0;
    std::optional<double> real;
    std::string display;
};
std::optional<Stored> readParam(Store& s, std::int64_t dev, const char* param) {
    SQLite::Statement st(s.db(),
        "SELECT normalized_value, real_value, display FROM plugin_params "
        "WHERE device_id = ? AND param_id = ?");
    st.bind(1, dev);
    st.bind(2, param);
    if (!st.executeStep()) return std::nullopt;
    Stored v;
    v.norm = st.getColumn(0).getDouble();
    if (!st.getColumn(1).isNull()) v.real = st.getColumn(1).getDouble();
    v.display = st.getColumn(2).getString();
    return v;
}

std::int64_t countRows(Store& s, const char* sql, std::int64_t bind) {
    SQLite::Statement st(s.db(), sql);
    st.bind(1, bind);
    return st.executeStep() ? st.getColumn(0).getInt64() : -1;
}

// --- the nine are registered -------------------------------------------------

void testAllNineExist() {
    section("all nine device ops of OPS.md 9.7 have handlers");
    const auto& reg = OpRegistry::instance();
    // Named individually rather than counted: a count passes when one op is
    // missing and another is misspelled.
    for (const char* name : {"device.insert", "device.remove", "device.move",
                             "device.setEnabled", "device.setParam",
                             "device.loadState", "device.setPreset",
                             "device.rename", "device.setLatency"}) {
        const auto* d = reg.find(name);
        check(d != nullptr && d->apply != nullptr,
              std::string(name) + " is registered with a handler");
    }
    for (const char* name : {"chain.create", "chain.delete"}) {
        const auto* d = reg.find(name);
        check(d != nullptr && d->apply != nullptr,
              std::string(name) + " is registered with a handler");
    }

    // OPS.md 9.7 marks setParam coalescable, and validate_ops check 6 refuses
    // a coalescable op with a paired inverse -- coalescing keeps the FIRST
    // op's inverse, so a state capture would discard the intermediate states.
    const auto* sp = reg.find("device.setParam");
    check(sp != nullptr && sp->coalescable, "device.setParam is coalescable");
    check(sp != nullptr && sp->inverseOp.empty(),
          "and symmetric, not paired -- OPS.md 6.4");

    // ADR-0058: a latency change moves every compensated node downstream, so
    // it cannot be a snapshot.
    const auto* sl = reg.find("device.setLatency");
    check(sl != nullptr && sl->engineImpact == EngineImpact::GraphRebuild,
          "device.setLatency rebuilds the graph rather than publishing a snapshot");
}

// --- the absence problem -----------------------------------------------------

void testFirstTouchUndoesToAbsence() {
    section("ADR-0057: undoing a parameter's FIRST touch removes the row");
    Scratch sc("absence");
    auto s = project(sc / "p.adi");
    check(s != nullptr, "project opens");
    if (!s) return;
    check(seedChainAndDevice(*s), "a track, a chain and a device");

    check(!readParam(*s, 9, "cutoff").has_value(),
          "the parameter starts with NO ROW, which is the case that matters");

    check(run(*s, "device.setParam", {{"dev", 9}, {"param", "cutoff"},
                                      {"norm", 0.75}, {"real", 4800.0},
                                      {"display", "4.80 kHz"}}),
          "first touch stores it");
    auto v = readParam(*s, 9, "cutoff");
    check(v.has_value(), "the row now exists");
    check(v && v->norm == 0.75, "normalized value is stored");
    check(v && v->real.has_value() && *v->real == 4800.0, "and the real value");

    History h(*s);
    check(h.undo().ok, "undo runs");

    // THE CHECK THIS SUITE EXISTS FOR. An inverse that recorded 0.0 rather
    // than absence leaves a row here, and the round-trip corpus does not see
    // it because undoing the device.insert cascades it away.
    check(!readParam(*s, 9, "cutoff").has_value(),
          "undo removed the ROW, it did not set the value to zero");

    check(h.redo().ok, "redo runs");
    v = readParam(*s, 9, "cutoff");
    check(v && v->norm == 0.75 && v->real && *v->real == 4800.0,
          "redo restores both representations");
}

void testLaterTouchUndoesToThePreviousValue() {
    section("a subsequent change undoes to the value before it, not to absence");
    Scratch sc("later");
    auto s = project(sc / "p.adi");
    if (!s) { check(false, "project opens"); return; }
    check(seedChainAndDevice(*s), "seeded");

    check(run(*s, "device.setParam", {{"dev", 9}, {"param", "gain"}, {"norm", 0.2}}),
          "first");
    check(run(*s, "device.setParam", {{"dev", 9}, {"param", "gain"}, {"norm", 0.9}}),
          "second");

    History h(*s);
    check(h.undo().ok, "undo the second");
    auto v = readParam(*s, 9, "gain");
    check(v.has_value() && v->norm == 0.2, "back to 0.2, and the row survives");

    check(h.undo().ok, "undo the first");
    check(!readParam(*s, 9, "gain").has_value(), "now the row is gone");
}

void testNullNormClearsAndRestores() {
    section("norm: null clears the row, and its inverse puts it back");
    Scratch sc("clear");
    auto s = project(sc / "p.adi");
    if (!s) { check(false, "project opens"); return; }
    check(seedChainAndDevice(*s), "seeded");

    check(run(*s, "device.setParam", {{"dev", 9}, {"param", "mix"},
                                      {"norm", 0.33}, {"display", "33%"}}), "set");
    check(run(*s, "device.setParam", {{"dev", 9}, {"param", "mix"},
                                      {"norm", nullptr}}), "clear");
    check(!readParam(*s, 9, "mix").has_value(), "cleared");

    History h(*s);
    check(h.undo().ok, "undo the clear");
    auto v = readParam(*s, 9, "mix");
    check(v.has_value() && v->norm == 0.33 && v->display == "33%",
          "the value AND its display come back");
}

void testMissingNormIsLoud() {
    section("omitting norm entirely is an error, not a silent delete");
    Scratch sc("loud");
    auto s = project(sc / "p.adi");
    if (!s) { check(false, "project opens"); return; }
    check(seedChainAndDevice(*s), "seeded");
    check(run(*s, "device.setParam", {{"dev", 9}, {"param", "q"}, {"norm", 0.5}}),
          "a value is stored");

    std::string err;
    const bool ok = run(*s, "device.setParam", {{"dev", 9}, {"param", "q"}}, &err);
    check(!ok, "a payload with no 'norm' key is refused");
    check(err.find("norm") != std::string::npos, "and says which key: " + err);
    check(readParam(*s, 9, "q").has_value(),
          "the stored value is untouched -- a typo must not delete a parameter");
}

// --- the VST3 edge -----------------------------------------------------------

void testRealValueStaysNull() {
    section("ADR-0057: a device with no real value stores NULL, never 0.0");
    Scratch sc("realnull");
    auto s = project(sc / "p.adi");
    if (!s) { check(false, "project opens"); return; }
    check(seedChainAndDevice(*s), "seeded");

    // The VST3 case. getParamStringByValue hands back "4.80 kHz" and parsing a
    // localised display string back into a number is a guess, so `real` is
    // genuinely absent -- and 0.0 there would read as "this parameter is at
    // zero Hz", which is a value, not a gap.
    check(run(*s, "device.setParam", {{"dev", 9}, {"param", "freq"},
                                      {"norm", 0.42}, {"display", "4.80 kHz"}}),
          "a VST3-shaped write with no real value");
    auto v = readParam(*s, 9, "freq");
    check(v.has_value(), "stored");
    check(v && !v->real.has_value(), "real_value is NULL, not 0.0");
    check(v && v->display == "4.80 kHz", "the display string is kept, since it is all there is");

    // And the other half: a format that CAN give a real value stores it, which
    // is what keeps an automation lane meaningful when the plugin is missing.
    check(run(*s, "device.setParam", {{"dev", 9}, {"param", "delay"},
                                      {"norm", 0.5}, {"real", 120.0},
                                      {"display", "120 ms"}}),
          "a CLAP/native-shaped write");
    v = readParam(*s, 9, "delay");
    check(v && v->real.has_value() && *v->real == 120.0, "real_value survives");
}

// --- state (ADR-0038) --------------------------------------------------------

void testOpaqueState() {
    section("ADR-0038: state is content-addressed, and two roles may share a blob");
    Scratch sc("state");
    auto s = project(sc / "p.adi");
    if (!s) { check(false, "project opens"); return; }
    check(seedChainAndDevice(*s), "seeded");

    check(run(*s, "device.loadState", {{"dev", 9}, {"role", "component"},
                                       {"hash", kHash}, {"hint", "vst3"}}), "component");
    check(run(*s, "device.loadState", {{"dev", 9}, {"role", "controller"},
                                       {"hash", kHash}}), "controller, same blob");
    check(countRows(*s, "SELECT COUNT(*) FROM plugin_state WHERE device_id = ?", 9) == 2,
          "two rows");
    // The re-tweak: a second state for the same role REPLACES rather than
    // duplicating. Without the (device_id, stream_role) primary key ADR-0057
    // introduced, this inserts a second row and the device has two states.
    check(run(*s, "device.loadState", {{"dev", 9}, {"role", "component"},
                                       {"hash", kHash2}}), "replace the component state");
    check(countRows(*s, "SELECT COUNT(*) FROM plugin_state WHERE device_id = ?", 9) == 2,
          "still two rows -- the write upserted rather than duplicating");

    History h(*s);
    check(h.undo().ok, "undo it");
    SQLite::Statement st(s->db(),
        "SELECT state_hash FROM plugin_state WHERE device_id = 9 AND stream_role = 'component'");
    check(st.executeStep() && st.getColumn(0).getString() == kHash,
          "the previous hash is restored, not the previous bytes");

    // A hash with no blob behind it is refused by the foreign key. A device
    // whose state points at nothing is a device whose state is gone, and
    // failing at the write is the only moment that is recoverable.
    std::string err;
    check(!run(*s, "device.loadState", {{"dev", 9}, {"role", "chunk"},
                                        {"hash", "b3nosuchblob"}}, &err),
          "a hash with no blob is refused");
}

// --- insert / remove ---------------------------------------------------------

void testRemoveRestoresEverything() {
    section("ADR-0011/0038: undoing a removal restores parameters and state too");
    Scratch sc("remove");
    auto s = project(sc / "p.adi");
    if (!s) { check(false, "project opens"); return; }
    check(seedChainAndDevice(*s), "seeded");

    check(run(*s, "device.setParam", {{"dev", 9}, {"param", "drive"},
                                      {"norm", 0.6}, {"real", 6.0},
                                      {"display", "+6 dB"}}), "a parameter");
    check(run(*s, "device.loadState", {{"dev", 9}, {"role", "component"},
                                       {"hash", kHash}}), "and some state");
    check(run(*s, "device.setPreset", {{"dev", 9}, {"preset", "Crunch"}}), "and a preset");

    check(run(*s, "device.remove", {{"id", 9}}), "remove the device");
    check(countRows(*s, "SELECT COUNT(*) FROM devices WHERE id = ?", 9) == 0, "it is gone");
    check(countRows(*s, "SELECT COUNT(*) FROM plugin_params WHERE device_id = ?", 9) == 0,
          "and its parameters cascaded");

    History h(*s);
    check(h.undo().ok, "undo the removal");

    // The check that matters. A pair inverse that restored only the `devices`
    // row would bring the device back at its factory defaults -- which LOOKS
    // like the undo worked, and is how somebody loses an hour of sound design.
    auto v = readParam(*s, 9, "drive");
    check(v.has_value() && v->norm == 0.6, "the parameter value came back");
    check(v && v->real.has_value() && *v->real == 6.0, "with its real value");
    check(v && v->display == "+6 dB", "and its display string");
    check(countRows(*s, "SELECT COUNT(*) FROM plugin_state WHERE device_id = ?", 9) == 1,
          "the state reference came back");

    SQLite::Statement st(s->db(), "SELECT preset_name FROM devices WHERE id = 9");
    check(st.executeStep() && st.getColumn(0).getString() == "Crunch",
          "and the preset name");
}

void testRemoveNestedRack() {
    section("rack removal restores nested chains and both directions of macro mappings");
    Scratch sc("rack-remove");
    auto s = project(sc / "p.adi");
    if (!s || !seedChainAndDevice(*s)) {
        check(false, "seed rack project");
        return;
    }
    auto& db = s->db();
    db.exec("UPDATE devices SET is_rack=1,rack_kind='effect' WHERE id=9;"
            "INSERT INTO device_chains(id,parent_device_id,ord,name,key_zone) "
            "VALUES(6,9,0,'Outer chain',X'000107ff');"
            "INSERT INTO devices(id,chain_id,ord,name,is_rack,rack_kind) "
            "VALUES(10,6,0,'Nested rack',1,'effect');"
            "INSERT INTO device_chains(id,parent_device_id,ord,name) VALUES(7,10,1,'Nested chain');"
            "INSERT INTO devices(id,chain_id,ord,name) VALUES(11,7,0,'Leaf'),(20,5,3,'Sibling');"
            "ALTER TABLE macros ADD COLUMN opaque BLOB NOT NULL DEFAULT X'00';"
            "INSERT INTO macros(id,device_id,ord,name,value,opaque) "
            "VALUES(50,9,0,'Outer',0.2,X'00ff'),(51,10,0,'Inner',0.7,X'0180'),"
            "(52,20,0,'Sibling macro',0.1,X'90');"
            "INSERT INTO macro_mappings(id,macro_id,target_device_id,target_param_ref,range_min) "
            "VALUES(60,50,11,'cutoff',0.2),(61,51,20,'wet',0.1),(62,52,11,'gain',0.4);"
            "INSERT INTO plugin_params(device_id,param_id,normalized_value) VALUES(11,'cutoff',0.8);"
            "INSERT INTO device_panels(device_id) VALUES(11);"
            "INSERT INTO device_panel_params(device_id,ord,param_id) VALUES(11,0,'cutoff');"
            "INSERT INTO device_expression_routes(device_id,route) VALUES(11,'mpe_midi')");
    check(run(*s, "device.loadState", {{"dev", 11}, {"role", "component"}, {"hash", kHash}}),
          "seed nested opaque state");
    const auto before = digestProject(*s).text;
    check(run(*s, "device.remove", {{"id", 9}}) &&
              db.execAndGet("SELECT count(*) FROM devices").getInt() == 1 &&
              db.execAndGet("SELECT count(*) FROM device_chains").getInt() == 1 &&
              db.execAndGet("SELECT count(*) FROM macros").getInt() == 1 &&
              db.execAndGet("SELECT count(*) FROM macro_mappings").getInt() == 0,
          "remove cascades nested material while the sibling remains");
    const auto after = digestProject(*s).text;
    History h(*s);
    check(h.undo().ok && digestProject(*s).text == before,
          "one Undo restores the full nested structure, macro mappings, typed bytes and state");
    check(h.redo().ok && digestProject(*s).text == after &&
              h.undo().ok && digestProject(*s).text == before,
          "Redo and second Undo produce the same exact material");
}

void testMissingPluginSurvivesTheRoundTrip() {
    section("ADR-0011: a missing plugin is inserted, not skipped, and stays missing");
    Scratch sc("missing");
    auto s = project(sc / "p.adi");
    if (!s) { check(false, "project opens"); return; }
    check(run(*s, "track.create", {{"id", 1}, {"kind", "audio"}, {"name", "Vox"}}), "track");
    check(run(*s, "chain.create", {{"id", 5}, {"track", 1}}), "chain");

    check(run(*s, "device.insert", {{"id", 30}, {"chain", 5}, {"ord", 0},
                                    {"name", "Valhalla VintageVerb"},
                                    {"missing", true}}),
          "a device whose plugin did not load is STILL inserted");

    SQLite::Statement st(s->db(), "SELECT missing, name FROM devices WHERE id = 30");
    check(st.executeStep(), "the row exists");
    check(st.getColumn(0).getInt() == 1, "flagged missing");
    check(st.getColumn(1).getString() == "Valhalla VintageVerb",
          "and named, because 'a plugin is missing' is not actionable");

    check(run(*s, "device.remove", {{"id", 30}}), "remove it");
    History h(*s);
    check(h.undo().ok, "undo");
    SQLite::Statement again(s->db(), "SELECT missing FROM devices WHERE id = 30");
    check(again.executeStep() && again.getColumn(0).getInt() == 1,
          "it comes back MISSING -- not as a device claiming to have loaded");
}

void testMoveAndScalars() {
    section("move, enable, rename, latency");
    Scratch sc("move");
    auto s = project(sc / "p.adi");
    if (!s) { check(false, "project opens"); return; }
    check(seedChainAndDevice(*s), "seeded");
    check(run(*s, "chain.create", {{"id", 6}, {"track", 1}, {"name", "Post"}}),
          "a second chain");

    check(run(*s, "device.move", {{"id", 9}, {"chain", 6}, {"ord", 3}}), "move it");
    SQLite::Statement st(s->db(), "SELECT chain_id, ord FROM devices WHERE id = 9");
    check(st.executeStep() && st.getColumn(0).getInt64() == 6 && st.getColumn(1).getInt64() == 3,
          "both columns moved");

    History h(*s);
    check(h.undo().ok, "undo the move");
    SQLite::Statement back(s->db(), "SELECT chain_id, ord FROM devices WHERE id = 9");
    check(back.executeStep() && back.getColumn(0).getInt64() == 5 && back.getColumn(1).getInt64() == 2,
          "both columns came back -- a move that restored only the chain would "
          "leave the device in the right chain at the wrong position");

    check(run(*s, "device.setEnabled", {{"id", 9}, {"enabled", false}}), "bypass");
    check(run(*s, "device.rename", {{"id", 9}, {"name", "EQ (off)"}}), "rename");
    check(run(*s, "device.setLatency", {{"id", 9}, {"latency", 2048}}), "latency");
    SQLite::Statement row(s->db(),
        "SELECT enabled, name, latency_samples FROM devices WHERE id = 9");
    check(row.executeStep(), "row reads back");
    check(row.getColumn(0).getInt() == 0, "bypassed");
    check(row.getColumn(1).getString() == "EQ (off)", "renamed");
    check(row.getColumn(2).getInt64() == 2048, "latency recorded");
}

void testChainPair() {
    section("chain.create / chain.delete, and the exactly-one-owner rule");
    Scratch sc("chain");
    auto s = project(sc / "p.adi");
    if (!s) { check(false, "project opens"); return; }
    check(run(*s, "track.create", {{"id", 1}, {"kind", "audio"}, {"name", "Gtr"}}), "track");
    check(run(*s, "chain.create", {{"id", 5}, {"track", 1}, {"name", "FX"}}), "create");

    // The schema CHECKs that exactly one owner is set. Neither is a chain that
    // belongs to nothing; both is a chain in two places.
    std::string err;
    check(!run(*s, "chain.create", {{"id", 6}}, &err), "a chain with no owner is refused");
    check(!run(*s, "chain.create", {{"id", 7}, {"track", 1}, {"device", 9}}, &err),
          "and one with two owners");

    check(run(*s, "chain.delete", {{"id", 5}}), "delete");
    History h(*s);
    check(h.undo().ok, "undo");
    SQLite::Statement st(s->db(), "SELECT track_id, name FROM device_chains WHERE id = 5");
    check(st.executeStep() && st.getColumn(0).getInt64() == 1
          && st.getColumn(1).getString() == "FX", "owner and name restored");
}

void testChainSubtreeInverse() {
    section("populated chain delete restores the complete causal image");
    Scratch sc("chain_subtree");
    auto s = project(sc / "p.adi");
    if (!s || !seedChainAndDevice(*s)) { check(false, "seed chain subtree"); return; }
    auto& db = s->db();
    db.exec(
        "UPDATE devices SET is_rack=1,rack_kind='audio_effect' WHERE id=9;"
        "INSERT INTO device_chains(id,parent_device_id,ord,name,key_zone,muted) "
        "VALUES(6,9,3,'Inner',X'00ff',1);"
        "INSERT INTO devices(id,chain_id,name,is_rack) VALUES(10,6,'Nested rack',1);"
        "INSERT INTO device_chains(id,parent_device_id,ord,name,vel_zone,soloed) "
        "VALUES(7,10,5,'Leaf chain',X'',1);"
        "INSERT INTO device_chains(id,track_id,name) VALUES(8,1,'Survivor');"
        "INSERT INTO devices(id,chain_id,name) VALUES(11,7,'Doomed leaf'),(12,8,'Surviving leaf');"
        "INSERT INTO plugin_params(device_id,param_id,display) VALUES(11,'gain','3 dB');"
        "INSERT INTO device_expression_routes(device_id,route) VALUES(11,'mpe_midi');"
        "INSERT INTO device_panels(device_id) VALUES(11);"
        "INSERT INTO device_panel_params(device_id,ord,param_id) VALUES(11,4,'gain');"
        "INSERT INTO macros(id,device_id,ord,name) VALUES(25,10,0,'Internal'),(26,9,0,'Outer');"
        "INSERT INTO macro_mappings(id,macro_id,target_device_id,target_param_ref) "
        "VALUES(35,25,11,'gain'),(36,26,11,'gain'),(37,25,12,'gain');"
        "INSERT INTO routing(id,src_kind,src_id,dst_kind,dst_id,kind,channel_map) "
        "VALUES(46,'track',1,'device',11,'sidechain',X''),"
        "(47,'hw_in',11,'device',12,'sidechain',NULL);"
        "INSERT INTO routing_key_taps(routing_id,tap) VALUES(46,'post_fx'),(47,'pre_fx');"
        "INSERT INTO automation_lanes(id,owner_kind,owner_id,param_ref) VALUES(55,'device',11,'gain');"
        "INSERT INTO automation_data(id,lane_id,data) VALUES(56,55,X'0011');"
        "INSERT INTO controller_maps(id,target_kind,target_id,target_param) VALUES(65,'device',11,'gain');"
        "CREATE TABLE rack_watch(id INTEGER PRIMARY KEY, device_id INTEGER "
        "REFERENCES devices(id) ON DELETE SET NULL, name TEXT NOT NULL) STRICT;"
        "INSERT INTO rack_watch VALUES(66,11,'Watch');");
    SQLite::Statement state(db, "INSERT INTO plugin_state(device_id,stream_role,state_hash) VALUES(11,'chunk',?)");
    state.bind(1, kHash);
    state.exec();

    std::string ownerError;
    check(!run(*s, "chain.create", {{"id", 90}, {"device", 12}}, &ownerError) &&
              ownerError.find("rack device") != std::string::npos,
          "an ordinary effect cannot own new rack chains");

    const auto before = digestProject(*s).text;
    if (!run(*s, "chain.delete", {{"id", 6}})) { check(false, "delete populated chain"); return; }
    const auto after = digestProject(*s).text;
    check(db.execAndGet("SELECT count(*) FROM devices WHERE id IN (10,11)").getInt() == 0 &&
          db.execAndGet("SELECT count(*) FROM macro_mappings").getInt() == 0,
          "nested devices and all causal macro links are removed");
    check(db.execAndGet("SELECT count(*) FROM routing WHERE id=46").getInt() == 0 &&
          db.execAndGet("SELECT count(*) FROM routing_key_taps WHERE routing_id=46").getInt() == 0 &&
          db.execAndGet("SELECT count(*) FROM automation_lanes").getInt() == 0 &&
          db.execAndGet("SELECT count(*) FROM controller_maps").getInt() == 0,
          "polymorphic route, its tap, automation and controllers are removed");
    check(db.execAndGet("SELECT count(*) FROM routing WHERE id=47").getInt() == 1 &&
          db.execAndGet("SELECT count(*) FROM state_blobs").getInt() == 2 &&
          db.execAndGet("SELECT device_id IS NULL FROM rack_watch WHERE id=66").getInt() == 1,
          "unrelated hardware endpoint and pooled state survive while SET NULL is applied");

    History history(*s);
    db.exec("UPDATE rack_watch SET name='Changed' WHERE id=66");
    const auto changed = digestProject(*s).text;
    const auto rejected = history.undo();
    check(!rejected.ok && digestProject(*s).text == changed &&
          db.execAndGet("SELECT name FROM rack_watch WHERE id=66").getString() == "Changed" &&
          db.execAndGet("SELECT count(*) FROM device_chains WHERE id=6").getInt() == 0,
          "Undo refuses a changed surviving row atomically");
    if (rejected.ok && !history.redo().ok) { check(false, "reset changed survivor fixture"); return; }
    db.exec("UPDATE rack_watch SET name='Watch' WHERE id=66");
    check(history.undo().ok && digestProject(*s).text == before &&
          db.execAndGet("SELECT device_id FROM rack_watch WHERE id=66").getInt() == 11 &&
          db.execAndGet("SELECT typeof(vel_zone)='blob' AND length(vel_zone)=0 FROM device_chains WHERE id=7").getInt() == 1,
          "Undo restores the exact image, the surviving link and an empty BLOB");
    db.exec("INSERT INTO devices(id,chain_id,name) VALUES(71,7,'Unlogged child')");
    const auto amended = digestProject(*s).text;
    const auto badRedo = history.redo();
    check(!badRedo.ok && digestProject(*s).text == amended &&
              db.execAndGet("SELECT count(*) FROM devices WHERE id=71").getInt() == 1,
          "typed Redo refuses an added descendant before deleting it");
    db.exec("DELETE FROM devices WHERE id=71");
    check(history.redo().ok && digestProject(*s).text == after, "Redo restores the exact deletion");
    SQLite::Statement foreignKeys(db, "PRAGMA foreign_key_check");
    check(!foreignKeys.executeStep() && db.execAndGet("PRAGMA defer_foreign_keys").getInt() == 0,
          "replay leaves valid foreign keys and restores the deferral flag");
}

void testRackInverseBudget() {
    section("rack and chain inverses enforce the exact byte budget before applying");
    Scratch sc("rack_budget");
    auto s = project(sc / "p.adi");
    if (!s || !seedChainAndDevice(*s)) { check(false, "seed rack budget"); return; }
    auto& db = s->db();
    db.exec("UPDATE devices SET is_rack=1 WHERE id=9;"
            "INSERT INTO device_chains(id,parent_device_id) VALUES(6,9);"
            "INSERT INTO devices(id,chain_id) VALUES(10,6);"
            "CREATE TABLE rack_capture(id INTEGER PRIMARY KEY, device_id INTEGER NOT NULL "
            "REFERENCES devices(id) ON DELETE CASCADE, data BLOB NOT NULL) STRICT;"
            "INSERT INTO rack_capture VALUES(11,10,X'')");
    const auto resize = [&](std::size_t size) {
        SQLite::Statement update(db, "UPDATE rack_capture SET data=zeroblob(?) WHERE id=11");
        update.bind(1, static_cast<std::int64_t>(size));
        update.exec();
    };
    constexpr auto limit = detail::kRackUndoLimitBytes;
    constexpr auto probeSize = limit - 8192;
    const auto packet = [&] {
        return encodePayload({{"id", 9}, {"chain", 5},
                              {"contents", detail::captureDevice(db, 9)}});
    };
    resize(probeSize);
    const auto overhead = packet().size() - probeSize;
    check(overhead < 8192, "fixture calibrates a single large cell and its exact framing");
    if (overhead >= 8192) return;
    OpJournal journal(*s);
    History history(*s);
    OpRequest remove;
    remove.opType = "device.remove";
    remove.payload = {{"id", 9}};
    std::int64_t lastRemoval = 0;
    for (const auto expected : {limit - 1, limit}) {
        const auto size = expected - overhead;
        resize(size);
        const auto before = digestProject(*s).text;
        const auto result = journal.commit(remove);
        check(result.ok && result.seqs.size() == 1, "an inverse within the byte limit commits");
        if (!result.ok || result.seqs.size() != 1) return;
        lastRemoval = result.seqs.front();
        {
            SQLite::Statement measure(db, "SELECT length(inverse),tags FROM ops WHERE seq=?");
            measure.bind(1, lastRemoval);
            check(measure.executeStep() && measure.getColumn(0).getInt64() == static_cast<std::int64_t>(expected) &&
                      measure.getColumn(1).getString().empty(), "the journal stores the exact byte count as an ordinary Undo");
        }
        check(history.undo().ok && digestProject(*s).text == before &&
                  db.execAndGet("SELECT length(data) FROM rack_capture").getInt64() == static_cast<std::int64_t>(size),
              "a near-limit inverse restores the entire cell");
    }

    resize(limit + 1 - overhead);
    const auto oversized = packet();
    check(oversized.size() == limit + 1, "the refusal fixture is exactly one byte above the cap");
    OpRequest rename;
    rename.opType = "track.rename";
    rename.payload = {{"id", 1}, {"name", "Must roll back"}};
    const std::array requests{rename, remove};
    const auto before = digestProject(*s).text;
    const auto head = journal.headSeq();
    const auto count = journal.count();
    const auto refused = journal.commit(requests);
    check(!refused.ok && refused.issues.size() == 1 && refused.issues.front().key == "lossy_undo" &&
              journal.headSeq() == head && journal.count() == count && digestProject(*s).text == before,
          "one byte over the cap reports lossy_undo and rolls back the whole edit");

    SQLite::Statement legacy(db, "UPDATE ops SET inverse=? WHERE seq=?");
    legacy.bind(1, oversized.data(), static_cast<int>(oversized.size()));
    legacy.bind(2, lastRemoval);
    legacy.exec();
    check(history.redo().ok && history.undo().ok && digestProject(*s).text == before,
          "an already recorded oversized inverse still replays exactly");

    resize(limit + 512);
    OpRequest chain;
    chain.opType = "chain.delete";
    chain.payload = {{"id", 6}};
    const auto chainResult = journal.commit(chain);
    check(!chainResult.ok && !chainResult.issues.empty() && chainResult.issues.front().key == "lossy_undo" &&
              db.execAndGet("SELECT length(data) FROM rack_capture").getInt64() == static_cast<std::int64_t>(limit + 512),
          "chain deletion refuses an oversized child cell before copying it into its inverse");
    db.exec("UPDATE devices SET is_rack=0 WHERE id=9");
    const auto legacyOwner = journal.commit(remove);
    check(!legacyOwner.ok && !legacyOwner.issues.empty() && legacyOwner.issues.front().key == "lossy_undo",
          "a legacy ordinary device owning a subtree uses the same bounded inverse");

    resize(limit / 4);
    db.exec("INSERT INTO rack_capture VALUES(12,10,zeroblob(1048576)),(13,10,zeroblob(1048576)),"
            "(14,10,zeroblob(1048576)),(15,10,zeroblob(1048576))");
    const auto cumulative = journal.commit(chain);
    check(!cumulative.ok && !cumulative.issues.empty() && cumulative.issues.front().key == "lossy_undo" &&
              db.execAndGet("SELECT count(*) FROM rack_capture").getInt() == 5,
          "individually small captured rows also count toward the same inverse budget");

    db.exec("DELETE FROM rack_capture");
    const std::vector<std::byte> opaque(limit + 512, std::byte{0x47});
    const auto hash = media::blake3Bytes(opaque).hex;
    {
        SQLite::Statement blob(db, "INSERT INTO state_blobs(hash_blake3,data,size_bytes) VALUES(?,?,?)");
        blob.bind(1, hash);
        blob.bindNoCopy(2, opaque.data(), static_cast<int>(opaque.size()));
        blob.bind(3, static_cast<std::int64_t>(opaque.size()));
        blob.exec();
        SQLite::Statement state(db, "INSERT INTO plugin_state(device_id,stream_role,state_hash) VALUES(10,'chunk',?)");
        state.bind(1, hash);
        state.exec();
    }
    const auto removed = journal.commit(chain);
    const auto pooled = s->getStateBlob(hash);
    check(removed.ok && pooled && pooled->size() == opaque.size() && history.undo().ok &&
              db.execAndGet("SELECT count(*) FROM plugin_state WHERE device_id=10").getInt() == 1,
          "large shared plug-in state remains pooled and its reference fits the budget");
}

bool installLegacyRedo(Store& store, const char* op, const Payload& forward, const Payload& reverse) {
    const auto forwardBytes = encodePayload(forward);
    const auto reverseBytes = encodePayload(reverse);
    SQLite::Statement replace(store.db(), "UPDATE ops SET op_type=?,payload=?,inverse=? "
                                          "WHERE seq=(SELECT MAX(seq) FROM ops)");
    replace.bind(1, op);
    replace.bind(2, forwardBytes.data(), static_cast<int>(forwardBytes.size()));
    replace.bind(3, reverseBytes.data(), static_cast<int>(reverseBytes.size()));
    return replace.exec() == 1;
}

void testLegacyChainReplay() {
    section("legacy chain delete replays its original direct cascade");
    Scratch sc("legacy_chain");
    auto s = project(sc / "p.adi");
    if (!s || !seedChainAndDevice(*s)) { check(false, "seed legacy chain"); return; }
    auto& db = s->db();
    db.exec("INSERT INTO routing(id,src_kind,src_id,dst_kind,dst_id,kind) "
            "VALUES(19,'track',1,'device',9,'sidechain');"
            "INSERT INTO routing_key_taps VALUES(19,'pre_fx');"
            "UPDATE device_chains SET muted=1,key_zone=X'00' WHERE id=5;");
    History history(*s);
    if (!run(*s, "track.rename", {{"id", 1}, {"name", "Bass"}}) || !history.undo().ok) {
        check(false, "seed legacy redo transaction");
        return;
    }

    // Put recorded legacy CBOR on the redo branch without running today's
    // chain.delete. The material still holds the child and its key route.
    check(installLegacyRedo(*s, "chain.delete", {{"id", 5}},
                            {{"id", 5}, {"track", 1}, {"device", nullptr}, {"ord", 0}, {"name", "FX"}}),
          "install the literal legacy forward and inverse");
    check(history.redo().ok &&
          db.execAndGet("SELECT count(*) FROM devices WHERE id=9").getInt() == 0 &&
          db.execAndGet("SELECT count(*) FROM routing_key_taps WHERE routing_id=19").getInt() == 1,
          "legacy Redo deletes the device and historically retains the dangling tap");
    const auto after = digestProject(*s).text;
    check(history.undo().ok &&
          db.execAndGet("SELECT muted=0 AND key_zone IS NULL FROM device_chains WHERE id=5").getInt() == 1 &&
          db.execAndGet("SELECT count(*) FROM devices WHERE id=9").getInt() == 0,
          "legacy Undo restores only the old flat chain with default fields");
    check(history.redo().ok && digestProject(*s).text == after, "repeated legacy Redo stays exact");

    if (!run(*s, "chain.create", {{"id", 21}, {"track", 1}})) {
        check(false, "seed paired creation inverse");
        return;
    }
    db.exec("INSERT INTO devices(id,chain_id) VALUES(22,21);"
            "INSERT INTO routing(id,src_kind,src_id,dst_kind,dst_id,kind) "
            "VALUES(23,'track',1,'device',22,'sidechain');");
    check(history.undo().ok &&
          db.execAndGet("SELECT count(*) FROM devices WHERE id=22").getInt() == 0 &&
          db.execAndGet("SELECT count(*) FROM routing WHERE id=23").getInt() == 1,
          "the recorded inverse of flat chain creation keeps its original cascade");
}

void testDeviceMoveAncestry() {
    section("device.move refuses a cycle without publishing a partial edit");
    Scratch sc("rack_cycle");
    auto s = project(sc / "p.adi");
    if (!s || !seedChainAndDevice(*s)) { check(false, "seed rack move"); return; }
    auto& db = s->db();
    db.exec(
        "UPDATE devices SET is_rack=1,rack_kind='audio_effect' WHERE id=9;"
        "INSERT INTO device_chains(id,parent_device_id) VALUES(6,9);"
        "INSERT INTO devices(id,chain_id,is_rack) VALUES(10,6,1);"
        "INSERT INTO device_chains(id,parent_device_id) VALUES(7,10);"
        "INSERT INTO device_chains(id,track_id) VALUES(8,1);"
        "INSERT INTO devices(id,chain_id,is_rack) VALUES(11,8,1);"
        "INSERT INTO device_chains(id,parent_device_id) VALUES(12,11);"
        "UPDATE devices SET chain_id=12 WHERE id=11;");
    OpJournal journal(*s);
    const auto before = digestProject(*s).text;
    const auto head = journal.headSeq();
    std::string error;
    const bool directlyMoved = run(*s, "device.move", {{"id", 9}, {"chain", 6}, {"ord", 9}}, &error);
    check(!directlyMoved && error.find("own rack") != std::string::npos, "direct self-nest refuses");
    if (directlyMoved && !History(*s).undo().ok) { check(false, "reset direct move fixture"); return; }
    const auto rows = db.execAndGet("SELECT count(*) FROM ops").getInt();
    OpRequest rename;
    rename.opType = "track.rename";
    rename.payload = {{"id", 1}, {"name", "Partial edit"}};
    OpRequest bad;
    bad.opType = "device.move";
    bad.payload = {{"id", 9}, {"chain", 7}, {"ord", 9}};
    const std::vector<OpRequest> requests{rename,bad};
    const auto batch = journal.commit(requests);
    check(!batch.ok && journal.headSeq() == head &&
          db.execAndGet("SELECT count(*) FROM ops").getInt() == rows && digestProject(*s).text == before,
          "grandchild move rolls back the whole batch and its log");
    if (batch.ok && !History(*s).undo().ok) { check(false, "reset grandchild move fixture"); return; }

    const bool movedIntoCycle = run(*s, "device.move", {{"id", 9}, {"chain", 12}, {"ord", 9}}, &error);
    check(!movedIntoCycle && error.find("cycle") != std::string::npos, "existing destination owner cycle refuses");
    if (movedIntoCycle && !History(*s).undo().ok) { check(false, "reset ownership cycle fixture"); return; }
    check(run(*s, "device.move", {{"id", 10}, {"chain", 8}, {"ord", 9}}, &error) &&
          History(*s).undo().ok && digestProject(*s).text == before,
          "moving a nested rack to an unrelated chain remains exactly undoable");
}

void testLegacyDeviceMoveReplay() {
    section("old cyclic device moves remain replayable");
    Scratch sc("legacy_device_move");
    auto s = project(sc / "p.adi");
    if (!s || !seedChainAndDevice(*s)) { check(false, "seed legacy rack move"); return; }
    auto& db = s->db();
    db.exec("UPDATE devices SET is_rack=1 WHERE id=9;"
            "INSERT INTO device_chains(id,parent_device_id) VALUES(6,9);"
            "INSERT INTO devices(id,chain_id,is_rack) VALUES(10,6,1);"
            "INSERT INTO device_chains(id,parent_device_id) VALUES(7,10);");
    History history(*s);
    if (!run(*s, "track.rename", {{"id", 1}, {"name", "Bass"}}) || !history.undo().ok) {
        check(false, "seed legacy move redo transaction");
        return;
    }
    const auto before = digestProject(*s).text;
    check(installLegacyRedo(*s, "device.move", {{"id", 9}, {"chain", 7}, {"ord", 9}},
                            {{"id", 9}, {"chain", 5}, {"ord", 2}}),
          "install the legacy self-nest move recorded by the old handler");
    check(history.redo().ok &&
          db.execAndGet("SELECT chain_id=7 AND ord=9 FROM devices WHERE id=9").getInt() == 1,
          "legacy Redo recovers the recorded cyclic state");
    const auto cyclic = digestProject(*s).text;

    const auto fixed = run(*s, "device.move", {{"id", 9}, {"chain", 5}, {"ord", 2}});
    check(fixed && digestProject(*s).text == before, "a new move can repair an old cycle");
    if (!fixed) return;
    check(history.undo().ok && digestProject(*s).text == cyclic,
          "undoing the repair restores the exact previously recorded state");
    check(history.undo().ok && digestProject(*s).text == before,
          "undoing the historical move restores the original valid rack");
}

}  // namespace

std::optional<std::string> readRoute(Store& s, std::int64_t dev) {
    SQLite::Statement st(s.db(), "SELECT route FROM device_expression_routes WHERE device_id = ?");
    st.bind(1, dev);
    if (!st.executeStep()) return std::nullopt;
    return st.getColumn(0).getString();
}

void testTheExpressionRoute() {
    section("ADR-0146, ADR-0149: the route a device plays with is an op, and travels with the device");
    Scratch sc("route");
    auto s = project(sc / "p.adi");
    if (!s) { check(false, "project opens"); return; }
    check(seedChainAndDevice(*s), "seeded");
    check(!readRoute(*s, 9).has_value(), "no row: Auto");

    check(run(*s, "device.setExpressionRoute", {{"dev", 9}, {"route", "mpe_midi"}}), "choose MPE over MIDI");
    check(readRoute(*s, 9) == std::optional<std::string>("mpe_midi"), "the row says so");
    check(run(*s, "device.setExpressionRoute", {{"dev", 9}, {"route", "plain"}}), "then plain");
    History h(*s);
    check(h.undo().ok, "undo");
    check(readRoute(*s, 9) == std::optional<std::string>("mpe_midi"), "back to MPE over MIDI");
    check(h.undo().ok, "undo again");
    check(!readRoute(*s, 9).has_value(), "and back to Auto: the row is gone, not set to something");

    std::string err;
    check(!run(*s, "device.setExpressionRoute", {{"dev", 9}, {"route", "sideways"}}, &err),
          "a route the format does not know is refused: " + err);
    check(!run(*s, "device.setExpressionRoute", {{"dev", 9}}, &err),
          "a missing route key is loud, not Auto: " + err);

    // It travels with the device: removal and its undo, and an insert that
    // carries the registry's proposal.
    check(run(*s, "device.setExpressionRoute", {{"dev", 9}, {"route", "note_expression"}}), "a route again");
    check(run(*s, "device.remove", {{"id", 9}}), "remove the device");
    check(!readRoute(*s, 9).has_value(), "the route cascaded with it");
    History h2(*s);
    check(h2.undo().ok, "undo the removal");
    check(readRoute(*s, 9) == std::optional<std::string>("note_expression"),
          "and the route came back with the device");
    check(run(*s, "device.insert", {{"id", 10}, {"chain", 5}, {"ord", 3}, {"name", "Synth"},
                                    {"route", "mpe_midi"}}),
          "a new device inserted with the registry's route in its payload");
    check(readRoute(*s, 10) == std::optional<std::string>("mpe_midi"), "is inserted on that route");
}


std::optional<std::vector<std::string>> readPanel(Store& s, std::int64_t dev) {
    SQLite::Statement has(s.db(), "SELECT 1 FROM device_panels WHERE device_id = ?");
    has.bind(1, dev);
    if (!has.executeStep()) return std::nullopt;
    std::vector<std::string> out;
    SQLite::Statement st(s.db(), "SELECT param_id FROM device_panel_params WHERE device_id = ? ORDER BY ord");
    st.bind(1, dev);
    while (st.executeStep()) out.push_back(st.getColumn(0).getString());
    return out;
}

void testThePanel() {
    section("ADR-0154: the plug-in panel is one op, the whole list, and travels with the device");
    Scratch sc("panel");
    auto s = project(sc / "p.adi");
    if (!s) { check(false, "project opens"); return; }
    check(seedChainAndDevice(*s), "seeded");
    check(!readPanel(*s, 9).has_value(), "no row: Live's default panel");

    check(run(*s, "device.setPanel", {{"dev", 9}, {"params", {"cutoff", "res"}}}), "configure two");
    check(readPanel(*s, 9) == std::optional<std::vector<std::string>>({"cutoff", "res"}), "saved, in order");
    check(run(*s, "device.setPanel", {{"dev", 9}, {"params", {"res", "cutoff", "drive"}}}),
          "reorder and add: the list is now three");
    check(run(*s, "device.setPanel", {{"dev", 9}, {"params", nlohmann::json::array()}}), "empty it");
    check(readPanel(*s, 9) == std::optional<std::vector<std::string>>(std::vector<std::string>{}),
          "a configured, empty panel -- not the default");
    History h(*s);
    check(h.undo().ok, "undo");
    check(readPanel(*s, 9) == std::optional<std::vector<std::string>>({"res", "cutoff", "drive"}), "the three came back, in their order");
    check(h.undo().ok && h.undo().ok, "undo twice more");
    check(!readPanel(*s, 9).has_value(), "and back to the default: the row is gone");

    std::string err;
    check(!run(*s, "device.setPanel", {{"dev", 9}}, &err), "a missing params key is loud: " + err);
    check(!run(*s, "device.setPanel", {{"dev", 9}, {"params", {"a", "a"}}}, &err),
          "a parameter twice is refused: " + err);

    check(run(*s, "device.setPanel", {{"dev", 9}, {"params", {"cutoff"}}}), "a panel again");
    check(run(*s, "device.remove", {{"id", 9}}), "remove the device");
    check(!readPanel(*s, 9).has_value(), "the panel cascaded with it");
    History h2(*s);
    check(h2.undo().ok, "undo the removal");
    check(readPanel(*s, 9) == std::optional<std::vector<std::string>>({"cutoff"}), "and the panel came back with it");
    check(run(*s, "device.insert", {{"id", 11}, {"chain", 5}, {"ord", 4}, {"name", "Configured"},
                                    {"panel", {"x", "y"}}}),
          "a device inserted with a panel, as a preset of a configured device would be");
    check(readPanel(*s, 11) == std::optional<std::vector<std::string>>({"x", "y"}), "arrives configured");
}

void testInsertRejectsVca() {
    section("device.insert rejects a VCA but preserves legacy history");
    Scratch temp("vca-device");
    auto s = project(temp / "vca.adi");
    if (!s) {
        check(false, "create VCA fixture");
        return;
    }
    auto& db = s->db();
    db.exec("INSERT INTO tracks(id,kind) VALUES (1,'audio'),(2,'vca');"
            "INSERT INTO device_chains(id,track_id) VALUES (5,1),(6,2);"
            "INSERT INTO devices(id,chain_id,is_rack) VALUES (9,6,1);"
            "INSERT INTO device_chains(id,parent_device_id) VALUES (7,9);"
            "INSERT INTO devices(id,chain_id,name) VALUES (11,7,'Saved device');"
            "INSERT INTO plugin_params(device_id,param_id,normalized_value) VALUES (11,'mix',0.42);");
    const auto contents = detail::captureDevice(db, 11);
    check(run(*s, "device.remove", {{"id",11}}), "remove a preexisting VCA device");
    History history(*s);
    check(history.undo().ok && detail::captureDevice(db, 11) == contents,
          "Undo restores the complete older VCA device and its parameter");
    check(history.redo().ok && countRows(*s, "SELECT count(*) FROM devices WHERE id=?", 11) == 0,
          "Redo can remove the older device again");

    const auto head = OpJournal(*s).headSeq();
    std::string error;
    check(!run(*s, "device.insert", {{"id",12},{"chain",6},{"ord",0}}, &error) &&
              error.find("VCA") != std::string::npos,
          "raw insertion on a top-level VCA chain refuses");
    error.clear();
    check(!run(*s, "device.insert", {{"id",13},{"chain",7},{"ord",0}}, &error) &&
              error.find("VCA") != std::string::npos,
          "raw insertion on a nested VCA rack chain refuses");
    error.clear();
    check(!run(*s, "device.insert", {{"id",11},{"chain",5},{"ord",0},{"contents",contents}}, &error) &&
              error.find("VCA") != std::string::npos,
          "a fresh captured insert cannot hide its actual VCA chain");
    check(OpJournal(*s).headSeq() == head &&
              db.execAndGet("SELECT count(*) FROM devices").getInt() == 1 &&
              db.execAndGet("SELECT count(*) FROM plugin_params").getInt() == 0,
          "refused inserts leave neither rows nor audit entries");
    check(run(*s, "device.insert", {{"id",14},{"chain",5},{"ord",0}}),
          "the same fresh op still inserts on an audio track");
}

void testChainZones() {
    section("DEV-02 -- exact chain zone bytes and admission");
    using namespace adi::rack;
    const std::array<std::byte, 4> code{std::byte{1}, std::byte{9}, std::byte{5}, std::byte{7}};
    const auto decoded = decodeZone(code);
    check(decoded == Zone{1, 9, 5, 7} && !decodeZone(std::span(code).first(3)),
          "zone decode preserves all four integers and rejects an unknown extent");
    const std::array gain{0.0, 0.25, 0.5, 0.75, 1.0};
    bool bothLaws = true;
    for (std::size_t i = 0; i < gain.size(); ++i) {
        const auto key = static_cast<int>(i) + 1;
        const auto linear = zoneWeight(decoded, key, FadeLaw::Gain);
        const auto power = zoneWeight(decoded, key, FadeLaw::Power);
        bothLaws = bothLaws && linear == gain[i] && power &&
                   std::abs(*power - std::sqrt(gain[i])) < 1e-12;
    }
    check(bothLaws && zoneWeight(decoded, 8, FadeLaw::Gain) == 0.5 &&
              zoneWeight(decoded, 9, FadeLaw::Power) == 0.0 &&
              !zoneWeight(decoded, 0, FadeLaw::Gain) && !zoneWeight(decoded, 10, FadeLaw::Gain),
          "integer gains and power law match both fade edges and do not admit outside notes");
    check(zoneWeight(Zone{64, 64, 64, 64}, 64, FadeLaw::Power) == 1.0,
          "equal bounds give one admitted key with zero fade width");

    Scratch scratch("rack-zones");
    auto store = project(scratch / "zones.adi");
    if (!store || !seedChainAndDevice(*store)) {
        check(false, "create native zone fixture");
        return;
    }
    store->db().exec(
        "UPDATE device_chains SET key_zone=X'01090507',chain_zone=X'00320028' WHERE id=5;"
        "INSERT INTO device_chains(id,track_id,ord,key_zone,vel_zone,chain_zone) "
        "VALUES(6,1,1,X'06640350',X'00800080',X'00140014');");
    const auto model = rows::readModel(*store);
    const auto id = [&](std::int64_t which) {
        return std::find_if(model.deviceChains.begin(), model.deviceChains.end(),
                            [&](const auto &chain) { return chain.id == which; });
    };
    const auto valid = id(5), malformed = id(6);
    check(valid != model.deviceChains.end() && malformed != model.deviceChains.end(),
          "a malformed rack chain does not truncate its neighbors in the model");
    if (valid == model.deviceChains.end() || malformed == model.deviceChains.end())
        return;
    check(valid->zones.key == decoded && valid->zones.velocity == Zone{} &&
              valid->zones.selector == Zone{0, 50, 0, 40},
          "stored SQL NULL means a full velocity range alongside both authored zones");
    const auto ordinary = evaluateZones(valid->zones, RackKind::Instrument, 3, 127, 45, FadeLaw::Gain);
    const auto midi = evaluateZones(valid->zones, RackKind::MidiEffect, 3, 127, 45, FadeLaw::Power);
    check(ordinary && ordinary->key == 0.5 && ordinary->velocity == 1 &&
              ordinary->selector == 0.5 && ordinary->combined() == 0.25 &&
              midi && std::abs(midi->combined() - 0.5) < 1e-12,
          "instrument and MIDI effect racks require all three applicable factors");
    check(!malformed->zones.key && !malformed->zones.velocity &&
              !evaluateZones(malformed->zones, RackKind::Instrument, 4, 5, 6, FadeLaw::Gain) &&
              std::any_of(model.problems.begin(), model.problems.end(), [](const auto &s) {
                  return s == "device_chains#6: invalid key zone";
              }),
          "malformed rows are diagnosed and block applicable note admission");
    const auto audio = evaluateZones(malformed->zones, RackKind::AudioEffect, -1, -1, 10, FadeLaw::Gain);
    const auto drum = evaluateZones(malformed->zones, RackKind::Drum, -1, -1, -1, FadeLaw::Gain);
    check(audio && audio->combined() == 1.0 && drum && drum->combined() == 1.0,
          "audio racks only consult selector and drum admission uses its separate pad mapping");
}

void testDrumPadRouting() {
    section("DEV-04 -- native Drum Rack Receive and Play mapping");
    using namespace adi::rack;
    using Status = DrumPadMap::Status;
    const std::array<DrumChainMapping, 6> mappings{{
        {0, {}, {}, {}}, {11, 36, 48, 1}, {12, 37, 127, 16},
        {13, {}, 1, {}}, {14, 36, 0, 1}, {15, 127, 42, {}}}};
    std::string error;
    const auto map = DrumPadMap::compile(mappings, error);
    check(map.has_value() && error.empty(), "owner compiles physical pads and All Notes chains");
    if (!map)
        return;
    std::array<DrumDestination, 6> output{};
    const auto was = output;
    const auto shortBuffer = map->route(36, std::span(output).first(3));
    check(shortBuffer.status == Status::InsufficientSpace && shortBuffer.size == 4 &&
              output == was, "short space leaves an entire multi-pad fan-out unwritten");
    DrumPadMap::Result first;
    {
        audio::CallbackScope callback;
        first = map->route(36, output);
    }
    const std::array<DrumDestination, 4> expected{{
        {0, 36, {}}, {11, 48, 1}, {13, 1, {}}, {14, 0, 1}}};
    check(first.status == Status::Ready && first.size == expected.size() &&
              std::equal(expected.begin(), expected.end(), output.begin()),
          "Multi pads and All Notes chains keep row order, explicit pitch and choke tags");
    const auto top = map->route(127, output);
    check(top.status == Status::Ready && top.size == 3 && output[0] == DrumDestination{0, 127, {}} &&
              output[2] == DrumDestination{15, 42, {}},
          "the highest MIDI key can be routed and translated");
    const auto lowest = map->route(0, output);
    check(lowest.status == Status::Ready && lowest.size == 2 &&
              output[0] == DrumDestination{0, 0, {}} && output[1] == DrumDestination{13, 1, {}},
          "an unoccupied pad still reaches the All Notes chains");
    check(map->route(-1, output).status == Status::InvalidKey &&
              map->route(128, output).status == Status::InvalidKey,
          "keys outside 0 to 127 receive no routes");

    auto swapped = mappings;
    std::swap(swapped[1].receive, swapped[2].receive);
    const auto remapped = DrumPadMap::compile(swapped, error);
    const auto after = remapped ? remapped->route(36, output) : DrumPadMap::Result{};
    check(remapped && after.size == 4 && output[1] == DrumDestination{12, 127, 16},
          "a pad swap changes only where the original played key routes");
    check(map->route(36, output).size == 4 && output[1] == DrumDestination{11, 48, 1},
          "the held routing generation keeps its original immutable assignments");

    swapped[1].chain = swapped[2].chain;
    check(!DrumPadMap::compile(swapped, error) && !error.empty(),
          "duplicate chain identity refuses all new mappings");
    swapped = mappings;
    swapped[2].choke = 17;
    check(!DrumPadMap::compile(swapped, error) && !error.empty(),
          "a choke group above sixteen refuses all new mappings");
    swapped = mappings;
    swapped[2].play = 128;
    check(!DrumPadMap::compile(swapped, error) && !error.empty(),
          "a played pitch outside the MIDI range is refused");
    bool rejected = false;
    {
        audio::CallbackScope callback;
        rejected = !DrumPadMap::compile(mappings, error);
    }
    check(rejected && error == "callback", "the audio driver can route through a prepared map only");
}

// --- main ---------------------------------------------------------------------

int main() {
    // Unbuffered: a crashing test binary loses its whole block-buffered stdout
    // on Windows, and the harness then prints a blank line where a failure
    // should be -- which is how adi_device_tests' segfault read as a harness
    // glitch, twice.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("adi_device_ops_tests -- OPS.md 9.7, ADR-0057\n\n");
    testAllNineExist();
    testFirstTouchUndoesToAbsence();
    testLaterTouchUndoesToThePreviousValue();
    testNullNormClearsAndRestores();
    testMissingNormIsLoud();
    testRealValueStaysNull();
    testOpaqueState();
    testRemoveRestoresEverything();
    testRemoveNestedRack();
    testMissingPluginSurvivesTheRoundTrip();
    testMoveAndScalars();
    testChainPair();
    testChainSubtreeInverse();
    testRackInverseBudget();
    testLegacyChainReplay();
    testDeviceMoveAncestry();
    testLegacyDeviceMoveReplay();
    testTheExpressionRoute();
    testThePanel();
    testInsertRejectsVca();
    testChainZones();
    testDrumPadRouting();
    std::printf("\n%s -- %d checks, %d failure(s)\n",
                g_failures ? "FAILED" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
