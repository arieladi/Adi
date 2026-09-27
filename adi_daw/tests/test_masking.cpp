// SPDX-License-Identifier: GPL-3.0-or-later
//
// The masking measure -- ADR-0195 d5, AI-AGENT §5.
//
// It is a pure function, so every property below is a statement about the
// measure itself rather than about a view or an agent. That is the point of
// testing it here: when the overlay highlights a band and the agent says
// "the bass masks the kick", these are the numbers both of them used.

#include "adi/dsp/masking.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace adi::dsp;

int g_failures = 0;
int g_checks = 0;

void check(bool cond, const std::string& what) {
    ++g_checks;
    if (!cond) { ++g_failures; std::printf("  FAIL  %s\n", what.c_str()); }
}
void near(double got, double want, double tol, const std::string& what) {
    ++g_checks;
    if (std::fabs(got - want) > tol) {
        ++g_failures;
        std::printf("  FAIL  %s\n          got %g, want %g +- %g\n",
                    what.c_str(), got, want, tol);
    }
}
void section(const char* s) { std::printf("[%s]\n", s); }

constexpr std::int32_t kBins = 1025;      // a 2048-point FFT
constexpr double kRate = 48000.0;

/// Per-band energy with `energy` in the band nearest `hz` and silence elsewhere.
std::vector<double> tone(const MaskingBands& b, double hz, double energy) {
    std::vector<double> e(static_cast<std::size_t>(b.count()), 0.0);
    std::int32_t best = 0;
    double bestD = 1e18;
    for (std::int32_t i = 0; i < b.count(); ++i) {
        const double d = std::fabs(b.centreHz(i) - hz);
        if (d < bestD) { bestD = d; best = i; }
    }
    e[static_cast<std::size_t>(best)] = energy;
    return e;
}

// ---------------------------------------------------------------------------

void testTheBandLayout() {
    section("ADR-0195 d5 -- the bands are auditory, not musical");

    const MaskingBands b(kBins, kRate);
    check(b.count() > 20, "there are bands to speak of");

    // The ERB scale's whole claim, checked at both ends: the ear's filters are
    // about 35 Hz wide at 100 Hz and about 565 Hz wide at 5 kHz. A third-octave
    // layout would give 23 Hz and 1150 Hz -- too narrow low down, twice too
    // wide high up, which is why this measure does not use one.
    near(erbRateToHz(erbRate(100.0) + 1.0) - 100.0, 35.0, 4.0,
         "one ERB at 100 Hz is about 35 Hz");
    near(erbRateToHz(erbRate(5000.0) + 1.0) - 5000.0, 565.0, 60.0,
         "and about 565 Hz at 5 kHz");

    // Round trip, because every band centre is built through it.
    near(erbRate(erbRateToHz(12.0)), 12.0, 1e-9, "erbRate inverts erbRateToHz");
    near(erbRate(0.0), 0.0, 1e-12, "and 0 Hz is 0 Cam");

    bool ascending = true;
    for (std::int32_t i = 1; i < b.count(); ++i)
        if (b.centreHz(i) <= b.centreHz(i - 1)) ascending = false;
    check(ascending, "centres ascend");
    check(b.centreHz(0) >= 20.0, "the lowest band is at or above 20 Hz");
    check(b.centreHz(b.count() - 1) <= 20000.0, "and the highest at or below 20 kHz");

    // EVERY BIN IN THE RANGE LANDS IN EXACTLY ONE BAND. Bins are mapped once
    // and reused, so a gap here would silently drop energy on every frame for
    // the life of the session.
    std::vector<float> mags(static_cast<std::size_t>(kBins), 0.f);
    for (auto& m : mags) m = 1.f;
    std::vector<double> banded;
    b.accumulate(mags, banded);
    double summed = 0.0;
    for (double v : banded) summed += v;
    std::int32_t inRange = 0;
    const double binHz = (kRate * 0.5) / static_cast<double>(kBins - 1);
    for (std::int32_t i = 0; i < kBins; ++i) {
        const double hz = static_cast<double>(i) * binHz;
        if (hz >= 20.0 && hz <= 20000.0) ++inRange;
    }
    near(summed, static_cast<double>(inRange), 0.5,
         "every in-range bin is counted once and only once");

    b.accumulate(mags, banded);
    check(static_cast<std::int32_t>(banded.size()) == b.count(),
          "accumulate sizes its output, so a caller cannot under-size it");

    // Energy, not magnitude: two bins of 1.0 make 2.0, not 4.0.
    std::vector<float> two(static_cast<std::size_t>(kBins), 0.f);
    two[100] = 2.f;
    b.accumulate(two, banded);
    double t = 0.0;
    for (double v : banded) t += v;
    near(t, 4.0, 1e-9, "magnitudes are squared into energy");
}

