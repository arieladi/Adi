// SPDX-License-Identifier: GPL-3.0-or-later
//
// ADR-0163: the mixer strip in the graph -- volume, pan and mute on every
// track, solo across the project, a ramp for every change, and strips that
// outlive the graphs every edit publishes.

#include "temp_directory.hpp"

#include "adi/blob.hpp"
#include "adi/engine/mixer.hpp"
#include "adi/engine/session.hpp"
#include "adi/engine/scope.hpp"
#include "adi/engine/summing.hpp"
#include "adi/history.hpp"
#include "adi/summing_flavors.hpp"
#include "adi/ops.hpp"
#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <map>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace adi;
using namespace adi::engine;

int g_failures = 0;
int g_checks = 0;

void check(bool cond, const std::string& what) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::printf("  FAIL  %s\n", what.c_str());
    }
}
void section(const char* s) { std::printf("[%s]\n", s); }
bool near(double a, double b, double tol = 1e-5) { return std::fabs(a - b) <= tol; }

/// Silence, then `v` from frame `at` of its own count on (ADR-0172's edge).
class StepNode final : public Node {
public:
    StepNode(float v, std::int64_t at) : v_(v), at_(at) {}
    void process(const NodeIo& io) noexcept override {
        for (std::int32_t i = 0; i < io.frames; ++i) {
            const float x = (count_ + i >= at_) ? v_ : 0.0f;
            for (std::int32_t c = 0; c < io.channels; ++c) io.out[c][io.blockOffset + i] = x;
        }
        count_ += io.frames;
    }
    [[nodiscard]] const char* name() const noexcept override { return "step"; }

private:
    float v_;
    std::int64_t at_;
    std::int64_t count_ = 0;
};

/// A sine at `hz` and `amp`, counted at 48 kHz (ADR-0174's children).
class SineNode final : public Node {
public:
    SineNode(double hz, float amp) : step_(2.0 * std::numbers::pi * hz / 48000.0), amp_(amp) {}
    void process(const NodeIo& io) noexcept override {
        for (std::int32_t i = 0; i < io.frames; ++i) {
            const float x = amp_ * static_cast<float>(std::sin(phase_));
            phase_ += step_;
            for (std::int32_t c = 0; c < io.channels; ++c) io.out[c][io.blockOffset + i] = x;
        }
    }
    [[nodiscard]] const char* name() const noexcept override { return "sine"; }

private:
    double step_;
    float amp_;
    double phase_ = 0.0;
};

class ToneNode final : public Node {
public:
    explicit ToneNode(float v) : v_(v) {}
    void process(const NodeIo& io) noexcept override {
        for (std::int32_t c = 0; c < io.channels; ++c)
            for (std::int32_t i = 0; i < io.frames; ++i) io.out[c][io.blockOffset + i] = v_;
    }
    [[nodiscard]] const char* name() const noexcept override { return "tone"; }

private:
    float v_;
};

/// Silence, 1000 samples late: a sibling the others are compensated against.
class LateSilence final : public Node {
public:
    void process(const NodeIo& io) noexcept override {
        for (std::int32_t c = 0; c < io.channels; ++c)
            for (std::int32_t i = 0; i < io.frames; ++i) io.out[c][io.blockOffset + i] = 0.0f;
    }
    [[nodiscard]] std::int32_t latencySamples() const noexcept override { return 1000; }
    [[nodiscard]] const char* name() const noexcept override { return "late"; }
};

/// A project of named tracks, each track id optionally a tone, and a session on it.
struct Mix {
    adi::test::TempDirectory temp;
    std::unique_ptr<Store> store;
    Session session;
    std::map<std::int64_t, std::unique_ptr<ToneNode>> tones;
    std::vector<float> l, r;
    explicit Mix(const char* name) : temp("mixer", name) {
        StoreError e = StoreError::Ok;
        store = Store::create(temp.path() / "p.adi", e);
        if (!store) throw std::runtime_error("create");
    }
    void track(std::int64_t id, const char* kind, const char* name, float tone = 0.0f) {
        OpRequest q;
        q.opType = "track.create";
        q.payload = {{"id", id}, {"kind", kind}, {"name", name}, {"index", static_cast<std::int64_t>(tones.size()) + 10 * id}};
        op(q);
        if (tone != 0.0f) tones[id] = std::make_unique<ToneNode>(tone);
    }
    void op(const OpRequest& q) {
        OpJournal j(*store);
        const auto res = j.commit(q);
        if (!res.ok) throw std::runtime_error(q.opType + ": " + res.error);
    }
    void set(const char* type, std::int64_t id, const char* key, Payload value) {
        OpRequest q;
        q.opType = type;
        q.payload = {{"id", id}, {key, std::move(value)}};
        op(q);
    }
    /// Sources other than tones: a track id's own node (ADR-0172's steps).
    std::map<std::int64_t, Node*> extra;
    void load(double sampleRate = 48000.0, Precision precision = Precision::Float32) {
        session.setSourcesFor([this](std::int64_t id) {
            if (const auto e = extra.find(id); e != extra.end()) return std::vector<Node*>{e->second};
            const auto it = tones.find(id);
            return it == tones.end() ? std::vector<Node*>{} : std::vector<Node*>{it->second.get()};
        });
        SessionSpec spec;
        spec.sampleRate = sampleRate;
        spec.maxFrames = 512;
        spec.precision = precision;
        if (!session.load(*store, {}, spec)) throw std::runtime_error(session.error());
    }
    void refresh() {
        if (!session.refresh(*store)) throw std::runtime_error(session.error());
    }
    void render(std::int32_t frames = 512) {
        l.assign(static_cast<std::size_t>(frames), -7.0f);
        r.assign(static_cast<std::size_t>(frames), -7.0f);
        float* out[2] = {l.data(), r.data()};
        AudioIo io;
        io.out = out;
        io.numOut = 2;
        io.frames = frames;
        session.process(io);
    }
    /// Render past any ramp, and leave the last block in l and r.
    void settle() {
        render();
        render();
    }
    /// ADR-0164: a lane on a track's strip parameter, points in nanoseconds.
    struct Point {
        double seconds;
        double value;
        std::uint8_t curve = 1;   // linear; 0 holds
    };
    void lane(std::int64_t id, std::int64_t trackId, const char* param, std::vector<Point> points,
              const char* domain = "real", const char* owner = "track") {
        SQLite::Statement q(store->db(),
            "INSERT INTO automation_lanes(id, owner_kind, owner_id, param_ref, time_base, value_domain) "
            "VALUES (?, ?, ?, ?, 1, ?)");
        q.bind(1, id);
        q.bind(2, owner);
        q.bind(3, trackId);
        q.bind(4, param);
        q.bind(5, domain);
        q.exec();
        std::vector<AutomationPoint> recs;
        for (std::size_t i = 0; i < points.size(); ++i) {
            AutomationPoint a{};
            a.time = static_cast<std::int64_t>(std::llround(points[i].seconds * 1e9));
            a.value = points[i].value;
            a.curve = points[i].curve;
            a.point_id = i + 1;
            recs.push_back(a);
        }
        // Nanosecond times, and the header says so: a lane with time_base 1
        // refuses a stream that claims ticks (SPEC 6.3).
        const auto blob = writeStream<AutomationPoint>(FourCC::Automation, recs, 1,
                                                        StreamFlags::SortedByTime | StreamFlags::TimeIsNanos);
        if (!store->putAutomationData(id, std::nullopt, blob)) throw std::runtime_error("AAUT");
    }
    /// Render `seconds` of playback into `hist`, left channel.
    std::vector<float> hist;
    void play(double seconds) {
        const auto frames = static_cast<std::int64_t>(seconds * 48000.0);
        for (std::int64_t done = 0; done < frames; done += 512) {
            render(512);
            hist.insert(hist.end(), l.begin(), l.end());
        }
    }
    [[nodiscard]] float at(double seconds) const { return hist[static_cast<std::size_t>(seconds * 48000.0)]; }
    [[nodiscard]] bool mentions(const std::string& needle) const {
        for (const auto& p : session.problems())
            if (p.find(needle) != std::string::npos) return true;
        return false;
    }
};

// ---------------------------------------------------------------------------

void testPanLaws() {
    section("ADR-0163 d2 -- the four pan laws, exact where they must be");
    const auto live0 = panGains(0.0, PanLaw::Live);
    check(live0.left == 1.0f && live0.right == 1.0f, "Live's law is exactly unity at the centre");
    const auto liveR = panGains(1.0, PanLaw::Live);
    check(liveR.left == 0.0f && near(liveR.right, std::numbers::sqrt2),
          "and a hard side raises that channel +3 dB and silences the other");
    const auto liveL = panGains(-1.0, PanLaw::Live);
    check(near(liveL.left, std::numbers::sqrt2) && liveL.right == 0.0f, "hard left, mirrored");
    const auto liveQ = panGains(0.5, PanLaw::Live);
    check(near(liveQ.left * liveQ.left + liveQ.right * liveQ.right, 2.0, 1e-5),
          "constant power at every position: L^2 + R^2 = 2");

    const auto eq0 = panGains(0.0, PanLaw::EqualPower);
    check(near(eq0.left, std::numbers::sqrt2 / 2) && near(eq0.right, std::numbers::sqrt2 / 2),
          "Equal Power is -3 dB at the centre");
    const auto eqR = panGains(1.0, PanLaw::EqualPower);
    check(eqR.left == 0.0f && eqR.right == 1.0f, "and unity at a hard side");

    const auto bal = panGains(0.5, PanLaw::Balance);
    check(bal.right == 1.0f && near(bal.left, 0.5), "Balance keeps the near side and fades the far one");
    const auto lin = panGains(0.0, PanLaw::Linear);
    check(lin.left == 0.5f && lin.right == 0.5f, "Linear is -6 dB at the centre");

    const auto clamp = panGains(3.0, PanLaw::Live);
    check(clamp.left == 0.0f, "pan is clamped to -1..1");
    const auto nan = panGains(std::nan(""), PanLaw::Live);
    check(nan.left == 1.0f && nan.right == 1.0f, "a pan that is not a number reads as the centre");

    check(faderGain(0.0) == 1.0, "0 dB is exactly unity");
    check(near(faderGain(-6.0206), 0.5, 1e-4), "-6.02 dB halves");
    check(faderGain(-150.0) == 0.0 && faderGain(-INFINITY) == 0.0, "-150 dB and -inf are silence");
}

