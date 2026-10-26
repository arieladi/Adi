// SPDX-License-Identifier: GPL-3.0-or-later
//
// Precision of the mix points -- ADR-0207 d2 (closing ADR-0205 d6 and d10).
//
// Live processes in 32-bit float and sums in 64-bit double at every mix point
// (Live 12 manual, Audio Fact Sheet 38.2.4). Its own test is the one this file
// is built around: pairs of 24-bit files and their phase-inverted copies,
// summed many times, cancel to exact zero (ADR-0205 d6's statement of the
// case; the manual's text is not in this checkout). That property is not an
// accident of any one order. In double, a sum of 24-bit samples is EXACT for
// any order of any number of them a project could hold (53 bits of mantissa
// against 24 bits of sample plus the count), so +x and -x meet as exact
// negatives wherever they meet. In float, every partial sum past the first
// rounds to 24 bits, so the copies arrive at a different rounding than the
// originals left, and the residue is audible as a noise floor that is not
// there in the source.
//
// So each check here is the cancellation case at one mix point, and each is
// run against the same sum done in float beside it, which must NOT cancel --
// otherwise the case would pass on a float graph and prove nothing.
//
// The mix points (ADR-0207 d2's list, as they exist in the tree today):
//   - a node's main inputs: track inputs, group sums, the master sum
//   - a node's sidechain inputs
//   - the clips on one track (`TrackSource`)
// Sends sum through the first of these once they play (LANES item 2).

#include "temp_directory.hpp"

#include "adi/audio/wav_file.hpp"
#include "adi/engine/graph.hpp"
#include "adi/engine/mixer.hpp"
#include "adi/engine/session.hpp"
#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

using namespace adi;
using namespace adi::engine;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool cond, const std::string& what) {
    ++g_checks;
    if (!cond) { ++g_failures; std::printf("  FAIL  %s\n", what.c_str()); }
}
void section(const char* s) { std::printf("[%s]\n", s); }

constexpr double kFull = 8388608.0;   // 2^23: a 24-bit sample is k / 2^23

/// One channel of 24-bit noise, deterministic across standard libraries:
/// mt19937's output sequence is specified, its distributions are not, so the
/// mapping from a draw to a sample is written here. `floorBits` keeps every
/// sample at or above 2^floorBits / 2^23 in magnitude (see the strip case).
std::vector<float> noise24(std::uint32_t seed, std::size_t frames, int floorBits = 0) {
    std::mt19937 random(seed);
    std::vector<float> v(frames);
    const std::uint32_t lo = std::uint32_t{1} << floorBits;
    const std::uint32_t span = (std::uint32_t{1} << 23) - lo;   // |k| in [lo, 2^23)
    for (auto& x : v) {
        const auto draw = static_cast<std::uint32_t>(random());
        const auto k = static_cast<std::int32_t>(lo + (draw >> 1) % span);
        x = static_cast<float>(((draw & 1u) != 0u ? -k : k) / kFull);
    }
    return v;
}

/// The same sum the graph would do if it summed in float: in this order,
/// rounding after every add. The largest magnitude it leaves is the residue a
/// 32-bit mix point leaves. Kept beside every check so the case is shown to
/// DISCRIMINATE: a case that cancels in float as well proves nothing.
float floatResidue(const std::vector<const std::vector<float>*>& inOrder, std::size_t frames) {
    float worst = 0.0f;
    for (std::size_t i = 0; i < frames; ++i) {
        float s = 0.0f;
        for (const auto* v : inOrder) s += (*v)[i];
        worst = (std::max)(worst, std::fabs(s));
    }
    return worst;
}

float largest(const std::vector<float>& v) {
    float worst = 0.0f;
    for (float x : v) worst = (std::max)(worst, std::fabs(x));
    return worst;
}

std::string show(float x) {
    char b[64];
    std::snprintf(b, sizeof b, "%.9g", static_cast<double>(x));
    return b;
}

// --- graph fixtures ---------------------------------------------------------

/// Plays a fixed buffer, the same on every channel, advancing by each block.
/// A generator, so it keeps the conservative infinite tail and never sleeps.
class PlayNode final : public Node {
public:
    explicit PlayNode(const std::vector<float>& data) : data_(&data) {}
    void process(const NodeIo& io) noexcept override {
        if (io.blockOffset == 0) { base_ += end_; end_ = 0; }
        const std::size_t n = data_->size();
        for (std::int32_t c = 0; c < io.channels; ++c)
            for (std::int32_t i = 0; i < io.frames; ++i)
                io.out[c][io.blockOffset + i] =
                    (*data_)[(base_ + static_cast<std::size_t>(io.blockOffset + i)) % n];
        end_ = (std::max)(end_, static_cast<std::size_t>(io.blockOffset + io.frames));
    }
    [[nodiscard]] const char* name() const noexcept override { return "play"; }
private:
    const std::vector<float>* data_;
    std::size_t base_ = 0;
    std::size_t end_ = 0;
};

/// Delays its input by a declared latency, so the graph compensates every
/// other path into the same mix point and the sum runs through delay lines.
class LateNode final : public Node {
public:
    explicit LateNode(std::int32_t latency) : latency_(latency) {}
    void prepare(double, std::int32_t) override {
        ring_.assign(2 * static_cast<std::size_t>(latency_ + 1), 0.0f);
        write_ = 0;
    }
    void process(const NodeIo& io) noexcept override {
        const auto ring = static_cast<std::size_t>(latency_ + 1);
        std::size_t w = 0;
        for (std::int32_t c = 0; c < io.channels && c < 2; ++c) {
            float* hist = ring_.data() + static_cast<std::size_t>(c) * ring;
            w = write_;
            for (std::int32_t i = 0; i < io.frames; ++i) {
                const float x = io.in != nullptr ? io.in[c][io.blockOffset + i] : 0.0f;
                hist[w] = x;
                std::size_t r = w + 1;          // the oldest: `latency` writes ago
                if (r == ring) r = 0;
                io.out[c][io.blockOffset + i] = hist[r];
                if (++w == ring) w = 0;
            }
        }
        write_ = w;
    }
    [[nodiscard]] std::int32_t latencySamples() const noexcept override { return latency_; }
    [[nodiscard]] std::int64_t tailSamples() const noexcept override { return latency_; }
    [[nodiscard]] const char* name() const noexcept override { return "late"; }
private:
    std::int32_t latency_;
    std::vector<float> ring_;
    std::size_t write_ = 0;
};

/// Passes its SIDECHAIN input out, so the sidechain sum is what is observed.
class KeyOutNode final : public Node {
public:
    void process(const NodeIo& io) noexcept override {
        for (std::int32_t c = 0; c < io.channels; ++c)
            for (std::int32_t i = 0; i < io.frames; ++i)
                io.out[c][io.blockOffset + i] =
                    io.sidechain != nullptr ? io.sidechain[c][io.blockOffset + i] : 0.0f;
    }
    [[nodiscard]] bool alwaysProcess() const noexcept override { return true; }
    [[nodiscard]] const char* name() const noexcept override { return "key out"; }
};

struct Render {
    std::vector<float> l, r;
    std::vector<double> l64, r64;   ///< the same output through AudioIo::out64
};

/// Renders `frames` in blocks of `block`, both channels, concatenated, at
/// both widths of the output.
Render renderGraph(Graph& g, std::int32_t frames, std::int32_t block) {
    Render out;
    const auto n0 = static_cast<std::size_t>(frames);
    out.l.resize(n0); out.r.resize(n0); out.l64.resize(n0); out.r64.resize(n0);
    for (std::int32_t at = 0; at < frames; at += block) {
        const std::int32_t n = (std::min)(block, frames - at);
        float* ptrs[2] = {out.l.data() + at, out.r.data() + at};
        double* wide[2] = {out.l64.data() + at, out.r64.data() + at};
        AudioIo io;
        io.out = ptrs;
        io.out64 = wide;
        io.numOut = 2;
        io.frames = n;
        g.process(io);
    }
    return out;
}

