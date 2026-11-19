// SPDX-License-Identifier: GPL-3.0-or-later
// HI-01: ready generations must not mutate the graph still being heard.
#include "adi/engine/host.hpp"
#include "juce/device_host.hpp"

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <stdexcept>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <malloc.h>
#endif

namespace {
std::atomic<bool> inAudio{false};
std::atomic<unsigned> audioAllocations{0}, audioFrees{0};
thread_local int failAllocation = -1;
void allocation() {
    if (inAudio.load(std::memory_order_relaxed)) audioAllocations.fetch_add(1, std::memory_order_relaxed);
    if (failAllocation == 0) { failAllocation = -1; throw std::bad_alloc(); }
    if (failAllocation > 0) --failAllocation;
}
void freeing(void* p) noexcept {
    if (p != nullptr && inAudio.load(std::memory_order_relaxed)) audioFrees.fetch_add(1, std::memory_order_relaxed);
}
}
void* operator new(std::size_t n) {
    allocation();
    if (void* p = std::malloc(n == 0 ? 1 : n)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { freeing(p); std::free(p); }
void operator delete[](void* p) noexcept { operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { operator delete(p); }
void* operator new(std::size_t n, std::align_val_t alignment) {
    allocation();
    const auto a = static_cast<std::size_t>(alignment);
#ifdef _WIN32
    if (void* p = _aligned_malloc(n == 0 ? 1 : n, a)) return p;
#else
    void* p = nullptr;
    if (posix_memalign(&p, a, n == 0 ? 1 : n) == 0) return p;
#endif
    throw std::bad_alloc();
}
void* operator new[](std::size_t n, std::align_val_t a) { return operator new(n, a); }
void operator delete(void* p, std::align_val_t) noexcept {
    freeing(p);
#ifdef _WIN32
    _aligned_free(p);
#else
    std::free(p);
#endif
}
void operator delete[](void* p, std::align_val_t a) noexcept { operator delete(p, a); }
void operator delete(void* p, std::size_t, std::align_val_t a) noexcept { operator delete(p, a); }
void operator delete[](void* p, std::size_t, std::align_val_t a) noexcept { operator delete(p, a); }

namespace {
using namespace adi;
using namespace adi::engine;
int checks = 0, failures = 0;
void check(bool value, const char* message) {
    ++checks;
    if (!value) { ++failures; std::printf("FAIL: %s\n", message); }
}
struct Probe {
    int prepared = 0, handed = 0, released = 0, calls = 0, emitted = 0;
    int adopted = 0, blocks = 0, destroyed = 0;
    bool badOrder = false, destroyedOnAudio = false;
};
class State final : public GraphTransportState {
public:
    explicit State(Probe& probe) : probe_(probe) {}
    void adopted() const noexcept override { ++probe_.adopted; }
    const TransportInfo* beginBlock() const noexcept override {
        if (probe_.adopted != 1) probe_.badOrder = true;
        ++probe_.blocks;
        return nullptr;
    }
private:
    Probe& probe_;
};
class Values final : public EventSource {
public:
    Values(Probe& probe, double value, bool ordered = false) : probe_(probe), value_(value), ordered_(ordered) {}
    ~Values() override {
        ++probe_.destroyed;
        probe_.destroyedOnAudio = probe_.destroyedOnAudio || inAudio.load(std::memory_order_relaxed);
    }
    void emit(EventList& out, std::int32_t) noexcept override {
        if (ordered_ && (probe_.adopted != 1 || probe_.blocks != probe_.emitted + 1)) probe_.badOrder = true;
        ++probe_.emitted;
        Event event;
        event.type = EventType::ParamValue;
        event.value = value_;
        (void)out.push(event);
    }
private:
    Probe& probe_;
    double value_;
    bool ordered_;
};
class Stateful : public Node {
public:
    explicit Stateful(Probe& probe) : probe_(probe) {}
    void prepare(double, std::int32_t) override {
        ++probe_.prepared;
        if (throwPrepare) throw std::runtime_error("deliberate new-node prepare refusal");
        value = 0.125;
    }
    void willBeHanded(Precision) override { ++probe_.handed; }
    void release() override { ++probe_.released; }
    std::uint64_t preparationShapeEpoch() const noexcept override { return shape; }
    bool processesDouble() const noexcept override { return doubleCapable; }
    bool canProcessOnWorker() const noexcept override { return workerSafe; }
    void process(const NodeIo& io) noexcept override {
        ++probe_.calls;
        for (const auto& event : io.events)
            if (event.type == EventType::ParamValue) value = event.value;
        for (std::int32_t c = 0; c < io.channels; ++c)
            for (std::int32_t i = 0; i < io.frames; ++i) {
                const auto at = io.blockOffset + i;
                if (io.out != nullptr && io.out[c] != nullptr) io.out[c][at] = static_cast<float>(value);
                if (io.out64 != nullptr && io.out64[c] != nullptr) io.out64[c][at] = value;
            }
    }
    double value = 0.125;
    std::uint64_t shape = 0;
    bool doubleCapable = true, workerSafe = false, throwPrepare = false;
private:
    Probe& probe_;
};
rows::Model model() {
    rows::Model value;
    rows::Track track; track.id = 1; track.kind = "audio"; track.name = "Track";
    value.tracks.push_back(track);
    track.id = 9; track.kind = "master"; track.name = "Main";
    value.tracks.push_back(track);
    return value;
}
RealizeOptions options(Node& node, std::shared_ptr<Values> values) {
    RealizeOptions result;
    result.devicesFor = [&node](std::int64_t track) {
        return track == 1 ? std::vector<Node*>{&node} : std::vector<Node*>{};
    };
    result.eventSourceFor = [&node, values = std::move(values)](const Node& target)
        -> std::optional<EventSourceBinding> {
        if (&target != &node) return std::nullopt;
        return EventSourceBinding{values.get(), values};
    };
    return result;
}
void render(GraphHost& host, double expected, bool fade = false) {
    std::array<std::array<float, 16>, 2> narrow{};
    std::array<std::array<double, 16>, 2> wide{};
    std::array<float*, 2> n{narrow[0].data(), narrow[1].data()};
    std::array<double*, 2> w{wide[0].data(), wide[1].data()};
    AudioIo io; io.out = n.data(); io.out64 = w.data(); io.numOut = 2; io.frames = 16;
    const auto allocations = audioAllocations.load(std::memory_order_relaxed);
    const auto frees = audioFrees.load(std::memory_order_relaxed);
    inAudio.store(true, std::memory_order_relaxed);
    host.process(io);
    inAudio.store(false, std::memory_order_relaxed);
    bool exact = true;
    for (std::size_t c = 0; c < narrow.size(); ++c)
        for (std::size_t i = 0; i < narrow[c].size(); ++i) {
            const double want = fade ? expected * static_cast<double>(i + 1) / 16.0 : expected;
            exact = exact && std::abs(static_cast<double>(narrow[c][i]) - want) < 1e-7 &&
                std::abs(wide[c][i] - want) < 1e-12;
        }
    check(exact, "literal PCM matches the selected generation in both output widths");
    check(audioAllocations.load(std::memory_order_relaxed) == allocations &&
              audioFrees.load(std::memory_order_relaxed) == frees,
          "graph adoption and processing allocate and free nothing on the callback");
}
void isolation(Precision precision) {
    Probe deviceProbe, oldProbe, nextProbe, restoreProbe;
    Stateful device(deviceProbe);
    GraphHost host; host.setPrecision(precision); host.setLatencyHeadroom(0);
    auto oldValues = std::make_shared<Values>(oldProbe, 0.25, true);
    auto nextValues = std::make_shared<Values>(nextProbe, 0.75, true);
    auto initial = host.prepare(model(), options(device, oldValues), 48000.0, 16, {}, std::make_shared<State>(oldProbe));
    check(initial.ok() && host.currentGraph() == nullptr && host.stats().published == 0,
          "preparing the first ready handle publishes nothing");
    if (!initial.graph) return;
    check(host.publish(*initial.graph) && !initial.graph->valid(), "first ready handle is consumed once on publish");
    render(host, 0.25);
    const auto* original = host.currentGraph();
    const auto calls = deviceProbe.calls;
    {
        auto discarded = host.prepare(model(), options(device, nextValues), 48000.0, 16, {},
                                      std::make_shared<State>(nextProbe), NodePreparePolicy::PreservePrepared);
        check(discarded.ok() && host.currentGraph() == original && deviceProbe.prepared == 1 && deviceProbe.handed == 1,
              "staging a candidate retains the running graph and never prepares its shared device");
    }
    check(deviceProbe.released == 0, "discarding a staged candidate never releases its shared device");
    render(host, 0.25);
    check(nextProbe.emitted == 0 && nextProbe.adopted == 0 && deviceProbe.calls == calls + 1,
          "discarded candidate never emits, adopts or processes the shared device");
    if (precision == Precision::Float64) {
        device.doubleCapable = false;
        auto incompatible = host.prepare(model(), options(device, nextValues), 48000.0, 16, {}, {},
                                         NodePreparePolicy::PreservePrepared);
        check(!incompatible.ok() && deviceProbe.prepared == 1 && deviceProbe.handed == 1,
              "changed handed width refuses preservation even without a shape epoch signal");
        device.doubleCapable = true;
    }
    auto restore = host.prepare(model(), options(device, std::make_shared<Values>(restoreProbe, 0.25, true)),
                                48000.0, 16, {}, std::make_shared<State>(restoreProbe), NodePreparePolicy::PreservePrepared);
    auto preview = host.prepare(model(), options(device, nextValues), 48000.0, 16, {},
                                std::make_shared<State>(nextProbe), NodePreparePolicy::PreservePrepared);
    check(restore.ok() && preview.ok(), "prepare both preview and fresh committed restoration buffers");
    if (!restore.graph || !preview.graph) return;
    host.setFadeFrames(16);
    check(host.publish(*preview.graph), "publish preview without a second device preparation");
    render(host, 0.75, true);
    check(deviceProbe.prepared == 1 && deviceProbe.handed == 1 && deviceProbe.calls == calls + 2,
          "fade-in still processes a stateful shared device exactly once");
    check(!host.publish(*preview.graph), "a consumed handle cannot publish twice");
    render(host, 0.75);
    host.setFadeFrames(0);
    check(host.publish(*restore.graph), "same-format preview does not invalidate its retained restoration handle");
    render(host, 0.25);
    check(oldProbe.adopted == 1 && oldProbe.blocks == 2 && nextProbe.adopted == 1 && nextProbe.blocks == 2 &&
              restoreProbe.adopted == 1 && restoreProbe.blocks == 1 &&
              !oldProbe.badOrder && !nextProbe.badOrder && !restoreProbe.badOrder,
          "adoption precedes source emission exactly once and beginBlock remains once per block");
    check(deviceProbe.prepared == 1 && deviceProbe.released == 0,
          "preview and restoration keep the same already prepared device");
    host.currentGraph()->release();
    check(deviceProbe.released == 1, "explicit stopped-host release still releases the device once");
}
void sourceSnapshotsAndLifetime() {
    Probe deviceProbe, a, b, c, legacy;
    Stateful device(deviceProbe);
    {
        GraphHost host; host.setLatencyHeadroom(0);
        auto first = std::make_shared<Values>(a, 0.25);
        device.setEventSource(first.get(), first);
        auto ordinary = options(device, {}); ordinary.eventSourceFor = {};
        check(host.rebuild(model(), ordinary, 48000.0, 16), "legacy node binding is captured by an ordinary graph");
        render(host, 0.25);
        auto replacement = std::make_shared<Values>(legacy, 0.875);
        device.setEventSource(replacement.get(), replacement);
        first.reset();
        render(host, 0.25);
        check(a.destroyed == 0 && legacy.emitted == 0,
              "changing standalone setup cannot redirect or free an already prepared graph source");
        auto second = host.prepare(model(), options(device, std::make_shared<Values>(b, 0.5)), 48000.0, 16,
                                   {}, std::make_shared<State>(b), NodePreparePolicy::PreservePrepared);
        auto third = host.prepare(model(), options(device, std::make_shared<Values>(c, 0.75)), 48000.0, 16,
                                  {}, std::make_shared<State>(c), NodePreparePolicy::PreservePrepared);
        check(second.ok() && third.ok(), "prepare independently owned next generations");
        if (!second.graph || !third.graph) return;
        check(host.publish(*second.graph) && host.publish(*third.graph), "publish two generations before one callback");
        check(host.collect() == 0 && a.destroyed == 0 && b.destroyed == 0,
              "reader still pins old and skipped generations before adoption");
        render(host, 0.75);
        check(b.emitted == 0 && b.adopted == 0 && c.emitted == 1 && c.adopted == 1,
              "only the actually selected generation emits or adopts");
        check(host.collect() == 2 && a.destroyed == 1 && b.destroyed == 1 && c.destroyed == 0,
              "owner collects only generations strictly older than the reader");
        auto none = host.prepare(model(), options(device, {}), 48000.0, 16, {}, {}, NodePreparePolicy::PreservePrepared);
        check(none.ok(), "an explicit empty event binding is a valid generation");
        if (!none.graph) return;
        device.value = 0.375;
        check(host.publish(*none.graph), "publish an explicitly disabled addressed source");
        render(host, 0.375);
        check(legacy.emitted == 0 && c.emitted == 1, "explicit null never falls back to either legacy or prior source");
        (void)host.collect();
        device.setEventSource(nullptr, {}); replacement.reset();
    }
    check(a.destroyed == 1 && b.destroyed == 1 && c.destroyed == 1 && legacy.destroyed == 1 &&
              !a.destroyedOnAudio && !b.destroyedOnAudio && !c.destroyedOnAudio && !legacy.destroyedOnAudio,
          "all source owners retire exactly once off the callback");
}
void refusalsAndStaleness() {
    Probe probe, otherProbe, valuesProbe;
    Stateful device(probe), other(otherProbe);
    GraphHost host; host.setLatencyHeadroom(0);
    auto values = std::make_shared<Values>(valuesProbe, 0.5);
    auto opts = options(device, values);
    check(host.rebuild(model(), opts, 48000.0, 16), "refusal fixture publishes its base");
    auto* original = host.currentGraph();
    const auto refused = [&](RealizeOptions candidate, double rate, std::int32_t frames) {
        const auto result = host.prepare(model(), candidate, rate, frames, {}, {}, NodePreparePolicy::PreservePrepared);
        check(!result.ok() && host.currentGraph() == original && probe.prepared == 1 && probe.handed == 1,
              "incompatible staged format refuses before touching shared device preparation");
    };
    refused(opts, 44100.0, 16); refused(opts, 48000.0, 32);
    auto mono = opts; mono.channels = 1; refused(mono, 48000.0, 16);
    host.setPrecision(Precision::Float64); refused(opts, 48000.0, 16); host.setPrecision(Precision::Float32);
    ++device.shape; refused(opts, 48000.0, 16); --device.shape;
    auto ready = host.prepare(model(), opts, 48000.0, 16, {}, {}, NodePreparePolicy::PreservePrepared);
    check(ready.ok(), "compatible format remains stageable after refused candidates");
    if (!ready.graph) return;
    GraphHost foreign;
    check(!foreign.publish(*ready.graph) && ready.graph->valid() && foreign.currentGraph() == nullptr,
          "foreign host refuses without consuming another host's ready graph");
    host.setLatencyHeadroom(1);
    check(!host.publish(*ready.graph) && ready.graph->valid() && host.currentGraph() == original,
          "configuration change invalidates a ready graph without consuming it");
    host.setLatencyHeadroom(0);
    ready = host.prepare(model(), opts, 48000.0, 16, {}, {}, NodePreparePolicy::PreservePrepared);
    if (!ready.graph) { check(false, "restage after configuration change"); return; }
    ++device.shape;
    check(!host.publish(*ready.graph) && ready.graph->valid(), "shape change after preparation rejects publication");
    --device.shape;
    check(host.rebuild(model(), opts, 48000.0, 16), "ordinary prepare-all rebuild advances node preparation revision");
    check(!host.publish(*ready.graph) && ready.graph->valid(), "same-format node re-preparation makes an older ready graph stale");
    original = host.currentGraph();
    auto throwing = opts;
    throwing.eventSourceFor = [](const Node&) -> std::optional<EventSourceBinding> { throw std::runtime_error("provider refusal"); };
    const auto badProvider = host.prepare(model(), throwing, 48000.0, 16, {}, {}, NodePreparePolicy::PreservePrepared);
    check(!badProvider.ok() && host.currentGraph() == original && probe.prepared == 2,
          "throwing event supplier refuses before changing a live node or publication");
    other.throwPrepare = true;
    auto withNew = opts;
    withNew.devicesFor = [&device, &other](std::int64_t id) {
        return id == 1 ? std::vector<Node*>{&device, &other} : std::vector<Node*>{};
    };
    const auto badNew = host.prepare(model(), withNew, 48000.0, 16, {}, {}, NodePreparePolicy::PreservePrepared);
    check(!badNew.ok() && host.currentGraph() == original && probe.prepared == 2 && otherProbe.prepared == 1,
          "a fresh node preparation failure cannot re-prepare an earlier shared live node");
    render(host, 0.5);
    auto beforeRelease = host.prepare(model(), opts, 48000.0, 16, {}, {}, NodePreparePolicy::PreservePrepared);
    if (!beforeRelease.graph) { check(false, "stage before explicit release"); return; }
    host.currentGraph()->release();
    check(!host.publish(*beforeRelease.graph) && beforeRelease.graph->valid(), "release invalidates already staged node revisions");
    const auto released = host.prepare(model(), opts, 48000.0, 16, {}, {}, NodePreparePolicy::PreservePrepared);
    check(!released.ok(), "preserve policy never silently reactivates a known released node");
    std::unique_ptr<GraphHost::PreparedGraph> expired;
    {
        GraphHost temporary;
        auto candidate = temporary.prepare(model(), {}, 48000.0, 16);
        expired = std::move(candidate.graph);
    }
    check(expired && !expired->valid() && !foreign.publish(*expired), "a retained handle cannot outlive its creating host identity");
}
void preparationPreflight() {
    Probe aProbe, bProbe, freshProbe;
    Stateful a(aProbe), b(bProbe), fresh(freshProbe);
    GraphHost host; host.setLatencyHeadroom(0);
    RealizeOptions opts;
    opts.devicesFor = [&a, &b](std::int64_t id) {
        return id == 1 ? std::vector<Node*>{&a, &b} : std::vector<Node*>{};
    };
    check(host.rebuild(model(), opts, 48000.0, 16), "whole-candidate preflight baseline");
    auto* original = host.currentGraph();
    ++b.shape;
    auto candidate = opts;
    candidate.devicesFor = [&fresh, &a, &b](std::int64_t id) {
        return id == 1 ? std::vector<Node*>{&fresh, &a, &b} : std::vector<Node*>{};
    };
    const auto refused = host.prepare(model(), candidate, 48000.0, 16, {}, {}, NodePreparePolicy::PreservePrepared);
    check(!refused.ok() && freshProbe.prepared == 0 && freshProbe.handed == 0 &&
              aProbe.prepared == 1 && bProbe.prepared == 1 && host.currentGraph() == original,
          "late incompatible node refuses before even a preceding fresh node is prepared");
    --b.shape;

    Probe parallelA, parallelB, workerState;
    Stateful sourceA(parallelA), sourceB(parallelB);
    sourceA.workerSafe = true; sourceB.workerSafe = true;
    auto parallelModel = model();
    auto secondTrack = parallelModel.tracks.front(); secondTrack.id = 2;
    parallelModel.tracks.insert(parallelModel.tracks.begin() + 1, secondTrack);
    RealizeOptions parallelOptions;
    parallelOptions.devicesFor = [&sourceA, &sourceB](std::int64_t id) {
        if (id == 1) return std::vector<Node*>{&sourceA};
        if (id == 2) return std::vector<Node*>{&sourceB};
        return std::vector<Node*>{};
    };
    GraphHost parallel; parallel.setLatencyHeadroom(0);
    check(parallel.rebuild(parallelModel, parallelOptions, 48000.0, 16), "parallel preparation baseline");
    original = parallel.currentGraph();
    const auto rejectWorker = [](void*, std::size_t) noexcept { return false; };
    check(parallel.setParallelRendering(1, 1, 2, rejectWorker), "admit deterministic failing worker initializer");
    const auto failed = parallel.prepare(parallelModel, parallelOptions, 48000.0, 16, {}, {},
                                         NodePreparePolicy::PreservePrepared);
    check(!failed.ok() && parallel.currentGraph() == original && parallelA.prepared == 1 && parallelB.prepared == 1,
          "worker initialization failure leaves all shared node preparation and current audio intact");
    render(parallel, 0.25);
    check(parallel.setParallelRendering(1, 1, 2), "admit normal worker initialization");
    parallelOptions.eventSourceFor = options(sourceA, std::make_shared<Values>(workerState, 0.125, true)).eventSourceFor;
    auto working = parallel.prepare(parallelModel, parallelOptions, 48000.0, 16, {}, std::make_shared<State>(workerState),
                                    NodePreparePolicy::PreservePrepared);
    check(working.ok(), "same compatible nodes can retry with ready worker resources");
    if (!working.graph) return;
    check(parallel.publish(*working.graph), "publish prepared worker generation");
    const auto callsA = parallelA.calls, callsB = parallelB.calls;
    render(parallel, 0.25);
    check(parallelA.calls == callsA + 1 && parallelB.calls == callsB + 1 &&
              parallelA.prepared == 1 && parallelB.prepared == 1,
          "worker adoption processes each retained stateful node once without re-preparation");
    check(workerState.adopted == 1 && workerState.blocks == 1 && workerState.emitted == 1 && !workerState.badOrder,
          "generation adoption and clock selection precede addressed events and worker rendering");
}

struct Counted final : Sequenced {
    Counted(int& count, int value) : destroyed(count), number(value) {}
    ~Counted() override { ++destroyed; }
    int& destroyed;
    int number;
};
void publisherFailureOwnership() {
    int destroyed = 0;
    {
        SnapshotPublisher<Counted> publisher;
        auto first = std::make_unique<Counted>(destroyed, 1); publisher.publishPrepared(first);
        auto next = std::make_unique<Counted>(destroyed, 2);
        const auto* original = publisher.peek();
        bool refused = false;
        failAllocation = 0;
        try { publisher.publishPrepared(next); } catch (const std::bad_alloc&) { refused = true; }
        failAllocation = -1;
        check(refused && next && publisher.peek() == original && publisher.publishedSeq() == 1 &&
                  publisher.retainedCount() == 0 && destroyed == 0,
              "retirement allocation failure preserves publication, sequence and caller ownership");
        publisher.publishPrepared(next);
        check(!next && publisher.peek()->number == 2 && publisher.publishedSeq() == 2,
              "the same owned candidate retries after publication preflight failure");
        publisher.announce(publisher.peek());
        check(publisher.collect() == 1 && destroyed == 1, "successful retry retires the original exactly once");
    }
    check(destroyed == 2, "publisher shutdown owns the final candidate");
    // MSVC Debug allocates an empty vector's iterator proxy. Walk allocation
    // positions instead of assuming reserve is the first allocation in collect.
    bool sawRefusal = false, sawSuccess = false;
    for (int budget = 0; budget < 4; ++budget) {
        int freed = 0;
        {
            SnapshotPublisher<Counted> publisher;
            for (int id = 0; id < 3; ++id) publisher.publish(std::make_unique<Counted>(freed, id));
            publisher.announce(publisher.peek());
            bool refused = false;
            std::size_t count = 0;
            failAllocation = budget;
            try { count = publisher.collect(); } catch (const std::bad_alloc&) { refused = true; }
            failAllocation = -1;
            if (refused) {
                sawRefusal = true;
                check(freed == 0 && publisher.retainedCount() == 2,
                      "collect allocation failure occurs before erasing any owned retired pointer");
                check(publisher.collect() == 2 && freed == 2, "collect retry still owns every retired generation");
            } else {
                sawSuccess = true;
                check(count == 2 && freed == 2 && publisher.retainedCount() == 0,
                      "collection succeeds once its temporary storage is admitted");
            }
        }
        check(freed == 3, "collection fault positions never leak erased raw pointers");
    }
    check(sawRefusal && sawSuccess, "allocation walk exercised both refused and successful collection");
    Probe probe, sourceProbe;
    Stateful node(probe);
    GraphHost host; host.setLatencyHeadroom(0);
    auto opts = options(node, std::make_shared<Values>(sourceProbe, 0.5));
    check(host.rebuild(model(), opts, 48000.0, 16), "ready retry fixture base");
    auto ready = host.prepare(model(), opts, 48000.0, 16, {}, {}, NodePreparePolicy::PreservePrepared);
    if (!ready.graph) { check(false, "ready retry candidate"); return; }
    auto* original = host.currentGraph();
    failAllocation = 0;
    const bool published = host.publish(*ready.graph);
    failAllocation = -1;
    check(!published && ready.graph->valid() && host.currentGraph() == original && host.stats().published == 1,
          "GraphHost does not lose a ready cancellation graph on publisher preflight failure");
    check(host.publish(*ready.graph) && !ready.graph->valid(), "GraphHost retries the exact preserved ready handle");
    render(host, 0.5);
}
class Instrument final : public device::DeviceInstance {
public:
    explicit Instrument(Probe& probe) : node(probe) {}
    const device::DeviceIdentity& identity() const noexcept override { return identity_; }
    bool loaded() const noexcept override { return true; }
    bool processesDouble() const noexcept override { return true; }
    std::uint64_t shapeEpoch() const noexcept override { return node.shape; }
    void prepare(double rate, std::int32_t frames) override { node.prepare(rate, frames); }
    void willBeHanded(Precision width) override { node.willBeHanded(width); }
    void release() override { node.release(); }
    void process(const NodeIo& io) noexcept override { node.process(io); }
    Stateful node;
private:
    device::DeviceIdentity identity_;
};
void deviceHostPropagation() {
    Probe deviceProbe, firstProbe, nextProbe;
    device::DeviceHost devices;
    GraphHost graph; graph.setLatencyHeadroom(0);
    auto instance = std::make_unique<Instrument>(deviceProbe);
    auto* instrument = instance.get();
    auto& node = devices.add(std::move(instance), "stable instance", 1);
    const auto rows = model();
    device::DeviceHost::RebuildSpec spec;
    spec.model = [&rows] { return &rows; }; spec.sampleRate = 48000.0; spec.maxFrames = 16;
    spec.eventSourceFor = options(node, std::make_shared<Values>(firstProbe, 0.25)).eventSourceFor;
    devices.attachHost(graph, spec);
    check(devices.rebuildNow(), "DeviceHost first rebuild passes its explicit addressed source"); render(graph, 0.25);
    spec.eventSourceFor = options(node, std::make_shared<Values>(nextProbe, 0.75)).eventSourceFor;
    spec.nodePreparation = NodePreparePolicy::PreservePrepared;
    devices.attachHost(graph, spec);
    check(devices.rebuildNow() && deviceProbe.prepared == 1 && deviceProbe.handed == 1,
          "DeviceHost forwards both generation binding and preserve preparation policy");
    render(graph, 0.75);
    ++instrument->node.shape;
    devices.coalescer().setQuietPeriodMs(1);
    (void)devices.tick(100); (void)devices.tick(102);
    check(devices.stats().rebuildsFailed == 1 && deviceProbe.prepared == 1,
          "coalescer shape change cannot bypass fixed-format preserve admission");
}
}
int main() {
    try {
        isolation(Precision::Float32); isolation(Precision::Float64);
        sourceSnapshotsAndLifetime(); refusalsAndStaleness(); preparationPreflight();
        publisherFailureOwnership(); deviceHostPropagation();
    } catch (const std::exception& exception) {
        failAllocation = -1; inAudio.store(false, std::memory_order_relaxed);
        check(false, exception.what());
    }
    std::printf("gesture graph: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
