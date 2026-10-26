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
#include "adi/engine/session.hpp"
#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <algorithm>
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
};

/// Renders `frames` in blocks of `block`, both channels, concatenated.
Render renderGraph(Graph& g, std::int32_t frames, std::int32_t block) {
    Render out;
    out.l.resize(static_cast<std::size_t>(frames));
    out.r.resize(static_cast<std::size_t>(frames));
    for (std::int32_t at = 0; at < frames; at += block) {
        const std::int32_t n = (std::min)(block, frames - at);
        float* ptrs[2] = {out.l.data() + at, out.r.data() + at};
        AudioIo io;
        io.out = ptrs;
        io.numOut = 2;
        io.frames = n;
        g.process(io);
    }
    return out;
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
    Render play(std::int32_t block, std::int32_t frames) {
        session.graph().setFadeFrames(0);
        if (!session.load(*store, {}, {2, 48000.0, block})) throw std::runtime_error(session.error());
        session.transport().play();
        if (!session.clips()->prime(std::chrono::seconds(60))) throw std::runtime_error("prime timeout");
        Render out;
        out.l.resize(static_cast<std::size_t>(frames));
        out.r.resize(static_cast<std::size_t>(frames));
        for (std::int32_t at = 0; at < frames; at += block) {
            const std::int32_t n = (std::min)(block, frames - at);
            std::vector<float> l(static_cast<std::size_t>(n)), r(static_cast<std::size_t>(n));
            float* ptrs[2] = {l.data(), r.data()};
            session.process({ptrs, nullptr, 2, 0, n, 0});
            std::copy(l.begin(), l.end(), out.l.begin() + at);
            std::copy(r.begin(), r.end(), out.r.begin() + at);
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
    } catch (const std::exception& e) {
        std::printf("\nFAILED -- exception escaped: %s\n", e.what());
        return 1;
    }
    std::printf("\n%s -- %d checks, %d failure(s)\n",
                g_failures ? "FAILED" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
