// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/realize.hpp"
#include "adi/history.hpp"
#include "adi/row_capture.hpp"
#include "adi/store.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <stdexcept>
namespace {
thread_local bool realtime = false;
thread_local unsigned allocations = 0, deallocations = 0;
} // namespace
void *operator new(std::size_t n) {
    if (realtime)
        ++allocations;
    if (auto *p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void *p) noexcept {
    if (p && realtime)
        ++deallocations;
    std::free(p);
}
void operator delete[](void *p) noexcept { ::operator delete(p); }
void operator delete(void *p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void *p, std::size_t) noexcept { ::operator delete(p); }
namespace {
using namespace adi;
using namespace adi::engine;
int checks = 0, failures = 0;
void check(bool ok, const char *why) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", why);
    }
}
OpRequest op(const char *type, Payload payload) {
    OpRequest r;
    r.opType = type;
    r.payload = std::move(payload);
    r.label = "Key route";
    return r;
}
struct Fixture {
    test::TempDirectory temp{"device_keys", "state"};
    std::unique_ptr<Store> store;
    Fixture() {
        StoreError e{};
        store = Store::create(temp.path() / "keys.adi", e);
        if (!store)
            throw std::runtime_error(toString(e));
        store->db().exec("INSERT INTO tracks(id,kind,name) "
                         "VALUES(1,'master','Main'),(2,'audio','Key'),(3,'audio','Programme')");
        store->db().exec("INSERT INTO device_chains(id,track_id,name) VALUES(3,3,'Devices')");
        store->db().exec("INSERT INTO devices(id,chain_id,ord,name) VALUES(30,3,0,'Target')");
    }
    OpRequest connect(const char *tap = "post_mixer") {
        return op("routing.connect", {{"id", 9},
                                      {"srcKind", "track"},
                                      {"src", 2},
                                      {"dstKind", "device"},
                                      {"dst", 30},
                                      {"kind", "sidechain"},
                                      {"keyTap", tap}});
    }
};
void persistence() {
    Fixture f;
    auto &s = *f.store;
    check(OpJournal(s).commit(f.connect("post_fx")).ok,
          "connect and explicit tap are one journal batch");
    const auto route = detail::captureRow(s.db(), "routing", "id", 9);
    check(rows::readModel(s).routing.front().keyTap == "post_fx",
          "model reads the persisted key tap");
    check(OpJournal(s).commit(op("routing.setKeyTap", {{"id", 9}, {"tap", "pre_fx"}})).ok &&
              rows::readModel(s).routing.front().keyTap == "pre_fx",
          "tap edit reaches model");
    check(History(s).undo().ok && rows::readModel(s).routing.front().keyTap == "post_fx",
          "tap Undo restores exact previous choice");
    check(OpJournal(s).commit(op("routing.disconnect", {{"id", 9}})).ok &&
              s.db().execAndGet("SELECT count(*) FROM routing_key_taps").getInt() == 0,
          "disconnect cascades its owned tap");
    check(History(s).undo().ok && detail::captureRow(s.db(), "routing", "id", 9) == route &&
              rows::readModel(s).routing.front().keyTap == "post_fx",
          "one Undo restores the full route and tap");
    check(OpJournal(s).commit(op("device.remove", {{"id", 30}})).ok &&
              s.db().execAndGet("SELECT count(*) FROM routing").getInt() == 0,
          "device removal removes its polymorphic key connection");
    check(History(s).undo().ok && detail::captureRow(s.db(), "routing", "id", 9) == route &&
              rows::readModel(s).routing.front().keyTap == "post_fx",
          "one device Undo restores key and owned tap");
    check(OpJournal(s).commit(op("track.delete", {{"id", 2}})).ok &&
              s.db().execAndGet("SELECT count(*) FROM routing").getInt() == 0,
          "source deletion removes key route");
    check(History(s).undo().ok && rows::readModel(s).routing.front().keyTap == "post_fx",
          "source Undo restores route and tap");
    const auto head = OpJournal(s).headSeq();
    check(!OpJournal(s).commit(op("routing.setKeyTap", {{"id", 9}, {"tap", "wrong"}})).ok &&
              OpJournal(s).headSeq() == head,
          "invalid tap refuses without history mutation");
    auto wrong = f.connect();
    wrong.payload["id"] = 10;
    wrong.payload["src"] = 999;
    check(!OpJournal(s).commit(wrong).ok && !detail::captureRow(s.db(), "routing", "id", 10),
          "missing endpoint refuses atomic connection");
    check(OpJournal(s).commit(op("routing.setKeyTap", {{"id", 9}, {"tap", nullptr}})).ok &&
              rows::readModel(s).routing.front().keyTap == "post_mixer",
          "absent tap defaults to Post Mixer");
    const auto path = s.path();
    f.store.reset();
    StoreError e{};
    f.store = Store::open(path, e);
    check(f.store && rows::readModel(*f.store).routing.front().keyTap == "post_mixer",
          "key default survives file reopen");
}
struct Pulse final : Node {
    explicit Pulse(double v) : value(v) {}
    double value;
    std::int64_t position = 0;
    void prepare(double, std::int32_t) override { position = 0; }
    void process(const NodeIo &io) noexcept override {
        for (int i = 0; i < io.frames; ++i) {
            const auto v = position++ == 64 ? value : 0.;
            for (int c = 0; c < io.channels; ++c)
                if (io.wide())
                    io.out64[c][io.blockOffset + i] = v;
                else
                    io.out[c][io.blockOffset + i] = static_cast<float>(v);
        }
    }
};
struct Delay final : Node {
    explicit Delay(int n) : delay(n) {}
    int delay = 0, at = 0;
    std::array<std::array<double, 64>, 2> ring{};
    std::int32_t latencySamples() const noexcept override { return delay; }
    std::int64_t tailSamples() const noexcept override { return 64; }
    bool processesDouble() const noexcept override { return true; }
    void prepare(double, std::int32_t) override {
        ring = {};
        at = 0;
    }
    void process(const NodeIo &io) noexcept override {
        for (int i = 0; i < io.frames; ++i) {
            for (int c = 0; c < io.channels; ++c) {
                auto &cell = ring[static_cast<std::size_t>(c)][static_cast<std::size_t>(at)];
                const auto value = io.wide() ? (io.in64 ? io.in64[c][io.blockOffset + i] : 0.)
                                             : (io.in ? io.in[c][io.blockOffset + i] : 0.);
                const auto old = cell;
                cell = value;
                if (io.wide())
                    io.out64[c][io.blockOffset + i] = old;
                else
                    io.out[c][io.blockOffset + i] = static_cast<float>(old);
            }
            at = (at + 1) % delay;
        }
    }
};
struct Probe final : Node {
    bool gotKey = false;
    double peakMain = 0, peakKey = 0, error = 0;
    int calls = 0;
    std::int64_t tailSamples() const noexcept override { return 0; }
    bool processesDouble() const noexcept override { return true; }
    void process(const NodeIo &io) noexcept override {
        ++calls;
        gotKey |= !io.sidechainSilent;
        for (int c = 0; c < io.channels; ++c)
            for (int i = 0; i < io.frames; ++i) {
                const auto at = io.blockOffset + i;
                const auto a =
                    io.wide() ? (io.in64 ? io.in64[c][at] : 0.) : (io.in ? io.in[c][at] : 0.);
                const auto k = io.wide() ? (io.sidechain64 ? io.sidechain64[c][at] : 0.)
                                         : (io.sidechain ? io.sidechain[c][at] : 0.);
                peakMain = std::max(peakMain, std::abs(a));
                peakKey = std::max(peakKey, std::abs(k));
                error = std::max(error, std::abs(k - 2 * a));
                if (io.wide())
                    io.out64[c][at] = a;
                else
                    io.out[c][at] = static_cast<float>(a);
            }
    }
};
void graph(Precision precision, int channels, const char *tap) {
    Fixture f;
    check(OpJournal(*f.store).commit(f.connect(tap)).ok, "graph fixture routes device key");
    const auto model = rows::readModel(*f.store);
    const auto plan = planGraph(model);
    check(!plan.cycle && plan.problems.empty(), "planner resolves key target's owning track");
    Pulse key(.25), main(.125);
    Delay keyFx(17), mainFx(5), after(29);
    Probe target;
    RealizeOptions options;
    options.channels = channels;
    options.sourcesFor = [&](std::int64_t track) {
        return track == 2   ? std::vector<Node *>{&key}
               : track == 3 ? std::vector<Node *>{&main}
                            : std::vector<Node *>{};
    };
    options.devicesFor = [&](std::int64_t track) {
        return track == 2   ? std::vector<Node *>{&keyFx}
               : track == 3 ? std::vector<Node *>{&mainFx, &target, &after}
                            : std::vector<Node *>{};
    };
    options.keyDeviceFor = [&](std::int64_t id) -> Node * { return id == 30 ? &target : nullptr; };
    options.keyTapFor = [&](std::int64_t track, Tap point) -> Node * {
        return track == 2 && point != Tap::PreFx ? &keyFx : nullptr;
    };
    auto built = realize(plan, options);
    check(built->ok() && built->problems().empty(), "key realizer names the exact in-chain target");
    built->graph().setPrecision(precision);
    built->graph().prepare(48000, 256);
    check(built->graph().ok(), "concrete key DAG prepares");
    std::array<std::array<float, 256>, 2> output{};
    std::array<std::array<double, 256>, 2> wide{};
    std::array<float *, 2> out{output[0].data(), output[1].data()};
    std::array<double *, 2> out64{wide[0].data(), wide[1].data()};
    AudioIo io;
    io.numOut = channels;
    io.out = out.data();
    io.out64 = precision == Precision::Float64 ? out64.data() : nullptr;
    io.frames = 256;
    allocations = deallocations = 0;
    realtime = true;
    for (int n = 0; n < 4; ++n) {
        io.streamTimeSamples = n * 256;
        built->graph().process(io);
    }
    realtime = false;
    check(allocations == 0 && deallocations == 0, "key callback allocates and frees nothing");
    check(target.gotKey && target.peakKey == .25 && target.peakMain == .125,
          "device receives key without summing it into main");
    check(target.error == 0, "programme and key impulses align at the device before downstream FX");
    options.keyDeviceFor = {};
    auto refused = realize(plan, options);
    check(!refused->problems().empty(),
          "unavailable key input is named rather than silently substituted");
    // A same-track Pre FX key is not a track-junction cycle. A downstream
    // Post Mixer self-key really is a concrete node cycle, rejected by Graph.
    auto self = model;
    self.routing.front().srcId = 3;
    self.routing.front().keyTap = "pre_fx";
    check(!planGraph(self).cycle, "Pre FX self-key is left to concrete DAG validation");
    options.keyDeviceFor = [&](std::int64_t id) -> Node * { return id == 30 ? &target : nullptr; };
    options.keyTapFor = [&](std::int64_t track, Tap point) -> std::optional<Node *> {
        if (track == 3)
            return point == Tap::PreFx ? static_cast<Node *>(nullptr) : &after;
        return &keyFx;
    };
    auto valid = realize(planGraph(self), options);
    valid->graph().prepare(48000, 256);
    check(valid->ok() && valid->graph().ok(), "concrete Pre FX self-key is acyclic");
    self.routing.front().keyTap = "post_mixer";
    auto loop = realize(planGraph(self), options);
    loop->graph().prepare(48000, 256);
    check(loop->ok() && !loop->graph().ok(),
          "downstream self-key is rejected as a real node cycle");
    options.keyTapFor = [](std::int64_t, Tap) -> std::optional<Node *> { return std::nullopt; };
    auto missing = realize(plan, options);
    check(!missing->problems().empty(), "unavailable tap differs from an available junction tap");
}
} // namespace
int main() {
    try {
        persistence();
        for (const auto p : {Precision::Float32, Precision::Float64})
            for (int c : {1, 2})
                for (const auto *tap : {"pre_fx", "post_fx", "post_mixer"})
                    graph(p, c, tap);
    } catch (const std::exception &e) {
        ++failures;
        std::printf("EXCEPTION %s\n", e.what());
    }
    std::printf("device keys: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