void testASendNode() {
    section("ADR-0203 d3 -- a send's node: a strip's gain stage, which plays no note");

    // A stereo sine whose samples a float cannot hold, one block of it.
    constexpr int n = 512;
    std::vector<double> inL(n), inR(n);
    for (int i = 0; i < n; ++i) {
        const double ph = 2.0 * std::numbers::pi * 220.0 * i / 48000.0;
        inL[static_cast<std::size_t>(i)] = 0.3 * std::sin(ph);
        inR[static_cast<std::size_t>(i)] = -0.2 * std::sin(1.5 * ph);
    }
    // Left then right, as doubles, from one block at either width.
    auto run = [&](SendNode& s, bool wide) {
        std::vector<double> l(n, 7.0), r(n, 7.0);
        std::vector<float> lf(n, 7.0f), rf(n, 7.0f), il(n), ir(n);
        for (std::size_t i = 0; i < il.size(); ++i) {
            il[i] = static_cast<float>(inL[i]);
            ir[i] = static_cast<float>(inR[i]);
        }
        const double* in64[2] = {inL.data(), inR.data()};
        double* out64[2] = {l.data(), r.data()};
        const float* in32[2] = {il.data(), ir.data()};
        float* out32[2] = {lf.data(), rf.data()};
        NodeIo io;
        io.channels = 2;
        io.frames = n;
        io.sampleRate = 48000.0;
        if (wide) { io.in64 = in64; io.out64 = out64; }
        else      { io.in = in32; io.out = out32; }
        s.process(io);
        std::vector<double> out;
        for (std::size_t i = 0; i < l.size(); ++i) out.push_back(wide ? l[i] : static_cast<double>(lf[i]));
        for (std::size_t i = 0; i < r.size(); ++i) out.push_back(wide ? r[i] : static_cast<double>(rf[i]));
        return out;
    };
    std::vector<double> in64(inL), in32;
    in64.insert(in64.end(), inR.begin(), inR.end());
    for (double x : in64) in32.push_back(static_cast<double>(static_cast<float>(x)));
    auto silent = [](const std::vector<double>& v) {
        for (double x : v) if (x != 0.0) return false;
        return true;
    };
    auto made = [](double db, double pan, bool on) {
        auto s = std::make_unique<SendNode>();
        s->set(db, pan, PanLaw::Live, on);
        s->prepare(48000.0, n);
        return s;
    };

    check(run(*made(0.0, 0.0, true), true) == in64, "0 dB, centred and on, at 64: its input bit for bit");
    check(run(*made(0.0, 0.0, true), false) == in32, "and at 32");
    check(silent(run(*made(kSilenceDb, 0.0, true), true)) && silent(run(*made(kSilenceDb, 0.0, true), false)),
          "-150 dB is silence, from the first sample: a send starts at its level, not ramping to it");
    check(silent(run(*made(std::nan(""), 0.0, true), true)), "a level that is not a number is silent");
    check(silent(run(*made(0.0, 0.0, false), true)), "a send switched off is silent");
    {
        const std::vector<double> half = run(*made(-6.0206, 0.0, true), true);
        double worst = 0.0;
        for (std::size_t i = 0; i < half.size(); ++i) worst = std::max(worst, std::fabs(half[i] - 0.5 * in64[i]));
        check(worst < 1e-5, "-6.02 dB halves it: worst " + std::to_string(worst));
    }
    {
        const std::vector<double> right = run(*made(0.0, 1.0, true), true);
        bool leftSilent = true;
        for (int i = 0; i < n; ++i) leftSilent = leftSilent && right[static_cast<std::size_t>(i)] == 0.0;
        check(leftSilent, "a send's pan, hard right under Live's law: the left is silent");
    }
    {
        // A move ramps over five milliseconds rather than jumping.
        auto s = made(0.0, 0.0, true);
        run(*s, true);
        s->set(-INFINITY, 0.0, PanLaw::Live, true);
        const std::vector<double> moved = run(*s, true);
        check(std::fabs(moved[1] - in64[1]) < 1e-3 && moved[300] == 0.0 && moved[100] != 0.0,
              "a level move ramps: still near the old level at once, gone after 240 samples");
    }
    {
        // A send pulled down while it sleeps -- its source silent, so the
        // graph skips it and only tells it so -- lands the level there and
        // then: woken, it does not play its old level's first 5 ms.
        NodeIo asleep;
        asleep.channels = 2;
        asleep.frames = n;
        for (const bool wide : {true, false}) {
            auto s = made(0.0, 0.0, true);
            run(*s, wide);
            s->set(kSilenceDb, 0.0, PanLaw::Live, true);
            s->silenceTaps(asleep);
            check(silent(run(*s, wide)), std::string("a send pulled down while asleep wakes silent, at ") +
                                             (wide ? "64" : "32"));
        }
        // A strip the same: a track muted in a gap between its clips.
        StripNode strip;
        strip.setStatic(0.0, 0.0, PanLaw::Live, true);
        strip.prepare(48000.0, n);
        std::vector<float> ol(n, 7.0f), orr(n, 7.0f), il(n, 0.5f), ir(n, 0.5f);
        const float* lin[2] = {il.data(), ir.data()};
        float* out[2] = {ol.data(), orr.data()};
        NodeIo io;
        io.channels = 2;
        io.frames = n;
        io.in = lin;
        io.out = out;
        strip.process(io);
        strip.setStatic(0.0, 0.0, PanLaw::Live, false);
        strip.silenceTaps(asleep);
        strip.process(io);
        float loudest = 0.0f;
        for (float x : ol) loudest = std::max(loudest, std::fabs(x));
        check(loudest == 0.0f, "a strip muted while asleep wakes silent: loudest " + std::to_string(loudest));
    }
    {
        // A level past the top -- +inf, a number whose gain a float cannot
        // hold, one no host offers -- plays at kSendTopDb, never as infinity.
        for (const double db : {std::numeric_limits<double>::infinity(), 1e39, 800.0}) {
            const std::vector<double> loud = run(*made(db, 0.0, true), true);
            const double g = faderGain(kSendTopDb);
            double worst = 0.0;
            bool finite = true;
            for (std::size_t i = 0; i < loud.size(); ++i) {
                finite = finite && std::isfinite(loud[i]);
                worst = std::max(worst, std::fabs(loud[i] - in64[i] * g));
            }
            check(finite && worst < 1e-12, "a send at " + std::to_string(db) + " dB plays at the top, " +
                                               std::to_string(kSendTopDb) + " dB: worst " + std::to_string(worst));
        }
    }
    SendNode s;
    check(s.eventFlow() == EventFlow::Stop && s.tailSamples() == 0 && s.processesDouble() && s.latencySamples() == 0,
          "it stops notes, holds nothing back, takes double, and adds no latency");
    {
        // The lanes a send's source stage reads are the realiser's to keep
        // alive with every graph that plays the send, as a strip's are (ADR-0164).
        auto program = std::make_shared<AutomationLaneProgram>();
        const long before = program.use_count();
        StripLanes lanes;
        lanes.mute = program.get();
        StripConfig given;
        given.lanes = &lanes;
        given.keepAlive = program;
        s.setSource(given, nullptr);
        given = {};
        check(s.sourceLifetime() != nullptr && program.use_count() > before,
              "a send given a source's lanes hands out what keeps them alive");
        s.setSource({}, nullptr);
        check(s.sourceLifetime() == nullptr && program.use_count() == before,
              "and, given none, holds nothing");
    }
    {
        // Its source stage plays a strip's values, track delay included: a
        // Post send at 0 dB is what the strip would play, and reports the
        // strip's latency, so the edge after it waits as the strip's does.
        auto half = made(0.0, 0.0, true);
        StripConfig strip;
        strip.volumeDb = -6.0206;
        strip.delaySamples = 100;
        half->setSource(strip, nullptr);
        const std::vector<double> out = run(*half, true);
        double worst = 0.0;
        for (std::size_t i = 0; i < out.size(); ++i) worst = std::max(worst, std::fabs(out[i] - 0.5 * in64[i]));
        check(worst < 1e-5 && half->latencySamples() == -100,
              "a source at -6.02 dB halves it, and 100 samples late is latency -100: worst " + std::to_string(worst) +
                  ", latency " + std::to_string(half->latencySamples()));
    }
}

void testVolumePanMute() {
    section("ADR-0163 d1, d3 -- a fader, a pan and a mute reach the output, ramped");
    Mix m("vpm");
    m.track(1, "audio", "A", 0.5f);
    m.track(2, "audio", "B", 0.25f);
    m.track(9, "master", "Master");
    m.load();
    m.render();
    check(m.l[0] == 0.75f && m.l[511] == 0.75f && m.r[0] == 0.75f,
          "at 0 dB and centred, the mix is exact -- and there is no fade-in on load");

    m.set("mixer.setVolume", 1, "db", -6.0206);
    m.refresh();
    m.render();
    check(m.l[0] > 0.74f, "a fader move does not jump: the first sample is still the old level");
    check(m.l[100] < m.l[0] && m.l[200] < m.l[100], "it ramps down");
    check(near(m.l[300], 0.5, 1e-4), "and lands on the new level after five milliseconds");
    bool monotonic = true;
    for (std::size_t i = 1; i < 300; ++i)
        if (m.l[i] > m.l[i - 1] + 1e-7f) monotonic = false;
    check(monotonic, "without going back up");

    m.set("mixer.setPan", 2, "pan", 1.0);
    m.refresh();
    m.settle();
    check(near(m.l[511], 0.25, 1e-4) && near(m.r[511], 0.25 + 0.25 * std::numbers::sqrt2, 1e-4),
          "a hard-right pan: B leaves the left and rises 3 dB on the right (Live's law)");

    m.set("track.setMute", 1, "muted", true);
    m.refresh();
    m.render();
    check(m.l[0] > 0.2f, "a mute ramps too: the first sample is not silence");
    check(m.l[511] == 0.0f, "and the track is silent after the ramp");
    m.set("track.setMute", 1, "muted", false);
    m.refresh();
    m.settle();
    check(near(m.l[511], 0.25, 1e-4), "unmuted, it comes back");

    m.set("mixer.setVolume", 9, "db", -6.0206);
    m.refresh();
    m.settle();
    check(near(m.l[511], 0.125, 1e-4), "the master's fader scales the whole mix");

    m.store->db().exec("UPDATE mixer_strip SET pan_law = 7 WHERE track_id = 2");
    m.refresh();
    bool named = false;
    for (const auto& p : m.session.problems())
        if (p.find("tracks#2: pan law 7") != std::string::npos) named = true;
    check(named, "an unknown pan law is named, and Live's is used");
}

void testTrackDelay() {
    section("ADR-0172 -- mixer.setDelay: a track delay played as latency, either sign");

    // The strip alone: the sign, the rate and the limit.
    StripNode s;
    s.prepare(96000.0, 512);
    s.setDelay(100, 48000);
    check(s.latencySamples() == -200, "100 samples late at 48 kHz is 200 at 96 kHz, as latency -200");
    s.setDelay(-100, 48000);
    check(s.latencySamples() == 200, "early is positive latency");
    s.setDelay(10 * 48000, 48000);
    check(s.latencySamples() == -96000, "held to one second either way");

    // Where the step lands at the master, first sample at or above `level`.
    auto firstAt = [](const std::vector<float>& h, float level) {
        for (std::size_t i = 0; i < h.size(); ++i)
            if (h[i] >= level - 1e-6f) return static_cast<long>(i);
        return -1L;
    };
    auto run = [&](std::int64_t delayA, double rate, const char* name) {
        Mix m(name);
        StepNode a(0.5f, 1000), b(0.25f, 1000);
        m.track(1, "audio", "A");
        m.track(2, "audio", "B");
        m.track(9, "master", "Master");
        m.extra[1] = &a;
        m.extra[2] = &b;
        if (delayA != 0) m.set("mixer.setDelay", 1, "samples", delayA);
        m.load(rate);
        m.hist.clear();
        for (int k = 0; k < 8; ++k) {
            m.render(512);
            m.hist.insert(m.hist.end(), m.l.begin(), m.l.end());
        }
        struct Result {
            long first, full;
            float between;   // frame 1050: who plays before the other arrives
        };
        return Result{firstAt(m.hist, 0.25f), firstAt(m.hist, 0.75f), m.hist[1050]};
    };
    const auto none = run(0, 48000.0, "delay0");
    check(none.first == 1000 && none.full == 1000, "no delay: both steps at frame 1000");
    const auto late = run(100, 48000.0, "delay+");
    check(late.first == 1000 && late.full == 1100 && late.between == 0.25f,
          "+100: B alone from 1000, A joins 100 samples later at 1100");
    const auto early = run(-100, 48000.0, "delay-");
    check(early.first == 1000 && early.full == 1100 && early.between == 0.5f,
          "-100: A alone from 1000, B joins 100 samples later: A leads by exactly 100");
    const auto fast = run(100, 96000.0, "delay96");
    check(fast.full - fast.first == 200, "at 96 kHz the same 100 project samples are 200");

    // Alone, a delayed track is still delayed: delay compensation starts from zero.
    Mix alone("delay-alone");
    StepNode a(0.5f, 1000);
    alone.track(1, "audio", "A");
    alone.track(9, "master", "Master");
    alone.extra[1] = &a;
    alone.set("mixer.setDelay", 1, "samples", 300);
    alone.load();
    alone.hist.clear();
    for (int k = 0; k < 4; ++k) {
        alone.render(512);
        alone.hist.insert(alone.hist.end(), alone.l.begin(), alone.l.end());
    }
    check(firstAt(alone.hist, 0.5f) == 1300, "a track alone is delayed too: its step at 1300");

    // The master is named, not played; past one second is named and held.
    Mix named("delay-named");
    named.track(1, "audio", "A", 0.5f);
    named.track(9, "master", "Master");
    named.set("mixer.setDelay", 9, "samples", 480);
    named.set("mixer.setDelay", 1, "samples", 5 * 48000);
    named.load();
    check(named.mentions("tracks#9: the master's delay is not played"), "a delay on the master is named");
    check(named.mentions("tracks#1: a delay of 240000 samples is past one second"),
          "a delay past one second is named, and played at one second");
}

/// A send row, written as a converter or the next ops would write it.
void sendRow(Mix& m, std::int64_t id, std::int64_t from, std::int64_t to, double db = 0.0,
             bool enabled = true, bool pre = false) {
    SQLite::Statement q(m.store->db(),
        "INSERT INTO routing(id,src_kind,src_id,dst_kind,dst_id,kind,gain_db,enabled,pre_fader) "
        "VALUES(?,'track',?,'track',?,'send',?,?,?)");
    q.bind(1, id);
    q.bind(2, from);
    q.bind(3, to);
    q.bind(4, db);
    q.bind(5, enabled ? 1 : 0);
    q.bind(6, pre ? 1 : 0);
    q.exec();
}

