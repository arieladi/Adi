// SPDX-License-Identifier: GPL-3.0-or-later
// ADR-0203 d4: bypass runs no DSP, retains delay, and keeps real history.
#include "adi/engine/host.hpp"
#include "juce/device_model.hpp"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>

namespace {
std::atomic<int> allocations{0};
bool countAllocations = false;
}
void* operator new(std::size_t n) {
    if (countAllocations) ++allocations;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {
using namespace adi;
using namespace adi::engine;
using namespace adi::device;
int checks = 0, failures = 0;
void check(bool ok, const char* label) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL: %s\n", label); }
}

double sample(int at, int channel, Precision precision) {
    if (at < 0) return 0.0;
    const double value = 1.0 + at / 1024.0 + channel / 4.0 + 0x1p-40;
    return precision == Precision::Float64 ? value : static_cast<float>(value);
}

class Source final : public Node {
public:
    int position = 0;
    int stop = 100000;
    double gain = 1.0;
    Precision sourcePrecision = Precision::Float64;
    bool processesDouble() const noexcept override { return true; }
    void process(const NodeIo& io) noexcept override {
        for (int c = 0; c < io.channels; ++c)
            for (int i = 0; i < io.frames; ++i) {
                const double v = position + i < stop ? gain * sample(position + i, c, sourcePrecision) : 0.0;
                if (io.wide()) io.out64[c][io.blockOffset + i] = v;
                else io.out[c][io.blockOffset + i] = static_cast<float>(v);
            }
        position += io.frames;
    }
};

// A float-only processor that actually delays and colours its input. Its
// stable instance survives graph rebuilds, as a hosted device does.
class Latent final : public DeviceInstance {
public:
    int delay = 51, calls = 0;
    mutable int latencyQueries = 0;
    DeviceIdentity id;
    std::array<std::array<float, 512>, 2> ring{};
    int write = 0;
    const DeviceIdentity& identity() const noexcept override { return id; }
    bool loaded() const noexcept override { return true; }
    std::int32_t latencySamples() const noexcept override {
        ++latencyQueries;
        return delay;
    }
    std::int64_t tailSamples() const noexcept override { return delay; }
    void process(const NodeIo& io) noexcept override {
        ++calls;
        for (int i = 0; i < io.frames; ++i) {
            const int read = (write + 512 - delay) % 512;
            for (int c = 0; c < io.channels; ++c) {
                auto& r = ring[static_cast<std::size_t>(c)];
                const float in = io.in ? io.in[c][io.blockOffset + i] : 0.0f;
                const float out = delay ? r[static_cast<std::size_t>(read)] : in;
                r[static_cast<std::size_t>(write)] = in;
                io.out[c][io.blockOffset + i] = 2.0f * out;
            }
            write = (write + 1) % 512;
        }
    }
};

struct Output {
    std::array<std::array<float, 256>, 2> floats{};
    std::array<std::array<double, 256>, 2> doubles{};
    std::array<float*, 2> fp{floats[0].data(), floats[1].data()};
    std::array<double*, 2> dp{doubles[0].data(), doubles[1].data()};
    AudioIo io(int frames) {
        AudioIo out;
        out.out = fp.data(); out.out64 = dp.data(); out.numOut = 2; out.frames = frames;
        return out;
    }
    bool matches(int start, int delay, int frames, Precision precision, int stop = 100000) const {
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < frames; ++i) {
                const int at = start + i - delay;
                const double expected = at < stop ? sample(at, c, precision) : 0.0;
                const double got = precision == Precision::Float64
                    ? doubles[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)]
                    : floats[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)];
                if (got != expected) return false;
            }
        return true;
    }
};

std::unique_ptr<Graph> graph(Source& source, DeviceNode& node, Precision precision, int headroom = 64) {
    auto g = std::make_unique<Graph>();
    g->setPrecision(precision); g->setLatencyHeadroom(headroom);
    const auto src = g->addNode(source), dev = g->addNode(node);
    g->connect(src, dev); g->setOutput(dev); g->prepare(48000.0, 256);
    check(g->ok(), "graph prepares");
    return g;
}

