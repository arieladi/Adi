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

#include "adi/audio/io_audit.hpp"
#include "adi/engine/session.hpp"
#include "adi/history.hpp"
#include "adi/ops.hpp"
#include "adi/store.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
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
    [[nodiscard]] bool wantsDeviceTransport() const noexcept override { return wantsClock; }
    void process(const NodeIo& io) noexcept override {
        if (!prepared) ++unpreparedBlocks;
        if (io.deviceTransport != nullptr) {
            sawPlaying = io.deviceTransport->playing;
            sawRecording = io.deviceTransport->recording;
        }
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
        if (role != "chunk") return false;   // a plugin that knows one role
        values["gain"] = 0.5;                // and whose chunk carries gain at 0.5
        return true;
    }
    bool setExpressionRoute(engine::RouteChoice c) override {
        routes.push_back(c);
        return acceptRoutes;
    }
    [[nodiscard]] device::ParamValue getParam(const std::string& paramId) const noexcept override {
        const auto it = values.find(paramId);
        return device::ParamValue::fromNormalized(it == values.end() ? 0.0 : it->second);
    }
    bool setParam(const std::string& paramId, const device::ParamValue& v) override {
        paramsSet.push_back(paramId);
        values[paramId] = v.normalized;
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
    std::map<std::string, double> values;
    std::vector<engine::RouteChoice> routes;
    bool acceptRoutes = true;
    bool wantsClock = false, sawPlaying = false, sawRecording = false; // SC-333

private:
    device::DeviceIdentity id_;
};

struct Loader {
    int calls = 0;
    bool acceptRoutes = true;   ///< ADR-0149: false makes a device that cannot take a route
    bool ghostInstalled = false;  ///< SC-318: the absent plug-in installed since the open
    bool clocked = false;         ///< SC-333: the devices made ask for the transport
    std::vector<HalfDevice*> made;
    std::vector<std::string> asked;