/// Plays a fixed buffer of doubles, the same on every channel, at either
/// width: a source that `processesDouble()`, so a graph at 64 keeps every bit.
class WidePlayNode final : public Node {
public:
    explicit WidePlayNode(const std::vector<double>& data) : data_(&data) {}
    void process(const NodeIo& io) noexcept override {
        if (io.blockOffset == 0) { base_ += end_; end_ = 0; }
        const std::size_t n = data_->size();
        for (std::int32_t c = 0; c < io.channels; ++c)
            for (std::int32_t i = 0; i < io.frames; ++i) {
                const double x = (*data_)[(base_ + static_cast<std::size_t>(io.blockOffset + i)) % n];
                if (io.wide()) io.out64[c][io.blockOffset + i] = x;
                else io.out[c][io.blockOffset + i] = static_cast<float>(x);
            }
        end_ = (std::max)(end_, static_cast<std::size_t>(io.blockOffset + io.frames));
    }
    [[nodiscard]] bool processesDouble() const noexcept override { return true; }
    [[nodiscard]] const char* name() const noexcept override { return "wide play"; }
private:
    const std::vector<double>* data_;
    std::size_t base_ = 0;
    std::size_t end_ = 0;
};

/// Delays its input by a declared latency at either width, so a graph at 64
/// compensates its other paths with double delay lines.
class WideLateNode final : public Node {
public:
    explicit WideLateNode(std::int32_t latency) : latency_(latency) {}
    void prepare(double, std::int32_t) override {
        ring_.assign(2 * static_cast<std::size_t>(latency_ + 1), 0.0);
        write_ = 0;
    }
    void process(const NodeIo& io) noexcept override {
        const auto ring = static_cast<std::size_t>(latency_ + 1);
        std::size_t w = 0;
        for (std::int32_t c = 0; c < io.channels && c < 2; ++c) {
            double* hist = ring_.data() + static_cast<std::size_t>(c) * ring;
            w = write_;
            for (std::int32_t i = 0; i < io.frames; ++i) {
                const std::int32_t at = io.blockOffset + i;
                double x = 0.0;
                if (io.wide()) { if (io.in64 != nullptr) x = io.in64[c][at]; }
                else if (io.in != nullptr) x = io.in[c][at];
                hist[w] = x;
                std::size_t r = w + 1;
                if (r == ring) r = 0;
                if (io.wide()) io.out64[c][at] = hist[r];
                else io.out[c][at] = static_cast<float>(hist[r]);
                if (++w == ring) w = 0;
            }
        }
        write_ = w;
    }
    [[nodiscard]] bool processesDouble() const noexcept override { return true; }
    [[nodiscard]] std::int32_t latencySamples() const noexcept override { return latency_; }
    [[nodiscard]] std::int64_t tailSamples() const noexcept override { return latency_; }
    [[nodiscard]] const char* name() const noexcept override { return "wide late"; }
private:
    std::int32_t latency_;
    std::vector<double> ring_;
    std::size_t write_ = 0;
};

/// A float-only node (no `processesDouble()`) that passes its input through
/// and records which pointers it was handed: in a graph at 64 it must see
/// float and only float.
class FloatProbe final : public Node {
public:
    void process(const NodeIo& io) noexcept override {
        sawWide = sawWide || io.in64 != nullptr || io.out64 != nullptr || io.sidechain64 != nullptr;
        sawFloatOut = sawFloatOut || io.out != nullptr;
        for (std::int32_t c = 0; c < io.channels; ++c)
            for (std::int32_t i = 0; i < io.frames; ++i)
                io.out[c][io.blockOffset + i] = io.in != nullptr ? io.in[c][io.blockOffset + i] : 0.0f;
    }
    [[nodiscard]] std::int64_t tailSamples() const noexcept override { return 0; }
    [[nodiscard]] const char* name() const noexcept override { return "float probe"; }
    bool sawWide = false;
    bool sawFloatOut = false;
};

/// Writes down what the graph tells it and when: "64" or "32" for each
/// `willBeHanded`, "p" for each `prepare`. Says `processesDouble()` when asked to.
class WidthProbe final : public Node {
public:
    explicit WidthProbe(bool doubles) : doubles_(doubles) {}
    void willBeHanded(Precision width) override { log += width == Precision::Float64 ? "64" : "32"; }
    void prepare(double, std::int32_t) override { log += "p"; }
    void process(const NodeIo& io) noexcept override {
        for (std::int32_t c = 0; c < io.channels; ++c)
            for (std::int32_t i = 0; i < io.frames; ++i) {
                if (io.wide()) io.out64[c][io.blockOffset + i] = 0.0;
                else io.out[c][io.blockOffset + i] = 0.0f;
            }
    }
    [[nodiscard]] bool processesDouble() const noexcept override { return doubles_; }
    [[nodiscard]] const char* name() const noexcept override { return "width probe"; }
    std::string log;
private:
    bool doubles_;
};

/// Doubles no float can hold: 24 bits of noise plus a part below float's
/// resolution, so a value that went through float anywhere is different.
std::vector<double> noise53(std::uint32_t seed, std::size_t frames) {
    std::mt19937 random(seed);
    std::vector<double> v(frames);
    for (auto& x : v) {
        const auto a = static_cast<std::uint32_t>(random()), b = static_cast<std::uint32_t>(random());
        const double hi = (static_cast<double>(a >> 8) - 8388608.0) / kFull;          // 24 bits
        const double lo = static_cast<double>(b >> 3) * std::ldexp(1.0, -23 - 29);   // 29 more
        x = hi + lo;
    }
    return v;
}

// --- the cases ----------------------------------------------------------------

constexpr std::int32_t kFrames = 2048;

void testOneRounding() {
    section("ADR-0207 d2 -- a mix point rounds ONCE, after the whole sum");

    // 1 + 2^-24 is a tie in float and rounds back to 1, twice over; in double
    // the two halves of an ulp add up to a whole one first. The smallest case
    // there is of "summed in double, converted once".
    const float tiny = std::ldexp(1.0f, -24);
    const std::vector<float> one(kFrames, 1.0f), small(kFrames, tiny);
    PlayNode a(one), b(small), c(small);
    SumNode master;
    Graph g;
    const NodeId na = g.addNode(a), nb = g.addNode(b), nc = g.addNode(c), nm = g.addNode(master);
    g.connect(na, nm);
    g.connect(nb, nm);
    g.connect(nc, nm);
    g.setOutput(nm);
    g.prepare(48000.0, 512);
    check(g.ok(), "the graph prepares");
    const Render out = renderGraph(g, 512, 512);

    const float want = static_cast<float>(1.0 + 2.0 * static_cast<double>(tiny));
    const float inFloat = (1.0f + tiny) + tiny;
    check(inFloat == 1.0f, "in float the sum is 1: " + show(inFloat));
    check(want != 1.0f, "converted once it is 1 + 2^-23: " + show(want));
    check(out.l[0] == want && out.r[511] == want,
          "the graph's sum is the double sum converted once: " + show(out.l[0]));

    // The same, with the last input on a compensated edge: 1 and one tiny
    // arrive 100 samples late, so the other tiny is delayed to meet them and
    // is the input whose add the conversion rides on.
    {
        PlayNode p1(one), p2(small), p3(small);
        LateNode l1(100), l2(100);
        SumNode m;
        Graph h;
        const NodeId h1 = h.addNode(p1), h2 = h.addNode(p2), h3 = h.addNode(p3),
                     hl1 = h.addNode(l1), hl2 = h.addNode(l2), hm = h.addNode(m);
        h.connect(h1, hl1);
        h.connect(h2, hl2);
        h.connect(hl1, hm);
        h.connect(hl2, hm);
        h.connect(h3, hm);
        h.setOutput(hm);
        h.prepare(48000.0, 512);
        check(h.compensationFor(h3, hm) == 100, "the last input is compensated by 100");
        const Render late = renderGraph(h, 512, 512);
        check(late.l[100] == want && late.r[511] == want,
              "through the delay line too, converted once: " + show(late.l[100]));
    }
}

void testMasterCancels() {
    section("ADR-0207 d2 -- Live's Fact Sheet case at the master: 24-bit files "
            "and their inverted copies cancel to zero");

    constexpr int kPairs = 32;
    std::vector<std::vector<float>> files, copies;
    for (int i = 0; i < kPairs; ++i) {
        files.push_back(noise24(1000u + static_cast<std::uint32_t>(i), kFrames));
        std::vector<float> inv = files.back();
        for (auto& x : inv) x = -x;
        copies.push_back(std::move(inv));
    }

    // Every file first, then every copy: the order a project numbered as
    // "the tracks, then their copies" sums in, and the hard one, because
    // nothing is cancelled until the second half starts arriving.
    std::vector<const std::vector<float>*> order;
    for (const auto& f : files) order.push_back(&f);
    for (const auto& c : copies) order.push_back(&c);
    const float residue = floatResidue(order, kFrames);
    check(residue > 0.0f, "the case discriminates: summed in float it leaves " + show(residue));

    for (const std::int32_t block : {512, 257, 64}) {
        std::vector<std::unique_ptr<PlayNode>> players;
        SumNode master;
        Graph g;
        std::vector<NodeId> ids;
        for (const auto* v : order) {
            players.push_back(std::make_unique<PlayNode>(*v));
            ids.push_back(g.addNode(*players.back()));
        }
        const NodeId nm = g.addNode(master);
        for (NodeId id : ids) g.connect(id, nm);
        g.setOutput(nm);
        g.prepare(48000.0, block);
        const Render out = renderGraph(g, kFrames, block);
        const float left = largest(out.l), right = largest(out.r);
        check(left == 0.0f && right == 0.0f,
              "block " + std::to_string(block) + ": " + std::to_string(2 * kPairs) +
              " tracks sum to exact silence (largest left " + show(left) +
              ", right " + show(right) + ")");
    }
}