void testPreSendsAndTheSendsPan() {
    section("ADR-0203 d3 -- a Pre send leaves before the fader; a send has a pan of its own");

    for (const Precision width : {Precision::Float32, Precision::Float64}) {
        const std::string at = width == Precision::Float64 ? " (at 64)" : "";
        Mix m(width == Precision::Float64 ? "pre64" : "pre32");
        m.track(1, "audio", "A", 0.5f);
        m.track(4, "return", "R");
        m.track(9, "master", "Master");
        sendRow(m, 11, 1, 4, 0.0, true, true);
        m.set("mixer.setVolume", 1, "db", kSilenceDb);
        m.load(48000.0, width);
        m.settle();
        check(m.l[511] == 0.5f, "the fader all the way down: the dry is gone and the Pre send plays on" + at +
                                    ": " + std::to_string(m.l[511]));
        m.set("mixer.setVolume", 1, "db", -6.0206);
        m.set("mixer.setPan", 1, "pan", -1.0);
        m.refresh();
        m.settle();
        check(near(m.r[511], 0.5, 1e-6), "the source's pan does not reach it either: hard left, the right is "
                                         "the send's alone" + at + ": " + std::to_string(m.r[511]));
        m.set("track.setMute", 1, "muted", true);
        m.refresh();
        m.settle();
        check(m.l[511] == 0.0f && m.r[511] == 0.0f, "the source's mute silences it" + at);
    }
    {
        // The mute lane too: the strip mutes the dry, the send's own gate the wet.
        Mix m("pre-lane");
        m.track(1, "audio", "A", 0.5f);
        m.track(4, "return", "R");
        m.track(9, "master", "Master");
        sendRow(m, 11, 1, 4, 0.0, true, true);
        m.lane(1, 1, "mute", {{0.0, 0.0, 0}, {0.02, 1.0, 0}});
        m.load();
        m.session.transport().play();
        m.hist.clear();
        m.play(0.05);
        check(m.at(0.01) == 1.0f, "before the lane mutes: the dry and the Pre send, 1.0: " + std::to_string(m.at(0.01)));
        check(m.at(0.04) == 0.0f, "after it: both silent, the send's gate reading the lane: " +
                                      std::to_string(m.at(0.04)));
    }
    {
        // A Pre send leaves before the strip, so the track delay is not on it.
        Mix m("pre-delay");
        StepNode a(0.5f, 1000);
        m.track(1, "audio", "A");
        m.track(4, "return", "R");
        m.track(9, "master", "Master");
        m.extra[1] = &a;
        sendRow(m, 11, 1, 4, 0.0, true, true);
        m.set("mixer.setDelay", 1, "samples", 300);
        m.load();
        m.hist.clear();
        for (int k = 0; k < 4; ++k) {
            m.render(512);
            m.hist.insert(m.hist.end(), m.l.begin(), m.l.end());
        }
        long wet = -1, both = -1;
        for (std::size_t i = 0; i < m.hist.size(); ++i) {
            if (wet < 0 && m.hist[i] >= 0.5f - 1e-6f) wet = static_cast<long>(i);
            if (both < 0 && m.hist[i] >= 1.0f - 1e-6f) both = static_cast<long>(i);
        }
        check(wet == 1000 && both == 1300, "the track 300 late: its Pre send at 1000, the dry at 1300 (" +
                                               std::to_string(wet) + ", " + std::to_string(both) + ")");
    }
    {
        // The send's own pan (ADI's addition, ADR-0203 d3), Live's law: hard
        // left, the return hears the left alone, 3 dB up.
        Mix m("send-pan");
        m.track(1, "audio", "A", 0.5f);
        m.track(4, "return", "R");
        m.track(9, "master", "Master");
        sendRow(m, 11, 1, 4);
        m.store->db().exec("UPDATE routing SET pan=-1 WHERE id=11");
        m.load();
        m.settle();
        check(m.r[511] == 0.5f && near(m.l[511], 0.5 + 0.5 * std::numbers::sqrt2, 1e-5),
              "a send panned hard left: the right is the dry alone, the left the dry and the send up 3 dB");
    }
}

void testHearing() {
    section("ADR-0203 d3 -- who is heard under solo, across sends: a soloed return hears its senders");

    // Bare models: tracks by id and kind, sends by row. Master is 1.
    struct T {
        std::int64_t id;
        const char* kind;
        std::optional<std::int64_t> parent = std::nullopt;
        bool muted = false, soloed = false, defeat = false;
    };
    struct S {
        std::int64_t id, from, to;
        bool pre = false;
    };
    auto heardIn = [](std::vector<T> tracks, std::vector<S> sends, HearingFlags flags = {}) {
        rows::Model m;
        m.tracks.push_back([] { rows::Track t; t.id = 1; t.kind = "master"; t.name = "Master"; return t; }());
        for (const T& x : tracks) {
            rows::Track t;
            t.id = x.id;
            t.kind = x.kind;
            t.name = "t" + std::to_string(x.id);
            t.parentId = x.parent;
            t.muted = x.muted;
            t.soloed = x.soloed;
            t.soloDefeat = x.defeat;
            m.tracks.push_back(t);
        }
        for (const S& s : sends) {
            rows::Routing r;
            r.id = s.id;
            r.srcKind = r.dstKind = "track";
            r.srcId = s.from;
            r.dstId = s.to;
            r.kind = "send";
            r.preFader = s.pre;
            m.routing.push_back(r);
        }
        return hearing(m, planGraph(m), flags);
    };
    auto is = [](const std::map<std::int64_t, bool>& m, std::int64_t id) {
        const auto it = m.find(id);
        return it != m.end() && it->second;
    };

    {
        const Hearing h = heardIn({{2, "audio"}, {7, "return"}}, {{11, 2, 7}});
        check(is(h.track, 2) && is(h.track, 7) && is(h.send, 11), "no solo: everything heard, the send too");
        const Hearing m = heardIn({{2, "audio", std::nullopt, true}, {7, "return"}}, {{11, 2, 7}});
        check(!is(m.track, 2) && is(m.send, 11) && is(m.track, 7),
              "a muted track is silent; its send's gate is solo's alone, the mute is the send's first stage");
    }
    {
        // R soloed: it hears A and B though they are soloed away; C is muted.
        const Hearing h = heardIn({{2, "audio"}, {3, "audio"}, {4, "audio", std::nullopt, true},
                                   {7, "return", std::nullopt, false, true}},
                                  {{11, 2, 7}, {12, 3, 7}, {13, 4, 7}});
        check(is(h.track, 7) && is(h.track, 1) && !is(h.track, 2) && !is(h.track, 3),
              "R soloed: R and the master heard, its senders' own outputs not");
        check(is(h.send, 11) && is(h.send, 12) && is(h.send, 13),
              "and R hears what A, B and C send it (C, muted, is silent at its send's first stage)");
    }
    {
        // R soloed hears a group's send: the children open into the group
        // alone, the group's own output stays closed.
        const Hearing h = heardIn({{2, "audio", 6}, {3, "audio", 6}, {6, "group"},
                                   {7, "return", std::nullopt, false, true}},
                                  {{11, 6, 7}});
        check(is(h.track, 2) && is(h.track, 3) && !is(h.track, 6) && is(h.send, 11),
              "R soloed hears G's send: G's children open into G, G's own output closed");
        // R and the child X soloed: G is heard through X, so Y stays closed.
        const Hearing x = heardIn({{2, "audio", 6, false, true}, {3, "audio", 6}, {6, "group"},
                                   {7, "return", std::nullopt, false, true}},
                                  {{11, 6, 7}});
        check(is(x.track, 2) && !is(x.track, 3) && is(x.track, 6) && is(x.send, 11),
              "R and X soloed: X and G heard, its sibling Y not");
    }
    {
        // A send's send: A into B, B into the soloed R.
        const Hearing h = heardIn({{2, "audio"}, {3, "audio"}, {7, "return", std::nullopt, false, true}},
                                  {{11, 2, 3}, {12, 3, 7}});
        check(!is(h.track, 2) && !is(h.track, 3) && is(h.send, 11) && is(h.send, 12),
              "R soloed hears B's send and, through it, A's: neither one's own output");
    }
    {
        // Both ends heard, by defeat: T soloed; A and its return R defeated.
        const Hearing h = heardIn({{2, "audio", std::nullopt, false, true}, {3, "audio", std::nullopt, false, false, true},
                                   {7, "return", std::nullopt, false, false, true}},
                                  {{11, 3, 7}});
        check(is(h.track, 3) && is(h.track, 7) && is(h.send, 11), "a defeated A into its defeated return: the send plays");
        HearingFlags off;
        off.soloInPlace = false;
        const Hearing t = heardIn({{2, "audio", std::nullopt, false, true}, {7, "return", std::nullopt, false, false, true}},
                                  {{11, 2, 7}}, off);
        check(is(t.track, 7) && is(t.send, 11), "Solo in Place off: soloed T into its defeated return still plays");
    }
    {
        // Pre sends told not to follow solo play wherever they go.
        HearingFlags flags;
        flags.preFollowsSolo = false;
        const Hearing h = heardIn({{2, "audio", std::nullopt, false, true}, {3, "audio"}, {4, "audio"}},
                                  {{11, 3, 4, true}}, flags);
        check(is(h.send, 11), "a Pre send not following solo plays into a track nothing else opens");
    }
    {
        // A soloed VCA is ignored, as a soloed master is: it carries no audio.
        const Hearing h = heardIn({{2, "audio"}, {5, "vca", std::nullopt, false, true}}, {});
        check(is(h.track, 2), "a soloed VCA silences nothing");
    }
    {
        // A grandchild of a soloed group is heard.
        const Hearing h = heardIn({{2, "audio", 5}, {5, "group", 6}, {6, "group", std::nullopt, false, true}}, {});
        check(is(h.track, 2) && is(h.track, 5), "G soloed: its child group and that group's child");
    }
    {
        // keepAllReturns: every return stays heard under Solo in Place.
        const std::vector<T> tracks{{2, "audio", std::nullopt, false, true}, {7, "return"}, {8, "return"}};
        const std::vector<S> sends{{11, 2, 7}};
        HearingFlags all;
        all.keepAllReturns = true;
        check(!is(heardIn(tracks, sends).track, 8) && is(heardIn(tracks, sends, all).track, 8),
              "a return nothing heard sends to: closed, or heard when every return is kept");
    }
    {
        // A soloed, Solo in Place: A, its return, the master. B is not, nor its send.
        const std::vector<T> tracks{{2, "audio", std::nullopt, false, true}, {3, "audio"}, {7, "return"}};
        const std::vector<S> sends{{11, 2, 7}, {12, 3, 7}};
        const Hearing on = heardIn(tracks, sends);
        check(is(on.track, 2) && is(on.track, 7) && !is(on.track, 3) && is(on.send, 11) && !is(on.send, 12),
              "A soloed, Solo in Place: A and its return, and nothing of B or B's send");
        HearingFlags off;
        off.soloInPlace = false;
        const Hearing h = heardIn(tracks, sends, off);
        check(is(h.track, 2) && !is(h.track, 7) && !is(h.send, 11),
              "Solo in Place off: A alone, its return and its send silent");
    }
    {
        // G soloed: its child C, and the return C sends to.
        const Hearing h = heardIn({{4, "audio", 6}, {6, "group", std::nullopt, false, true}, {7, "return"}},
                                  {{13, 4, 7}});
        check(is(h.track, 4) && is(h.track, 6) && is(h.track, 7) && is(h.send, 13),
              "G soloed: its child, and the return its child sends to, and the send");
    }
    {
        // A soloed sends to R; D is solo-defeated and sends to R and to S,
        // which nothing heard sends to.
        const Hearing h = heardIn({{2, "audio", std::nullopt, false, true}, {5, "audio", std::nullopt, false, false, true},
                                   {7, "return"}, {8, "return"}},
                                  {{11, 2, 7}, {14, 5, 7}, {15, 5, 8}});
        check(is(h.track, 5) && is(h.send, 14) && !is(h.send, 15) && !is(h.track, 8),
              "a defeated D is heard, and its send into a heard return; not one into a return nothing opens");
    }
    {
        // B's Pre send under A's solo, which keeps R open.
        const std::vector<T> tracks{{2, "audio", std::nullopt, false, true}, {3, "audio"}, {7, "return"}};
        const std::vector<S> sends{{11, 2, 7}, {12, 3, 7, true}};
        check(!is(heardIn(tracks, sends).send, 12), "a Pre send follows its source's solo (preFollowsSolo)");
        HearingFlags flags;
        flags.preFollowsSolo = false;
        check(is(heardIn(tracks, sends, flags).send, 12), "and, told not to, plays into the open return");
    }
    {
        // A send into a soloed group's child: the child is listening.
        const Hearing h = heardIn({{2, "audio"}, {4, "audio", 6}, {6, "group", std::nullopt, false, true}},
                                  {{11, 2, 4}});
        check(!is(h.track, 2) && is(h.send, 11), "a send into a soloed group's child is heard; its sender is not");
    }
    {
        // Soloing the master is ignored.
        const Hearing ms = [&] {
            rows::Model mm;
            rows::Track master;
            master.id = 1;
            master.kind = "master";
            master.name = "Master";
            master.soloed = true;
            rows::Track a;
            a.id = 2;
            a.kind = "audio";
            a.name = "A";
            rows::Track r;
            r.id = 7;
            r.kind = "return";
            r.name = "R";
            mm.tracks = {master, a, r};
            rows::Routing s;
            s.id = 11;
            s.srcKind = s.dstKind = "track";
            s.srcId = 2;
            s.dstId = 7;
            s.kind = "send";
            mm.routing = {s};
            return hearing(mm, planGraph(mm));
        }();
        check(is(ms.track, 2) && is(ms.track, 7) && is(ms.send, 11), "the master soloed is ignored: all heard");
    }
}

