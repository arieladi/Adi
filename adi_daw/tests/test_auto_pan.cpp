// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live 12.4's Auto Pan-Tremolo, the core (DEV-22). Each property is checked
// against a number derived apart from the code: the gain at the LFO's crest
// and trough from Amount, the period from Rate or the tempo, the channels'
// separation from Phase. Parity with Live waits on a Live 12.4 reading.
#include "adi/dsp/auto_pan.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <string>
#include <vector>

namespace {
bool counting = false;
std::size_t allocations = 0;
int checks = 0, failures = 0;
void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}
}  // namespace
void* operator new(std::size_t n) {
    if (counting) {
        ++allocations;
    }
    if (void* p = std::malloc(n ? n : 1)) {
        return p;
    }
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {
using P = adi::dsp::AutoPan;

/// Unity in on both channels: the output is each channel's gain.
std::pair<std::vector<float>, std::vector<float>> gains(P& p, std::size_t frames, std::size_t block = 97) {
    std::vector<float> in(frames, 1.f), l(frames), r(frames);
    for (std::size_t at = 0; at < frames; at += block) {
        const auto n = std::min(block, frames - at);
        p.process(in.data() + at, in.data() + at, l.data() + at, r.data() + at, n);
    }
    return {l, r};
}

void motion() {
    // Tremolo at 1 Hz, Amount 100 %: unity at the crest (a quarter cycle,
    // 12,000 samples), silence at the trough (36,000).
    P t;
    t.set(P::Mode, P::Tremolo);
    t.prepare(48000);
    auto [tl, tr] = gains(t, 48000);
    check(std::abs(tl[12000] - 1) < 1e-6 && std::abs(tl[36000]) < 1e-6 && tl == tr,
          "Tremolo at 100 %: unity at the crest, silence at the trough, both channels together");
    // Amount 40 %: the trough is 0.6, the midpoint 0.8.
    P a;
    a.set(P::Mode, P::Tremolo);
    a.set(P::Amount, 40);
    a.prepare(48000);
    auto [al, ar] = gains(a, 48000);
    (void)ar;
    check(std::abs(al[36000] - 0.6) < 1e-6 && std::abs(al[0] - 0.8) < 1e-6, "Amount 40 %: the trough at 0.6, the middle 0.8");
    // Pan, Phase 180 degrees: one channel at its crest while the other is at its trough.
    P pan;
    pan.prepare(48000);
    auto [pl, pr] = gains(pan, 48000);
    check(std::abs(pl[12000] - 1) < 1e-6 && std::abs(pr[12000]) < 1e-6 && std::abs(pl[36000]) < 1e-6 &&
              std::abs(pr[36000] - 1) < 1e-6,
          "Pan at Phase 180: one side loud while the other is quiet");
    // Phase 90: the right a quarter cycle ahead (its crest at sample 0).
    P quarter;
    quarter.set(P::Phase, 90);
    quarter.prepare(48000);
    auto [ql, qr] = gains(quarter, 48000);
    check(std::abs(qr[0] - 1) < 1e-6 && std::abs(ql[0] - 0.5) < 1e-6, "Phase 90: the right a quarter cycle ahead");
    // Invert: the crest becomes the trough.
    P inverted;
    inverted.set(P::Mode, P::Tremolo);
    inverted.set(P::Invert, 1);
    inverted.prepare(48000);
    check(std::abs(gains(inverted, 48000).first[12000]) < 1e-6, "Invert: the crest becomes the trough");
    // Shape 100 %: a sine at a sixth of its cycle (0.866) is clipped to unity.
    P shaped;
    shaped.set(P::Mode, P::Tremolo);
    shaped.set(P::Shape, 100);
    shaped.prepare(48000);
    check(std::abs(gains(shaped, 48000).first[8000] - 1) < 1e-6, "Shape 100 %: the wave hardens toward a square");
    // A plant: Amount 40 % is not a trough of 0.
    check(std::abs(al[36000]) > 0.5, "plant: a 40 % tremolo does not reach silence");
}

void rates() {
    // 4 Hz free: a cycle of 12,000 samples, the crest at 3,000 and 15,000.
    P f;
    f.set(P::Mode, P::Tremolo);
    f.set(P::Rate, 4);
    f.prepare(48000);
    auto [fl, fr] = gains(f, 24000);
    (void)fr;
    check(std::abs(fl[3000] - 1) < 1e-6 && std::abs(fl[15000] - 1) < 1e-6, "4 Hz: a cycle of 12,000 samples");
    // Synced 1/4 at 120 BPM: 2 Hz, a cycle of 24,000 samples.
    P s;
    s.set(P::Mode, P::Tremolo);
    s.set(P::Sync, 1);
    s.set(P::Division, 4);
    s.tempo(120);
    s.prepare(48000);
    auto [sl, sr] = gains(s, 48000);
    (void)sr;
    check(std::abs(sl[6000] - 1) < 1e-6 && std::abs(sl[30000] - 1) < 1e-6 && std::abs(sl[18000]) < 1e-6,
          "synced 1/4 at 120 BPM: a cycle of 24,000 samples");
}

void blocksAndRobustness() {
    bool identical = true;
    for (int wave = 0; wave < 4; ++wave) {
        P base;
        base.set(P::Wave, wave);
        base.set(P::Rate, 3.7);
        base.set(P::Shape, 30);
        base.prepare(48000);
        const auto one = gains(base, 20000, 1);
        for (std::size_t block : {32u, 64u, 100u, 4096u}) {
            P other;
            other.set(P::Wave, wave);
            other.set(P::Rate, 3.7);
            other.set(P::Shape, 30);
            other.prepare(48000);
            identical = identical && gains(other, 20000, block) == one;
        }
    }
    check(identical, "every wave gives the same samples at every block size");
    // Aliased planes, non-finite input, no allocation.
    P z;
    z.set(P::Amount, 0);
    z.prepare(48000);
    std::vector<float> left(64, 0.25f), shared(64, -0.5f), rightOut(64);
    z.process(left.data(), shared.data(), shared.data(), rightOut.data(), 64);
    check(std::all_of(rightOut.begin(), rightOut.end(), [](float v) { return v == -0.5f; }),
          "an output plane that is the other channel's input is read before it is written");
    std::vector<float> bad(512, std::numeric_limits<float>::quiet_NaN()), o(512), q(512);
    counting = true;
    z.process(bad.data(), bad.data(), o.data(), q.data(), bad.size());
    counting = false;
    check(allocations == 0 && std::all_of(o.begin(), o.end(), [](float v) { return v == 0.f; }),
          "a non-finite input is silence, and nothing allocates");
}

}  // namespace

int main() {
    motion();
    rates();
    blocksAndRobustness();
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAILED" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
