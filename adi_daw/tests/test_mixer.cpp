// SPDX-License-Identifier: GPL-3.0-or-later
//
// ADR-0163: the mixer strip in the graph -- volume, pan and mute on every
// track, solo across the project, a ramp for every change, and strips that
// outlive the graphs every edit publishes.

#include "temp_directory.hpp"

#include "adi/blob.hpp"
#include "adi/engine/mixer.hpp"
#include "adi/engine/session.hpp"
#include "adi/ops.hpp"
#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <algorithm>
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
    void load(double sampleRate = 48000.0) {
        session.setSourcesFor([this](std::int64_t id) {
            if (const auto e = extra.find(id); e != extra.end()) return std::vector<Node*>{e->second};
            const auto it = tones.find(id);
            return it == tones.end() ? std::vector<Node*>{} : std::vector<Node*>{it->second.get()};
        });
        SessionSpec spec;
        spec.sampleRate = sampleRate;
        spec.maxFrames = 512;
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
        testVolumePanMute();
        testSolo();
        testStripsOutliveGraphs();
        testAutomationPlays();
        testOverride();
        testTrackDelay();
    } catch (const std::exception& e) {
        check(false, std::string("exception: ") + e.what());
    }
    std::printf("\n%s -- %d checks, %d failure(s)\n", g_failures ? "FAILED" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