void testSilenceAndIdentity() {
    section("ADR-0195 d5 -- nothing, and the same thing twice");

    const MaskingBands b(kBins, kRate);
    const std::vector<double> quiet(static_cast<std::size_t>(b.count()), 0.0);

    const auto nothing = measureMasking(quiet, quiet, b);
    near(nothing.maskedFraction, 0.0, 1e-9, "silence masks nothing");
    for (float e : nothing.excessDb)
        check(std::fabs(e) < 1e-6f, "and reports 0 dB rather than a floor minus a floor");

    const auto part = tone(b, 1000.0, 1.0);
    const auto unmasked = measureMasking(quiet, part, b);
    near(unmasked.maskedFraction, 0.0, 1e-9, "a silent masker buries nothing");

    const auto noVictim = measureMasking(part, quiet, b);
    near(noVictim.maskedFraction, 0.0, 1e-9,
         "and a track with no energy cannot be buried -- 0, not a division by zero");

    // EQUAL IS NOT MASKED, at the neutral threshold. The excess is exactly 0
    // in the band they share, and `> threshold` is strict, so neither track
    // buries the other by being identical to it.
    const auto same = measureMasking(part, part, b);
    near(same.maskedFraction, 0.0, 1e-9, "a track does not mask its own twin");
    near(same.worstExcessDb, 0.0, 1e-4, "the excess where they meet is 0 dB");
}

void testTheAsymmetry() {
    section("ADR-0195 d5 -- masking spreads UPWARD, which is the whole point");

    // A loud 1 kHz masker, and two quiet victims an equal distance away on the
    // ERB scale -- one above, one below. The one ABOVE must be buried more.
    // This is the property a symmetric "spectral overlap" gets wrong half the
    // time, and it is why a bass guitar buries a kick's low mids while the
    // kick does not equally bury the bass's top.
    const MaskingBands b(kBins, kRate);
    const double cam = erbRate(1000.0);
    const double aboveHz = erbRateToHz(cam + 3.0);
    const double belowHz = erbRateToHz(cam - 3.0);

    const auto masker = tone(b, 1000.0, 1.0);
    const auto above = measureMasking(masker, tone(b, aboveHz, 1e-6), b);
    const auto below = measureMasking(masker, tone(b, belowHz, 1e-6), b);

    check(above.worstExcessDb > below.worstExcessDb,
          "the band above the masker is buried more deeply than the one below\n"
          "          above " + std::to_string(above.worstExcessDb) +
          " dB, below " + std::to_string(below.worstExcessDb) + " dB");

    // And by roughly the slopes' difference: three ERBs at 27 dB down against
    // three at 10 dB up is 51 dB between them.
    near(above.worstExcessDb - below.worstExcessDb, 51.0, 6.0,
         "by about (27 - 10) dB per ERB over three ERBs");
}

void testLevelAndFraction() {
    section("ADR-0195 d5 -- the one number, weighted by where the energy is");

    const MaskingBands b(kBins, kRate);
    const auto victim = tone(b, 1000.0, 1.0);

    // Monotonic in the masker's level, and in dB: ten times the energy is
    // ten more dB of excess.
    const auto soft = measureMasking(tone(b, 1000.0, 1.0), victim, b);
    const auto loud = measureMasking(tone(b, 1000.0, 10.0), victim, b);
    check(loud.worstExcessDb > soft.worstExcessDb, "a louder masker buries more");
    near(loud.worstExcessDb - soft.worstExcessDb, 10.0, 0.01,
         "and ten times the energy is ten more dB");

    near(loud.maskedFraction, 1.0, 1e-6,
         "a victim whose only energy is buried reads as fully masked");

    // THE FRACTION IS WEIGHTED BY ENERGY, NOT BY BAND COUNT, and the two parts
    // are deliberately UNEQUAL so that the test can tell the difference. An
    // earlier version used two equal tones, where energy-weighting and
    // band-counting both give 0.5 -- a planted "count the bands" fault passed
    // it. Nine tenths of the victim's energy is low, one tenth is high; only
    // the low part is buried. Energy-weighted that is 0.9; counted by bands it
    // would be 0.5.
    std::vector<double> split(static_cast<std::size_t>(b.count()), 0.0);
    const auto lowTone = tone(b, 200.0, 9.0);
    const auto highTone = tone(b, 8000.0, 1.0);
    for (std::size_t i = 0; i < split.size(); ++i) split[i] = lowTone[i] + highTone[i];

    // A masker at 200 Hz reaches 8 kHz only 10 dB per ERB down, which over
    // that distance is far below the victim's own level there.
    const auto uneven = measureMasking(tone(b, 200.0, 1000.0), split, b);
    near(uneven.maskedFraction, 0.9, 0.01,
         "the buried part is nine tenths of the victim's ENERGY, so the fraction "
         "is 0.9 and not the 0.5 a band count would give");

    // The threshold is a knob, and turning it up buries less.
    const auto strict = measureMasking(tone(b, 1000.0, 10.0), victim, b, {}, 20.0);
    near(strict.maskedFraction, 0.0, 1e-6,
         "a 10 dB excess does not clear a 20 dB threshold");
    check(strict.worstExcessDb > 0.f,
          "though the excess is still reported -- the threshold decides what COUNTS, "
          "not what is measured");
}

