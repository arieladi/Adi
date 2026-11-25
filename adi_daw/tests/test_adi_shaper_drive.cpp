// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/adi_shaper_drive.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdio>
#include <limits>
#include <numbers>

namespace {
using Drive = adi::dsp::AdiShaperDrive;
using Mode = Drive::Mode;
int failures = 0;
void check(bool passed, const char* label) {
    if (!passed) { std::printf("FAIL %s\n", label); ++failures; }
}

void impulses() {
    struct Case { Mode mode; double rate; float amount; std::array<double,8> response; };
    constexpr std::array<Case,6> cases{{
        {Mode::HardClip,44100,0,{-.00363831,-.01536444,.02083380,.10192420,.02065902,-.01554675,-.00381838,-.00015444}},
        {Mode::HardClip,48000,0,{-.00363843,-.01536463,.02083392,.10193142,.02067333,-.01553217,-.00380394,-.00014354}},
        {Mode::HardClip,96000,0,{-.00459968,-.01026885,.01244087,.11170604,.01235965,-.01035145,-.00468213,-.00007663}},
        {Mode::Saturate,48000,1,{-.07837487,-.08423101,.01442659,.33629188,.01396779,-.08459789,-.07875308,-.00027552}},
        {Mode::HardClip,48000,1,{-.08414382,-.03569859,-.04886862,.25321501,.03683953,-.09468612,-.06127308,-.00022677}},
        {Mode::Rectify,48000,1,{-.01041990,-.04560130,.05512711,.26107171,.05471348,-.04602684,-.01084031,-.00036333}}
    }};
    constexpr std::array offsets{-4,-2,-1,0,1,2,4,100};
    static_assert(Drive::latencySamples==16);
    double worst=0.0;
    for (const auto& test : cases) {
        Drive drive;
        drive.prepare(test.rate);
        drive.setMode(test.mode);
        int peakFrame=0; float peak=0.0f;
        std::size_t tap=0;
        for (int n=0; n<200; ++n) {
            float l=n==20 ? .125f : 0.0f, r=0.0f;
            drive.process(l,r,test.amount);
            check(r==0,"one-sided impulse keeps the other channel quiet");
            if (std::abs(l)>peak) { peak=std::abs(l); peakFrame=n; }
            if (tap<offsets.size() && n==20+Drive::latencySamples+offsets[tap]) {
                worst=std::max(worst,std::abs(l-test.response[tap]));
                ++tap;
            }
        }
        check(peakFrame==20+Drive::latencySamples,"impulse maximum coincides with host compensation");
    }
    std::printf("Drive impulse maximum absolute error %.8g\n",worst);
    check(worst<.001,"impulses at three rates and three nonlinear transfers");
}

std::array<double,4> spectrum(Drive& drive, int rate, int frequency, float level, float amount,
                              const std::array<int,4>& bins) {
    std::array<std::complex<double>,4> rotating{}, oscillator{}, sums{};
    for (std::size_t i=0; i<bins.size(); ++i) {
        rotating[i]=std::polar(1.0,-2.0*std::numbers::pi*bins[i]/rate);
        oscillator[i]=1;
    }
    for (int n=0; n<rate*2; ++n) {
        float l=level*static_cast<float>(std::sin(2.0*std::numbers::pi*frequency*n/rate)), r=0;
        drive.process(l,r,amount);
        if (n<rate) continue;
        for (std::size_t i=0; i<bins.size(); ++i) {
            sums[i]+=static_cast<double>(l)*oscillator[i];
            oscillator[i]*=rotating[i];
        }
    }
    std::array<double,4> result{};
    for (std::size_t i=0; i<bins.size(); ++i) result[i]=2*std::abs(sums[i])/rate;
    return result;
}

void harmonicTransfers() {
    struct Case { Mode mode; float amplitude; std::array<double,4> bins; };
    constexpr std::array<Case,6> cases{{
        {Mode::Saturate,.03f,{.2157627,.0050316,.0424691,.0140903}},
        {Mode::Saturate,.65f,{.2928504,.0101200,.0937065,.0541297}},
        {Mode::HardClip,.03f,{.2339189,0,.0634160,.0234800}},
        {Mode::HardClip,.2f,{.2396593,0,.0796309,.0473702}},
        {Mode::Rectify,.2f,{.2202738,.0605848,.0969154,.0140775}},
        {Mode::Rectify,.65f,{.0179701,.0976110,.0480446,.0631293}}
    }};
    double worst=0;
    for (const auto& test : cases) {
        Drive drive;
        drive.setMode(test.mode);
        const auto out=spectrum(drive,48000,199,test.amplitude,.75f,{199,398,597,995});
        for (std::size_t i=0; i<out.size(); ++i) worst=std::max(worst,std::abs(out[i]-test.bins[i]));
    }
    std::printf("Drive 199 Hz harmonic matrix maximum error %.8g\n",worst);
    check(worst<.0005,"held-out tone levels through saturation, clipping and rectification");
}

void aliases() {
    Drive drive;
    drive.setMode(Mode::HardClip);
    const auto out=spectrum(drive,48000,9000,.35f,1,{9000,21000,3000,15000});
    constexpr std::array expected{.2191487,.0006555,.0001827,.0010004};
    double worst=0;
    for (std::size_t i=0; i<out.size(); ++i) worst=std::max(worst,std::abs(out[i]-expected[i]));
    std::printf("Drive 9 kHz alias fixture maximum error %.8g\n",worst);
    check(worst<.00012,"hard clipping rejects harmonics folded into the audible band");
}

void nearZeroContinuity() {
    constexpr float firstStep = .0001f; // 0.01% in the rack
    constexpr int section = 512;
    struct Setting { Mode mode; bool rounded; };
    constexpr std::array modes{Setting{Mode::Saturate, false}, Setting{Mode::HardClip, false},
                               Setting{Mode::Rectify, false}, Setting{Mode::HardClip, true}};
    constexpr std::array rates{44100, 48000, 96000};
    constexpr std::array trims{std::array{.5f, .5f}, std::array{1.f, .25f}};
    double worstNearZero = 0., worstAtKnot = 0.;
    for (auto [mode, rounded] : modes) {
        const float firstKnot = rounded ? 1.f / 64.f : 1.f / 32.f;
        for (auto rate : rates) {
            for (auto trim : trims) {
                Drive zero, moving, between, belowKnot, atKnot;
                for (auto* drive : {&zero, &moving, &between, &belowKnot, &atKnot}) {
                    drive->prepare(rate);
                    drive->setMode(mode);
                    drive->setRoundedClip(rounded);
                    drive->setTrims(trim[0], trim[1]);
                }
                zero.setMode(Mode::HardClip); // already pinned by the zero-Drive impulse fixture
                zero.setRoundedClip(false);
                std::array<double, 2> peak{}, movingError{}, betweenError{}, knotError{};
                bool initialZeroExact = true, finite = true;
                for (int n = 0; n < 6 * section; ++n) {
                    const auto phase = 2. * std::numbers::pi * n / rate;
                    const std::array input{1.45f * static_cast<float>(std::sin(133. * phase)),
                                           -1.25f * static_cast<float>(std::cos(211. * phase))};
                    auto dry = input, live = input, mid = input, below = input, at = input;
                    float amount = 0.f;
                    if (n >= section && n < 2 * section) {
                        amount = firstStep;
                    } else if (n >= 3 * section && n < 5 * section) {
                        const int distance = std::min(n - 3 * section, 5 * section - n);
                        amount = firstStep * static_cast<float>(distance) / section;
                    }
                    zero.process(dry[0], dry[1], 0.f);
                    moving.process(live[0], live[1], amount);
                    between.process(mid[0], mid[1], firstKnot * .5f);
                    belowKnot.process(below[0], below[1], std::nextafter(firstKnot, 0.f));
                    atKnot.process(at[0], at[1], firstKnot);
                    if (n < section) initialZeroExact = initialZeroExact && live == dry;
                    for (std::size_t channel = 0; channel < peak.size(); ++channel) {
                        const auto unshaped = static_cast<double>(dry[channel]);
                        finite = finite && std::isfinite(unshaped) && std::isfinite(live[channel]) &&
                                 std::isfinite(mid[channel]) && std::isfinite(below[channel]) &&
                                 std::isfinite(at[channel]);
                        peak[channel] = std::max(peak[channel], std::abs(unshaped));
                        movingError[channel] = std::max(movingError[channel], std::abs(live[channel] - unshaped));
                        betweenError[channel] = std::max(betweenError[channel], std::abs(mid[channel] - unshaped));
                        knotError[channel] = std::max(knotError[channel],
                            std::abs(static_cast<double>(at[channel]) - below[channel]));
                    }
                }
                check(initialZeroExact && finite, "exact zero Drive keeps the pinned finite response in every mode");
                for (std::size_t channel = 0; channel < peak.size(); ++channel) {
                    const auto scale = std::max(1., peak[channel]);
                    const auto nearZero = movingError[channel] / scale;
                    const auto knot = knotError[channel] / scale;
                    worstNearZero = std::max(worstNearZero, nearZero);
                    worstAtKnot = std::max(worstAtKnot, knot);
                    check(nearZero < .001, "entering and leaving 0.01% Drive stays close to zero without a jump");
                    check(betweenError[channel] / scale > .005, "low Drive develops its effect below the first knot");
                    check(knot < .000001, "Drive joins the existing transfer continuously at its first knot");
                }
            }
        }
    }
    std::printf("Drive near-zero and first-knot normalized peak errors %.8g %.8g\n", worstNearZero, worstAtKnot);
}

void trimsAndReset() {
    Drive reference, trimmed;
    reference.setMode(Mode::Rectify);
    trimmed.setMode(Mode::Rectify);
    trimmed.setTrims(.75f,.25f);
    double worst=0;
    for (int n=0; n<1400; ++n) {
        float a=static_cast<float>((n%23)-11)/128.0f, b=0, c=a*8, d=0;
        trimmed.process(a,b,.5f);
        reference.process(c,d,.5f);
        worst=std::max(worst,std::abs(static_cast<double>(a-c*.125f)));
    }
    check(worst<2e-7,"input trim precedes shaping and output trim follows it");
    trimmed.reset();
    for (int n=0; n<40; ++n) {
        float l=0,r=0;
        trimmed.process(l,r,.5f);
        check(l==0 && r==0,"reset removes oversampling memory and DC history");
    }
}

void movingStereo() {
    constexpr std::array modes{Mode::Saturate, Mode::HardClip, Mode::Rectify};
    for (const double rate : {44100., 48000., 96000.}) {
        Drive stereo, leftOnly, rightOnly;
        for (auto* drive : {&stereo, &leftOnly, &rightOnly}) drive->prepare(rate);
        bool independent = true;
        for (int n = 0; n < 6144; ++n) {
            if (n % 2048 == 0) {
                for (auto* drive : {&stereo, &leftOnly, &rightOnly})
                    drive->setMode(modes[static_cast<std::size_t>(n / 2048)]);
            }
            // The channels have different periods and reach opposite rails.
            float l = static_cast<float>(n % 37 - 18) / 21.f;
            float r = static_cast<float>(14 - n % 31) / 29.f;
            float expectedL = l, expectedR = r, quietL = 0.f, quietR = 0.f;
            const auto amount = static_cast<float>(n % 513) / 512.f;
            stereo.process(l, r, amount);
            leftOnly.process(expectedL, quietR, amount);
            rightOnly.process(quietL, expectedR, amount);
            independent &= l == expectedL && r == expectedR && quietL == 0.f && quietR == 0.f;
        }
        check(independent, "moving Drive keeps both stereo FIR histories independent at each rate");
    }
}

void roundedSignedSteps() {
    // Independent stereo plateaus below the knee, on its shoulder, and near its limit.
    // Undo the 10 Hz DC blocker to recover the signed transfer at each settled level.
    struct Step { int rate; float amount, left, right; double outLeft, outRight; };
    constexpr std::array cases{
        Step{48000, .179f, .270550229f, -.352634428f, .30338347, -.38269814},
        Step{48000, .179f, 1.593652036f, -.325579405f, .41370466, -.36494506},
        Step{48000, .786f, .012404823f, -.165087084f, .18314348, -.19041769},
        Step{96000, .341f, .146545882f, -.110182604f, .25694102, -.20016453},
        Step{96000, .923f, .077229678f, -.005757862f, .17849944, -.17169416}};
    for (const auto& step : cases) {
        Drive drive;
        drive.prepare(step.rate);
        drive.setMode(Mode::HardClip);
        drive.setRoundedClip(true);
        const double poleLoss = 1. - std::exp(-2. * std::numbers::pi * 10. / step.rate);
        double sumL = 0., sumR = 0.;
        for (int n = 0; n < 160; ++n) {
            float l = step.left, r = step.right;
            drive.process(l, r, step.amount);
            if (n == 159)
                check(std::abs(l + poleLoss * sumL - step.outLeft) < .00025 &&
                      std::abs(r + poleLoss * sumR - step.outRight) < .00025,
                      "rounded clipping reproduces signed plateaus at independent Drive settings");
            sumL += l;
            sumR += r;
        }
    }
}

void roundedImpulses() {
    constexpr std::array offsets{-3, -1, 0, 1, 3};
    constexpr std::array left{.12646748, -.01168041, .33956680, -.01210934, .12615719};
    constexpr std::array right{-.06120633, -.02325201, -.31063837, -.02281523, -.06091620};
    for (const int rate : {48000, 96000}) {
        Drive drive;
        drive.prepare(rate);
        drive.setMode(Mode::HardClip);
        drive.setRoundedClip(true);
        std::array<std::array<float, 96>, 2> output{};
        constexpr std::array onset{11, 43};
        output[0][onset[0]] = .97f;
        output[1][onset[1]] = -.38f;
        for (std::size_t n = 0; n < output[0].size(); ++n)
            drive.process(output[0][n], output[1][n], .5f);
        for (std::size_t ch = 0; ch < output.size(); ++ch) {
            const auto& samples = output[ch];
            const auto peak = std::max_element(samples.begin(), samples.end(),
                [](float a, float b) { return std::abs(a) < std::abs(b); });
            const auto center = onset[ch] + Drive::latencySamples;
            check(peak - samples.begin() == center, "rounded clipping retains its 16-frame impulse latency");
            if (rate == 48000) {
                const auto& reference = ch == 0 ? left : right;
                for (std::size_t i = 0; i < offsets.size(); ++i)
                    check(std::abs(samples[static_cast<std::size_t>(center + offsets[i])] - reference[i]) < .002,
                          "rounded clipping reconstructs both signed impulse shoulders and peaks");
            }
        }
    }
}

void roundedScopeAndSwitching() {
    for (auto mode : {Mode::Saturate, Mode::HardClip, Mode::Rectify}) {
        Drive original, rounded;
        original.setMode(mode);
        rounded.setRoundedClip(true);
        rounded.setMode(mode);
        double difference = 0.;
        for (int n = 0; n < 240; ++n) {
            float l = static_cast<float>(n % 29 - 14) / 31.f, r = -.375f * l, a = l, b = r;
            original.process(l, r, .5f);
            rounded.process(a, b, .5f);
            difference = std::max({difference, std::abs(static_cast<double>(l) - a), std::abs(static_cast<double>(r) - b)});
        }
        check(mode == Mode::HardClip ? difference > .01 : difference == 0.,
              "the rounded switch changes only the hard clipping style");
    }

    Drive live;
    live.setMode(Mode::HardClip);
    for (const bool enable : {true, false}) {
        Drive expected;
        expected.setMode(Mode::HardClip);
        expected.setRoundedClip(enable);
        live.reset();
        float quietL = 0.f, quietR = 0.f;
        live.process(quietL, quietR, .5f);
        live.setRoundedClip(enable);
        bool exact = true;
        for (int n = 0; n < 64; ++n) {
            float l = n == 5 ? .8f : 0.f, r = n == 9 ? -.4f : 0.f, a = l, b = r;
            live.process(l, r, .5f);
            expected.process(a, b, .5f);
            exact &= l == a && r == b;
        }
        check(exact, "changing the clipping law refreshes the transfer at the current Drive amount");
    }
    live.setRoundedClip(true);
    float l = .75f, r = -.625f;
    live.process(l, r, .5f);
    auto copy = live;
    live.setRoundedClip(false);
    live.setRoundedClip(true);
    bool sameTail = true;
    for (int n = 0; n < 48; ++n) {
        float a = 0.f, b = 0.f, c = 0.f, d = 0.f;
        live.process(a, b, .5f);
        copy.process(c, d, .5f);
        sameTail &= a == c && b == d;
    }
    check(sameTail, "copying and switching the rounded law preserve queued stereo samples");
}

void roundedStrengthTiming() {
    const auto sample = [](Drive& drive, float amount) {
        std::array output{.375f, -.625f};
        drive.process(output[0], output[1], amount);
        return output;
    };
    // Both copies already have audible input in their filters. A one-frame
    // strength edit first appears in the leading coefficient of the output FIR.
    const auto firstDifference = [&](const Drive& state, float held, float pulse) {
        auto plain = state, edited = state;
        for (int n = 0; n < 24; ++n)
            if (sample(plain, held) != sample(edited, n == 0 ? pulse : held)) return n;
        return -1;
    };
    for (const int rate : {48000, 96000}) {
        Drive fresh;
        fresh.prepare(rate);
        fresh.setMode(Mode::HardClip);
        fresh.setRoundedClip(true);
        for (const float held : {.125f, .875f}) {
            const float pulse = 1.f - held;
            auto drive = fresh;
            for (int n = 0; n < 64; ++n) sample(drive, held);
            check(firstDifference(drive, held, pulse) == 8,
                  "rounded Drive delays a rising or falling strength edit by eight base-rate frames");
            auto clean = drive;
            sample(drive, pulse); // pending in the rounded control history
            sample(clean, held);
            auto reset = drive, cold = fresh;
            reset.reset();
            bool resetMatches = true;
            for (int n = 0; n < 32; ++n) resetMatches &= sample(reset, .5f) == sample(cold, .5f);
            check(resetMatches, "reset primes rounded Drive with the first fresh strength");

            drive.setRoundedClip(false);
            clean.setRoundedClip(false);
            check(firstDifference(drive, held, pulse) == 0,
                  "switching off rounded clipping immediately restores hard clipping strength timing");
            for (const float amount : {.375f, .625f}) {
                sample(drive, amount);
                sample(clean, amount);
            }
            drive.setRoundedClip(true);
            check(firstDifference(drive, held, pulse) == 0,
                  "re-enabling rounded clipping applies its first fresh strength immediately");
            bool freshStrength = true;
            for (int n = 0; n < 24; ++n) {
                // Re-prime the control on the comparison path to hold this value
                // throughout the first eight frames while retaining its audio.
                clean.setRoundedClip(false);
                clean.setRoundedClip(true);
                freshStrength &= sample(drive, .5f) == sample(clean, .5f);
            }
            check(freshStrength, "strengths queued before enabling rounded clipping never reappear");
        }
    }
}

void roundedTrimsAndRecovery() {
    Drive reference, trimmed;
    for (auto* drive : {&reference, &trimmed}) {
        drive->setMode(Mode::HardClip);
        drive->setRoundedClip(true);
    }
    trimmed.setTrims(.75f, .25f);
    double worst = 0.;
    for (int n = 0; n < 1024; ++n) {
        float l = static_cast<float>(n % 37 - 18) / 64.f;
        float r = static_cast<float>(n % 17 - 8) / 64.f, a = l * 8.f, b = r * 8.f;
        const float amount = static_cast<float>(n % 257) / 256.f;
        trimmed.process(l, r, amount);
        reference.process(a, b, amount);
        worst = std::max({worst, std::abs(static_cast<double>(l) - .125 * a), std::abs(static_cast<double>(r) - .125 * b)});
    }
    check(worst < 2.e-7, "rounded clipping applies the input and output trims around the swept transfer");
    check(!trimmed.finishBlock(), "a full sweep of rounded Drive remains finite");
    for (int n = 0; n < 8; ++n) {
        float l = .125f, r = -.25f;
        trimmed.process(l, r, .5f);
    }
    auto clean = trimmed;
    auto repaired = trimmed;
    repaired.reset();
    float bad = std::numeric_limits<float>::quiet_NaN(), good = .25f, quiet = 0.f, other = good;
    trimmed.process(bad, good, .5f);
    clean.process(quiet, other, .5f);
    check(trimmed.finishBlock(), "rounded clipping detects nonfinite channel history at the block boundary");
    bool recovered = true;
    for (int n = 0; n < 64; ++n) {
        float a = .1f, b = -.2f, c = a, d = b, e = a, f = b;
        trimmed.process(a, b, .5f);
        repaired.process(c, d, .5f);
        clean.process(e, f, .5f);
        recovered &= std::isfinite(a) && std::isfinite(b) && a == c && b == f;
    }
    check(recovered && !trimmed.finishBlock(), "rounded clipping repairs one channel and retains the other channel's history");
}
} // namespace

int main() {
    impulses();
    harmonicTransfers();
    aliases();
    nearZeroContinuity();
    trimsAndReset();
    movingStereo();
    roundedSignedSteps();
    roundedImpulses();
    roundedScopeAndSwitching();
    roundedStrengthTiming();
    roundedTrimsAndRecovery();
    return failures==0 ? 0 : 1;
}