    DeviceLoader fn() {
        return [this](const DeviceRequest& rq, std::string& err)
                   -> std::unique_ptr<device::DeviceInstance> {
            ++calls;
            asked.push_back(rq.ref != nullptr ? rq.ref->uid : "<no ref>");
            if (rq.ref != nullptr && (rq.ref->uid == "adi.test.half" ||
                                      (ghostInstalled && rq.ref->uid == "adi.test.absent"))) {
                device::DeviceIdentity id;
                id.format = rq.ref->format;
                id.uid = rq.ref->uid;
                id.name = rq.ref->name;
                auto d = std::make_unique<HalfDevice>(id);
                d->acceptRoutes = acceptRoutes;
                d->wantsClock = clocked;
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

class HalfKeyDevice final : public device::DeviceInstance {
public:
    const device::DeviceIdentity &identity() const noexcept override { return identity_; }
    bool loaded() const noexcept override { return true; }
    std::int32_t sidechainInputChannels() const noexcept override { return 2; }
    void prepare(double, std::int32_t) override {}
    void process(const NodeIo &io) noexcept override {
        for (std::int32_t c = 0; c < io.channels; ++c) {
            const auto *input = io.in ? io.in[c] : nullptr;
            const auto *key = io.sidechain ? io.sidechain[c] : nullptr;
            const auto end = io.blockOffset + io.frames;
            for (auto i = io.blockOffset; i < end; ++i)
                io.out[c][i] = 0.5f * ((input ? input[i] : 0.0f) + (key ? key[i] : 0.0f));
        }
    }
private:
    const device::DeviceIdentity identity_{"internal", "adi.test.half-key", "Half and key", "ADI", "1"};
};

void testViaMainIsolation() {
    section("ADR-0221 -- Via Main renders an isolated mix while its detector still listens");
    const test::TempDirectory scratch("session", "via-main");
    const auto path = scratch.path() / "mix.adi";
    StoreError error{};
    auto source = Store::create(path, error);
    check(source != nullptr, "create the Via Main fixture");
    if (!source)
        return;
    source->db().exec(
        "UPDATE project SET sample_rate=48000;"
        "INSERT INTO tracks(id,kind,name,parent_id) VALUES"
        "(1,'master','Main',NULL),(10,'group','Selected group',NULL),"
        "(11,'audio','Included child',10),(12,'audio','Redirected child',10),"
        "(20,'audio','Key source',NULL),(21,'audio','Key receiver',NULL),"
        "(22,'audio','Outside audio',NULL),(30,'return','Audible return',NULL),"
        "(31,'return','Unrelated return',NULL),(32,'return','Cascaded return',NULL);"
        "INSERT INTO mixer_strip(track_id,delay_samples) VALUES(22,-37);"
        "INSERT INTO plugin_refs(id,format,uid,name) VALUES(1,'internal','adi.test.half-key','Half and key');"
        "INSERT INTO device_chains(id,track_id) VALUES(1,1),(11,11);"
        "INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name) VALUES"
        "(1,1,0,1,'Main half'),(11,11,0,1,'Include the detector');"
        "INSERT INTO routing(id,src_kind,src_id,dst_kind,dst_id,kind,enabled) VALUES"
        "(1,'track',12,'track',1,'main',1),(2,'track',20,'track',21,'main',1),"
        "(3,'track',22,'track',10,'main',1),(4,'track',21,'device',11,'sidechain',1),"
        "(5,'track',11,'track',30,'send',1),(6,'track',22,'track',30,'send',1),"
        "(7,'track',30,'track',32,'send',1),(8,'track',11,'track',31,'send',0);"
        "INSERT INTO routing_send_taps(routing_id,tap) VALUES(5,'post_fx'),(6,'pre_fx'),(7,'post_mixer');");
    auto reader = Store::open(path, error, true);
    check(reader && reader->readOnly(), "open the coherent mix fixture read-only");
    if (!reader)
        return;

    struct Audition {
        float main = 0.0f, selectedDirect = 0.0f;
        std::int64_t mainLatency = 0;
    };
    const auto audition = [&](Precision precision, std::optional<std::int64_t> isolated) {
        const std::array<std::pair<std::int64_t, float>, 6> levels = {{
            {11, 0.125f}, {12, 0.25f}, {20, 0.0625f}, {21, 0.0625f}, {22, 0.25f}, {31, 0.0625f}}};
        std::map<std::int64_t, std::unique_ptr<ToneNode>> tones;
        for (const auto &[id, level] : levels)
            tones.emplace(id, std::make_unique<ToneNode>(level));
        Session session;
        session.setSourcesFor([&](std::int64_t id) {
            const auto found = tones.find(id);
            return found == tones.end() ? std::vector<Node *>{} : std::vector<Node *>{found->second.get()};
        });
        SessionSpec spec{2, 48000, 64, precision};
        spec.performanceState = false;
        spec.offline = true;
        spec.viaMainTrack = isolated;
        const auto loader = [](const DeviceRequest &, std::string &) {
            return std::make_unique<HalfKeyDevice>();
        };
        if (!session.load(*reader, loader, spec)) {
            check(false, "offline fixture failed to load: " + session.error());
            return Audition{};
        }
        const auto main = session.offlineTapOutput(1, Tap::PostFader);
        const auto direct = session.offlineTapOutput(11, Tap::PostFx);
        if (!main || !direct) {
            check(false, "offline Main and direct endpoints are available");
            return Audition{};
        }
        session.transport().play();
        Out destination(64), stem(64);
        for (int i = 0; i < 4; ++i)
            session.process(makeIo(destination, 64));
        const auto *graph = session.graph().currentGraph();
        if (!graph || !graph->copyNodeOutput(direct->node, makeIo(stem, 64))) {
            check(false, "offline graph captures a real post-FX stem");
            return Audition{};
        }
        return Audition{destination.last(), stem.last(), main->latency};
    };
    for (const auto precision : {Precision::Float32, Precision::Float64}) {
        const auto full = audition(precision, std::nullopt);
        const auto group = audition(precision, 10);
        const auto direct = audition(precision, 11);
        check(full.main == 0.78125f && full.mainLatency == 37,
              "full Main renders all inputs and the longest audible path, got " +
                  std::to_string(full.main) + " at latency " + std::to_string(full.mainLatency));
        check(group.main == 0.1875f && direct.main == group.main &&
                  group.mainLatency == full.mainLatency,
              "Via Main has only the selected child and two returns, with unchanged compensation");
        check(group.selectedDirect == 0.125f && direct.selectedDirect == group.selectedDirect,
              "the unselected feeding chain remains alive at the selected device's Post Mixer key");
        float sum = 0.0f;
        for (const auto id : {11, 12, 20, 22, 31})
            sum += audition(precision, id).main;
        check(sum == full.main, "linear Via Main sources sum to the same Main output");
    }
    source->db().exec("UPDATE tracks SET soloed=1 WHERE id=12;");
    const auto withSolo = audition(Precision::Float32, 10);
    check(withSolo.main == 0.1875f && withSolo.selectedDirect == 0.125f,
          "an unrelated saved solo does not affect the isolated mix, got " +
              std::to_string(withSolo.main) + " and stem " + std::to_string(withSolo.selectedDirect));
    SessionSpec invalid;
    invalid.viaMainTrack = 10;
    Session live;
    check(!live.load(*source, {}, invalid) && live.error().find("offline read-only") != std::string::npos,
          "a live writer cannot load an export isolation override");
    invalid.performanceState = false;
    invalid.offline = true;
    invalid.viaMainTrack = 999;
    Session missing;
    check(!missing.load(*reader, {}, invalid) && missing.error().find("regular track") != std::string::npos,
          "a missing Via Main source is named before audio starts");
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
    auto store = Store::createEmpty(file, e);
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
    eqi(loader.calls, 4, "the loader resolves Warm, Ghost, Off and Nested");
    eqi(s.stats().loaded, 3, "three available plug-in instances");
    eqi(s.stats().placeholders, 1, "one placeholder");
    eqi(s.stats().skipped, 0, "the rack and its child participate in playback");
    eqi(static_cast<long long>(s.entryCount()), 4, "the four leaf device entries");
    check(s.instanceFor(300) != nullptr && s.instanceFor(205) == nullptr,
          "the rack is structural and its nested device loads normally");
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
    check(loader.made.size() == 3 && loader.made[0]->prepares == 1 &&
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
    eqi(loader.calls, 4, "the loader was NOT asked again");
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
    eqi(loader.calls, 4, "the same four loader calls after eight sizes");
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
    eqi(loader.calls, 5, "only the new row went through the loader");
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
    eqi(static_cast<long long>(s.devices().deviceCount()), 5, "the host still holds five instances");
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
    eqi(loader.calls, 5, "the loader reuses the retained instance");
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
    section("SPEC 7.1, ADR-0142 -- state loads first; a row the chunk disagrees with is newer, and goes on top");
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
                 {"params", nlohmann::json::array({{{"param", "gain"}, {"norm", 0.5}},
                                                   {{"param", "mix"}, {"norm", 0.8}}})},
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
    warm = dynamic_cast<HalfDevice*>(s.instanceFor(100));
    off = dynamic_cast<HalfDevice*>(s.instanceFor(200));
    check(warm != nullptr && off != nullptr, "both fakes were made");
    if (warm == nullptr || off == nullptr) return;

    check(warm->statesSeen.size() == 2, "Warm was offered both roles");
    check(warm->lastState == std::vector<std::uint8_t>{1, 2, 3, 4}, "with the blob's bytes");
    eqi(s.stats().statesLoaded, 1, "one role was accepted (the fake knows 'chunk')");
    check(mentions(s.problems(), "devices#100 (Warm): the plugin refused its 'controller' state"),
          "the refused role is named");
    // ADR-0142: the chunk carries gain at 0.5, as the row does -- not pushed.
    // The mix row disagrees with it, so it is an edit made after the chunk
    // was taken, and it goes on top.
    check(warm->paramsSet.size() == 1 && warm->paramsSet[0] == "mix",
          "Warm: only the row its chunk disagrees with was pushed on top of its state");
    check(std::fabs(warm->getParam("mix").normalized - 0.8) < 1e-9, "Warm's mix is the row's 0.8");
    eqi(s.stats().paramsMatched, 1, "the row the chunk agreed with is counted as matched");
    check(off->paramsSet.size() == 2 && off->paramsSet[0] == "gain" && off->paramsSet[1] == "mix",
          "Off, with no state, got its mirror");
    eqi(s.stats().paramsApplied, 3, "counted: Warm's mix and Off's two");
}

void testAProjectWithoutAMasterIsRefusedNotCrashed() {
    const adi::test::TempDirectory scratch("session", "nomaster");
    section("no master -- the plan has no output, the session says so, and process writes silence");
    StoreError e = StoreError::Ok;
    auto store = Store::createEmpty(scratch.path() / "m.adi", e);
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

void testTheExpressionRoute() {
    const adi::test::TempDirectory scratch("session", "route");
    section("ADR-0146, ADR-0149 -- the project's route, applied at load and on every refresh; never the registry's");
    auto store = makeProject(scratch.path() / "p.adi");
    if (!store) return;
    OpJournal j(*store);
    OpRequest r;
    r.opType = "device.setExpressionRoute";
    r.payload = {{"dev", 100}, {"route", "mpe_midi"}};
    commits(j, r, "Warm on MPE over MIDI");

    Loader loader;
    Session s;
    check(s.load(*store, loader.fn(), SessionSpec{}), "load: " + s.error());
    HalfDevice* warm = nullptr;
    HalfDevice* off = nullptr;
    for (HalfDevice* d : loader.made)
        if (s.instanceFor(100) == d) warm = d;
        else off = d;
    check(warm != nullptr, "Warm was made");
    if (warm == nullptr) return;
    check(warm->routes.size() == 1 && warm->routes[0] == engine::RouteChoice::MpeMidi,
          "Warm was asked for MPE over MIDI, once, at load");
    check(off == nullptr || off->routes.empty(), "a device with no row is left on Auto: nothing asked");
    eqi(s.stats().routesApplied, 1, "counted");

    r.payload = {{"dev", 100}, {"route", "plain"}};
    commits(j, r, "then plain");
    check(s.refresh(*store), "refresh: " + s.error());
    check(warm->routes.size() == 2 && warm->routes[1] == engine::RouteChoice::Plain, "a refresh applies the new route");
    check(s.refresh(*store), "refresh again");
    check(warm->routes.size() == 2, "and an unchanged route is not asked again");
    r.payload = {{"dev", 100}, {"route", nullptr}};
    commits(j, r, "back to Auto");
    check(s.refresh(*store), "refresh");
    check(warm->routes.size() == 3 && warm->routes[2] == engine::RouteChoice::Auto, "clearing the row asks for Auto");
}

void testARouteADeviceCannotTake() {
    const adi::test::TempDirectory scratch("session", "route_refused");
    section("SPEC 7.5 -- a device that cannot take the recorded route plays on, the row is kept, and it is said");
    auto store = makeProject(scratch.path() / "p.adi");
    if (!store) return;
    OpJournal j(*store);
    OpRequest r;
    r.opType = "device.setExpressionRoute";
    r.payload = {{"dev", 100}, {"route", "note_expression"}};
    commits(j, r, "Warm on note expression");
    Loader loader;
    loader.acceptRoutes = false;
    Session s;
    check(s.load(*store, loader.fn(), SessionSpec{}), "load: " + s.error());
    eqi(s.stats().routesRefused, 1, "refused, and counted");
    check(mentions(s.problems(), "devices#100 (Warm): the project plays it on route 'note_expression', which this device cannot take"),
          "and named");
    SQLite::Statement st(store->db(), "SELECT route FROM device_expression_routes WHERE device_id = 100");
    check(st.executeStep() && st.getColumn(0).getString() == "note_expression", "the row is kept");
}

namespace rack_fixture {
constexpr std::int32_t delaySamples = 64;
constexpr std::int32_t impulseSample = 11;
constexpr std::int32_t blockFrames = 32;
constexpr std::size_t horizon = 128;

class StereoImpulse final : public Node {
public:
    explicit StereoImpulse(bool continuous = false) : continuous_(continuous) {}
    bool processesDouble() const noexcept override { return true; }
    void process(const NodeIo& io) noexcept override {
        for (std::int32_t channel=0; channel<io.channels; ++channel) {
            for (std::int32_t i=0; i<io.frames; ++i) {
                const double x=continuous_ || position_+i==impulseSample ? (channel==0 ? .25 : -.125) : 0;
                const auto frame=io.blockOffset+i;
                if (io.wide()) io.out64[channel][frame]=x;
                else io.out[channel][frame]=static_cast<float>(x);
            }
        }
        position_+=io.frames;
    }
private:
    bool continuous_;
    std::int64_t position_=0;
};

class Delay final : public device::DeviceInstance {
public:
    explicit Delay(const DeviceRequest& request, std::int32_t report = delaySamples) : report_(report) {
        id_.format=request.ref->format;
        id_.uid=request.ref->uid;
        id_.name=request.ref->name;
    }
    const device::DeviceIdentity& identity() const noexcept override { return id_; }
    bool loaded() const noexcept override { return true; }
    bool processesDouble() const noexcept override { return true; }
    std::int64_t tailSamples() const noexcept override { return delaySamples; }
    std::int32_t latencySamples() const noexcept override { return report_; }
    void process(const NodeIo& io) noexcept override {
        if (report_ < 0) {
            dry_.process(io);
            return;
        }
        for (std::int32_t i=0; i<io.frames; ++i) {
            const auto frame=io.blockOffset+i;
            for (std::int32_t c=0; c<io.channels; ++c) {
                auto& value=ring_[static_cast<std::size_t>(c)][head_];
                const auto sample=io.wide() ? io.in64[c][frame] : static_cast<double>(io.in[c][frame]);
                if (io.wide()) io.out64[c][frame]=value;
                else io.out[c][frame]=static_cast<float>(value);
                value=sample;
            }
            head_=(head_+1)%delaySamples;
        }
    }
private:
    std::int32_t report_;
    device::DeviceIdentity id_;
    MixNode dry_;
    std::array<std::array<double,delaySamples>,2> ring_{};
    std::size_t head_=0;
};

enum class Topology { Dry, Parallel, Nested, NestedMissing, NestedInvalid };
const char* name(Topology topology) {
    switch (topology) {
        case Topology::Dry: return "dry";
        case Topology::Parallel: return "parallel";
        case Topology::Nested: return "nested";
        case Topology::NestedMissing: return "nested-missing";
        case Topology::NestedInvalid: return "nested-invalid";
    }
    return "unknown";
}

void commit(OpJournal& journal,std::string type,Payload payload) {
    OpRequest op;
    op.opType=std::move(type);
    op.payload=std::move(payload);
    const auto result=journal.commit(op);
    if (!result.ok) throw std::runtime_error(op.opType+": "+result.error);
}

void seed(Store& store,Topology shape) {
    OpJournal journal(store);
    commit(journal,"track.create",{{"id",1},{"kind","audio"},{"name","Rack track"},{"index",0}});
    commit(journal,"track.create",{{"id",9},{"kind","master"},{"name","Main"},{"index",1}});
    store.db().exec("INSERT INTO plugin_refs(id,format,uid,name,subtype) "
                    "VALUES(1,'vst3','adi.test.fixed-delay-64','Owned Sample Delay','effect')");
    commit(journal,"chain.create",{{"id",10},{"track",1}});
    commit(journal,"device.insert",{{"id",20},{"chain",10},{"name","Rack"},{"rack",true},{"rackKind","audio_effect"}});
    commit(journal,"chain.create",{{"id",21},{"device",20},{"ord",0},{"name","Direct, no devices"}});
    if (shape==Topology::Dry) return;
    commit(journal,"chain.create",{{"id",22},{"device",20},{"ord",1},{"name","Late"}});
    const bool nested = shape != Topology::Parallel;
    if (nested) {
        commit(journal,"device.insert",{{"id",30},{"chain",22},{"ord",0},{"name","Nested rack"},{"rack",true},{"rackKind","audio_effect"}});
        commit(journal,"chain.create",{{"id",31},{"device",30},{"ord",0},{"name","Nested dry"}});
        commit(journal,"chain.create",{{"id",32},{"device",30},{"ord",1},{"name","Nested delay"}});
    }
    commit(journal,"device.insert",{{"id",40},{"chain",nested?32:22},{"ord",0},
                                     {"ref",1},{"name","Fixed latency and real delay"}});
    if (shape == Topology::NestedMissing || shape == Topology::NestedInvalid)
        commit(journal,"device.insert",{{"id",41},{"chain",31},{"ord",0},{"ref",1},{"name","Unknown latency plug-in"}});
}

struct Observed {
    std::array<std::array<double,horizon>,2> output{};
    int loads=0;
    std::int32_t latency=0;
    std::optional<RackLatency> outer, inner;
    std::int64_t skipped=0, swaps=0;
    bool sameInstance=false;
};

Observed render(const std::filesystem::path& path,Topology shape,Precision width,bool swap) {
    StoreError error{};
    auto store=Store::createEmpty(path,error);
    if (!store) throw std::runtime_error("cannot create rack fixture");
    seed(*store,shape);
    Observed result;
    StereoImpulse source;
    Session session;
    session.graph().setFadeFrames(0);
    session.setSourcesFor([&](std::int64_t track) {
        return track==1 ? std::vector<Node*>{&source} : std::vector<Node*>{};
    });
    if (!session.load(*store,[&](const DeviceRequest& request,std::string& failure)->std::unique_ptr<device::DeviceInstance> {
            ++result.loads;
            if (request.device->id==40) return std::make_unique<Delay>(request);
            if (request.device->id==41 && shape==Topology::NestedInvalid) return std::make_unique<Delay>(request, -1);
            failure="only the owned delay fixture is loaded";
            return {};
        },SessionSpec{2,48000,blockFrames,width}))
        throw std::runtime_error("rack Session load: "+session.error());
    const auto* before=session.instanceFor(40);
    for (std::size_t clock=0; clock<horizon; clock+=blockFrames) {
        if (swap && clock==blockFrames) {
            OpJournal journal(*store);
            commit(journal,"track.create",{{"id",5},{"kind","audio"},{"name","New silent independent track"},{"index",0}});
            if (!session.refresh(*store)) throw std::runtime_error("rack Session refresh: "+session.error());
        }
        std::array<float,blockFrames> left{},right{};
        std::array<double,blockFrames> leftWide{},rightWide{};
        float* out[]{left.data(),right.data()};
        double* wide[]{leftWide.data(),rightWide.data()};
        AudioIo io;
        io.frames=blockFrames;
        io.numOut=2;
        io.streamTimeSamples=static_cast<std::int64_t>(clock);
        if (width==Precision::Float64) io.out64=wide;
        else io.out=out;
        {
            const audio::CallbackScope callback;
            session.process(io);
        }
        for (std::size_t i=0;i<left.size();++i) {
            result.output[0][clock+i]=width==Precision::Float64 ? leftWide[i] : left[i];
            result.output[1][clock+i]=width==Precision::Float64 ? rightWide[i] : right[i];
        }
    }
    result.latency=session.graph().renderedLatency();
    const auto* current = session.graph().current();
    result.outer = current->rackLatency(20);
    result.inner = current->rackLatency(30);
    result.skipped=session.stats().skipped;
    result.swaps=session.graph().stats().swaps;
    result.sameInstance=before!=nullptr && before==session.instanceFor(40);
    return result;
}

bool exact(const Observed& heard,std::size_t expectedFrame,double left,double right) {
    for (std::size_t i=0;i<horizon;++i) {
        if (heard.output[0][i]!=(i==expectedFrame?left:0) ||
            heard.output[1][i]!=(i==expectedFrame?right:0))
            return false;
    }
    return true;
}

void testRackPlayback() {
    section("DEV-01 -- serial and nested rack chains share one compensated graph");
    const adi::test::TempDirectory scratch("session", "rack_playback");
    try {
        for (const auto width : {Precision::Float32, Precision::Float64}) {
            for (const auto shape : {Topology::Dry, Topology::Parallel, Topology::Nested, Topology::NestedMissing, Topology::NestedInvalid}) {
                const bool dry = shape == Topology::Dry;
                const bool unknown = shape == Topology::NestedMissing || shape == Topology::NestedInvalid;
                const bool nested = !dry && shape != Topology::Parallel;
                const auto caseName = std::string(name(shape)) + (width == Precision::Float64 ? "-64" : "-32");
                const auto heard = render(scratch.path() / (caseName + ".adi"), shape, width, nested);
                const auto position = static_cast<std::size_t>(impulseSample + (dry ? 0 : delaySamples));
                const auto voices = dry ? 1.0 : (nested ? 3.0 : 2.0);
                check(exact(heard, position, .25 * voices, -.125 * voices), caseName + " aligns every dry and wet sample");
                eqi(heard.latency, dry ? 0 : delaySamples, caseName + " reports the aligned graph latency");
                check(heard.outer && heard.outer->samples == (dry ? 0 : delaySamples) && heard.outer->known == !unknown,
                      caseName + " reports known zero separately from unavailable latency");
                check(nested ? heard.inner && heard.inner->samples == delaySamples && heard.inner->known == !unknown : !heard.inner,
                      caseName + " reports the nested rack's own applied latency");
                eqi(heard.loads, dry ? 0 : unknown ? 2 : 1, caseName + " loads only the hosted leaves");
                eqi(heard.skipped, 0, caseName + " realizes all stored rack chains");
                if (nested)
                    check(heard.swaps >= 2 && heard.sameInstance, caseName + " preserves the child across a graph swap");
            }
        }
    } catch (const std::exception& error) {
        check(false, error.what());
    }
}

void testRackChainControls() {
    section("DEV-01 -- chain mute and sibling solo use a five-millisecond ramp");
    const adi::test::TempDirectory scratch("session", "rack_controls");
    StoreError error{};
    auto store = Store::createEmpty(scratch.path() / "project.adi", error);
    check(store != nullptr, "create a project for chain controls");
    if (!store) return;
    seed(*store, Topology::Dry);
    OpJournal journal(*store);
    commit(journal, "chain.create", {{"id",22}, {"device",20}, {"ord",1}});
    commit(journal, "device.insert", {{"id",30}, {"chain",22}, {"rack",true}, {"rackKind","audio_effect"}});
    commit(journal, "chain.create", {{"id",31}, {"device",30}, {"ord",0}});
    commit(journal, "chain.create", {{"id",32}, {"device",30}, {"ord",1}});

    constexpr int frames = 256;
    StereoImpulse source(true);
    Session session;
    session.setSourcesFor([&](std::int64_t track) {
        return track == 1 ? std::vector<Node*>{&source} : std::vector<Node*>{};
    });
    check(session.load(*store, {}, SessionSpec{2, 48000, frames}), "load the nested rack controls fixture");
    const auto published = session.graph().stats().published;
    std::array<float, frames> left{}, right{};
    float* output[]{left.data(), right.data()};
    AudioIo io;
    io.out = output;
    io.numOut = 2;
    io.frames = frames;
    auto renderBlock = [&] {
        const audio::CallbackScope callback;
        session.process(io);
        io.streamTimeSamples += frames;
    };
    renderBlock();
    check(left[0] == .75f && right[0] == -.375f, "three child chains start at unity");
    const auto* playing = session.graph().currentGraph();
    commit(journal, "chain.setSolo", {{"id",31}, {"soloed",true}});
    check(session.refresh(*store), "adopt nested solo");
    renderBlock();
    check(left[0] > .74f && left[100] > .55f && left[100] < .7f && left[239] == .5f && left.back() == .5f,
          "inner solo ramps away one sibling in 240 samples and keeps the outer dry chain");
    commit(journal, "chain.setMute", {{"id",21}, {"muted",true}});
    check(session.refresh(*store), "adopt outer mute");
    renderBlock();
    check(left[0] > .49f && left[239] == .25f && left.back() == .25f, "outer mute ramps away only its own branch");
    check(History(*store).undo().ok, "undo the outer mute");
    check(session.refresh(*store), "adopt inverse chain mute");
    renderBlock();
    check(left[0] < .26f && left[239] == .5f && left.back() == .5f, "undo ramps the outer dry branch back");
    check(session.graph().stats().published == published && session.graph().currentGraph() == playing,
          "solo, mute and Undo retain the same graph");

    store->db().exec("UPDATE tracks SET name='Changed without a new receipt' WHERE id=1");
    check(session.refresh(*store) && session.graph().stats().published == published + 1 &&
              std::ranges::any_of(session.model().tracks, [](const auto& track) {
                  return track.id == 1 && track.name == "Changed without a new receipt";
              }), "an already consumed mute receipt cannot hide later project material changes");
}
} // namespace rack_fixture

}  // namespace

void testAPlaceholderWakesOnceInstalled() {
    const adi::test::TempDirectory scratch("session", "wakes");
    section("SC-318 -- a missing plug-in wakes once it is installed and a scan has run");
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
    check(s.entryFor(101) != nullptr && s.entryFor(101)->placeholder, "Ghost opens as a placeholder");

    // Still not installed: the retry asks, nothing changes, nothing is rebuilt.
    const auto rebuildsBefore = s.stats().rebuilds;
    const int callsBefore = loader.calls;
    eqi(static_cast<long long>(s.retryPlaceholders(*store)), 0, "nothing wakes while it is missing");
    eqi(loader.calls - callsBefore, 1, "the loader was asked for Ghost alone, not the loaded ones");
    eqi(s.stats().rebuilds - rebuildsBefore, 0, "and no graph was rebuilt");
    check(s.entryFor(101) != nullptr && s.entryFor(101)->placeholder, "Ghost is still a placeholder");

    // Installed: the next scan's retry brings it in, in its place, with its stored state.
    loader.ghostInstalled = true;
    eqi(static_cast<long long>(s.retryPlaceholders(*store)), 1, "Ghost wakes");
    const Session::Entry* ge = s.entryFor(101);
    check(ge != nullptr && !ge->placeholder && !ge->retired && ge->trackId == 1,
          "its entry is a loaded device on Keys");
    device::DeviceInstance* ghost = s.instanceFor(101);
    check(ghost != nullptr && ghost->loaded(), "its instance is the plug-in, not a stand-in");
    HalfDevice* woke = loader.made.empty() ? nullptr : loader.made.back();
    check(woke != nullptr && woke == ghost, "the instance the loader made");
    if (woke != nullptr) {
        check(woke->lastState == std::vector<std::uint8_t>{1, 2, 3, 4},
              "the state the placeholder kept reached the plug-in byte for byte");
        check(woke->prepared && woke->lastRate == 48000.0 && woke->lastFrames == 512,
              "prepared at the session's format");
    }
    const std::vector<device::DeviceNode*> keys = s.chainFor(1);
    check(keys.size() == 2 && keys[1] == s.nodeFor(101) && !keys[1]->bypassed(),
          "Keys plays Warm then Ghost, Ghost no longer bypassed");
    eqi(s.stats().placeholders, 0, "no placeholder is counted");
    check(!mentions(s.problems(), "devices#101 (Ghost): kept as a placeholder"),
          "the open's placeholder line is gone");

    Out o(512);
    AudioIo io = makeIo(o, 512);
    s.process(io);
    check(o.first() == 0.125f && o.last() == 0.125f,
          "the tone now passes through Warm AND Ghost (halved twice): got " + std::to_string(o.first()));

    // A later edit keeps the woken instance: refresh finds it compatible, not new.
    const int made = static_cast<int>(loader.made.size());
    check(s.refresh(*store), "refresh: " + s.error());
    check(s.instanceFor(101) == ghost, "a refresh keeps the woken Ghost");
    eqi(static_cast<long long>(loader.made.size()) - made, 0, "and loads nothing again");
    eqi(static_cast<long long>(s.retryPlaceholders(*store)), 0, "a second retry finds nothing to wake");
}

void testADeviceKnowsADIRecords() {
    const adi::test::TempDirectory scratch("session", "records");
    section("SC-333 -- a hosted device is told ADI records, while it plays");
    auto store = makeProject(scratch.path() / "p.adi");
    if (!store) return;
    Loader loader;
    loader.clocked = true;
    Session s;
    ToneNode tone(0.5f); // a sounding input, so the device is not left to doze
    s.setSourcesFor([&](std::int64_t id) {
        return id == 1 ? std::vector<Node*>{&tone} : std::vector<Node*>{};
    });
    SessionSpec spec;
    spec.sampleRate = 48000.0;
    spec.maxFrames = 512;
    check(s.load(*store, loader.fn(), spec), "load: " + s.error());
    HalfDevice* warm = loader.made.empty() ? nullptr : loader.made[0];
    if (warm == nullptr) return;
    Out o(512);
    AudioIo io = makeIo(o, 512);
    s.transport().play();
    s.process(io);
    check(warm->sawPlaying && !warm->sawRecording, "playing: the device is told it plays, not that it records");
    s.setRecordingForDevices(true);
    s.process(io);
    check(warm->sawPlaying && warm->sawRecording, "recording: it is told so");
    s.transport().play(false);
    s.process(io);
    check(!warm->sawPlaying && !warm->sawRecording, "stopped: a recording that has not ended yet does not say it records");
    s.transport().play();
    s.setRecordingForDevices(false);
    s.process(io);
    check(warm->sawPlaying && !warm->sawRecording, "the take ended: playing only");
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("adi_session_tests -- a project, live (ADR-0122)\n\n");
    testAProjectLoadsAndPlays();
    rack_fixture::testRackPlayback();
    rack_fixture::testRackChainControls();
    testBlockSizeChangesWithoutReload();
    testRefreshFollowsTheRows();
    testStateAndTheMirror();
    testAProjectWithoutAMasterIsRefusedNotCrashed();
    testNothingLoadedIsSilence();
    testTheExpressionRoute();
    testARouteADeviceCannotTake();
    testAPlaceholderWakesOnceInstalled();
    testADeviceKnowsADIRecords();
    testViaMainIsolation();
    std::printf("\n%s -- %d checks, %d failure(s)\n",
                g_failures ? "FAILED" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