void testGroupCancels() {
    section("ADR-0207 d2 -- the same case inside a group, the group into the master");

    constexpr int kPairs = 16;
    std::vector<std::vector<float>> data;
    for (int i = 0; i < kPairs; ++i) data.push_back(noise24(2000u + static_cast<std::uint32_t>(i), kFrames));
    for (int i = 0; i < kPairs; ++i) {
        std::vector<float> inv = data[static_cast<std::size_t>(i)];
        for (auto& x : inv) x = -x;
        data.push_back(std::move(inv));
    }
    std::vector<const std::vector<float>*> order;
    for (const auto& v : data) order.push_back(&v);
    check(floatResidue(order, kFrames) > 0.0f, "the case discriminates in float");

    // An unrelated track beside the group at the master: the group's zero must
    // leave it untouched, bit for bit, through the master's own sum.
    const std::vector<float> other = noise24(77u, kFrames);

    std::vector<std::unique_ptr<PlayNode>> players;
    for (const auto* v : order) players.push_back(std::make_unique<PlayNode>(*v));
    PlayNode beside(other);
    SumNode group, master;
    Graph g;
    std::vector<NodeId> ids;
    for (auto& p : players) ids.push_back(g.addNode(*p));
    const NodeId ng = g.addNode(group), nb = g.addNode(beside), nm = g.addNode(master);
    for (NodeId id : ids) g.connect(id, ng);
    g.connect(ng, nm);
    g.connect(nb, nm);
    g.setOutput(nm);
    g.prepare(48000.0, 512);
    const Render out = renderGraph(g, kFrames, 512);
    check(std::equal(out.l.begin(), out.l.end(), other.begin()) &&
              std::equal(out.r.begin(), out.r.end(), other.begin()),
          "the group cancels and the master is exactly the other track");
}

void testCompensatedPathsCancel() {
    section("ADR-0207 d2 -- the sum is double through compensation delay lines too");

    // The files arrive directly, the copies through a node 300 samples late:
    // the direct edges are delayed to meet them, so the mix point sums delayed
    // edges, which take the other path through the accumulator.
    constexpr int kPairs = 12;
    constexpr std::int32_t kLate = 300;
    std::vector<std::vector<float>> files, copies;
    for (int i = 0; i < kPairs; ++i) {
        files.push_back(noise24(3000u + static_cast<std::uint32_t>(i), kFrames));
        std::vector<float> inv = files.back();
        for (auto& x : inv) x = -x;
        copies.push_back(std::move(inv));
    }
    std::vector<const std::vector<float>*> order;
    for (const auto& f : files) order.push_back(&f);
    for (const auto& c : copies) order.push_back(&c);
    check(floatResidue(order, kFrames) > 0.0f, "the case discriminates in float");

    std::vector<std::unique_ptr<PlayNode>> players;
    std::vector<std::unique_ptr<LateNode>> lates;
    SumNode master;
    Graph g;
    std::vector<NodeId> into;
    for (const auto& f : files) {
        players.push_back(std::make_unique<PlayNode>(f));
        into.push_back(g.addNode(*players.back()));
    }
    for (const auto& c : copies) {
        players.push_back(std::make_unique<PlayNode>(c));
        lates.push_back(std::make_unique<LateNode>(kLate));
        const NodeId p = g.addNode(*players.back());
        const NodeId l = g.addNode(*lates.back());
        g.connect(p, l);
        into.push_back(l);
    }
    const NodeId nm = g.addNode(master);
    for (NodeId id : into) g.connect(id, nm);
    g.setOutput(nm);
    g.prepare(48000.0, 256);
    check(g.compensationFor(into.front(), nm) == kLate, "the direct edges are compensated by 300");
    const Render out = renderGraph(g, kFrames, 256);
    check(largest(out.l) == 0.0f && largest(out.r) == 0.0f,
          "aligned by compensation, the pairs cancel to exact silence (largest " +
              show(largest(out.l)) + ")");
}

void testSidechainCancels() {
    section("ADR-0207 d2 -- the sidechain bus is a mix point too");

    constexpr int kPairs = 12;
    std::vector<std::vector<float>> data;
    for (int i = 0; i < kPairs; ++i) data.push_back(noise24(4000u + static_cast<std::uint32_t>(i), kFrames));
    for (int i = 0; i < kPairs; ++i) {
        std::vector<float> inv = data[static_cast<std::size_t>(i)];
        for (auto& x : inv) x = -x;
        data.push_back(std::move(inv));
    }
    std::vector<const std::vector<float>*> order;
    for (const auto& v : data) order.push_back(&v);
    check(floatResidue(order, kFrames) > 0.0f, "the case discriminates in float");

    std::vector<std::unique_ptr<PlayNode>> players;
    for (const auto* v : order) players.push_back(std::make_unique<PlayNode>(*v));
    KeyOutNode key;
    Graph g;
    std::vector<NodeId> ids;
    for (auto& p : players) ids.push_back(g.addNode(*p));
    const NodeId nk = g.addNode(key);
    for (NodeId id : ids) g.connect(id, nk, Bus::Sidechain);
    g.setOutput(nk);
    g.prepare(48000.0, 512);
    const Render out = renderGraph(g, kFrames, 512);
    check(largest(out.l) == 0.0f && largest(out.r) == 0.0f,
          "the keyed sum cancels to exact silence (largest " + show(largest(out.l)) + ")");
}

// --- the same case through a real project -------------------------------------

std::int64_t nsAt48k(std::int64_t frames) {
    return static_cast<std::int64_t>(std::llround(static_cast<double>(frames) * 1e9 / 48000.0));
}

/// A project on disk: 24-bit files, a clip per file, tracks to taste. The
/// master is track 1000 and every other track routes by the default rule.
struct Project {
    test::TempDirectory dir{"precision", "project"};
    std::unique_ptr<Store> store;
    Session session;
    int media = 0;
    int clips = 0;

