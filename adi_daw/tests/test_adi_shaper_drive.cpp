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
    constexpr float firstKnot = 1.f / 32.f;
    constexpr int section = 512;
    constexpr std::array modes{Mode::Saturate, Mode::HardClip, Mode::Rectify};
    constexpr std::array rates{44100, 48000, 96000};
    constexpr std::array trims{std::array{.5f, .5f}, std::array{1.f, .25f}};
    double worstNearZero = 0., worstAtKnot = 0.;
    for (auto mode : modes) {
        for (auto rate : rates) {
            for (auto trim : trims) {
                Drive zero, moving, between, belowKnot, atKnot;
                for (auto* drive : {&zero, &moving, &between, &belowKnot, &atKnot}) {
                    drive->prepare(rate);
                    drive->setMode(mode);
                    drive->setTrims(trim[0], trim[1]);
                }
                zero.setMode(Mode::HardClip); // already pinned by the zero-Drive impulse fixture
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

// Recover the settled transfer by undoing the measured 10 Hz DC blocker.
std::array<double, 2> powerPlateau(int rate, float amount, float left, float right) {
    Drive drive;
    drive.prepare(rate);
    drive.setMode(Mode::Saturate);
    drive.setPowerSaturation(true);
    const double poleLoss = 1. - std::exp(-2. * std::numbers::pi * 10. / rate);
    std::array<double, 2> sum{}, plateau{};
    for (int n = 0; n < 128; ++n) {
        std::array out{left, right};
        drive.process(out[0], out[1], amount);
        for (std::size_t ch = 0; ch < out.size(); ++ch) {
            if (n == 127) plateau[ch] = (out[ch] + poleLoss * sum[ch]) / (1. - .5 * poleLoss);
            sum[ch] += out[ch];
        }
    }
    return plateau;
}

void powerTransfers() {
    struct Step { int rate; float amount, left, right; double outLeft, outRight; };
    constexpr std::array cases{
        Step{48000, .125f, .16f, -3.5f, .17120872, -2.87554055},
        Step{48000, .5f, .015f, -.6875f, .08446007, -.39013755},
        Step{96000, .75f, .0090625f, -.25f, .11773005, -.25130772},
        Step{96000, 1.f, .00046875f, -.1875f, .10099485, -.23770496}};
    for (const auto& step : cases) {
        const auto out = powerPlateau(step.rate, step.amount, step.left, step.right);
        check(std::abs(out[0] - step.outLeft) < .00025 && std::abs(out[1] - step.outRight) < .00035,
              "signed power saturation matches measured positive and negative levels across strengths");
    }
    for (const int rate : {48000, 96000}) {
        for (const float amount : {1.f/256.f, 1.f/64.f, 3.f/128.f}) {
            const auto out = powerPlateau(rate, amount, -.0625f, 1.f);
            const double ratio = std::pow(.0625, 1. / (1. + 6. * amount * amount));
            check(std::abs(-out[0]/out[1] - ratio) < 2.e-6,
                  "power saturation follows its natural exponent down to the first strength increments");
        }
        for (const float sign : {-1.f, 1.f}) {
            const auto out = powerPlateau(rate, 1.f, sign*1.e-12f, .25f);
            check(std::abs(out[0] - sign*.005829) < 3.e-6,
                  "a signed input at 1e-12 survives beside an active channel");
        }
    }
}

void powerImpulses() {
    constexpr std::array offsets{-3, -1, 0, 1, 3};
    constexpr std::array left{.09524528, .05098106, .49998841, .05026031, .09463098};
    constexpr std::array right{-.08948985, -.04789534, -.46976095, -.04721818, -.08891269};
    for (const int rate : {44100, 48000, 96000}) {
        Drive drive;
        drive.prepare(rate);
        drive.setPowerSaturation(true);
        std::array<std::array<float, 96>, 2> output{};
        constexpr std::array onset{11, 43};
        output[0][onset[0]] = .97f;
        output[1][onset[1]] = -.83f;
        for (std::size_t n = 0; n < output[0].size(); ++n)
            drive.process(output[0][n], output[1][n], .5f);
        for (std::size_t ch = 0; ch < output.size(); ++ch) {
            const auto& samples = output[ch];
            const auto peak = std::max_element(samples.begin(), samples.end(),
                [](float a, float b) { return std::abs(a) < std::abs(b); });
            const int center = onset[ch] + Drive::latencySamples;
            check(peak - samples.begin() == center, "signed power saturation keeps 16 frames of audio latency");
            if (rate == 48000) {
                const auto& reference = ch == 0 ? left : right;
                for (std::size_t i = 0; i < offsets.size(); ++i)
                    check(std::abs(samples[static_cast<std::size_t>(center + offsets[i])] - reference[i]) < .001,
                          "signed power saturation reconstructs the measured impulse shoulders and peak");
            }
        }
    }
}

void powerScopeAndTrims() {
    for (auto mode : {Mode::Saturate, Mode::HardClip, Mode::Rectify}) {
        Drive original, power, trimmed;
        for (auto* drive : {&original, &power, &trimmed}) drive->setMode(mode);
        trimmed.setTrims(.75f, .25f);
        power.setPowerSaturation(true);
        trimmed.setPowerSaturation(true);
        bool zeroExact = true, same = true, trimsMatch = true;
        for (int n = 0; n < 640; ++n) {
            float l = static_cast<float>(n%37 - 18)/8.f, r = static_cast<float>(n%19 - 9)/8.f;
            float a = l, b = r, c = l * .125f, d = r * .125f;
            const float amount = n < 128 ? 0.f : static_cast<float>(n%129)/128.f;
            original.process(l, r, amount);
            power.process(a, b, amount);
            trimmed.process(c, d, amount);
            if (n < 128) zeroExact &= l == a && r == b;
            same &= l == a && r == b;
            trimsMatch &= std::abs(c - .125f*a) < 2.e-7f && std::abs(d - .125f*b) < 2.e-7f;
        }
        check(zeroExact, "zero strength preserves the original signed audio through the FIRs");
        check(same == (mode != Mode::Saturate), "the signed power switch changes only saturation");
        check(trimsMatch, "input trim precedes the signed transfer and output trim follows it");
    }
}

void powerStrengthTiming() {
    const auto sample = [](Drive& drive, float amount) {
        std::array out{.375f, -.625f};
        drive.process(out[0], out[1], amount);
        return out;
    };
    const auto firstDifference = [&](const Drive& state, float held, float pulse) {
        auto plain = state, edited = state;
        for (int n = 0; n < 24; ++n)
            if (sample(plain, held) != sample(edited, n == 0 ? pulse : held)) return n;
        return -1;
    };
    for (const int rate : {48000, 96000}) {
        Drive fresh;
        fresh.prepare(rate);
        fresh.setPowerSaturation(true);
        for (const float held : {.125f, .875f}) {
            const float pulse = 1.f - held;
            auto warm = fresh;
            for (int n = 0; n < 64; ++n) sample(warm, held);
            check(firstDifference(warm, held, pulse) == 8,
                  "signed power delays rising and falling strength changes by eight base-rate frames");
            auto pending = warm;
            sample(pending, pulse);
            sample(warm, held);
            auto reset = pending, cold = fresh;
            reset.reset();
            bool resetMatches = true;
            for (int n = 0; n < 32; ++n) resetMatches &= sample(reset, .5f) == sample(cold, .5f);
            check(resetMatches, "reset discards pending power strength and primes from the first new value");

            for (const bool changeStyle : {false, true}) {
                const auto select = [changeStyle](Drive& drive, bool active) {
                    if (changeStyle) drive.setMode(active ? Mode::Saturate : Mode::HardClip);
                    else drive.setPowerSaturation(active);
                };
                auto live = pending, clean = warm;
                select(live, false);
                select(clean, false);
                check(firstDifference(live, held, pulse) == 0, "leaving signed power restores immediate strength changes");
                for (const float amount : {.375f, .625f}) {
                    sample(live, amount);
                    sample(clean, amount);
                }
                select(live, true);
                check(firstDifference(live, held, pulse) == 0, "entering signed power applies the first fresh strength immediately");
                bool same = true;
                for (int n = 0; n < 24; ++n) {
                    // Hold the first fresh value on a re-primed comparison path.
                    select(clean, false);
                    select(clean, true);
                    same &= sample(live, .5f) == sample(clean, .5f);
                }
                check(same, "changes of style or power selection discard queued strengths and retain the audio history");
            }
        }
    }
}

void powerResetAndRecovery() {
    Drive drive;
    drive.setPowerSaturation(true);
    bool silent = true;
    for (int n = 0; n < 64; ++n) {
        float l = 0.f, r = 0.f;
        drive.process(l, r, static_cast<float>(n)/63.f);
        silent &= l == 0.f && r == 0.f;
    }
    check(silent, "a moving signed power strength preserves exact silence");
    for (int n = 0; n < 64; ++n) {
        float l = .1f, r = -.2f;
        drive.process(l, r, .5f);
    }
    auto clean = drive, fresh = drive;
    fresh.reset();
    float bad = std::numeric_limits<float>::quiet_NaN(), r = .3f, zero = 0.f, other = r;
    drive.process(bad, r, .5f);
    clean.process(zero, other, .5f);
    check(drive.finishBlock(), "power saturation detects nonfinite channel history");
    bool recovered = true;
    for (int n = 0; n < 64; ++n) {
        float a = .1f, b = -.2f, c = a, d = b, e = a, f = b;
        drive.process(a, b, .5f);
        fresh.process(c, d, .5f);
        clean.process(e, f, .5f);
        recovered &= std::isfinite(a) && std::isfinite(b) && a == c && b == f;
    }
    check(recovered && !drive.finishBlock(), "power saturation recovers the damaged channel and preserves the other");
}
} // namespace

int main() {
    impulses();
    harmonicTransfers();
    aliases();
    nearZeroContinuity();
    trimsAndReset();
    movingStereo();
    powerTransfers();
    powerImpulses();
    powerScopeAndTrims();
    powerStrengthTiming();
    powerResetAndRecovery();
    return failures==0 ? 0 : 1;
}
