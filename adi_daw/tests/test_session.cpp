// SPDX-License-Identifier: GPL-3.0-or-later
//
// The session, end to end: a real .adi written through the op registry, read
// back, its devices resolved through an injected loader, played through the
// BlockProcessor contract at every block size the engine supports, edited and
// refreshed. ADR-0122.
//
// No JUCE. The loader here hands back a fake that halves its input and counts
// what the session does to it, which is what lets these checks say "the same
// instance, prepared again, never reloaded" as numbers rather than as a
// sentence. The JUCE half -- a driver callback reaching `Session::process`,
// a real VST3 behind the loader -- is `adi_play`, and it is checked by ear.

#include "temp_directory.hpp"

#include "adi/engine/session.hpp"
#include "adi/ops.hpp"
#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace adi;
using namespace adi::engine;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool cond, const std::string& what) {
    ++g_checks;
    if (!cond) { ++g_failures; std::printf("  FAIL  %s\n", what.c_str()); }
}

void eqi(long long got, long long want, const std::string& what) {
    ++g_checks;
    if (got != want) {
        ++g_failures;
        std::printf("  FAIL  %s\n          got %lld, want %lld\n", what.c_str(), got, want);
    }
}

void section(const char* s) { std::printf("[%s]\n", s); }

// ---------------------------------------------------------------------------
// The fake plugin, and the loader that hands it out
// ---------------------------------------------------------------------------

/// Halves its input. Counts every call the session makes on it, so "not
/// reloaded" and "prepared again at the new size" are assertable.
class HalfDevice final : public device::DeviceInstance {
public:
    explicit HalfDevice(device::DeviceIdentity id) : id_(std::move(id)) {}

    [[nodiscard]] const device::DeviceIdentity& identity() const noexcept override { return id_; }
    [[nodiscard]] bool loaded() const noexcept override { return true; }
    [[nodiscard]] std::int64_t tailSamples() const noexcept override { return 0; }

    void prepare(double sampleRate, std::int32_t maxFrames) override {
        ++prepares;
        lastRate = sampleRate;
        lastFrames = maxFrames;
        prepared = true;
    }
    void release() override {
        ++releases;
        prepared = false;
    }
    void process(const NodeIo& io) noexcept override {
        if (!prepared) ++unpreparedBlocks;
        for (std::int32_t c = 0; c < io.channels; ++c) {
            float* o = io.out[c] + io.blockOffset;
            const float* i = (io.in != nullptr && io.in[c] != nullptr)
                                 ? io.in[c] + io.blockOffset : nullptr;
            for (std::int32_t k = 0; k < io.frames; ++k)
                o[k] = (i != nullptr ? i[k] : 0.0f) * 0.5f;
        }
    }
    bool loadState(const std::string& role, const std::vector<std::uint8_t>& b) override {
        statesSeen.push_back(role);
        lastState = b;
        return role == "chunk";   // a plugin that knows one role
    }
    bool setParam(const std::string& paramId, const device::ParamValue&) override {
        paramsSet.push_back(paramId);
        return true;
    }

    int prepares = 0;
    int releases = 0;
    int unpreparedBlocks = 0;
    double lastRate = 0.0;
    std::int32_t lastFrames = 0;
    bool prepared = false;
    std::vector<std::string> statesSeen;
    std::vector<std::uint8_t> lastState;
    std::vector<std::string> paramsSet;

private:
    device::DeviceIdentity id_;
};

struct Loader {
    int calls = 0;
    std::vector<HalfDevice*> made;
    std::vector<std::string> asked;

    DeviceLoader fn() {
        return [this](const DeviceRequest& rq, std::string& err)
                   -> std::unique_ptr<device::DeviceInstance> {
            ++calls;
            asked.push_back(rq.ref != nullptr ? rq.ref->uid : "<no ref>");
            if (rq.ref != nullptr && rq.ref->uid == "adi.test.half") {
                device::DeviceIdentity id;
                id.format = rq.ref->format;
                id.uid = rq.ref->uid;
                id.name = rq.ref->name;
                auto d = std::make_unique<HalfDevice>(id);
                made.push_back(d.get());
                return d;
            }
            err = "not installed on this machine";
            return nullptr;
        };
    }
};