    Project() {
        StoreError error{};
        store = Store::create(dir.path() / "session.adi", error);
        if (!store) throw std::runtime_error("create project");
        store->db().exec("INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(1000,'master','Master',999)");
    }
    /// A 24-bit stereo file, the same signal in both channels; returns its media id.
    int file(const std::vector<float>& mono) { return file(std::vector<std::vector<float>>{mono, mono}); }
    /// A 24-bit file of these channels, all the same length.
    int file(const std::vector<std::vector<float>>& chans) {
        ++media;
        const std::size_t ch = chans.size(), frames = chans.front().size();
        std::vector<float> interleaved(frames * ch);
        for (std::size_t i = 0; i < frames; ++i)
            for (std::size_t c = 0; c < ch; ++c) interleaved[i * ch + c] = chans[c][i];
        const std::string name = "f" + std::to_string(media) + ".wav";
        audio::WavWriter w(dir.path() / name, 48000, static_cast<std::uint16_t>(ch),
                           audio::WavFormat::Pcm24);
        w.write(interleaved.data(), static_cast<std::uint32_t>(frames));
        w.close();
        SQLite::Statement q(store->db(),
            "INSERT INTO media_files(id,hash_blake3,rel_path,format) VALUES(?,?,?,'wav')");
        q.bind(1, media);
        q.bind(2, "h" + std::to_string(media));
        q.bind(3, name);
        q.exec();
        return media;
    }
    void track(int id, const char* kind, int parent = 0) {
        SQLite::Statement q(store->db(),
            "INSERT INTO tracks(id,kind,name,index_in_parent,parent_id) VALUES(?,?,?,?,?)");
        q.bind(1, id);
        q.bind(2, kind);
        q.bind(3, "t" + std::to_string(id));
        q.bind(4, id);
        if (parent != 0) q.bind(5, parent); else q.bind(5);
        q.exec();
    }
    void strip(int trackId, double volumeDb, double pan) {
        SQLite::Statement q(store->db(),
            "INSERT INTO mixer_strip(track_id,volume_db,pan) VALUES(?,?,?)");
        q.bind(1, trackId);
        q.bind(2, volumeDb);
        q.bind(3, pan);
        q.exec();
    }
    void clip(int trackId, int mediaId, std::int64_t frames) {
        ++clips;
        SQLite::Statement q(store->db(),
            "INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns) VALUES(?,?,'audio',1,0,?)");
        q.bind(1, clips);
        q.bind(2, trackId);
        q.bind(3, nsAt48k(frames));
        q.exec();
        SQLite::Statement a(store->db(),
            "INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(?,?,0,?)");
        a.bind(1, clips);
        a.bind(2, mediaId);
        a.bind(3, frames);
        a.exec();
    }
    Render play(std::int32_t block, std::int32_t frames, Precision precision = Precision::Float32) {
        session.graph().setFadeFrames(0);
        SessionSpec spec{2, 48000.0, block};
        spec.precision = precision;
        if (!session.load(*store, {}, spec)) throw std::runtime_error(session.error());
        return render(block, frames);
    }
    /// Renders from the top (or on from where it is), at both widths.
    Render render(std::int32_t block, std::int32_t frames, bool fromTop = true) {
        if (fromTop) session.transport().locate(0);
        session.transport().play();
        if (!session.clips()->prime(std::chrono::seconds(60))) throw std::runtime_error("prime timeout");
        Render out;
        const auto n0 = static_cast<std::size_t>(frames);
        out.l.resize(n0); out.r.resize(n0); out.l64.resize(n0); out.r64.resize(n0);
        for (std::int32_t at = 0; at < frames; at += block) {
            const std::int32_t n = (std::min)(block, frames - at);
            std::vector<float> l(static_cast<std::size_t>(n)), r(static_cast<std::size_t>(n));
            std::vector<double> l64(static_cast<std::size_t>(n)), r64(static_cast<std::size_t>(n));
            float* ptrs[2] = {l.data(), r.data()};
            double* wide[2] = {l64.data(), r64.data()};
            AudioIo io{ptrs, nullptr, 2, 0, n, 0};
            io.out64 = wide;
            session.process(io);
            std::copy(l.begin(), l.end(), out.l.begin() + at);
            std::copy(r.begin(), r.end(), out.r.begin() + at);
            std::copy(l64.begin(), l64.end(), out.l64.begin() + at);
            std::copy(r64.begin(), r64.end(), out.r64.begin() + at);
        }
        return out;
    }
};

void testProjectMasterCancels() {
    section("ADR-0207 d2 -- the Fact Sheet case through a project: 24-bit WAV "
            "files on clip tracks, their inverted copies on others, at the master");

    constexpr int kPairs = 16;
    std::vector<std::vector<float>> files;
    for (int i = 0; i < kPairs; ++i) files.push_back(noise24(5000u + static_cast<std::uint32_t>(i), kFrames));
    std::vector<const std::vector<float>*> order;
    std::vector<std::vector<float>> copies;
    for (const auto& f : files) {
        std::vector<float> inv = f;
        for (auto& x : inv) x = -x;
        copies.push_back(std::move(inv));
    }
    for (const auto& f : files) order.push_back(&f);
    for (const auto& c : copies) order.push_back(&c);
    check(floatResidue(order, kFrames) > 0.0f, "the case discriminates in float");

    for (const std::int32_t block : {512, 257}) {
        Project p;
        // Tracks 1..16 hold the files, 17..32 the copies: the master sums
        // them in id order (ADR-0056's fixed order), files first.
        for (int i = 0; i < 2 * kPairs; ++i) {
            const int id = i + 1;
            p.track(id, "audio");
            p.clip(id, p.file(*order[static_cast<std::size_t>(i)]), kFrames);
        }
        const Render out = p.play(block, kFrames);
        check(largest(out.l) == 0.0f && largest(out.r) == 0.0f,
              "block " + std::to_string(block) + ": the master renders exact silence "
              "(largest " + show(largest(out.l)) + ")");
        check(p.session.clips()->underrunSamples() == 0, "and every clip was read");
    }
}

void testProjectStripsAndGroupCancel() {
    section("ADR-0207 d2 -- each pair at its own volume and pan, inside a group");

    // A fader or pan gain makes each sample a float product, so a sum of them
    // is exact in double only while the sum's range fits 53 bits: the samples
    // are kept at or above 2^-7 of full scale, which still spans the 24-bit
    // values the gains turn into products of every exponent the case needs.
    constexpr int kPairs = 8;
    std::vector<std::vector<float>> files;
    for (int i = 0; i < kPairs; ++i)
        files.push_back(noise24(6000u + static_cast<std::uint32_t>(i), kFrames, 16));
    std::vector<std::vector<float>> copies;
    for (const auto& f : files) {
        std::vector<float> inv = f;
        for (auto& x : inv) x = -x;
        copies.push_back(std::move(inv));
    }

    Project p;
    p.track(500, "group");
    for (int i = 0; i < kPairs; ++i) {
        // The pair shares one fader and one pan; across pairs they differ.
        const double volume = -1.5 * i;
        const double pan = (i % 5 - 2) * 0.35;
        const int file = i + 1, copy = kPairs + i + 1;
        p.track(file, "audio", 500);
        p.track(copy, "audio", 500);
        p.strip(file, volume, pan);
        p.strip(copy, volume, pan);
        p.clip(file, p.file(files[static_cast<std::size_t>(i)]), kFrames);
        p.clip(copy, p.file(copies[static_cast<std::size_t>(i)]), kFrames);
    }
    const Render out = p.play(512, kFrames);
    check(largest(out.l) == 0.0f && largest(out.r) == 0.0f,
          "the group's sum is exact silence at the master (largest " +
              show(largest(out.l)) + ", " + show(largest(out.r)) + ")");
}

void testClipsOnOneTrackCancel() {
    section("ADR-0207 d2 -- the clips on one track are a mix point: overlapping "
            "clips of the files and their copies cancel");

    constexpr int kPairs = 8;
    std::vector<std::vector<float>> files;
    for (int i = 0; i < kPairs; ++i) files.push_back(noise24(7000u + static_cast<std::uint32_t>(i), kFrames));
    std::vector<std::vector<float>> copies;
    for (const auto& f : files) {
        std::vector<float> inv = f;
        for (auto& x : inv) x = -x;
        copies.push_back(std::move(inv));
    }
    std::vector<const std::vector<float>*> order;
    for (const auto& f : files) order.push_back(&f);
    for (const auto& c : copies) order.push_back(&c);
    check(floatResidue(order, kFrames) > 0.0f, "the case discriminates in float");

    Project p;
    p.track(1, "audio");
    // Clip ids in this order, all at the same place: files, then copies.
    for (const auto* v : order) p.clip(1, p.file(*v), kFrames);
    const Render out = p.play(512, kFrames);
    check(largest(out.l) == 0.0f && largest(out.r) == 0.0f,
          "sixteen overlapping clips on one track render exact silence (largest " +
              show(largest(out.l)) + ")");

    // And one clip alone is played bit for bit: one clip is not a sum.
    Project q;
    q.track(1, "audio");
    q.clip(1, q.file(files[0]), kFrames);
    const Render one = q.play(512, kFrames);
    check(std::equal(one.l.begin(), one.l.end(), files[0].begin()) &&
              std::equal(one.r.begin(), one.r.end(), files[0].begin()),
          "a lone clip is its file, bit for bit");
}

void testAFoldIsASum() {
    section("ADR-0207 d2 -- a clip's mono fold sums its channels in double");

    // Six channels: three signals, then their inverted copies. Folded to mono
    // (channel mode 3) they cancel; folded in float they do not.
    std::vector<std::vector<float>> chans;
    for (int i = 0; i < 3; ++i) chans.push_back(noise24(8000u + static_cast<std::uint32_t>(i), kFrames));
    for (int i = 0; i < 3; ++i) {
        std::vector<float> inv = chans[static_cast<std::size_t>(i)];
        for (auto& x : inv) x = -x;
        chans.push_back(std::move(inv));
    }
    std::vector<const std::vector<float>*> order;
    for (const auto& c : chans) order.push_back(&c);
    check(floatResidue(order, kFrames) > 0.0f, "the case discriminates in float");

    Project p;
    p.track(1, "audio");
    p.clip(1, p.file(chans), kFrames);
    p.store->db().exec("UPDATE audio_clips SET channel_mode=3");
    const Render out = p.play(512, kFrames);
    check(largest(out.l) == 0.0f && largest(out.r) == 0.0f,
          "the six-channel fold renders exact silence (largest " + show(largest(out.l)) + ")");
}