/// A main route row: what the user's own output routing writes.
void mainRoute(Mix& m, std::int64_t id, std::int64_t from, std::int64_t to) {
    SQLite::Statement q(m.store->db(),
        "INSERT INTO routing(id,src_kind,src_id,dst_kind,dst_id,kind,gain_db,enabled) "
        "VALUES(?,'track',?,'track',?,'main',0,1)");
    q.bind(1, id);
    q.bind(2, from);
    q.bind(3, to);
    q.exec();
}

void testSoloAcrossSends() {
    section("ADR-0203 d3 -- solo across sends: a soloed return hears its senders, and nothing leaks");

    for (const Precision width : {Precision::Float32, Precision::Float64}) {
        const std::string at = width == Precision::Float64 ? " (at 64)" : "";
        Mix m(width == Precision::Float64 ? "solo-r64" : "solo-r32");
        m.track(1, "audio", "A", 0.5f);
        m.track(2, "audio", "B", 0.25f);
        m.track(4, "return", "R");
        m.track(9, "master", "Master");
        sendRow(m, 11, 1, 4);
        sendRow(m, 12, 2, 4);
        m.set("track.setMute", 2, "muted", true);
        m.set("track.setSolo", 4, "soloed", true);
        m.load(48000.0, width);
        m.settle();
        check(m.l[511] == 0.5f, "R soloed hears A's send and not A's own output, nor the muted B" + at + ": " +
                                    std::to_string(m.l[511]));
    }
    {
        // A Pre send into a soloed return, from a track pulled all the way down.
        Mix m("solo-pre");
        m.track(1, "audio", "A", 0.5f);
        m.track(4, "return", "R");
        m.track(9, "master", "Master");
        sendRow(m, 11, 1, 4, 0.0, true, true);
        m.set("mixer.setVolume", 1, "db", kSilenceDb);
        m.set("track.setSolo", 4, "soloed", true);
        m.load();
        m.settle();
        check(m.l[511] == 0.5f, "a soloed return hears a Pre send too: " + std::to_string(m.l[511]));
    }
    {
        // A soloed group whose child sends: the child through the group, and its send.
        Mix m("solo-group");
        m.track(1, "audio", "C", 0.5f);
        m.track(2, "audio", "D", 0.25f);
        m.track(4, "return", "R");
        m.track(5, "group", "G");
        m.track(9, "master", "Master");
        m.set("track.setParent", 1, "parent", 5);
        sendRow(m, 11, 1, 4);
        sendRow(m, 12, 2, 4);
        m.set("track.setSolo", 5, "soloed", true);
        m.load();
        m.settle();
        check(m.l[511] == 1.0f, "G soloed: its child, 0.5, and the child's send, 0.5; nothing of D: " +
                                    std::to_string(m.l[511]));
    }
    {
        // R soloed hears a group's send: X 0.5 and Y 0.25 through G, G's own
        // output closed. Then R and X soloed: X's dry through G, and R
        // carrying X alone -- never Y.
        auto run = [](bool soloX) {
            Mix m(soloX ? "solo-gx" : "solo-g");
            m.track(1, "audio", "X", 0.5f);
            m.track(2, "audio", "Y", 0.25f);
            m.track(4, "return", "R");
            m.track(5, "group", "G");
            m.track(9, "master", "Master");
            m.set("track.setParent", 1, "parent", 5);
            m.set("track.setParent", 2, "parent", 5);
            sendRow(m, 11, 5, 4);
            m.set("track.setSolo", 4, "soloed", true);
            if (soloX) m.set("track.setSolo", 1, "soloed", true);
            m.load();
            m.settle();
            return m.l[511];
        };
        const float g = run(false), gx = run(true);
        check(g == 0.75f && gx == 1.0f, "a soloed return hears a group's send (0.75); with X soloed too, 1.0, "
                                        "nothing of Y: " + std::to_string(g) + ", " + std::to_string(gx));
    }
    {
        // A defeated vocal into its defeated reverb, under another's solo.
        Mix m("solo-defeat");
        m.track(1, "audio", "T", 0.5f);
        m.track(2, "audio", "A", 0.25f);
        m.track(4, "return", "R");
        m.track(9, "master", "Master");
        sendRow(m, 11, 2, 4);
        m.store->db().exec("UPDATE tracks SET solo_defeat=1 WHERE id IN (2, 4)");
        m.set("track.setSolo", 1, "soloed", true);
        m.load();
        m.settle();
        check(m.l[511] == 1.0f, "T soloed; A and its reverb defeated: T, A, and A's wet: " + std::to_string(m.l[511]));
    }
    {
        // Pinned, for win's question 7: Solo in Place opens a track that is
        // not a return when a soloed track's send into it carries something,
        // its own audio with it.
        Mix m("solo-nonreturn");
        m.track(1, "audio", "T", 0.5f);
        m.track(2, "audio", "B", 0.25f);
        m.track(9, "master", "Master");
        sendRow(m, 11, 1, 2);
        m.set("track.setSolo", 1, "soloed", true);
        m.load();
        m.settle();
        check(m.l[511] == 1.25f, "T soloed, its send into B: T, the wet, and B's own (pinned): " +
                                     std::to_string(m.l[511]));
    }
    {
        // A soloed-away track gains its first send into a soloed return, then
        // loses it: its own output is never heard on the way.
        Mix m("solo-come-go");
        m.track(1, "audio", "A", 0.5f);
        m.track(4, "return", "R");
        m.track(9, "master", "Master");
        m.set("track.setSolo", 4, "soloed", true);
        m.load();
        m.settle();
        const float before = m.l[511];
        sendRow(m, 11, 1, 4);
        m.refresh();
        float loudest = 0.0f;
        for (int k = 0; k < 4; ++k) {
            m.render();
            for (float x : m.l) loudest = std::max(loudest, x);
        }
        const float with = m.l[511];
        m.store->db().exec("DELETE FROM routing WHERE id=11");
        m.refresh();
        for (int k = 0; k < 4; ++k) {
            m.render();
            for (float x : m.l) loudest = std::max(loudest, x);
        }
        check(before == 0.0f && with == 0.5f && loudest <= 0.5f && m.l[511] == 0.0f,
              "the first send in, then gone: only ever the wet (" + std::to_string(before) + ", " +
                  std::to_string(with) + ", loudest " + std::to_string(loudest) + ", after " +
                  std::to_string(m.l[511]) + ")");
    }
    // A Post send plays what the source's strip plays, sample for sample: the
    // dry alone against the wet alone, at 32 and 64, through each of these.
    struct Case {
        const char* what;
        double rate;
        double db, pan;
        int law;                 // SPEC 6.9's pan law
        std::int64_t delay;      // project samples (the project is at 48 kHz)
        bool lanes;              // volume, pan and mute lanes playing
        bool moves;              // a fader move, then a mute in and out, mid-run
    };
    const Case cases[] = {
        {"a fader, a pan, three lanes, a delay and a mute", 48000.0, -3.0, 0.3, 0, 100, true, true},
        {"at 96 kHz, a delay of 100 project samples played as 200", 96000.0, -6.0206, -0.2, 0, 100, false, false},
        {"at 44.1 kHz, a fader move ramping over its own 220 frames", 44100.0, 0.0, 0.0, 0, 0, false, true},
        {"constant power, panned left, 200 samples early", 48000.0, -6.0206, -0.4, 1, -200, false, false},
    };
    for (const Case& c : cases) {
        for (const Precision width : {Precision::Float32, Precision::Float64}) {
            auto run = [&c, width](const std::string& name, bool wet) {
                Mix m(name.c_str());
                SineNode a(220.0, 0.2f);
                m.track(1, "audio", "A");
                m.track(3, "audio", "Sink");
                m.track(4, "return", "R");
                m.track(9, "master", "Master");
                m.extra[1] = &a;
                m.set("mixer.setVolume", 1, "db", c.db);
                m.set("mixer.setPan", 1, "pan", c.pan);
                if (c.delay != 0) m.set("mixer.setDelay", 1, "samples", c.delay);
                if (c.lanes) {
                    m.lane(1, 1, "volume", {{0.0, -3.0}, {0.1, -12.0}});
                    m.lane(2, 1, "pan", {{0.0, 0.3}, {0.1, -0.5}});
                    m.lane(3, 1, "mute", {{0.0, 0.0, 0}, {0.05, 1.0, 0}, {0.07, 0.0, 0}});
                }
                if (wet) {
                    m.set("track.setMute", 3, "muted", true);
                    mainRoute(m, 20, 1, 3);
                    sendRow(m, 21, 1, 4);
                }
                m.load(c.rate, width);
                if (c.law != 0) {
                    m.store->db().exec("UPDATE mixer_strip SET pan_law = " + std::to_string(c.law) +
                                       " WHERE track_id = 1");
                    m.refresh();
                }
                m.session.transport().play();
                std::vector<float> out;
                for (int k = 0; k < 16; ++k) {
                    if (c.moves && k == 4) {
                        m.set("mixer.setVolume", 1, "db", c.db - 12.0);
                        m.refresh();
                    }
                    // A static mute, in and out, ramps the same in both.
                    if (c.moves && (k == 10 || k == 12)) {
                        m.set("track.setMute", 1, "muted", k == 10);
                        m.refresh();
                    }
                    m.render(512);
                    out.insert(out.end(), m.l.begin(), m.l.end());
                    out.insert(out.end(), m.r.begin(), m.r.end());
                }
                return out;
            };
            const std::string w = width == Precision::Float64 ? "64" : "32";
            const auto dry = run("strip-dry" + w, false), wet = run("strip-wet" + w, true);
            std::size_t differ = 0;
            float loudest = 0.0f;
            for (std::size_t i = 0; i < dry.size() && i < wet.size(); ++i) {
                if (dry[i] != wet[i]) ++differ;
                loudest = std::max(loudest, std::fabs(dry[i]));
            }
            check(dry.size() == wet.size() && differ == 0 && loudest > 0.01f,
                  std::string("a Post send's wet is its strip's output bit for bit: ") + c.what + ", at " + w +
                      " (" + std::to_string(differ) + " samples differ)");
        }
    }
    // A send's own level acts before the track delay, as the strip's does:
    // a level move on a track 300 samples late reaches the return 300
    // samples into the block it is made in, as a fader move would.
    {
        Mix m("send-level-late");
        m.track(1, "audio", "A", 0.5f);
        m.track(3, "audio", "Sink");
        m.track(4, "return", "R");
        m.track(9, "master", "Master");
        m.set("track.setMute", 3, "muted", true);
        mainRoute(m, 20, 1, 3);
        sendRow(m, 21, 1, 4);
        m.set("mixer.setDelay", 1, "samples", 300);
        m.load();
        m.settle();
        m.settle();
        m.store->db().exec("UPDATE routing SET gain_db=-6.0206 WHERE id=21");
        m.refresh();
        m.render(512);
        long moved = -1;
        for (std::size_t i = 0; i < m.l.size(); ++i)
            if (moved < 0 && m.l[i] != 0.5f) moved = static_cast<long>(i);
        check(m.l[0] == 0.5f && moved == 300, "a send level moved on a track 300 late: heard 300 samples on (" +
                                                  std::to_string(moved) + ")");
    }
    // A delayed track's first send, and its last, with nothing soloed: the
    // dry never moves (no node on its own path comes or goes).
    {
        Mix m("first-send-late");
        m.track(1, "audio", "A", 0.5f);
        m.track(4, "return", "R");
        m.track(9, "master", "Master");
        m.set("mixer.setDelay", 1, "samples", 300);
        m.load();
        m.settle();
        m.settle();
        sendRow(m, 11, 1, 4, kSilenceDb);
        m.refresh();
        float lo = 1.0f, hi = 0.0f;
        for (int k = 0; k < 3; ++k) {
            m.render();
            for (float x : m.l) {
                lo = std::min(lo, x);
                hi = std::max(hi, x);
            }
        }
        m.store->db().exec("DELETE FROM routing WHERE id=11");
        m.refresh();
        for (int k = 0; k < 3; ++k) {
            m.render();
            for (float x : m.l) {
                lo = std::min(lo, x);
                hi = std::max(hi, x);
            }
        }
        check(lo == 0.5f && hi == 0.5f, "a delayed track's first send in and out: its dry stays 0.5 (" +
                                            std::to_string(lo) + " .. " + std::to_string(hi) + ")");
    }
    // A sender 300 samples late, soloed away, into a soloed return: the
    // send's delayed edge crosses a rebuild.
    {
        auto run = [](const std::string& name, bool rename) {
            Mix m(name.c_str());
            SineNode a(220.0, 0.1f);
            m.track(1, "audio", "A");
            m.track(4, "return", "R");
            m.track(9, "master", "Master");
            m.extra[1] = &a;
            sendRow(m, 11, 1, 4);
            m.set("mixer.setDelay", 1, "samples", 300);
            m.set("track.setSolo", 4, "soloed", true);
            m.load();
            std::vector<float> out;
            for (int k = 0; k < 12; ++k) {
                if (rename && k == 6) {
                    m.set("track.rename", 1, "name", "A2");
                    m.refresh();
                }
                m.render(512);
                out.insert(out.end(), m.l.begin(), m.l.end());
            }
            return out;
        };
        const auto ref = run("solo-late-ref", false), edited = run("solo-late-edit", true);
        std::size_t differ = 0;
        float loudest = 0.0f;
        for (std::size_t i = 0; i < ref.size() && i < edited.size(); ++i) {
            if (ref[i] != edited[i]) ++differ;
            loudest = std::max(loudest, std::fabs(ref[i]));
        }
        check(differ == 0 && loudest > 0.05f, "a late, soloed-away sender into a soloed return crosses a rebuild (" +
                                                  std::to_string(differ) + " samples differ)");
    }
    // Solo adds no node: a project's graph is the same size soloed or not.
    {
        auto nodes = [](bool solo) {
            Mix m(solo ? "nodes-solo" : "nodes-plain");
            m.track(1, "audio", "A", 0.5f);
            m.track(2, "audio", "B", 0.25f);
            m.track(4, "return", "R");
            m.track(9, "master", "Master");
            sendRow(m, 11, 1, 4);
            if (solo) m.set("track.setSolo", 4, "soloed", true);
            m.load();
            const Graph* g = m.session.graph().currentGraph();
            return g != nullptr ? g->nodeCount() : std::size_t{0};
        };
        const std::size_t plain = nodes(false), soloed = nodes(true);
        check(plain > 0 && plain == soloed, "solo adds no node: " + std::to_string(plain) + " and " +
                                                std::to_string(soloed));
    }
}