void basic(Precision precision) {
    Source source;
    Latent device;
    device.delay = 151;
    DeviceNode node(device);
    node.setBypassed(true);
    auto g = graph(source, node, precision);
    Output out;
    bool exact = true;
    for (int block = 0; block < 20; ++block) {
        const int frames = block % 2 ? 129 : 97;
        const int start = source.position;
        Event event{}; event.frame = 70;
        g->pushInputEvent(g->find(source), event); // nonzero segment offsets
        g->process(out.io(frames));
        exact = exact && out.matches(start, device.delay, frames, precision);
    }
    check(exact, "stereo segmented bypass is an exact delay, longer than a block");
    check(g->stats().segments > g->stats().blocks, "segment-offset test actually splits blocks");
    check(device.calls == 0, "bypass never calculates the device");
    check(g->latencySamples() == device.delay, "bypass retains graph latency");

    node.setBypassed(false);
    for (int block = 0; block < 6; ++block) g->process(out.io(32));
    bool wet = true;
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < 32; ++i) {
            const double expected = 2.0 * sample(source.position - 32 + i - device.delay, c, Precision::Float32);
            if (out.doubles[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)] != expected) wet = false;
        }
    check(wet, "active output is coloured DSP, never overwritten by dry history");
    const int calls = device.calls;
    const int start = source.position;
    node.setBypassed(true);
    g->process(out.io(32));
    check(calls > 0, "active control really runs DSP");
    check(out.matches(start, device.delay, 32, precision), "first bypass block uses active-period dry history");
    check(device.calls == calls, "toggle stops DSP immediately at next block");

    source.stop = source.position + 5;
    exact = true;
    for (int block = 0; block < 10; ++block) {
        const int begin = source.position;
        g->process(out.io(19));
        exact = exact && out.matches(begin, device.delay, 19, precision, source.stop);
    }
    check(exact, "dry tail drains entirely and stays silent");
    check(g->stats().nodesSuspended > 0, "fully drained bypass can sleep");
    source.stop = 100000;
    const int wake = source.position;
    g->process(out.io(32));
    bool quiet = true;
    for (int i = 0; i < 32; ++i)
        if (out.floats[0][static_cast<std::size_t>(i)] != 0.0f) quiet = false;
    check(quiet && source.position > wake, "wake does not replay pre-sleep audio");
    bool resumed = true;
    for (int block = 0; block < 6; ++block) {
        const int start = source.position;
        g->process(out.io(32));
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < 32; ++i) {
                const int at = start + i - device.delay;
                const double expected = at < wake ? 0.0 : sample(at, c, precision);
                if (out.doubles[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)] != expected) resumed = false;
            }
    }
    check(resumed, "resumed onset arrives after exactly the retained delay");
}

void retap(Precision precision) {
    Source source; Latent device; device.delay = 5;
    DeviceNode node(device); node.setBypassed(true);
    auto g = graph(source, node, precision, 16);
    Output out;
    for (int b = 0; b < 5; ++b) g->process(out.io(32));
    device.delay = 13;
    check(g->retapLatency(), "bypass latency retaps within reserved history");
    g->process(out.io(32));
    int begin = source.position;
    g->process(out.io(32));
    check(out.matches(begin, 13, 32, precision), "retap settles to new dry delay");
    device.delay = 93;
    check(!g->retapLatency(), "larger bypass delay requires growth");
    check(g->escalateLatency() > 0, "message thread allocates a larger bypass ring");
    bool continuous = true;
    for (int b = 0; b < 12; ++b) {
        const int start = source.position;
        g->process(out.io(32));
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < 32; ++i) {
                const double value = out.doubles[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)];
                const double low = sample(start + i - 93, c, precision);
                const double high = sample(start + i - 13, c, precision);
                if (!(value >= low && value <= high)) continuous = false;
            }
    }
    check(continuous, "growth and crossfade emit only old/new history, never zeros or NaN");
    begin = source.position;
    g->process(out.io(32));
    check(out.matches(begin, 93, 32, precision), "grown ring settles to the requested delay");
    check(g->collectRings() > 0 && g->growingEdges() == 0, "retired bypass ring is reclaimed off-thread");
    check(device.calls == 0, "retapping bypass never calls DSP");
}