void testOnlyASumGoesWide() {
    section("ADR-0207 d2 -- one live input is not a sum: a chain and a mix of "
            "sleepers stay on the float path, byte for byte");

    const std::vector<float> a = noise24(9000u, kFrames), b = noise24(9001u, kFrames);

    // A chain: every node has one input. Nothing to sum, nothing goes wide.
    {
        PlayNode src(a);
        SumNode one, two;
        Graph g;
        const NodeId ns = g.addNode(src), n1 = g.addNode(one), n2 = g.addNode(two);
        g.connect(ns, n1);
        g.connect(n1, n2);
        g.setOutput(n2);
        g.prepare(48000.0, 512);
        const Render out = renderGraph(g, kFrames, 512);
        check(g.stats().wideMixes == 0, "a chain sums nothing in double: " +
                                            std::to_string(g.stats().wideMixes));
        check(std::equal(out.l.begin(), out.l.end(), a.begin()), "and passes its samples through");
    }

    // Two live inputs: every segment of the consumer is a wide mix.
    {
        PlayNode pa(a), pb(b);
        SumNode master;
        Graph g;
        const NodeId na = g.addNode(pa), nb = g.addNode(pb), nm = g.addNode(master);
        g.connect(na, nm);
        g.connect(nb, nm);
        g.setOutput(nm);
        g.prepare(48000.0, 512);
        (void)renderGraph(g, kFrames, 512);
        check(g.stats().wideMixes == kFrames / 512,
              "two live inputs: one wide mix per block: " + std::to_string(g.stats().wideMixes));
    }

    // A live input beside a sleeper: the sleeper is skipped and the live one
    // is the mix, on the float path; when the sleeper wakes, the mix is wide.
    {
        class Pass final : public Node {
        public:
            void process(const NodeIo& io) noexcept override {
                for (std::int32_t c = 0; c < io.channels; ++c)
                    for (std::int32_t i = 0; i < io.frames; ++i)
                        io.out[c][io.blockOffset + i] =
                            io.in != nullptr ? io.in[c][io.blockOffset + i] : 0.0f;
            }
            [[nodiscard]] std::int64_t tailSamples() const noexcept override { return 0; }
        };
        // Silent for the first two blocks, then `b`.
        std::vector<float> late = b;
        std::fill(late.begin(), late.begin() + 1024, 0.0f);
        PlayNode pa(a), pq(late);
        Pass sleeper;
        SumNode master;
        Graph g;
        const NodeId nq = g.addNode(pq), ns = g.addNode(sleeper), na = g.addNode(pa),
                     nm = g.addNode(master);
        g.connect(nq, ns);
        g.connect(ns, nm);
        g.connect(na, nm);
        g.setOutput(nm);
        g.prepare(48000.0, 512);
        const Render out = renderGraph(g, kFrames, 512);
        // Blocks 1-2: the pass node (tail 0) sleeps from its first silent
        // block, so the master's one live input is `a` and the mix is float.
        // Blocks 3-4: it wakes, two inputs are live, the mix is wide.
        check(g.stats().inputsSkipped == 2, "the sleeper is skipped while it sleeps: " +
                                                std::to_string(g.stats().inputsSkipped));
        check(g.stats().wideMixes == 2, "and only its waking blocks are wide: " +
                                            std::to_string(g.stats().wideMixes));
        check(std::equal(out.l.begin(), out.l.begin() + 1024, a.begin()),
              "asleep, the mix is the live input, bit for bit");
        bool once = true;
        for (std::size_t i = 1024; i < static_cast<std::size_t>(kFrames); ++i)
            once = once && out.l[i] == static_cast<float>(static_cast<double>(a[i]) + b[i]);
        check(once, "awake, the mix is the double sum converted once");
    }
}


// --- 64-bit end to end: the Processing Precision switch at 64 ----------------

bool sameBits(const std::vector<double>& a, const std::vector<double>& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(double)) == 0;
}

void test64KeepsEveryBit() {
    section("ADR-0207 d2 at 64 -- a chain of double nodes keeps every bit, "
            "and the same chain at 32 does not");

    // A gain no float can hold, so the double path's own gain is what shows.
    const std::vector<double> data = noise53(10000u, kFrames);
    std::vector<double> half(data.size());
    for (std::size_t i = 0; i < data.size(); ++i) half[i] = data[i] * 0.1;
    bool someNotFloat = false;
    for (double x : data) someNotFloat = someNotFloat || static_cast<double>(static_cast<float>(x)) != x;
    check(someNotFloat, "the source holds values no float can");

    for (const Precision p : {Precision::Float64, Precision::Float32}) {
        WidePlayNode src(data);
        SumNode a, b;
        GainNode gain(7);
        Graph g;
        g.setPrecision(p);
        const NodeId ns = g.addNode(src), na = g.addNode(a), ng = g.addNode(gain), nb = g.addNode(b);
        g.connect(ns, na);
        g.connect(na, ng);
        g.connect(ng, nb);
        g.setOutput(nb);
        g.prepare(48000.0, 512);
        check(g.ok() && g.precision() == p, "the graph prepares at its precision");
        Event e;
        e.type = EventType::ParamValue;
        e.paramId = 7;
        e.value = 0.1;
        g.pushInputEvent(ng, e);
        const Render out = renderGraph(g, kFrames, 512);
        if (p == Precision::Float64) {
            check(sameBits(out.l64, half) && sameBits(out.r64, half),
                  "at 64 out64 is the source times 0.1 (a double 0.1), every bit of it");
            bool narrowed = true;
            for (std::size_t i = 0; i < half.size(); ++i)
                narrowed = narrowed && out.l[i] == static_cast<float>(half[i]);
            check(narrowed, "and the float output is that, rounded once at the very end");
        } else {
            check(!sameBits(out.l64, half), "at 32 the same chain loses the low bits, as it must");
            bool floatChain = true;
            for (std::size_t i = 0; i < half.size(); ++i)
                floatChain = floatChain && out.l64[i] == static_cast<double>(static_cast<float>(data[i]) * 0.1f);
            check(floatChain, "at 32 out64 is the float chain's result, widened: nothing else changed");
        }
    }
}

void test64FloatNodeIsConverted() {
    section("ADR-0207 d2 at 64 -- a node that does not process double is handed "
            "float, converted either side; the paths around it stay double");

    const std::vector<double> a = noise53(11000u, kFrames), b = noise53(11001u, kFrames);
    WidePlayNode pa(a), pb(b);
    FloatProbe probe;
    SumNode master;
    Graph g;
    g.setPrecision(Precision::Float64);
    const NodeId na = g.addNode(pa), nb = g.addNode(pb), np = g.addNode(probe), nm = g.addNode(master);
    g.connect(na, np);    // a goes through the float node
    g.connect(np, nm);
    g.connect(nb, nm);    // b does not
    g.setOutput(nm);
    g.prepare(48000.0, 512);
    const Render out = renderGraph(g, kFrames, 512);
    check(!probe.sawWide && probe.sawFloatOut, "the float node saw float pointers and only float");
    bool exact = true;
    for (std::size_t i = 0; i < a.size(); ++i)
        exact = exact && out.l64[i] == static_cast<double>(static_cast<float>(a[i])) + b[i];
    check(exact, "the master is float(a) + b in double: a rounded once at the float node, b untouched");
}

void test64TellsEachNodeItsWidth() {
    section("ADR-0207 d2 -- the graph tells each node the width it will be handed, "
            "before the node prepares: a VST3 fixes its sample size at activation");

    WidthProbe wide(true), narrow(false);
    SumNode master;
    Graph g;
    g.setPrecision(Precision::Float64);
    const NodeId nw = g.addNode(wide), nn = g.addNode(narrow), nm = g.addNode(master);
    g.connect(nw, nm);
    g.connect(nn, nm);
    g.setOutput(nm);
    g.prepare(48000.0, 512);
    check(wide.log == "64p", "at 64, a node that processes double is told 64, then prepared: " + wide.log);
    check(narrow.log == "32p", "and one that does not is told 32, which is what it is handed: " + narrow.log);

    g.setPrecision(Precision::Float32);
    g.prepare(48000.0, 512);
    check(wide.log == "64p32p", "at 32, every node is told 32, whatever it supports: " + wide.log);
    check(narrow.log == "32p32p", "the float node too: " + narrow.log);
}