void testSendsPlay() {
    section("ADR-0203 d3 -- sends and returns play: after the fader, into any track, compensated");

    for (const Precision width : {Precision::Float32, Precision::Float64}) {
        const std::string at = width == Precision::Float64 ? " (at 64)" : "";
        Mix m(width == Precision::Float64 ? "sends64" : "sends32");
        m.track(1, "audio", "A", 0.5f);
        m.track(4, "return", "R");
        m.track(9, "master", "Master");
        sendRow(m, 11, 1, 4);
        m.load(48000.0, width);
        m.settle();
        check(m.l[511] == 1.0f && m.r[511] == 1.0f,
              "0.5 dry and 0.5 through the return, at 0 dB: 1.0 at the master, exactly" + at + ": " +
                  std::to_string(m.l[511]));
        check(m.session.problems().empty() || !m.mentions("routing#11"), "and nothing names the send" + at);

        m.store->db().exec("UPDATE routing SET gain_db=-6.0206 WHERE id=11");
        m.refresh();
        m.settle();
        check(near(m.l[511], 0.75, 1e-4), "the send at -6.02 dB: 0.5 + 0.25" + at);

        m.set("mixer.setVolume", 1, "db", -6.0206);
        m.refresh();
        m.settle();
        check(near(m.l[511], 0.375, 1e-4), "a Post send follows the source's fader: 0.25 + 0.125" + at);

        m.set("track.setMute", 1, "muted", true);
        m.refresh();
        m.settle();
        check(m.l[511] == 0.0f, "and its mute: the send is silent with the track" + at);
        m.set("track.setMute", 1, "muted", false);
        m.store->db().exec("UPDATE routing SET enabled=0 WHERE id=11");
        m.refresh();
        m.settle();
        check(near(m.l[511], 0.25, 1e-4), "switched off, the send is silent and the track plays on" + at);
    }
    {
        // ADI's addition: a send into a track that is not a return.
        Mix m("send-any");
        m.track(1, "audio", "A", 0.5f);
        m.track(2, "audio", "B", 0.25f);
        m.track(9, "master", "Master");
        sendRow(m, 11, 1, 2);
        m.load();
        m.settle();
        check(m.l[511] == 1.25f, "a send into an audio track sums with its own: 0.5 + (0.25 + 0.5)");
    }
    {
        // Compensation reaches across the send: a return 64 samples early
        // holds the dry track back 64, so the wet leads by exactly that.
        Mix m("send-pdc");
        StepNode a(0.5f, 1000);
        m.track(1, "audio", "A");
        m.track(4, "return", "R");
        m.track(9, "master", "Master");
        m.extra[1] = &a;
        sendRow(m, 11, 1, 4);
        m.set("mixer.setDelay", 4, "samples", -64);
        m.load();
        m.hist.clear();
        for (int k = 0; k < 4; ++k) {
            m.render(512);
            m.hist.insert(m.hist.end(), m.l.begin(), m.l.end());
        }
        long wet = -1, both = -1;
        for (std::size_t i = 0; i < m.hist.size(); ++i) {
            if (wet < 0 && m.hist[i] >= 0.5f - 1e-6f) wet = static_cast<long>(i);
            if (both < 0 && m.hist[i] >= 1.0f - 1e-6f) both = static_cast<long>(i);
        }
        check(wet == 1000 && both == 1064, "the return early by 64: the wet at 1000, the dry joins at 1064 (" +
                                               std::to_string(wet) + ", " + std::to_string(both) + ")");
    }
    {
        // A rebuild moves nothing: two sends from A, one from C whose strip
        // is 32 early so A's sends wait 32 into the return, and the return
        // 64 early so A's own output waits 64 into the master.
        auto run = [](const std::string& name, bool rename) {
            Mix m(name.c_str());
            SineNode a(220.0, 0.1f), c(330.0, 0.1f);
            m.track(1, "audio", "A");
            m.track(3, "audio", "C");
            m.track(4, "return", "R");
            m.track(9, "master", "Master");
            m.extra[1] = &a;
            m.extra[3] = &c;
            sendRow(m, 11, 1, 4, -6.0);
            sendRow(m, 12, 1, 4, -12.0);
            sendRow(m, 13, 3, 4);
            m.set("mixer.setDelay", 3, "samples", -32);
            m.set("mixer.setDelay", 4, "samples", -64);
            m.load();
            std::vector<float> out;
            for (int k = 0; k < 12; ++k) {
                if (rename && k == 6) {
                    m.set("track.rename", 1, "name", "A2");
                    m.refresh();
                }
                m.render(512);
                out.insert(out.end(), m.l.begin(), m.l.end());
            }
            return out;
        };
        const auto ref = run("sends-ref", false), edited = run("sends-edit", true);
        std::size_t differ = 0;
        for (std::size_t i = 0; i < ref.size() && i < edited.size(); ++i) differ += ref[i] != edited[i] ? 1u : 0u;
        check(differ == 0, "a rebuild with sends compensated both ways moves no sample of the master (" +
                               std::to_string(differ) + " do)");
    }
    {
        // Live's Solo in Place: a soloed track's return stays audible. B is
        // not soloed, so its own output and its send are silent.
        Mix m("send-solo");
        m.track(1, "audio", "A", 0.5f);
        m.track(2, "audio", "B", 0.25f);
        m.track(4, "return", "R");
        m.track(9, "master", "Master");
        sendRow(m, 11, 1, 4);
        sendRow(m, 12, 2, 4);
        m.set("track.setSolo", 1, "soloed", true);
        m.load();
        m.settle();
        check(m.l[511] == 1.0f, "A soloed: A and its return, 0.5 + 0.5, and nothing of B: " +
                                     std::to_string(m.l[511]));
    }
    {
        // A send into a summing group is not one of its children: the sender
        // keeps its own output as it was, and the send gets a channel half
        // of its own (ADR-0174), so the group's buss decodes what was encoded.
        Mix m("send-summing");
        m.track(1, "audio", "A", 0.5f);
        m.track(2, "audio", "Child", 0.25f);
        m.track(5, "group", "G");
        m.track(9, "master", "Master");
        m.set("track.setParent", 2, "parent", 5);
        m.set("group.setSumming", 5, "enabled", true);
        sendRow(m, 11, 1, 5);
        m.load();
        m.settle();
        const GroupSumming& s = m.session.summing();
        check(s.channelOf(2) != nullptr && s.channelOf(1) == nullptr && s.sendChannelOf(11) != nullptr &&
                  s.sendChannelOf(11) != s.channelOf(2),
              "the child has a channel half, the sender none, and the send one of its own");
    }
}

/// RMS of the left channel over the second half of `blocks` blocks of 512.
double rmsOf(Mix& m, int blocks) {
    double acc = 0.0;
    long count = 0;
    for (int k = 0; k < blocks; ++k) {
        m.render(512);
        if (k < blocks / 2) continue;
        for (float x : m.l) {
            acc += static_cast<double>(x) * static_cast<double>(x);
            ++count;
        }
    }
    return std::sqrt(acc / static_cast<double>(count));
}