void rebuild(bool active, bool changePrecision) {
    Source source;
    // Float-exact samples isolate handover from the unavoidable old-width
    // history conversion when switching precision.
    source.sourcePrecision = Precision::Float32;
    Latent device; DeviceNode node(device); node.setBypassed(!active);
    GraphHost host; host.setFadeFrames(0);
    rows::Model model;
    rows::Track track; track.id = 1; track.kind = "audio"; model.tracks.push_back(track);
    track.id = 9; track.kind = "master"; model.tracks.push_back(track);
    RealizeOptions opts;
    opts.devicesFor = [&](std::int64_t id) -> std::vector<Node*> {
        if (id == 1) return {&source, &node};
        return {};
    };
    check(host.rebuild(model, opts, 48000.0, 64), "host publishes first graph");
    Output out;
    for (int b = 0; b < 8; ++b) host.process(out.io(32));
    if (changePrecision) host.setPrecision(Precision::Float64);
    check(host.rebuild(model, opts, 48000.0, 32), "host rebuilds with a new maximum block size");
    check(host.currentGraph()->precision() == (changePrecision ? Precision::Float64 : Precision::Float32),
          "precision-switch case really builds the selected width");
    node.setBypassed(true);
    const int begin = source.position, calls = device.calls;
    const int queries = device.latencyQueries;
    allocations = 0; countAllocations = true;
    host.process(out.io(32));
    countAllocations = false;
    check(out.matches(begin, device.delay, 32, Precision::Float32), "host swap retains active or bypassed dry history");
    check(host.stats().historyCarried > 0, "handover actually carried device history");
    check(allocations == 0, "graph swap and dry processing allocate nothing");
    check(device.latencyQueries == queries, "audio callback never queries plug-in latency");
    check(device.calls == calls, "rebuilt bypass runs no DSP");
    if (changePrecision) {
        source.sourcePrecision = Precision::Float64;
        for (int b = 0; b < 4; ++b) host.process(out.io(32));
        check(out.matches(source.position - 32, device.delay, 32, Precision::Float64),
              "after carried history, new double samples retain their low bits");
        source.sourcePrecision = Precision::Float32;
        for (int b = 0; b < 4; ++b) host.process(out.io(32));
    }
    source.stop = source.position + 5;
    host.process(out.io(19));
    check(host.rebuild(model, opts, 48000.0, 64), "rebuild during dry drain");
    bool drained = true;
    for (int b = 0; b < 8; ++b) {
        const int at = source.position;
        host.process(out.io(17));
        drained = drained && out.matches(at, device.delay, 17, Precision::Float32, source.stop);
    }
    check(drained, "rebuilt dry tail completes even though the source is already silent");
    // Removing the node releases compensation, as switching it off must not.
    opts.devicesFor = [&](std::int64_t id) -> std::vector<Node*> {
        if (id == 1) return {&source};
        return {};
    };
    check(host.rebuild(model, opts, 48000.0, 64) && host.currentGraph()->latencySamples() == 0,
          "deleting the device releases its delay");
}

// A silent branch declaring latency forces compensation on the live branch.
// It cannot supply audio that might conceal the compensated branch's drain.
class SilentLatency final : public Node {
public:
    std::int32_t latencySamples() const noexcept override { return 73; }
    std::int64_t tailSamples() const noexcept override { return 0; }
    bool processesDouble() const noexcept override { return true; }
    void process(const NodeIo& io) noexcept override { passThrough(io); }
};