void test64CompensatesInDouble() {
    section("ADR-0207 d2 at 64 -- compensation delay lines hold double");

    constexpr std::int32_t kLate = 300;
    const std::vector<double> a = noise53(12000u, kFrames), b = noise53(12001u, kFrames);
    WidePlayNode pa(a), pb(b);
    WideLateNode late(kLate);
    SumNode master;
    Graph g;
    g.setPrecision(Precision::Float64);
    const NodeId na = g.addNode(pa), nb = g.addNode(pb), nl = g.addNode(late), nm = g.addNode(master);
    g.connect(na, nm);    // direct: delayed by the graph to meet b
    g.connect(nb, nl);
    g.connect(nl, nm);
    g.setOutput(nm);
    g.prepare(48000.0, 256);
    check(g.compensationFor(na, nm) == kLate, "the direct edge is compensated by 300");
    const Render out = renderGraph(g, kFrames, 256);
    bool exact = true;
    for (std::size_t i = kLate; i < a.size(); ++i)
        exact = exact && out.l64[i] == a[i - kLate] + b[i - kLate];
    check(exact, "after 300 samples the master is a + b exactly: the delayed edge kept every bit");
    // The same graph at 32 holds the same rings in float: exactly half.
    std::size_t bytes32 = 0;
    {
        WidePlayNode qa(a), qb(b);
        WideLateNode qlate(kLate);
        SumNode qm;
        Graph h;
        const NodeId ha = h.addNode(qa), hb = h.addNode(qb), hl = h.addNode(qlate), hm = h.addNode(qm);
        h.connect(ha, hm);
        h.connect(hb, hl);
        h.connect(hl, hm);
        h.setOutput(hm);
        h.prepare(48000.0, 256);
        bytes32 = h.compensationBytes();
    }
    check(bytes32 > 0 && g.compensationBytes() == 2 * bytes32,
          "and its rings are double: " + std::to_string(g.compensationBytes()) + " bytes, twice the " +
              std::to_string(bytes32) + " the same graph holds at 32");
}

void test64DelayLine() {
    section("ADR-0207 d2 -- a DelayLine prepared wide delays doubles and refuses floats");

    DelayLine d;
    d.prepare(2, 16, true);
    d.setDelay(5);
    check(d.wide() && d.bytes() == 2 * 17 * sizeof(double), "a wide line holds double: " + std::to_string(d.bytes()));
    std::vector<double> in(32), out(32);
    for (std::size_t i = 0; i < in.size(); ++i) in[i] = 1.0 + std::ldexp(static_cast<double>(i + 1), -40);
    d.process(0, in.data(), out.data(), 32);
    bool delayed = true;
    for (std::size_t i = 0; i < 32; ++i) delayed = delayed && out[i] == (i < 5 ? 0.0 : in[i - 5]);
    check(delayed, "the doubles come out five samples late, every bit kept");
    std::vector<float> fin(8, 0.25f), fout(8, 0.0f);
    d.process(1, fin.data(), fout.data(), 8);
    check(fout[0] == 0.25f, "a float call on a wide line passes through undelayed, as an unprepared line does");
    check(!d.offerRing(std::vector<float>(2 * 33, 0.0f), 32, 20), "a float ring is refused by a wide line");
    check(d.offerRing(std::vector<double>(2 * 33, 0.0), 32, 20), "a double ring is taken");
    check(!d.reclaim(), "nothing to reclaim while it is in flight");

    // The whole grow at 64, driven as Graph::accumulate drives it: the shared
    // cursors restored per channel, endEdge once per edge.
    {
        DelayLine g64;
        g64.prepare(2, 16, true);
        g64.setDelay(5);
        auto x = [](int c, std::int64_t n) {
            return static_cast<double>(c * 1000 + n) + std::ldexp(static_cast<double>(n + 1), -40);
        };
        std::int64_t at = 0;
        std::vector<double> got[2] = {std::vector<double>(8), std::vector<double>(8)};
        auto edge = [&] {
            const DelayLine::Cursors before = g64.cursors();
            for (int c = 0; c < 2; ++c) {
                g64.setCursors(before);
                std::vector<double> in(8);
                for (int i = 0; i < 8; ++i) in[static_cast<std::size_t>(i)] = x(c, at + i);
                g64.process(c, in.data(), got[c].data(), 8);
            }
            g64.endEdge();
            at += 8;
        };
        for (int b = 0; b < 4; ++b) edge();             // history at the old delay
        check(g64.offerRing(std::vector<double>(2 * 33, 0.0), 32, 20), "a wide line takes a bigger double ring");
        bool primedOnOld = true;
        for (int b = 0; b < 3; ++b) {                   // 24 samples prime a 20-sample tap
            edge();
            for (int i = 0; i < 8; ++i)
                primedOnOld = primedOnOld && got[1][static_cast<std::size_t>(i)] == x(1, at - 8 + i - 5);
        }
        check(primedOnOld && g64.primeRemaining() == 0, "while priming it still plays the old delay, every bit");
        edge();                                         // the fade, and the swap at its end
        check(g64.delay() == 20 && g64.capacity() == 32 &&
                  g64.bytes() == 2 * 33 * sizeof(double) + 2 * 17 * sizeof(double),
              "swapped: delay 20 in the 32-sample ring, the old ring parked: " + std::to_string(g64.bytes()));
        bool late20 = true;
        for (int b = 0; b < 2; ++b) {
            edge();
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < 8; ++i)
                    late20 = late20 && got[c][static_cast<std::size_t>(i)] == x(c, at - 8 + i - 20);
        }
        check(late20, "after the swap both channels come out 20 late, every bit");
        check(g64.reclaim() && g64.bytes() == 2 * 33 * sizeof(double) && !g64.growing(),
              "and the parked double ring is reclaimed");
    }
    DelayLine f;
    f.prepare(2, 16);
    check(!f.wide() && f.bytes() == 2 * 17 * sizeof(float) &&
              !f.offerRing(std::vector<double>(2 * 33, 0.0), 32, 20),
          "a float line holds float and refuses a double ring");
}

void test64SleepsAndWakes() {
    section("ADR-0207 d2 at 64 -- a sleeping node clears its double buffer once, "
            "to capacity, and its consumer reads zeros until it wakes");

    // Loud for 512 frames, silent for 768, loud again: the sleeper is the
    // graph's output, so what its double buffer holds is what comes out.
    std::vector<double> data = noise53(13000u, 4096);
    std::fill(data.begin() + 512, data.begin() + 1280, 0.0);
    class WidePass final : public Node {
    public:
        // Both widths, as the processesDouble() contract asks.
        void process(const NodeIo& io) noexcept override {
            for (std::int32_t c = 0; c < io.channels; ++c)
                for (std::int32_t i = 0; i < io.frames; ++i) {
                    const std::int32_t at = io.blockOffset + i;
                    if (io.wide()) io.out64[c][at] = io.in64 != nullptr ? io.in64[c][at] : 0.0;
                    else io.out[c][at] = io.in != nullptr ? io.in[c][at] : 0.0f;
                }
        }
        [[nodiscard]] bool processesDouble() const noexcept override { return true; }
        [[nodiscard]] std::int64_t tailSamples() const noexcept override { return 0; }
    };
    WidePlayNode src(data);
    WidePass sleeper;
    Graph g;
    g.setPrecision(Precision::Float64);
    const NodeId ns = g.addNode(src), nz = g.addNode(sleeper);
    g.connect(ns, nz);
    g.setOutput(nz);
    g.prepare(48000.0, 512);

    std::vector<double> l(512), r(512);
    double* wide[2] = {l.data(), r.data()};
    auto block = [&](std::int32_t frames) {
        AudioIo io;
        io.out64 = wide;
        io.numOut = 2;
        io.frames = frames;
        g.process(io);
    };
    block(512);                       // [0, 512): loud
    check(std::equal(l.begin(), l.end(), data.begin()), "awake, the output is the source");
    block(256);                       // [512, 768): silent, and it sleeps
    check(g.stats().suspendClears == 1, "it sleeps and clears once: " + std::to_string(g.stats().suspendClears));
    check(std::all_of(l.begin(), l.begin() + 256, [](double x) { return x == 0.0; }), "asleep, the output is zeros");
    block(512);                       // [768, 1280): silent; a longer block than the one it slept on
    check(std::all_of(l.begin(), l.end(), [](double x) { return x == 0.0; }),
          "and the whole buffer was cleared, not the 256 frames it slept on: no stale loud block");
    check(g.stats().suspendClears == 1, "cleared once per silence, not per block");
    block(512);                       // [1280, 1792): loud again
    check(std::equal(l.begin(), l.end(), data.begin() + 1280), "it wakes and the output is the source again");
}