void testSendsHoldTheirLevel() {
    section("ADR-0203 d3 -- a send holds its level: into summing, under solo, asleep, away and back, past the top");

    // A route into a muted sink, so the send alone reaches the group.
    auto mainRow = [](Mix& m, std::int64_t id, std::int64_t from, std::int64_t to) {
        SQLite::Statement q(m.store->db(),
            "INSERT INTO routing(id,src_kind,src_id,dst_kind,dst_id,kind,gain_db,enabled) "
            "VALUES(?,'track',?,'track',?,'main',0,1)");
        q.bind(1, id);
        q.bind(2, from);
        q.bind(3, to);
        q.exec();
    };
    // A send into a summing group plays at the level it plays with summing
    // off, at every drive (ADR-0174: summing is a colour, not a level).
    for (const char* flavor : {"console9", "console.md"}) {
        for (const double drive : {0.0, 6.0}) {
            auto viaSend = [&](bool summing) {
                Mix m((std::string("send-sum-") + (summing ? "on-" : "off-") + flavor + std::to_string(int(drive))).c_str());
                SineNode a(220.0, 0.1f);
                m.track(1, "audio", "A");
                m.track(3, "audio", "Sink");
                m.track(5, "group", "G");
                m.track(9, "master", "Master");
                m.extra[1] = &a;
                m.set("track.setMute", 3, "muted", true);
                mainRow(m, 20, 1, 3);
                sendRow(m, 21, 1, 5);
                if (summing) {
                    m.set("group.setSumming", 5, "enabled", true);
                    m.set("group.setSummingFlavor", 5, "flavor", flavor);
                    m.set("group.setSummingDrive", 5, "db", drive);
                }
                m.load();
                return rmsOf(m, 40);
            };
            const double off = viaSend(false), on = viaSend(true);
            const double db = 20.0 * std::log10(on / off);
            check(std::fabs(db) < 0.1, std::string("a send into a group summed by ") + flavor + " at +" +
                                           std::to_string(int(drive)) + " dB drive keeps its level: " +
                                           std::to_string(db) + " dB");
        }
    }
    // Summing turned on under a send whose last edge delays -- the group's
    // child plays 300 early, so everything else into the sum, the send
    // included, waits 300 -- never plays the plain send's history as a
    // console's (ADR-0092): no burst.
    {
        Mix m("sum-toggle-send");
        SineNode a(220.0, 0.1f), c(330.0, 0.1f);
        m.track(1, "audio", "A");
        m.track(2, "audio", "C");
        m.track(3, "audio", "Sink");
        m.track(5, "group", "G");
        m.track(9, "master", "Master");
        m.extra[1] = &a;
        m.extra[2] = &c;
        m.set("track.setParent", 2, "parent", 5);
        m.set("mixer.setDelay", 2, "samples", -300);
        m.set("track.setMute", 3, "muted", true);
        mainRow(m, 20, 1, 3);
        sendRow(m, 21, 1, 5);
        m.load();
        float steady = 0.0f;
        for (int k = 0; k < 16; ++k) {
            m.render(512);
            if (k >= 8)
                for (float x : m.l) steady = std::max(steady, std::fabs(x));
        }
        m.set("group.setSumming", 5, "enabled", true);
        m.refresh();
        float during = 0.0f;
        for (int k = 0; k < 4; ++k) {
            m.render(512);
            for (float x : m.l) during = std::max(during, std::fabs(x));
        }
        check(steady > 0.1f && during < 1.1f * steady,
              "summing turned on under a delayed send: no burst (steady peak " + std::to_string(steady) +
                  ", through the switch " + std::to_string(during) + ")");
    }
    // A child sending into its own summing group: two paths into the sum,
    // each through a channel half, so the level doubles as it does unsummed.
    {
        auto run = [](bool summing) {
            Mix m(summing ? "own-group-on" : "own-group-off");
            SineNode a(220.0, 0.1f);
            m.track(1, "audio", "C");
            m.track(5, "group", "G");
            m.track(9, "master", "Master");
            m.extra[1] = &a;
            m.set("track.setParent", 1, "parent", 5);
            sendRow(m, 11, 1, 5);
            if (summing) m.set("group.setSumming", 5, "enabled", true);
            m.load();
            return rmsOf(m, 40);
        };
        const double off = run(false), on = run(true);
        check(std::fabs(20.0 * std::log10(on / off)) < 0.1,
              "a child's send into its own summing group: " + std::to_string(off) + " unsummed, " +
                  std::to_string(on) + " summed");
    }
    // A summed child's Post send leaves at its strip, before its channel
    // half: the wet is the same with summing on or off. The group is muted
    // so only the wet reaches the master.
    {
        auto wetOnly = [](bool summing) {
            Mix m(summing ? "child-send-on" : "child-send-off");
            m.track(1, "audio", "A", 0.5f);
            m.track(4, "return", "R");
            m.track(5, "group", "G");
            m.track(9, "master", "Master");
            m.set("track.setParent", 1, "parent", 5);
            if (summing) m.set("group.setSumming", 5, "enabled", true);
            m.set("track.setMute", 5, "muted", true);
            sendRow(m, 11, 1, 4);
            m.load();
            m.settle();
            return m.l[511];
        };
        const float off = wetOnly(false), on = wetOnly(true);
        check(off == 0.5f && std::fabs(on - off) < 1e-6f,
              "a summed child's send leaves before its channel half: off " + std::to_string(off) + ", on " +
                  std::to_string(on));
    }
    // A send is not a main route: soloing its destination does not make
    // the sender's own output audible. (The send is fully down, so this
    // holds when a soloed track hears its senders too.)
    {
        Mix m("solo-dst");
        m.track(1, "audio", "A", 0.5f);
        m.track(2, "audio", "B", 0.25f);
        m.track(9, "master", "Master");
        sendRow(m, 11, 1, 2, kSilenceDb);
        m.set("track.setSolo", 2, "soloed", true);
        m.load();
        m.settle();
        check(m.l[511] == 0.25f, "B soloed: B alone, nothing of the sender's own output: " + std::to_string(m.l[511]));
    }
    // Solo in Place opens another track (ADI's addition) only through a send
    // that carries something: switched off, or fully down, it opens nothing.
    {
        auto run = [](const char* name, bool enabled, double db) {
            Mix m(name);
            m.track(1, "audio", "A", 0.5f);
            m.track(2, "audio", "B", 0.25f);
            m.track(9, "master", "Master");
            sendRow(m, 11, 1, 2, db, enabled);
            m.set("track.setSolo", 1, "soloed", true);
            m.load();
            m.settle();
            return m.l[511];
        };
        const float off = run("sip-off", false, 0.0), down = run("sip-down", true, kSilenceDb);
        check(off == 0.5f && down == 0.5f, "A soloed, its send into B off or fully down: A alone (" +
                                               std::to_string(off) + ", " + std::to_string(down) + ")");
    }
    // A send row with id 0 is refused, named, and plays nothing.
    {
        Mix m("id-zero");
        m.track(1, "audio", "A", 0.5f);
        m.track(4, "return", "R");
        m.track(9, "master", "Master");
        sendRow(m, 0, 1, 4, kSilenceDb, false);
        m.load();
        m.settle();
        check(m.l[511] == 0.5f && m.mentions("routing#0"),
              "a send row with id 0, off and fully down, is silent and named: " + std::to_string(m.l[511]));
    }
    // A sender 300 samples late: the edge after its send's node, which plays
    // the strip and its delay, into the return plays the delay, and a
    // rebuild carries that ring.
    for (const Precision width : {Precision::Float32, Precision::Float64}) {
        auto run = [width](const std::string& name, bool rename) {
            Mix m(name.c_str());
            SineNode a(220.0, 0.1f);
            m.track(1, "audio", "A");
            m.track(4, "return", "R");
            m.track(9, "master", "Master");
            m.extra[1] = &a;
            sendRow(m, 11, 1, 4, -6.0);
            sendRow(m, 12, 1, 4, -12.0);
            m.set("mixer.setDelay", 1, "samples", 300);
            m.load(48000.0, width);
            std::vector<float> out;
            for (int k = 0; k < 12; ++k) {
                if (rename && k == 6) {
                    m.set("track.rename", 4, "name", "R2");
                    m.refresh();
                }
                m.render(512);
                out.insert(out.end(), m.l.begin(), m.l.end());
            }
            return out;
        };
        const std::string w = width == Precision::Float64 ? "64" : "32";
        const auto ref = run("late-ref" + w, false), edited = run("late-edit" + w, true);
        std::size_t differ = 0;
        for (std::size_t i = 0; i < ref.size() && i < edited.size(); ++i)
            if (ref[i] != edited[i]) ++differ;
        check(differ == 0, "a late sender's two sends cross a rebuild seamlessly at " + w + " (" +
                               std::to_string(differ) + " samples differ)");
    }
    // A send that leaves the plan and comes back fully down does not replay
    // its old level on the way in.
    {
        Mix m("away-and-back");
        m.track(1, "audio", "A", 0.5f);
        m.track(4, "return", "R");
        m.track(9, "master", "Master");
        sendRow(m, 11, 1, 4);
        m.load();
        m.settle();
        m.store->db().exec("UPDATE routing SET dst_id=99 WHERE id=11");   // a track that is not there
        m.refresh();
        m.settle();
        const float away = m.l[511];
        m.store->db().exec("UPDATE routing SET gain_db=-150 WHERE id=11");
        m.refresh();
        m.store->db().exec("UPDATE routing SET dst_id=4 WHERE id=11");
        m.refresh();
        m.render();
        float worst = 0.0f;
        for (float x : m.l) worst = std::max(worst, x);
        check(away == 0.5f && worst == 0.5f,
              "back in the plan fully down, nothing but the dry: away " + std::to_string(away) + ", loudest " +
                  std::to_string(worst));
    }
    // Turned fully down while its source is muted: when the source comes
    // back, nothing reaches the return. (A Post send reads before the strip,
    // so it is awake through the mute; the gap below is where it sleeps.)
    {
        auto run = [](const char* name, double startDb) {
            Mix m(name);
            m.track(1, "audio", "A", 0.5f);
            m.track(4, "return", "R");
            m.track(9, "master", "Master");
            sendRow(m, 11, 1, 4, startDb);
            m.load();
            m.settle();
            m.set("track.setMute", 1, "muted", true);
            m.refresh();
            m.settle();
            m.settle();
            m.store->db().exec("UPDATE routing SET gain_db=-150 WHERE id=11");
            m.refresh();
            m.settle();
            m.set("track.setMute", 1, "muted", false);
            m.refresh();
            m.render();
            return m.l;
        };
        const auto ref = run("asleep-ref", kSilenceDb), got = run("asleep", 0.0);
        double worst = 0.0;
        for (std::size_t i = 0; i < ref.size(); ++i)
            worst = std::max(worst, std::fabs(static_cast<double>(got[i]) - static_cast<double>(ref[i])));
        check(worst < 1e-6, "a send turned down while asleep stays silent on waking: worst " + std::to_string(worst));
    }
    // The same with the source simply silent, a gap between clips: the send
    // turned down, or the track muted, meanwhile.
    {
        struct Gate final : Node {
            std::atomic<bool> on{true};
            void process(const NodeIo& io) noexcept override {
                const float v = on.load() ? 0.5f : 0.0f;
                for (std::int32_t c = 0; c < io.channels; ++c)
                    for (std::int32_t i = 0; i < io.frames; ++i) io.out[c][io.blockOffset + i] = v;
            }
        };
        auto run = [](const char* name, bool muteInstead) {
            Mix m(name);
            Gate src;
            m.track(1, "audio", "A");
            m.track(4, "return", "R");
            m.track(9, "master", "Master");
            m.extra[1] = &src;
            sendRow(m, 11, 1, 4);
            m.load();
            m.settle();
            src.on = false;
            m.settle();
            m.settle();
            if (muteInstead) m.set("track.setMute", 1, "muted", true);
            else m.store->db().exec("UPDATE routing SET gain_db=-150 WHERE id=11");
            m.refresh();
            m.settle();
            src.on = true;
            m.render();
            float worst = 0.0f;
            for (float x : m.l) worst = std::max(worst, x);
            return worst;
        };
        const float send = run("gap-send", false), mute = run("gap-mute", true);
        check(send == 0.5f, "a send pulled down in a gap: the next sound is the dry alone; loudest " +
                                std::to_string(send));
        check(mute == 0.0f, "a track muted in a gap is silent from its next sound; loudest " + std::to_string(mute));
    }
    // A level past the top plays there, named; the master stays finite.
    {
        Mix m("past-the-top");
        m.track(1, "audio", "A", 0.5f);
        m.track(4, "return", "R");
        m.track(9, "master", "Master");
        sendRow(m, 11, 1, 4, 1000.0);
        m.load();
        m.settle();
        const double expect = 0.5 + 0.5 * faderGain(kSendTopDb);
        check(std::isfinite(m.l[511]) && near(m.l[511], expect, 1e-4) && m.mentions("routing#11: a send level"),
              "a send at +1000 dB plays at the top and is named: " + std::to_string(m.l[511]));
    }
}

void testATrackDelayCrossesARebuild() {
    section("ADR-0092, ADR-0172 -- a track delay crosses a rebuild, wherever in the chain it is played");

    // A delay is a negative latency, played by the edge AFTER the strip. When
    // the strip ends the chain that edge is the plan's, and its history has
    // always been carried. A summed child's strip is followed by its channel
    // half, so the edge is inside the chain -- and every refresh, a fader move
    // included, rebuilds the graph.
    auto run = [](const std::string& name, bool summing, int edits, bool fader, Precision width) {
        Mix m(name.c_str());
        SineNode a(220.0, 0.1f);
        m.track(1, "audio", "A");
        m.track(5, "group", "G");
        m.track(9, "master", "Master");
        m.set("track.setParent", 1, "parent", 5);
        m.extra[1] = &a;
        if (summing) m.set("group.setSumming", 5, "enabled", true);
        m.set("mixer.setDelay", 1, "samples", 300);
        m.load(48000.0, width);
        const std::int64_t built = m.session.graph().stats().published;
        std::vector<float> out;
        for (int k = 0; k < 12; ++k) {
            if (k >= 6 && k < 6 + edits) {
                // Nothing audible: a rename, or the group's fader set to where it is.
                if (fader) m.set("mixer.setVolume", 5, "db", 0.0);
                else m.set("track.rename", 1, "name", "A" + std::to_string(k));
                m.refresh();
            }
            m.render(512);
            out.insert(out.end(), m.l.begin(), m.l.end());
        }
        return std::make_pair(out, m.session.graph().stats().published - built);
    };
    // Not bit for bit: a console half dithers, from its own random seed, so
    // two summed sessions differ at about 1e-8 from their first sample. A
    // hole in a 0.1 sine is up to 0.1.
    auto sameAs = [](const std::vector<float>& x, const std::vector<float>& ref) {
        std::size_t differ = 0;
        for (std::size_t i = 0; i < x.size() && i < ref.size(); ++i)
            if (std::fabs(static_cast<double>(x[i]) - static_cast<double>(ref[i])) > 1e-6) ++differ;
        return differ;
    };
    for (const Precision width : {Precision::Float32, Precision::Float64})
    for (const bool summing : {false, true}) {
        const std::string tag = std::string(summing ? "summed" : "plain") +
                                (width == Precision::Float64 ? " at 64" : "");
        const std::string file = (summing ? "s" : "p") + std::string(width == Precision::Float64 ? "64" : "32");
        const auto ref = run("delay-ref-" + file, summing, 0, false, width);
        const auto once = run("delay-once-" + file, summing, 1, false, width);
        const auto three = run("delay-three-" + file, summing, 3, true, width);
        check(once.second == 1 && three.second == 3, tag + ": the edits rebuilt the graph, once and three times");
        const std::size_t d1 = sameAs(once.first, ref.first), d3 = sameAs(three.first, ref.first);
        check(d1 == 0, tag + ": a child 300 samples late, and a rebuild moves no sample of the master past 1e-6 (" +
                           std::to_string(d1) + " do)");
        check(d3 == 0, tag + ": nor do three fader moves in a row (" + std::to_string(d3) + " do)");
    }

    // A NEW FLAVOUR is a new channel half after the same strip. The delayed
    // edge leaves the strip, and the ring holds what the strip sent, so it is
    // carried: the sound changes, and nothing drops out.
    {
        Mix m("delay-flavour");
        SineNode a(220.0, 0.1f);
        m.track(1, "audio", "A");
        m.track(5, "group", "G");
        m.track(9, "master", "Master");
        m.set("track.setParent", 1, "parent", 5);
        m.extra[1] = &a;
        m.set("group.setSumming", 5, "enabled", true);
        m.set("mixer.setDelay", 1, "samples", 300);
        m.load();
        for (int k = 0; k < 6; ++k) m.render(512);
        const ConsoleNode* before = m.session.summing().channelOf(1);
        m.set("group.setSummingFlavor", 5, "flavor", "console.md");
        m.refresh();
        m.render(512);
        std::size_t run = 0, longest = 0;
        for (float x : m.l) {
            run = std::fabs(x) < 1e-5f ? run + 1 : 0;
            longest = std::max(longest, run);
        }
        check(m.session.summing().channelOf(1) != before, "a new flavour is a new half");
        check(longest < 8, "and the block after it has no hole: its longest near-silent run is " +
                               std::to_string(longest) + " samples, not the delay's 300");
    }

    // SUMMING TURNED ON OR OFF changes what the child's output into its group
    // IS: with drive, a channel half's output is hotter than the track by the
    // drive, and the group's buss takes it back. The old line's history is
    // not handed to the new one (`wireKind`), so the swap never plays one as
    // the other -- with +18 dB of drive that was a +12 dB burst.
    for (const bool startOn : {true, false}) {
        Mix m(startOn ? "toggle-off" : "toggle-on");
        SineNode a(220.0, 0.1f);
        LateSilence lag;
        m.track(1, "audio", "A");
        m.track(2, "audio", "B");
        m.track(5, "group", "G");
        m.track(9, "master", "Master");
        m.set("track.setParent", 1, "parent", 5);
        m.set("track.setParent", 2, "parent", 5);
        m.extra[1] = &a;
        m.extra[2] = &lag;
        m.set("group.setSumming", 5, "enabled", startOn);
        m.set("group.setSummingDrive", 5, "db", 18.0);
        m.set("mixer.setDelay", 1, "samples", 300);
        m.load();
        for (int k = 0; k < 8; ++k) m.render(512);
        m.set("group.setSumming", 5, "enabled", !startOn);
        m.refresh();
        double swap = 0.0, settled = 0.0;
        for (int k = 0; k < 10; ++k) {
            m.render(512);
            for (std::size_t i = 0; i < m.l.size(); ++i) {
                const double v = std::fabs(static_cast<double>(m.l[i]));
                if (k * 512 + static_cast<int>(i) < 1300) swap = std::max(swap, v);
                else if (k >= 4) settled = std::max(settled, v);
            }
        }
        check(swap <= settled * 1.05,
              std::string("summing ") + (startOn ? "on to off" : "off to on") + ": no burst at the swap -- peak " +
                  std::to_string(swap) + " against " + std::to_string(settled) + " settled");
    }
}