class ToneNode final : public Node {
public:
    explicit ToneNode(float v) : v_(v) {}
    void process(const NodeIo& io) noexcept override {
        for (std::int32_t c = 0; c < io.channels; ++c) {
            float* o = io.out[c] + io.blockOffset;
            for (std::int32_t i = 0; i < io.frames; ++i) o[i] = v_;
        }
    }
    [[nodiscard]] const char* name() const noexcept override { return "tone"; }
private:
    float v_;
};

struct Out {
    std::vector<float> l, r;
    std::vector<float*> ptrs;
    explicit Out(std::int32_t n)
        : l(static_cast<std::size_t>(n), -7.0f), r(static_cast<std::size_t>(n), -7.0f) {
        ptrs = {l.data(), r.data()};
    }
    float first() const { return l.front(); }
    float last() const { return l.back(); }
};

AudioIo makeIo(Out& o, std::int32_t frames) {
    AudioIo io;
    io.out = o.ptrs.data();
    io.numOut = 2;
    io.frames = frames;
    return io;
}

// ---------------------------------------------------------------------------
// The project
// ---------------------------------------------------------------------------

bool commits(OpJournal& j, const OpRequest& r, const std::string& what) {
    const CommitResult res = j.commit(r);
    check(res.ok, what + ": " + res.error);
    return res.ok;
}

/// Three tracks, two chains, one rack with a nested chain, one plugin the
/// loader has and one it does not:
///
///   Keys (1):  Warm [half]  ->  Ghost [absent; params + state]
///   Bass (2):  Off  [half, disabled]  ->  Rack [rack] { Nested [half] }
///   Master (9)
std::unique_ptr<Store> makeProject(const fs::path& file) {
    StoreError e = StoreError::Ok;
    auto store = Store::create(file, e);
    check(store != nullptr, "created the project");
    if (!store) return nullptr;

    OpJournal j(*store);
    OpRequest r;
    r.opType = "track.create";
    r.payload = {{"id", 1}, {"kind", "midi"}, {"name", "Keys"}};
    commits(j, r, "Keys");
    r.payload = {{"id", 2}, {"kind", "audio"}, {"name", "Bass"}, {"index", 1}};
    commits(j, r, "Bass");
    r.payload = {{"id", 9}, {"kind", "master"}, {"name", "Master"}, {"index", 2}};
    commits(j, r, "Master");

    // plugin_refs and state_blobs have no op of their own (the former is a
    // normalisation table; the latter is content-addressed), so they are
    // written directly, as an importer would.
    try {
        store->db().exec(
            "INSERT INTO plugin_refs(id, format, uid, vendor, name, version, subtype) VALUES "
            "(1, 'vst3', 'adi.test.half',   'ADI', 'Half',   '1.0', 'effect'), "
            "(2, 'vst3', 'adi.test.absent', 'ADI', 'Absent', '1.0', 'effect')");
        SQLite::Statement st(store->db(),
            "INSERT INTO state_blobs(hash_blake3, data, size_bytes) VALUES (?, ?, ?)");
        const unsigned char bytes[4] = {1, 2, 3, 4};
        st.bind(1, "h1");
        st.bind(2, bytes, 4);
        st.bind(3, 4);
        st.exec();
    } catch (const std::exception& ex) {
        check(false, std::string("seeding plugin_refs/state_blobs: ") + ex.what());
        return nullptr;
    }

    r.opType = "chain.create";
    r.payload = {{"id", 10}, {"track", 1}};
    commits(j, r, "chain on Keys");
    r.payload = {{"id", 20}, {"track", 2}};
    commits(j, r, "chain on Bass");

    r.opType = "device.insert";
    r.payload = {{"id", 100}, {"chain", 10}, {"ord", 0}, {"ref", 1}, {"name", "Warm"}};
    commits(j, r, "Warm");
    r.payload = {{"id", 101}, {"chain", 10}, {"ord", 1}, {"ref", 2}, {"name", "Ghost"},
                 {"params", nlohmann::json::array({
                      {{"param", "cutoff"}, {"name", "Cutoff"}, {"norm", 0.25},
                       {"real", 440.0}, {"unit", "Hz"}}})},
                 {"state", nlohmann::json::array({{{"role", "chunk"}, {"hash", "h1"}}})}};
    commits(j, r, "Ghost");
    r.payload = {{"id", 200}, {"chain", 20}, {"ord", 5}, {"ref", 1}, {"name", "Off"},
                 {"enabled", false}};
    commits(j, r, "Off");
    r.payload = {{"id", 205}, {"chain", 20}, {"ord", 9}, {"name", "Rack"}, {"rack", true}};
    commits(j, r, "Rack");

    r.opType = "chain.create";
    r.payload = {{"id", 40}, {"device", 205}};
    commits(j, r, "the rack's chain");
    r.opType = "device.insert";
    r.payload = {{"id", 300}, {"chain", 40}, {"ord", 0}, {"ref", 1}, {"name", "Nested"}};
    commits(j, r, "Nested");
    return store;
}

