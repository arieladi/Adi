// SPDX-License-Identifier: GPL-3.0-or-later
//
// ADR-0163: the mixer strip in the graph -- volume, pan and mute on every
// track, solo across the project, a ramp for every change, and strips that
// outlive the graphs every edit publishes.

#include "temp_directory.hpp"

#include "adi/engine/mixer.hpp"
#include "adi/engine/session.hpp"
#include "adi/ops.hpp"
#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <map>
#include <numbers>
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
    void load() {
        session.setSourcesFor([this](std::int64_t id) {
            const auto it = tones.find(id);
            return it == tones.end() ? std::vector<Node*>{} : std::vector<Node*>{it->second.get()};
        });
        SessionSpec spec;
        spec.sampleRate = 48000.0;
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

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("adi_mixer_tests -- the mixer strip (ADR-0163)\n\n");
    try {
        testPanLaws();
        testVolumePanMute();
        testSolo();
        testStripsOutliveGraphs();
    } catch (const std::exception& e) {
        check(false, std::string("exception: ") + e.what());
    }
    std::printf("\n%s -- %d checks, %d failure(s)\n", g_failures ? "FAILED" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