void test64FactSheetCase() {
    section("ADR-0207 d2 at 64 -- the Fact Sheet case cancels at 64 too, through "
            "a real project");

    constexpr int kPairs = 16;
    std::vector<std::vector<float>> files, copies;
    for (int i = 0; i < kPairs; ++i) {
        files.push_back(noise24(14000u + static_cast<std::uint32_t>(i), kFrames));
        std::vector<float> inv = files.back();
        for (auto& x : inv) x = -x;
        copies.push_back(std::move(inv));
    }
    Project p;
    for (int i = 0; i < kPairs; ++i) {
        p.track(i + 1, "audio");
        p.clip(i + 1, p.file(files[static_cast<std::size_t>(i)]), kFrames);
    }
    for (int i = 0; i < kPairs; ++i) {
        p.track(kPairs + i + 1, "audio");
        p.clip(kPairs + i + 1, p.file(copies[static_cast<std::size_t>(i)]), kFrames);
    }
    const Render out = p.play(512, kFrames, Precision::Float64);
    double worst = 0.0;
    for (double x : out.l64) worst = (std::max)(worst, std::fabs(x));
    check(worst == 0.0 && largest(out.l) == 0.0f,
          "32 unity-gain tracks at the master render exact silence at 64 (largest " +
              std::to_string(worst) + ")");
}

void test64ThroughAProject() {
    section("ADR-0207 d2 at 64 -- a project's fader and pan and clip gain are "
            "applied in double");

    const std::vector<float> file = noise24(15000u, kFrames);
    Project p;
    p.track(1, "audio");
    p.strip(1, -6.0, 0.25);
    p.clip(1, p.file(file), kFrames);
    p.store->db().exec("UPDATE clips SET gain_db=-3.0");
    const Render wide = p.play(512, kFrames, Precision::Float64);

    const double fader = faderGain(-6.0);
    const StereoGainWide pan = panGainsWide(0.25, PanLaw::Live);
    const double clipGain = std::pow(10.0, -3.0 / 20.0);
    bool exactL = true, exactR = true;
    for (std::size_t i = 0; i < file.size(); ++i) {
        const double x = clipGain * static_cast<double>(file[i]);
        exactL = exactL && wide.l64[i] == x * (fader * pan.left);
        exactR = exactR && wide.r64[i] == x * (fader * pan.right);
    }
    check(exactL && exactR, "out64 is sample x clip gain x fader x pan, each in double");

    // The same project at 32 is the float product, so the two differ, by no
    // more than float's resolution of the signal.
    Project q;
    q.track(1, "audio");
    q.strip(1, -6.0, 0.25);
    q.clip(1, q.file(file), kFrames);
    q.store->db().exec("UPDATE clips SET gain_db=-3.0");
    const Render narrow = q.play(512, kFrames);
    double diff = 0.0, peak = 0.0;
    for (std::size_t i = 0; i < file.size(); ++i) {
        diff = (std::max)(diff, std::fabs(wide.l64[i] - narrow.l64[i]));
        peak = (std::max)(peak, std::fabs(wide.l64[i]));
    }
    check(diff > 0.0 && diff < peak * 1e-6,
          "at 32 the same project differs below float's resolution: " + std::to_string(diff));
}

void test64SwitchIsARebuild() {
    section("ADR-0207 d2 -- switching precision builds a new graph; the same "
            "precision does nothing");

    Project p;
    p.track(1, "audio");
    p.clip(1, p.file(noise24(16000u, kFrames)), kFrames);
    (void)p.play(512, 1024);
    const Graph* before = p.session.graph().currentGraph();
    const auto rebuilds = p.session.stats().rebuilds;
    check(before != nullptr && before->precision() == Precision::Float32, "a session starts at 32");
    check(p.session.setPrecision(Precision::Float32) && p.session.stats().rebuilds == rebuilds,
          "asking for 32 again rebuilds nothing");
    check(p.session.setPrecision(Precision::Float64), "switching to 64 succeeds");
    const Graph* after = p.session.graph().currentGraph();
    check(p.session.stats().rebuilds == rebuilds + 1 && after != nullptr &&
              after->precision() == Precision::Float64 && p.session.spec().precision == Precision::Float64,
          "and is one rebuild, to a graph at 64");
    const Render out = p.render(512, 1024);
    check(largest(out.l) > 0.0f, "which plays");
}

void test64SwitchKeepsCompensationHistory() {
    section("ADR-0207 d2 -- a precision switch mid-play carries every compensated "
            "edge's history across the widths: no hole");

    // Track 1 plays 300 samples late (a track delay), so the master delays
    // track 2's edge by 300 to meet it. At the switch that edge's ring is
    // rebuilt at the other width; without the history it would play 300
    // samples of silence.
    const std::vector<float> a = noise24(17000u, 4096), b = noise24(17001u, 4096);
    auto build = [&](Project& p) {
        p.track(1, "audio");
        p.track(2, "audio");
        p.strip(1, 0.0, 0.0);
        p.store->db().exec("UPDATE mixer_strip SET delay_samples=300 WHERE track_id=1");
        p.clip(1, p.file(a), 4096);
        p.clip(2, p.file(b), 4096);
    };
    Project ref;
    build(ref);
    const Render whole = ref.play(512, 4096);

    for (const Precision to : {Precision::Float64, Precision::Float32}) {
        Project p;
        build(p);
        const Precision from = to == Precision::Float64 ? Precision::Float32 : Precision::Float64;
        const Render first = p.play(512, 2048, from);
        check(p.session.setPrecision(to), "the switch builds");
        const Render second = p.render(512, 2048, false);
        bool same = true;
        for (std::size_t i = 0; i < 2048; ++i)
            same = same && first.l[i] == whole.l[i] && second.l[i] == whole.l[2048 + i] &&
                   second.r[i] == whole.r[2048 + i];
        check(same, std::string("switching ") + (to == Precision::Float64 ? "32 -> 64" : "64 -> 32") +
                        " mid-play renders exactly what staying at 32 renders");
    }
}

void test64OutputContract() {
    section("ADR-0207 d2 -- AudioIo::out64 is written wherever out is: no graph "
            "yet, silence, and the swap's fade");

    std::vector<double> wide(64, 7.0);
    std::vector<float> narrow(64, 7.0f);
    double* w[1] = {wide.data()};
    float* f[1] = {narrow.data()};
    AudioIo io;
    io.out = f;
    io.out64 = w;
    io.numOut = 1;
    io.frames = 64;
    SilenceProcessor silence;
    silence.process(io);
    check(std::all_of(wide.begin(), wide.end(), [](double x) { return x == 0.0; }) &&
              std::all_of(narrow.begin(), narrow.end(), [](float x) { return x == 0.0f; }),
          "SilenceProcessor writes silence to both");
    std::fill(wide.begin(), wide.end(), 7.0);
    GraphHost host;
    host.process(io);
    check(std::all_of(wide.begin(), wide.end(), [](double x) { return x == 0.0; }),
          "a GraphHost with nothing published writes silence to out64 too");

    // The swap's fade-in ramps both outputs alike.
    const std::vector<float> file = noise24(18000u, 4096);
    Project p;
    p.track(1, "audio");
    p.clip(1, p.file(file), 4096);
    (void)p.play(256, 1024);
    p.session.graph().setFadeFrames(256);
    check(p.session.setPrecision(Precision::Float64), "a switch to 64, faded in");
    const Render faded = p.render(256, 256, false);
    double worst = 0.0;
    bool ramped = true;
    for (std::size_t i = 0; i < 256; ++i) {
        worst = (std::max)(worst, std::fabs(static_cast<double>(faded.l[i]) - faded.l64[i]));
        const double want = static_cast<double>(file[1024 + i]) * static_cast<double>(i + 1) / 256.0;
        ramped = ramped && std::fabs(faded.l64[i] - want) < 1e-12;
    }
    check(ramped, "out64 is the new graph's output on the fade's ramp");
    check(worst < 1e-6, "and the float output agrees with it across the fade (largest gap " +
                            std::to_string(worst) + ")");
}