void testGroupSumming() {
    section("ADR-0174 -- native analog group summing: a console's channel half on each child, its buss on the sum");

    // --- the ops -------------------------------------------------------------
    {
        Mix m("sum-ops");
        m.track(1, "audio", "A");
        m.track(5, "group", "G");
        m.track(9, "master", "Master");
        auto tryOp = [&](const char* type, std::int64_t id, const char* key, Payload v) {
            OpRequest q;
            q.opType = type;
            q.payload = {{"id", id}, {key, std::move(v)}};
            OpJournal j(*m.store);
            return j.commit(q).ok;
        };
        check(!tryOp("group.setSumming", 1, "enabled", true), "an audio track has no summing: refused");
        check(tryOp("group.setSumming", 5, "enabled", true), "a group does");
        check(!tryOp("group.setSummingFlavor", 5, "flavor", "console12"), "an unknown flavour is refused");
        check(!tryOp("group.setSummingFlavor", 5, "flavor", "every.c7"),
              "EveryConsole's are not flavours: it cannot choose a system at the pinned commit");
        check(tryOp("group.setSummingFlavor", 5, "flavor", "console.md"), "Console MD is a flavour");
        check(!tryOp("group.setSummingDrive", 5, "db", 30.0), "a drive past +24 dB is refused");
        check(tryOp("group.setSummingDrive", 5, "db", 6.0), "+6 dB is not");
        {
            SQLite::Statement row(m.store->db(),
                                  "SELECT enabled, flavor, drive_db FROM group_summing WHERE track_id = 5");
            check(row.executeStep() && row.getColumn(0).getInt() == 1 &&
                      row.getColumn(1).getString() == "console.md" && row.getColumn(2).getDouble() == 6.0,
                  "one row: on, console.md, +6 dB");
        }
        History h(*m.store);
        const bool undone = h.undo().ok && h.undo().ok && h.undo().ok;
        check(undone, "three undos");
        SQLite::Statement back(m.store->db(), "SELECT enabled, flavor, drive_db FROM group_summing WHERE track_id = 5");
        check(back.executeStep() && back.getColumn(0).getInt() == 0 && back.getColumn(1).getString() == "console9" &&
                  back.getColumn(2).getDouble() == 0.0,
              "undone to the defaults: off, console9, 0 dB");
    }

    // --- what it plays --------------------------------------------------------
    // Two children at -20 dBFS (220 and 330 Hz) into group 5, into the master.
    struct Played {
        std::vector<float> out;
        bool placed = false;
        std::string what;
    };
    auto play = [](const std::string& name, const char* flavor, double drive, double hzA = 220.0,
                   double hzB = 330.0) {
        Mix m(name.c_str());
        SineNode a(hzA, 0.1f), b(hzB, 0.1f);
        m.track(1, "audio", "A");
        m.track(2, "audio", "B");
        m.track(5, "group", "G");
        m.track(9, "master", "Master");
        m.set("track.setParent", 1, "parent", 5);
        m.set("track.setParent", 2, "parent", 5);
        m.extra[1] = &a;
        m.extra[2] = &b;
        if (flavor != nullptr) {
            m.set("group.setSumming", 5, "enabled", true);
            m.set("group.setSummingFlavor", 5, "flavor", flavor);
            if (drive != 0.0) m.set("group.setSummingDrive", 5, "db", drive);
        }
        m.load();
        Played p;
        for (int k = 0; k < 24; ++k) {
            m.render(512);
            p.out.insert(p.out.end(), m.l.begin(), m.l.end());
        }
        const GroupSumming& s = m.session.summing();
        p.placed = s.bussOf(5) != nullptr && s.channelOf(1) != nullptr && s.channelOf(2) != nullptr &&
                   s.channelOf(5) == nullptr && s.channelOf(9) == nullptr && s.bussOf(9) == nullptr;
        if (s.bussOf(5) != nullptr && s.channelOf(1) != nullptr)
            p.what = s.channelOf(1)->what() + " / " + s.bussOf(5)->what();
        return p;
    };
    auto rmsDb = [](const std::vector<float>& v) {
        double e = 0.0;
        for (std::size_t i = v.size() / 2; i < v.size(); ++i) e += static_cast<double>(v[i]) * v[i];
        return 10.0 * std::log10(e / static_cast<double>(v.size() / 2) + 1e-30);
    };
    auto deviation = [](const std::vector<float>& x, const std::vector<float>& ref) {
        double d = 0.0, r = 0.0;
        for (std::size_t i = x.size() / 2; i < x.size(); ++i) {
            const double e = static_cast<double>(x[i]) - static_cast<double>(ref[i]);
            d += e * e;
            r += static_cast<double>(ref[i]) * ref[i];
        }
        return std::sqrt(d / (r + 1e-30));
    };

    const Played plain = play("sum-plain", nullptr, 0.0);
    const double plainDb = rmsDb(plain.out);
    // The level is matched at 1 kHz, where it is measured (summing.cpp); at
    // other frequencies a console's own EQ is part of its colour -- Console MC
    // is "the bright take on MCI", and sits lower at 220 Hz by design.
    const Played plainK = play("sum-plain-1k", nullptr, 0.0, 1000.0, 1000.0);
    const double plainKDb = rmsDb(plainK.out);
    int placed = 0, level = 0, processed = 0, finite = 0;
    std::string report;
    for (const SummingFlavor& f : summingFlavors()) {
        const std::string key(f.key);
        const Played p = play("sum-" + key, key.c_str(), 0.0);
        const Played k = play("sum-1k-" + key, key.c_str(), 0.0, 1000.0, 1000.0);
        const double delta = rmsDb(k.out) - plainKDb;
        const double tone = rmsDb(p.out) - plainDb;
        bool fin = true;
        for (float x : p.out) fin = fin && std::isfinite(x);
        placed += p.placed ? 1 : 0;
        level += std::fabs(delta) < 0.5 ? 1 : 0;
        processed += p.out != plain.out ? 1 : 0;
        finite += fin ? 1 : 0;
        char line[200];
        std::snprintf(line, sizeof(line), "        %-11s %+5.2f dB at 1 kHz, %+5.2f dB at 220/330 Hz  deviation %.4f  (%s)\n",
                      key.c_str(), delta, tone, deviation(p.out, plain.out), p.what.c_str());
        report += line;
    }
    std::printf("%s", report.c_str());
    const int n = static_cast<int>(summingFlavors().size());
    check(n == 8, "eight flavours, the channel/buss pairs");
    check(placed == n, "every flavour: a buss half on the group, a channel half on each child, none elsewhere");
    check(finite == n, "every flavour plays finite audio");
    check(processed == n, "every flavour is heard: none is the plain sum bit for bit");
    check(level == n, "every flavour is at unity at 1 kHz, -20 dBFS, its own gain staging made up: within 0.5 dB");

    // Drive: the curves hit harder, the level held.
    const Played d0 = play("sum-d0", "console9", 0.0);
    const Played d18 = play("sum-d18", "console9", 18.0);
    const double dev0 = deviation(d0.out, plain.out);
    const double dev18 = deviation(d18.out, plain.out);
    const double lvl18 = rmsDb(d18.out) - plainDb;
    std::printf("        console9 drive: 0 dB deviation %.4f, +18 dB deviation %.4f at %+.2f dB\n", dev0, dev18,
                lvl18);
    check(dev18 > 2.0 * dev0, "+18 dB of drive colours more than 0 dB does");
    check(std::fabs(lvl18) < 3.0, "and the level stays within 3 dB: the drive comes back off after the buss");

    // --- kept, replaced, named ------------------------------------------------
    {
        Mix m("sum-keep");
        SineNode a(220.0, 0.1f);
        m.track(1, "audio", "A");
        m.track(5, "group", "G");
        m.track(9, "master", "Master");
        m.set("track.setParent", 1, "parent", 5);
        m.extra[1] = &a;
        m.set("group.setSumming", 5, "enabled", true);
        m.load();
        const ConsoleNode* first = m.session.summing().channelOf(1);
        m.set("track.rename", 1, "name", "A2");
        m.refresh();
        check(first != nullptr && m.session.summing().channelOf(1) == first,
              "an unrelated edit keeps the same halves, and their state");
        m.set("group.setSummingFlavor", 5, "flavor", "console.la");
        m.refresh();
        const ConsoleNode* la = m.session.summing().channelOf(1);
        check(la != nullptr && la != first && la->what() == "ConsoleLAChannel", "a new flavour is a new half");
        m.render();
        m.set("group.setSumming", 5, "enabled", false);
        m.refresh();
        check(m.session.summing().channelOf(1) == nullptr && m.session.summing().bussOf(5) == nullptr,
              "off: no halves at all");
        m.render();
        m.store->db().exec("INSERT INTO group_summing(track_id, enabled) VALUES (1, 1)");
        m.store->db().exec("UPDATE group_summing SET enabled = 1, flavor = 'nope' WHERE track_id = 5");
        m.refresh();
        check(m.mentions("tracks#1: summing is on a track that is not a group"),
              "a row on a track that is not a group is named, not played");
        check(m.mentions("tracks#5: summing flavour 'nope' is unknown"), "an unknown flavour is named, not played");
    }
}

