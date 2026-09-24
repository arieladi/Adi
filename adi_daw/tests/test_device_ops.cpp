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

#include "adi/history.hpp"
#include "adi/ops.hpp"
#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

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
    auto st = Store::create(p, err);
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
    testMissingPluginSurvivesTheRoundTrip();
    testMoveAndScalars();
    testChainPair();
    testTheExpressionRoute();
    std::printf("\n%s -- %d checks, %d failure(s)\n",
                g_failures ? "FAILED" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