/// A delay the test can change while the graph runs, at either width: its
/// own ring always holds `maxLatency` samples of history.
class WideVarLateNode final : public Node {
public:
    explicit WideVarLateNode(std::int32_t maxLatency) : max_(maxLatency) {}
    void prepare(double, std::int32_t) override {
        ring_.assign(2 * static_cast<std::size_t>(max_ + 1), 0.0);
        write_ = 0;
    }
    void setLatency(std::int32_t l) noexcept { latency_.store(l); }
    void process(const NodeIo& io) noexcept override {
        const auto ring = static_cast<std::size_t>(max_ + 1);
        const auto d = static_cast<std::size_t>(latency_.load());
        std::size_t w = 0;
        for (std::int32_t c = 0; c < io.channels && c < 2; ++c) {
            double* hist = ring_.data() + static_cast<std::size_t>(c) * ring;
            w = write_;
            for (std::int32_t i = 0; i < io.frames; ++i) {
                const std::int32_t at = io.blockOffset + i;
                hist[w] = io.wide() ? (io.in64 != nullptr ? io.in64[c][at] : 0.0)
                                    : (io.in != nullptr ? io.in[c][at] : 0.0);
                const std::size_t r = (w + ring - d) % ring;
                if (io.wide()) io.out64[c][at] = hist[r];
                else io.out[c][at] = static_cast<float>(hist[r]);
                if (++w == ring) w = 0;
            }
        }
        write_ = w;
    }
    [[nodiscard]] bool processesDouble() const noexcept override { return true; }
    [[nodiscard]] std::int32_t latencySamples() const noexcept override { return latency_.load(); }
private:
    std::int32_t max_;
    std::atomic<std::int32_t> latency_{0};
    std::vector<double> ring_;
    std::size_t write_ = 0;
};

void test64EscalatesInDouble() {
    section("ADR-0207 d2 at 64 -- a latency change past the headroom grows a "
            "double ring, and the edge comes out aligned to every bit");

    const std::vector<double> a = noise53(19000u, 8192), b = noise53(19001u, 8192);
    WidePlayNode pa(a), pb(b);
    WideVarLateNode late(200);
    SumNode master;
    Graph g;
    g.setPrecision(Precision::Float64);
    g.setLatencyHeadroom(16);    // so a change to 100 cannot fit the ring
    const NodeId na = g.addNode(pa), nb = g.addNode(pb), nl = g.addNode(late), nm = g.addNode(master);
    g.connect(na, nm);
    g.connect(nb, nl);
    g.connect(nl, nm);
    g.setOutput(nm);
    g.prepare(48000.0, 256);
    std::vector<double> l(256), r(256);
    double* wide[2] = {l.data(), r.data()};
    AudioIo io;
    io.out64 = wide;
    io.numOut = 2;
    io.frames = 256;
    for (int i = 0; i < 4; ++i) g.process(io);
    late.setLatency(100);
    check(!g.retapLatency(), "a change to 100 does not fit a 16-sample headroom");
    check(g.escalateLatency() == 1, "so the direct edge is offered a bigger ring, at its width");
    for (int i = 0; i < 8; ++i) g.process(io);
    check(g.collectRings() == 1 && g.compensationFor(na, nm) == 100, "it swapped, delays 100, and the old ring is collected");
    g.process(io);   // block 13: samples [3072, 3328)
    bool aligned = true;
    for (std::size_t i = 0; i < 256; ++i)
        aligned = aligned && l[i] == a[3072 + i - 100] + b[3072 + i - 100];
    check(aligned, "the master is a + b, 100 late, exactly: the grown ring is double");
}

void test64StripHandover() {
    section("ADR-0207 d2 -- a strip switched between widths goes on from the "
            "gain it was playing, never from the other width's old one");

    StripNode strip;
    strip.prepare(48000.0, 512);
    strip.setStatic(0.0, 0.0, PanLaw::Live, true);
    std::vector<float> fin(512, 1.0f), fl(512), fr(512);
    std::vector<double> win(512, 1.0), wl(512), wr(512);
    const float* fi[2] = {fin.data(), fin.data()};
    float* fo[2] = {fl.data(), fr.data()};
    const double* wi[2] = {win.data(), win.data()};
    double* wo[2] = {wl.data(), wr.data()};
    auto at32 = [&](std::int32_t frames) {
        NodeIo io; io.in = fi; io.out = fo; io.channels = 2; io.frames = frames; io.sampleRate = 48000.0;
        strip.process(io);
    };
    auto at64 = [&](std::int32_t frames) {
        NodeIo io; io.in64 = wi; io.out64 = wo; io.channels = 2; io.frames = frames; io.sampleRate = 48000.0;
        strip.process(io);
    };
    at32(512);
    at64(512);
    check(wl[0] == 1.0 && wl[511] == 1.0, "unity at 32, then at 64");
    strip.setStatic(0.0, 0.0, PanLaw::Live, false);   // muted, while at 64
    at64(512);
    check(wl[511] == 0.0, "the mute ramps to silence at 64");
    at32(512);
    check(std::all_of(fl.begin(), fl.end(), [](float x) { return x == 0.0f; }),
          "back at 32 it stays silent: no 5 ms of the old unity gain (first " + std::to_string(fl[0]) + ")");

    // Mid-ramp: unmuted at 32, switched to 64 100 samples into the 240.
    strip.setStatic(0.0, 0.0, PanLaw::Live, true);
    at32(100);
    const float before = fl[99];
    at64(512);
    check(before > 0.40f && before < 0.43f && wl[0] > before && wl[0] < 0.43 &&
              wl[138] < 1.0 && wl[139] == 1.0,
          "a ramp in flight goes on at 64 from where it was and lands on time (" +
              std::to_string(before) + ", " + std::to_string(wl[0]) + ")");
}

void test64EveryEngineNode() {
    section("ADR-0207 d2 at 64 -- a project that runs every engine node's double "
            "path renders what 32 renders, to float's resolution, and not its bits");

    // A group with Console9 summing (both console halves), a track with input
    // gain and two overlapping faded clips, a six-channel file folded to mono,
    // and a MIDI track.
    std::vector<std::vector<float>> six;
    for (int i = 0; i < 6; ++i) six.push_back(noise24(20000u + static_cast<std::uint32_t>(i), kFrames, 16));
    auto build = [&](Project& p) {
        p.track(100, "group");
        p.store->db().exec("INSERT INTO group_summing(track_id,enabled,flavor) VALUES(100,1,'console9')");
        p.track(1, "audio", 100);
        p.track(2, "audio", 100);
        p.track(3, "midi", 100);
        p.strip(1, -2.0, -0.3);
        p.store->db().exec("UPDATE mixer_strip SET input_gain_db=-3.0 WHERE track_id=1");
        p.clip(1, p.file(noise24(20100u, kFrames, 16)), kFrames);
        p.clip(1, p.file(noise24(20101u, kFrames, 16)), kFrames);
        p.store->db().exec("UPDATE clips SET fade_in_ticks=24024, fade_out_ticks=24024 WHERE track_id=1");
        p.clip(2, p.file(six), kFrames);
        p.store->db().exec("UPDATE audio_clips SET channel_mode=3 WHERE clip_id=3");
    };
    Project p32, p64;
    build(p32);
    build(p64);
    const Render a = p32.play(512, kFrames);
    const Render b = p64.play(512, kFrames, Precision::Float64);
    check(p64.session.graph().currentGraph() != nullptr &&
              p64.session.graph().currentGraph()->precision() == Precision::Float64 &&
              p64.session.problems().empty(),
          "the project plays at 64 with no problems");
    double diff = 0.0, peak = 0.0;
    for (std::size_t i = 0; i < a.l64.size(); ++i) {
        diff = (std::max)(diff, std::fabs(a.l64[i] - b.l64[i]));
        peak = (std::max)(peak, std::fabs(b.l64[i]));
    }
    check(peak > 0.01 && diff > 0.0 && diff < peak * 1e-5,
          "64 differs from 32 by no more than float's resolution of the signal: " +
              std::to_string(diff) + " of " + std::to_string(peak));
}
}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("adi_precision_tests -- 64-bit summing at every mix point (ADR-0207 d2)\n\n");
    try {
        testOneRounding();
        testMasterCancels();
        testGroupCancels();
        testCompensatedPathsCancel();
        testSidechainCancels();
        testProjectMasterCancels();
        testProjectStripsAndGroupCancel();
        testClipsOnOneTrackCancel();
        testAFoldIsASum();
        testOnlyASumGoesWide();
        test64KeepsEveryBit();
        test64FloatNodeIsConverted();
        test64TellsEachNodeItsWidth();
        test64CompensatesInDouble();
        test64DelayLine();
        test64SleepsAndWakes();
        test64FactSheetCase();
        test64ThroughAProject();
        test64SwitchIsARebuild();
        test64SwitchKeepsCompensationHistory();
        test64OutputContract();
        test64EscalatesInDouble();
        test64StripHandover();
        test64EveryEngineNode();
    } catch (const std::exception& e) {
        std::printf("\nFAILED -- exception escaped: %s\n", e.what());
        return 1;
    }
    std::printf("\n%s -- %d checks, %d failure(s)\n",
                g_failures ? "FAILED" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