bool mentions(const std::vector<std::string>& problems, const std::string& needle) {
    for (const std::string& p : problems)
        if (p.find(needle) != std::string::npos) return true;
    return false;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void testAProjectLoadsAndPlays() {
    const adi::test::TempDirectory scratch("session", "loads");
    section("ADR-0122 -- a .adi loads, its devices resolve, and audio reaches the master");
    auto store = makeProject(scratch.path() / "p.adi");
    if (!store) return;

    Loader loader;
    Session s;
    ToneNode tone(0.5f);
    s.setSourcesFor([&](std::int64_t id) {
        return id == 1 ? std::vector<Node*>{&tone} : std::vector<Node*>{};
    });

    SessionSpec spec;
    spec.sampleRate = 48000.0;
    spec.maxFrames = 512;
    check(s.load(*store, loader.fn(), spec), "load: " + s.error());
    check(s.loaded(), "the session reports loaded");
    check(s.model().problems.empty(), "every table read cleanly");

    // --- what was resolved --------------------------------------------------
    eqi(loader.calls, 3, "the loader was asked three times: Warm, Ghost, Off -- not the rack, not Nested");
    eqi(s.stats().loaded, 2, "two instances");
    eqi(s.stats().placeholders, 1, "one placeholder");
    eqi(s.stats().skipped, 2, "the rack and its nested device are skipped, and counted");
    eqi(static_cast<long long>(s.entryCount()), 3, "three entries");
    check(mentions(s.problems(), "devices#205 (Rack): a rack; not realised yet (ADR-0060)"),
          "the rack is named as not realised");
    check(mentions(s.problems(), "devices#300 (Nested): inside a rack's chain"),
          "the nested device is named too");
    check(mentions(s.problems(), "devices#101 (Ghost): kept as a placeholder (ADR-0011): "
                                 "not installed on this machine"),
          "the placeholder says why");

    // --- the chains follow the rows -----------------------------------------
    const std::vector<device::DeviceNode*> keys = s.chainFor(1);
    eqi(static_cast<long long>(keys.size()), 2, "Keys has two devices");
    check(keys.size() == 2 && keys[0] == s.nodeFor(100) && keys[1] == s.nodeFor(101),
          "in ord order: Warm then Ghost");
    const std::vector<device::DeviceNode*> bass = s.chainFor(2);
    eqi(static_cast<long long>(bass.size()), 1, "Bass has one realised device (the rack is not one)");
    check(bass.size() == 1 && bass[0] == s.nodeFor(200), "and it is Off");
    check(s.chainFor(9).empty(), "the master has none");
    check(s.chainFor(0).empty(), "track 0 is nothing");

    // --- the placeholder (ADR-0011, SPEC 7.1) --------------------------------
    device::DeviceInstance* ghost = s.instanceFor(101);
    check(ghost != nullptr && !ghost->loaded(), "Ghost is a placeholder");
    check(s.nodeFor(101) != nullptr && s.nodeFor(101)->bypassed(), "and its node is bypassed");
    if (ghost != nullptr) {
        eqi(ghost->paramCount(), 1, "it carries its one mirrored parameter");
        const device::ParamValue v = ghost->getParam("cutoff");
        check(v.hasReal && v.real == 440.0 && v.normalized == 0.25,
              "and answers with the mirrored value, real and normalised");
        const auto roles = ghost->stateRoles();
        check(roles.size() == 1 && roles[0] == "chunk", "its state role is kept");
        const auto bytes = ghost->saveState("chunk");
        check(bytes == std::vector<std::uint8_t>{1, 2, 3, 4}, "byte for byte");
        check(ghost->identity().uid == "adi.test.absent" && ghost->identity().format == "vst3",
              "it knows what it stands in for");
    }
    const Session::Entry* ge = s.entryFor(101);
    check(ge != nullptr && ge->placeholder && ge->trackId == 1 && ge->name == "Ghost",
          "the entry records placeholder, track and name");

    // --- the loaded ones ----------------------------------------------------
    check(s.nodeFor(200) != nullptr && s.nodeFor(200)->bypassed(), "Off is bypassed: enabled = 0");
    check(s.nodeFor(100) != nullptr && !s.nodeFor(100)->bypassed(), "Warm is not");
    check(loader.made.size() == 2 && loader.made[0]->prepares == 1 &&
              loader.made[0]->lastRate == 48000.0 && loader.made[0]->lastFrames == 512,
          "Warm was prepared once, at the spec's format, by the first graph");
    eqi(s.stats().statesLoaded, 0, "no state stream loaded (Warm and Off have none)");
    eqi(s.stats().paramsApplied, 0, "and no parameter mirror was applied to them");

    // --- audio --------------------------------------------------------------
    eqi(s.graph().stats().published, 1, "one graph published");
    Out o(512);
    AudioIo io = makeIo(o, 512);
    s.process(io);
    check(o.first() == 0.25f && o.last() == 0.25f,
          "the tone reaches the master THROUGH Warm (halved) and Ghost (bypassed): got " +
              std::to_string(o.first()));
    eqi(loader.made[0]->unpreparedBlocks, 0, "nothing processed an unprepared device");
}

void testBlockSizeChangesWithoutReload() {
    const adi::test::TempDirectory scratch("session", "resize");
    section("ADR-0042 d5 / ADR-0122 d3 -- a format change rebuilds; it never reloads");
    auto store = makeProject(scratch.path() / "p.adi");
    if (!store) return;

    Loader loader;
    Session s;
    ToneNode tone(0.5f);
    s.setSourcesFor([&](std::int64_t id) {
        return id == 1 ? std::vector<Node*>{&tone} : std::vector<Node*>{};
    });
    SessionSpec spec;
    spec.sampleRate = 48000.0;
    spec.maxFrames = 512;
    check(s.load(*store, loader.fn(), spec), "load: " + s.error());
    HalfDevice* warm = loader.made.empty() ? nullptr : loader.made[0];
    if (warm == nullptr) return;
    device::DeviceInstance* warmInst = s.instanceFor(100);

    // The device ran on the first graph -- otherwise the audio thread never
    // swaps to it, and the second publish is the FIRST swap (host.hpp: two
    // rebuilds between blocks skip a graph that never ran).
    {
        Out o0(512);
        AudioIo io0 = makeIo(o0, 512);
        s.process(io0);
        check(o0.first() == 0.25f, "plays at 512 first");
    }

    // The driver granted something else.
    s.prepare(48000.0, 2048);
    eqi(s.stats().formatChanges, 1, "one format change");
    eqi(s.graph().stats().published, 2, "a second graph was published");
    eqi(loader.calls, 3, "the loader was NOT asked again");
    check(s.instanceFor(100) == warmInst, "the same instance is behind Warm");
    eqi(warm->prepares, 2, "prepared again");
    eqi(warm->lastFrames, 2048, "at the new size");
    eqi(warm->releases, 0, "and not released in between -- a rebuild is not a stop");

    Out o(2048);
    AudioIo io = makeIo(o, 2048);
    s.process(io);
    eqi(s.graph().stats().swaps, 2, "the audio thread picked up the new graph");
    check(o.first() == 0.25f && o.last() == 0.25f,
          "and it plays at 2048: got " + std::to_string(o.first()) + " / " +
              std::to_string(o.last()));

    // Every size the engine supports (ADR-0102), in one session, no reload.
    const std::int32_t sizes[] = {32, 64, 128, 256, 512, 1024, 2048, 4096};
    long long published = s.graph().stats().published;
    for (const std::int32_t n : sizes) {
        s.prepare(48000.0, n);
        ++published;
        Out on(n);
        AudioIo ion = makeIo(on, n);
        s.process(ion);
        check(on.first() == 0.25f && on.last() == 0.25f,
              "plays at " + std::to_string(n) + ": got " + std::to_string(on.first()));
        eqi(warm->lastFrames, n, "Warm prepared at " + std::to_string(n));
    }
    eqi(s.graph().stats().published, published, "one graph per size");
    eqi(loader.calls, 3, "still three loader calls after eight sizes");
    eqi(s.stats().formatChanges, 9, "nine format changes");

    // Same format again: a restart the user did not ask to hear.
    s.prepare(48000.0, 4096);
    eqi(s.graph().stats().published, published, "prepare at the SAME format publishes nothing");
    eqi(s.stats().prepares, 10, "though it was counted");
    eqi(s.stats().formatChanges, 9, "and not as a change");

    // Stop, then start at the same format: that IS a rebuild, because the
    // devices were released and the guard would otherwise leave them so.
    s.release();
    eqi(warm->releases, 1, "release reached the plugin");
    s.prepare(48000.0, 4096);
    eqi(s.graph().stats().published, published + 1, "a prepare after release rebuilds");
    eqi(warm->prepares, 11, "and prepares the plugin again");
    Out o2(4096);
    AudioIo io2 = makeIo(o2, 4096);
    s.process(io2);
    check(o2.first() == 0.25f, "and it plays again: got " + std::to_string(o2.first()));
    eqi(warm->unpreparedBlocks, 0, "no block ever reached an unprepared device");

    // The retired graphs are freed by the tick, once the reader has moved past.
    const long long before = s.graph().stats().reclaimed;
    s.tick(1000);
    check(s.graph().stats().reclaimed > before,
          "tick collects retired graphs: " + std::to_string(s.graph().stats().reclaimed));
}

void testRefreshFollowsTheRows() {
    const adi::test::TempDirectory scratch("session", "refresh");
    section("ADR-0122 d1 -- an edit re-read is a new chain from the rows, on the same instances");
    auto store = makeProject(scratch.path() / "p.adi");
    if (!store) return;

    Loader loader;
    Session s;
    ToneNode tone(0.5f);
    s.setSourcesFor([&](std::int64_t id) {
        return id == 1 ? std::vector<Node*>{&tone} : std::vector<Node*>{};
    });
    SessionSpec spec;
    check(s.load(*store, loader.fn(), spec), "load: " + s.error());
    device::DeviceInstance* warm = s.instanceFor(100);
    device::DeviceInstance* off = s.instanceFor(200);

    OpJournal j(*store);
    OpRequest r;

    // A device inserted AHEAD of an existing one, by ord.
    r.opType = "device.insert";
    r.payload = {{"id", 201}, {"chain", 20}, {"ord", 1}, {"ref", 1}, {"name", "New"}};
    commits(j, r, "New on Bass at ord 1");
    check(s.refresh(*store), "refresh: " + s.error());
    eqi(loader.calls, 4, "only the new row went through the loader");
    check(s.instanceFor(200) == off && s.instanceFor(100) == warm,
          "the existing instances are the same objects");
    const auto bass = s.chainFor(2);
    check(bass.size() == 2 && bass[0] == s.nodeFor(201) && bass[1] == s.nodeFor(200),
          "Bass is New (ord 1) then Off (ord 5) -- row order, not insertion order");

    // A flag flipped.
    r.opType = "device.setEnabled";
    r.payload = {{"id", 200}, {"enabled", true}};
    commits(j, r, "Off enabled");
    check(s.refresh(*store), "refresh: " + s.error());
    check(s.nodeFor(200) != nullptr && !s.nodeFor(200)->bypassed(), "Off is no longer bypassed");

    // A device removed: retired, not destroyed, and out of the chain.
    r.opType = "device.remove";
    r.payload = {{"id", 100}};
    commits(j, r, "Warm removed");
    check(s.refresh(*store), "refresh: " + s.error());
    eqi(s.stats().retired, 1, "one retired");
    const Session::Entry* we = s.entryFor(100);
    check(we != nullptr && we->retired && we->trackId == 0, "Warm's entry says retired");
    eqi(static_cast<long long>(s.devices().deviceCount()), 4, "the host still holds four instances");
    const auto keys = s.chainFor(1);
    check(keys.size() == 1 && keys[0] == s.nodeFor(101), "Keys is Ghost alone");
    Out o(512);
    AudioIo io = makeIo(o, 512);
    s.process(io);
    check(o.first() == 0.5f, "the tone now passes Ghost undivided: got " + std::to_string(o.first()));

    // ...and undone: the row returns, and finds ITS instance waiting.
    r.opType = "device.insert";
    r.payload = {{"id", 100}, {"chain", 10}, {"ord", 0}, {"ref", 1}, {"name", "Warm"}};
    commits(j, r, "Warm back");
    check(s.refresh(*store), "refresh: " + s.error());
    eqi(s.stats().retired, 0, "nothing retired now");
    eqi(loader.calls, 4, "and the loader was not asked -- the instance was kept");
    check(s.instanceFor(100) == warm, "the same Warm, state and all");
    Out o2(512);
    AudioIo io2 = makeIo(o2, 512);
    s.process(io2);
    check(o2.first() == 0.25f, "and it halves again: got " + std::to_string(o2.first()));

    // A second load on the same session is refused, not stacked.
    check(!s.load(*store, loader.fn(), spec), "a second load is refused");
    check(s.error().find("already loaded") != std::string::npos, "and says so");
}

void testStateAndTheMirror() {
    const adi::test::TempDirectory scratch("session", "state");
    section("SPEC 7.1 -- state loads first; the parameter mirror is the fallback, not a second pass");
    auto store = makeProject(scratch.path() / "p.adi");
    if (!store) return;

    // Give Warm a state stream AND a mirror; give Off only a mirror.
    OpJournal j(*store);
    OpRequest r;
    r.opType = "device.remove";
    r.payload = {{"id", 100}};
    commits(j, r, "Warm out");
    r.opType = "device.insert";
    r.payload = {{"id", 100}, {"chain", 10}, {"ord", 0}, {"ref", 1}, {"name", "Warm"},
                 {"params", nlohmann::json::array({{{"param", "gain"}, {"norm", 0.5}}})},
                 {"state", nlohmann::json::array({{{"role", "chunk"}, {"hash", "h1"}},
                                                  {{"role", "controller"}, {"hash", "h1"}}})}};
    commits(j, r, "Warm back with state and a mirror");
    r.opType = "device.remove";
    r.payload = {{"id", 200}};
    commits(j, r, "Off out");
    r.opType = "device.insert";
    r.payload = {{"id", 200}, {"chain", 20}, {"ord", 5}, {"ref", 1}, {"name", "Off"},
                 {"params", nlohmann::json::array({{{"param", "gain"}, {"norm", 0.5}},
                                                   {{"param", "mix"}, {"norm", 1.0}}})}};
    commits(j, r, "Off back with a mirror only");

    Loader loader;
    Session s;
    check(s.load(*store, loader.fn(), SessionSpec{}), "load: " + s.error());
    HalfDevice* warm = nullptr;
    HalfDevice* off = nullptr;
    for (HalfDevice* d : loader.made) {
        if (!d->statesSeen.empty()) warm = d;
        else off = d;
    }
    check(warm != nullptr && off != nullptr, "both fakes were made");
    if (warm == nullptr || off == nullptr) return;

    check(warm->statesSeen.size() == 2, "Warm was offered both roles");
    check(warm->lastState == std::vector<std::uint8_t>{1, 2, 3, 4}, "with the blob's bytes");
    eqi(s.stats().statesLoaded, 1, "one role was accepted (the fake knows 'chunk')");
    check(mentions(s.problems(), "devices#100 (Warm): the plugin refused its 'controller' state"),
          "the refused role is named");
    check(warm->paramsSet.empty(), "Warm's mirror was NOT applied on top of its state");
    check(off->paramsSet.size() == 2 && off->paramsSet[0] == "gain" && off->paramsSet[1] == "mix",
          "Off, with no state, got its mirror");
    eqi(s.stats().paramsApplied, 2, "counted");
}

void testAProjectWithoutAMasterIsRefusedNotCrashed() {
    const adi::test::TempDirectory scratch("session", "nomaster");
    section("no master -- the plan has no output, the session says so, and process writes silence");
    StoreError e = StoreError::Ok;
    auto store = Store::create(scratch.path() / "m.adi", e);
    check(store != nullptr, "created");
    if (!store) return;
    OpJournal j(*store);
    OpRequest r;
    r.opType = "track.create";
    r.payload = {{"id", 1}, {"kind", "audio"}, {"name", "Only"}};
    commits(j, r, "one track");

    Loader loader;
    Session s;
    check(!s.load(*store, loader.fn(), SessionSpec{}), "load refuses");
    check(s.error().find("master") != std::string::npos, "and names the master: " + s.error());
    check(s.loaded(), "the project is still loaded (a fix and a refresh can follow)");
    Out o(256);
    AudioIo io = makeIo(o, 256);
    s.process(io);
    check(o.first() == 0.0f && o.last() == 0.0f, "process writes silence, not the sentinel");

    // Fix it, refresh, play.
    r.payload = {{"id", 9}, {"kind", "master"}, {"name", "Master"}, {"index", 1}};
    commits(j, r, "a master");
    check(s.refresh(*store), "refresh after the fix: " + s.error());
    eqi(s.graph().stats().published, 1, "and now a graph exists");
}

void testNothingLoadedIsSilence() {
    section("an empty session is a silent processor, at any size");
    Session s;
    s.prepare(48000.0, 512);
    Out o(512);
    AudioIo io = makeIo(o, 512);
    s.process(io);
    check(o.first() == 0.0f && o.last() == 0.0f, "silence");
    eqi(s.graph().stats().published, 0, "nothing was built");
    check(!s.rebuild() && s.error() == "no project loaded", "rebuild refuses with a reason");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("adi_session_tests -- a project, live (ADR-0122)\n\n");
    testAProjectLoadsAndPlays();
    testBlockSizeChangesWithoutReload();
    testRefreshFollowsTheRows();
    testStateAndTheMirror();
    testAProjectWithoutAMasterIsRefusedNotCrashed();
    testNothingLoadedIsSilence();
    std::printf("\n%s -- %d checks, %d failure(s)\n",
                g_failures ? "FAILED" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