void testTheSpreadIsAMaximum() {
    section("ADR-0195 d5 -- the spread masker is a maximum, not a sum");

    // Two maskers, one on each side of a victim and each reaching it at the
    // same level. Under a MAXIMUM the victim sees one of them; under a sum it
    // would see 3 dB more, and the measure would then depend on how many
    // partials the masker happens to have rather than on how loud it is where
    // the victim lives. Finer bands would inflate it further, which is the
    // part that makes a sum indefensible: the answer would change with the
    // band count for the same audio.
    const MaskingBands b(kBins, kRate);
    const double victimCam = erbRate(1000.0);
    const double loHz = erbRateToHz(victimCam - 2.0);
    const double hiHz = erbRateToHz(victimCam + 2.0);

    const auto victim = tone(b, 1000.0, 1e-9);
    const auto one = measureMasking(tone(b, loHz, 1.0), victim, b);

    // The second masker is placed so that it reaches the victim at the SAME
    // level as the first: above it, where the slope is shallower, so it is
    // scaled down to compensate.
    const double dLo = victimCam - erbRate(b.centreHz(0));   // unused, kept explicit below
    (void)dLo;
    std::vector<double> both(static_cast<std::size_t>(b.count()), 0.0);
    const auto a = tone(b, loHz, 1.0);
    const auto c = tone(b, hiHz, 1.0);
    for (std::size_t i = 0; i < both.size(); ++i) both[i] = a[i] + c[i];
    const auto pair = measureMasking(both, victim, b);

    // Whatever the two contribute individually, the pair must equal the LOUDER
    // of them and not their sum. A sum would be strictly greater than both.
    const auto other = measureMasking(c, victim, b);
    const float louder = std::max(one.worstExcessDb, other.worstExcessDb);
    near(pair.worstExcessDb, louder, 1e-4,
         "two maskers give the louder one's masking, not the two added together");
}

void testDegenerateInputs() {
    section("ADR-0195 d5 -- inputs that should not crash anything");

    const MaskingBands empty(0, kRate);
    check(empty.count() == 0, "no bins, no bands");
    check(empty.bandOf(0) == -1, "and no band for any bin");
    const auto none = measureMasking({}, {}, empty);
    check(none.worstBand == -1, "measuring nothing reports nothing");

    const MaskingBands narrow(kBins, 100.0);   // Nyquist 50 Hz, under 20 Hz of range
    check(narrow.count() >= 0, "a sample rate with almost no audible range does not crash");

    const MaskingBands b(kBins, kRate);
    std::vector<double> shortVec(2, 1.0);
    const auto mismatched = measureMasking(shortVec, shortVec, b);
    check(mismatched.worstBand == -1,
          "spans shorter than the band count are refused rather than read past");

    check(b.bandOf(-1) == -1 && b.bandOf(1 << 20) == -1, "bins outside the range have no band");
    check(b.centreHz(-1) == 0.0 && b.centreHz(1 << 20) == 0.0, "and no centre");

    // "Falls back to the defaults" is a statement about EQUALITY, so it is
    // tested as one. Asserting merely that the result is non-zero would pass
    // for any slopes at all -- and the first version of this check asserted
    // something weaker still, and failed, because the excess it predicted from
    // the two TONE frequencies is computed between BAND CENTRES: 955.5 Hz and
    // 2029.8 Hz are exactly six bands apart, six ERB at 10 dB each is 60 dB,
    // and the victim sat 60 dB down. The arithmetic was the test's, not the
    // measure's.
    const auto masker = tone(b, 1000.0, 1.0);
    const auto victim = tone(b, 2000.0, 1e-9);
    const auto defaults = measureMasking(masker, victim, b);
    const MaskingSlopes bad{0.0, -5.0};
    const auto fixed = measureMasking(masker, victim, b, bad);
    check(fixed.worstExcessDb > 0.f, "the case is one where masking is visible at all");
    near(fixed.worstExcessDb, defaults.worstExcessDb, 1e-6,
         "and nonsense slopes give exactly the default slopes' answer");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("adi_masking_tests -- spectral masking between two tracks\n\n");
    testTheBandLayout();
    testSilenceAndIdentity();
    testTheAsymmetry();
    testLevelAndFraction();
    testTheSpreadIsAMaximum();
    testDegenerateInputs();
    std::printf("\n%s -- %d checks, %d failure(s)\n",
                g_failures ? "FAILED" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