void testScopeTaps() {
    section("ADR-0175 -- the scope's taps: a wait-free ring, stamped with when each frame is heard");

    // --- the ring --------------------------------------------------------------
    {
        ScopeTap tap(48000.0, 0.01);   // 480 frames
        check(tap.capacity() == 480, "0.01 s at 48 kHz is 480 frames");
        std::vector<float> l(100), r(100);
        for (int b = 0; b < 3; ++b) {
            for (int i = 0; i < 100; ++i) {
                l[static_cast<std::size_t>(i)] = static_cast<float>(b * 100 + i);
                r[static_cast<std::size_t>(i)] = -l[static_cast<std::size_t>(i)];
            }
            tap.write(l.data(), r.data(), 100, 1000 + b * 100, true);
        }
        std::vector<float> ol(250), orr(250);
        std::int64_t first = 0;
        check(tap.read(ol, orr, first), "250 of the 300 written read back");
        check(ol[0] == 50.0f && ol[249] == 299.0f && orr[249] == -299.0f && first == 1050,
              "the newest 250, both channels, and the stamp of the first: 1050");
        std::vector<float> big(481), big2(481);
        check(!tap.read(big, big2, first), "more than the ring holds is refused");

        for (int b = 3; b < 10; ++b) {
            for (int i = 0; i < 100; ++i) l[static_cast<std::size_t>(i)] = static_cast<float>(b * 100 + i);
            tap.write(l.data(), nullptr, 100, 1000 + b * 100, true);
        }
        std::vector<float> all(480), all2(480);
        check(tap.read(all, all2, first) && all[0] == 520.0f && all[479] == 999.0f && first == 1520,
              "lapped: the newest 480 of 1000, oldest overwritten");
        check(all2[479] == 999.0f, "a mono strip's one channel is both sides");

        tap.write(l.data(), nullptr, 100, 5000, false);
        std::vector<float> p1(100), p2(100);
        check(tap.read(p1, p2, first) && first == 5000, "parked, the stamp does not move");
    }

    // --- one writer, one reader, full speed --------------------------------------
    {
        ScopeTap tap(48000.0, 0.02);
        std::atomic<bool> stop{false};
        std::atomic<long> reads{0}, torn{0};
        std::thread reader([&] {
            std::vector<float> a(256), b(256);
            std::int64_t first = 0;
            while (!stop.load()) {
                if (!tap.read(a, b, first)) continue;
                ++reads;
                // The writer stamps the frame index and writes it, wrapped at
                // 2^16, as the sample: a whole window is its stamp counting up,
                // frame by frame. The wrap is short so every run crosses it
                // many times; comparing neighbouring samples instead failed
                // once per wrap, on runners fast enough to reach the old one.
                bool ok = true;
                for (std::size_t i = 0; i < a.size(); ++i)
                    ok = ok && a[i] == static_cast<float>((first + static_cast<std::int64_t>(i)) % 65536);
                if (!ok) ++torn;
            }
        });
        std::vector<float> block(64);
        std::int64_t frame = 0;
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
        while (std::chrono::steady_clock::now() < until) {
            for (std::size_t i = 0; i < block.size(); ++i) block[i] = static_cast<float>((frame + static_cast<std::int64_t>(i)) % 65536);
            tap.write(block.data(), block.data(), 64, frame, true);
            frame += 64;
        }
        stop.store(true);
        reader.join();
        std::printf("        %ld consistent reads while %lld frames were written; %ld torn\n", reads.load(),
                    static_cast<long long>(frame), torn.load());
        check(reads.load() > 0 && torn.load() == 0,
              "every read the ring accepts is whole: never a window the writer lapped mid-copy");
    }

    // --- the compare ---------------------------------------------------------------
    {
        std::vector<float> a(4800), b(4800), inv(4800), late(4800);
        unsigned seed = 42u;
        for (std::size_t i = 0; i < a.size(); ++i) {
            seed = seed * 1664525u + 1013904223u;
            a[i] = static_cast<float>(static_cast<double>(seed >> 8) / 16777216.0 - 0.5);
            b[i] = a[i];
            inv[i] = -a[i];
        }
        for (std::size_t i = 0; i < late.size(); ++i) late[i] = i >= 37 ? a[i - 37] : 0.0f;
        check(near(correlation(a, b), 1.0, 1e-12), "a signal with itself: correlation +1");
        check(near(correlation(a, inv), -1.0, 1e-12), "with its polarity flipped: -1");
        const Offset o = bestOffset(a, late, 960);
        check(o.samples == 37 && o.correlation > 0.99, "a copy 37 samples late is found 37 samples late");
        const Offset f = bestOffset(a, inv, 960);
        check(f.samples != 0 || f.correlation < 0.0, "a flipped copy is not a match at zero lag");
    }

    // --- in the session: two tracks compared as heard -------------------------------
    {
        Mix m("scope-heard");
        StepNode a(0.5f, 1000), b(0.25f, 1000);
        m.track(1, "audio", "A");
        m.track(2, "audio", "B");
        m.track(9, "master", "Master");
        m.extra[1] = &a;
        m.extra[2] = &b;
        m.set("mixer.setDelay", 1, "samples", 100);
        m.load();
        const auto ta = m.session.openScope(1, 1.0);
        const auto tb = m.session.openScope(2, 1.0);
        check(ta != nullptr && tb != nullptr && m.session.openScope(1) == ta, "a tap per track, the same on a second call");
        check(m.session.openScope(77) == nullptr, "no tap for a track the graph does not have");
        check(ta->latency() == -100 && tb->latency() == 0,
              "A's tap knows it is heard 100 samples late (the delay as -100 latency); B's, on time");
        m.session.transport().play();
        for (int k = 0; k < 8; ++k) m.render(512);
        auto stepStamp = [](const ScopeTap& t) {
            // Everything written so far: a session's first block after a
            // publish may go to the graph before the tap is attached.
            const auto n = static_cast<std::size_t>(std::min<std::int64_t>(t.written(), t.capacity()));
            std::vector<float> x(n), y(n);
            std::int64_t first = 0;
            if (!t.read(x, y, first)) return std::int64_t{-1};
            for (std::size_t i = 0; i < x.size(); ++i)
                if (x[i] > 0.1f) return first + static_cast<std::int64_t>(i);
            return std::int64_t{-1};
        };
        const std::int64_t sa = stepStamp(*ta), sb = stepStamp(*tb);
        std::printf("        A's step is heard at %lld, B's at %lld\n", static_cast<long long>(sa),
                    static_cast<long long>(sb));
        check(sb == 1000 && sa == 1100,
              "B's step is heard at 1000 and A's, delayed, at 1100: matching stamps is matching what is heard");
        m.session.closeScope(1);
        const std::int64_t before = ta->written();
        m.render(512);
        check(ta->written() == before && tb->written() > before - 1, "a closed tap is written no more; the open one is");
    }
}

void testSolo() {
    section("ADR-0163 d4 -- solo: what feeds a soloed track and what it feeds stay audible");
    Mix m("solo");
    m.track(1, "audio", "A", 0.5f);
    m.track(2, "audio", "B", 0.25f);
    m.track(3, "group", "G");
    m.track(4, "audio", "C1", 0.0625f);
    m.track(5, "audio", "C2", 0.125f);
    m.track(9, "master", "Master");
    m.store->db().exec("UPDATE tracks SET parent_id = 3 WHERE id IN (4, 5)");
    m.load();
    m.render();
    check(m.l[511] == 0.9375f, "everything at unity: 0.5 + 0.25 + 0.0625 + 0.125");

    m.set("track.setSolo", 2, "soloed", true);
    m.refresh();
    m.settle();
    check(m.l[511] == 0.25f, "B soloed: only B is heard");

    m.set("track.setSoloDefeat", 1, "defeat", true);
    m.refresh();
    m.settle();
    check(m.l[511] == 0.75f, "A is solo-defeated: it stays in");
    m.set("track.setSoloDefeat", 1, "defeat", false);
    m.set("track.setSolo", 2, "soloed", false);

    m.set("track.setSolo", 4, "soloed", true);
    m.refresh();
    m.settle();
    check(m.l[511] == 0.0625f,
          "C1 soloed: its group stays open for it, its sibling C2 and the others are silent");

    m.set("track.setSolo", 4, "soloed", false);
    m.set("track.setSolo", 3, "soloed", true);
    m.refresh();
    m.settle();
    check(m.l[511] == 0.1875f, "the group soloed: both its children are heard, nothing else");

    m.set("track.setSolo", 3, "soloed", false);
    m.set("track.setMute", 3, "muted", true);
    m.refresh();
    m.settle();
    check(m.l[511] == 0.75f, "the group muted: its children go with it");
}

void testStripsOutliveGraphs() {
    section("ADR-0163 d1 -- a strip survives the graph every edit publishes");
    Mix m("outlive");
    m.track(1, "audio", "A", 0.5f);
    m.track(9, "master", "Master");
    m.load();
    m.render();
    const auto before = m.session.stats().rebuilds;
    m.set("mixer.setVolume", 1, "db", -20.0);
    m.refresh();
    check(m.session.stats().rebuilds == before + 1, "a fader move publishes a new graph");
    m.render(64);
    check(m.l[0] > 0.45f, "and the new graph's strip starts where the old one was, not at the target");
}

void testAutomationPlays() {
    section("ADR-0164 -- a strip plays its own lanes: volume, pan and mute");
    {
        Mix m("volume");
        m.track(1, "audio", "A", 0.5f);
        m.track(9, "master", "Master");
        m.lane(1, 1, "volume", {{0.0, 0.0}, {1.0, -20.0}});
        m.load();
        m.session.transport().play();
        m.play(1.5);
        check(near(m.at(0.0), 0.5, 1e-4), "a volume lane starts at its first point: 0 dB");
        check(near(m.at(0.5), 0.5 * std::pow(10.0, -10.0 / 20.0), 0.004),
              "and at 0.5 s reads -10 dB, half way down a linear ramp to -20 dB");
        check(near(m.at(1.25), 0.05, 5e-4), "after the last point it holds the last value (ADR-0159)");
        bool falling = true;
        for (std::size_t i = 1; i < 48000; ++i)
            if (m.hist[i] > m.hist[i - 1] + 1e-6f) falling = false;
        check(falling, "and the sweep only ever falls: no steps back up between control points");

        m.session.transport().play(false);
        m.session.transport().locate(36000);
        m.render();
        m.render();
        check(near(m.l[511], 0.5 * std::pow(10.0, -15.0 / 20.0), 0.003),
              "parked at 0.75 s, the strip reads the lane at the playhead: -15 dB");
        const auto [lo, hi] = std::minmax_element(m.l.begin(), m.l.end());
        check(*lo == *hi,
              "and holds one level across the block: a parked playhead is one position, not a block's worth");
    }
    {
        Mix m("pan-mute");
        m.track(1, "audio", "A", 0.5f);
        m.track(9, "master", "Master");
        m.lane(1, 1, "pan", {{0.0, 0.0, 0}, {0.5, 1.0, 0}});
        m.load();
        m.session.transport().play();
        m.play(1.0);
        check(m.at(0.25) == 0.5f, "a held pan lane keeps the centre, exactly, before its step");
        check(m.at(0.75) == 0.0f, "and after the step to hard right the left channel is silent");
    }
    {
        Mix m("mute");
        m.track(1, "audio", "A", 0.5f);
        m.track(9, "master", "Master");
        // The step falls 100 samples into a 512-sample block, off every grid.
        m.lane(1, 1, "mute", {{0.0, 0.0, 0}, {24100.0 / 48000.0, 1.0, 0}});
        m.load();
        m.session.transport().play();
        m.play(1.0);
        check(m.at(0.25) == 0.5f && m.at(0.75) == 0.0f, "a mute lane silences the track at its step");
        check(m.hist[24110] > 0.0f, "through the ramp, not a click");
        check(m.hist[24400] == 0.0f,
              "and within one control interval and one ramp of the step: read every 32 samples, "
              "not at the next block");
    }
    {
        Mix m("refused");
        m.track(1, "audio", "A", 0.5f);
        m.track(9, "master", "Master");
        m.lane(1, 1, "volume", {{0.0, 0.0}, {1.0, -20.0}}, "normalized");
        m.lane(2, 1, "width", {{0.0, 1.0}});
        m.load();
        check(m.mentions("automation_lanes#1: a volume lane must be in real units"),
              "a normalized volume lane is named and not played: no fader curve is decided");
        check(m.mentions("automation_lanes#2: a track has no parameter 'width'"),
              "a lane for a parameter the strip does not have is named");
        m.session.transport().play();
        m.play(0.5);
        check(m.at(0.4) == 0.5f, "and the strip keeps the model's value");
    }
}

void testOverride() {
    section("ADR-0162 -- touching an automated control overrides its lane until re-enabled");
    Mix m("override");
    m.track(1, "audio", "A", 0.5f);
    m.track(2, "audio", "B", 0.25f);
    m.track(9, "master", "Master");
    m.lane(7, 1, "volume", {{0.0, 0.0}, {1.0, -20.0}});
    m.load();
    m.session.transport().play();
    m.play(0.25);
    check(!m.session.automationOverridden(), "nothing is overridden before anyone touches anything");

    m.set("mixer.setVolume", 2, "db", -6.0);
    m.refresh();
    check(!m.session.automationOverridden(), "moving a fader with no automation overrides nothing");

    m.set("mixer.setVolume", 1, "db", -6.0206);
    m.refresh();
    check(m.session.automationOverridden() && m.session.overriddenLanes().count(7) == 1,
          "moving A's automated fader overrides its lane: the Re-Enable button lights");
    m.hist.clear();
    m.play(1.0);
    const double b = 0.25 * std::pow(10.0, -6.0 / 20.0);
    check(near(m.at(0.9), 0.25 + b, 1e-3), "and the value just set stands while the lane would keep falling");

    check(!m.session.reenableAutomation(99), "re-enabling a lane that was not overridden is refused");
    check(m.session.reenableAutomation(7) && !m.session.automationOverridden(), "re-enabling that lane clears it");
    m.hist.clear();
    m.play(0.25);
    check(near(m.at(0.2), 0.05 + b, 1e-3), "and the strip follows the lane again at once, from the playhead");

    m.set("mixer.setVolume", 1, "db", -3.0);
    m.refresh();
    check(m.session.automationOverridden(), "overridden again");
    check(m.session.reenableAutomation() && !m.session.automationOverridden(),
          "and Re-Enable Automation clears every lane");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("adi_mixer_tests -- the mixer strip (ADR-0163)\n\n");
    try {
        testPanLaws();
        testASendNode();
        testVolumePanMute();
        testSolo();
        testStripsOutliveGraphs();
        testAutomationPlays();
        testOverride();
        testTrackDelay();
        testATrackDelayCrossesARebuild();
        testHearing();
        testSoloAcrossSends();
        testSendsPlay();
        testSendsHoldTheirLevel();
        testPreSendsAndTheSendsPan();
        testGroupSumming();
        testScopeTaps();
    } catch (const std::exception& e) {
        check(false, std::string("exception: ") + e.what());
    }
    std::printf("\n%s -- %d checks, %d failure(s)\n", g_failures ? "FAILED" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