void compensatedDrain(Precision precision) {
    Source source; source.stop = 5;
    SilentLatency silent;
    Latent device; DeviceNode node(device); node.setBypassed(true);
    Graph g; g.setPrecision(precision);
    const auto a = g.addNode(source), b = g.addNode(silent), d = g.addNode(node);
    g.connect(a, d); g.connect(b, d); g.setOutput(d); g.prepare(48000.0, 64);
    Output out; bool exact = true;
    for (int block = 0; block < 16; ++block) {
        const int start = source.position;
        g.process(out.io(16));
        exact = exact && out.matches(start, 73 + device.delay, 16, precision, source.stop);
    }
    check(exact, "compensated input still drains through bypass after upstream silence");
    check(g.latencySamples() == 73 + device.delay, "input compensation and bypass latency add");

    // No input may reuse a previous node's mixed scratch. Always-process
    // keeps this case running so it cannot pass by suspension alone.
    DeviceNode empty(device); empty.setBypassed(true); empty.setAlwaysProcess(true);
    Graph noInput; noInput.setPrecision(precision);
    noInput.addNode(source);
    noInput.setOutput(noInput.addNode(empty)); noInput.prepare(48000.0, 64);
    const int calls = device.calls;
    bool zero = true;
    for (int block = 0; block < 8; ++block) {
        noInput.process(out.io(32));
        for (int i = 0; i < 32; ++i)
            if (out.doubles[0][static_cast<std::size_t>(i)] != 0.0) zero = false;
    }
    check(zero && device.calls == calls, "always-process bypass with no input writes silence without DSP");

    Source zeroSource; Latent zeroDevice; zeroDevice.delay = 0;
    DeviceNode zeroNode(zeroDevice); zeroNode.setBypassed(true);
    auto zeroGraph = graph(zeroSource, zeroNode, precision, 0);
    zeroGraph->process(out.io(32));
    check(out.matches(0, 0, 32, precision), "zero-latency bypass is exact with no allocated delay ring");
}

void parallelAlignment(Precision precision) {
    Source wetSource, drySource;
    wetSource.sourcePrecision = Precision::Float32;
    drySource.sourcePrecision = Precision::Float32;
    drySource.gain = -2.0;
    Latent device; DeviceNode node(device);
    SumNode sum;
    Graph g; g.setPrecision(precision);
    const auto w = g.addNode(wetSource), d = g.addNode(drySource);
    const auto fx = g.addNode(node), outId = g.addNode(sum);
    g.connect(w, fx); g.connect(fx, outId); g.connect(d, outId); g.setOutput(outId);
    g.prepare(48000.0, 64);
    Output out;
    for (int b = 0; b < 6; ++b) g.process(out.io(32));
    bool null = true;
    for (int i = 0; i < 32; ++i) if (out.doubles[0][static_cast<std::size_t>(i)] != 0.0) null = false;
    check(null, "parallel dry path cancels the delayed active processor exactly");
    node.setBypassed(true);
    check(g.retapLatency(), "bypass needs no new compensation");
    const int start = wetSource.position;
    g.process(out.io(32));
    bool aligned = true;
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < 32; ++i)
            if (out.doubles[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)] !=
                -sample(start + i - device.delay, c, Precision::Float32)) aligned = false;
    check(aligned, "bypass removes colour while sibling and dry remain sample-aligned");
}
}

int main() {
    countAllocations = true;
    void* control = ::operator new(7);
    countAllocations = false;
    ::operator delete(control);
    check(allocations > 0, "allocation counter positive control");
    for (Precision precision : {Precision::Float32, Precision::Float64}) {
        basic(precision);
        retap(precision);
        compensatedDrain(precision);
        parallelAlignment(precision);
    }
    for (bool active : {false, true})
        for (bool changePrecision : {false, true}) rebuild(active, changePrecision);
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
