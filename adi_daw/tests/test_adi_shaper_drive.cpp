// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/adi_shaper_drive.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdio>
#include <numbers>

namespace {
using Drive = adi::dsp::AdiShaperDrive;
using Mode = Drive::Mode;
using Fold = Drive::Fold;
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

void foldMarkers() {
    // Two isolated signed markers, then two stereo steps above the first wrap.
    // Each reading samples the causal output, including the DC filter.
    struct Case { Fold shape; int rate; float strength; std::array<double,4> expected; };
    constexpr std::array<Case,15> cases{{
        {Fold::Sine,     44100, .37f, {.26150358,-.21372926,.20258918,.17685172}},
        {Fold::Triangle, 44100, .37f, {.20828502,-.25206289,.15857016,.17463632}},
        {Fold::Saw,      44100, .37f, {.26097262,-.34279054,.18175191,.19954555}},
        {Fold::Sine,     48000, .37f, {.26151153,-.21373630,.20258996,.17697892}},
        {Fold::Triangle, 48000, .37f, {.20829174,-.25207469,.15857042,.17523086}},
        {Fold::Saw,      48000, .37f, {.26098210,-.34281272,.18175474,.19987714}},
        {Fold::Sine,     96000, .37f, {.16108949,-.17815891,.18738486,.18831335}},
        {Fold::Triangle, 96000, .37f, {.18408582,-.16295061,.19079559,.13858804}},
        {Fold::Saw,      96000, .37f, {.41836667,-.24060534,.26817626,.23202392}},
        {Fold::Sine,     48000, 0.f, {.59527951,-.49742535,1.09761047,1.28790915}},
        {Fold::Triangle, 48000, 0.f, {.59527951,-.49742535,1.09761047,1.28790915}},
        {Fold::Saw,      48000, 0.f, {.59527951,-.49742535,1.09761047,1.28790915}},
        {Fold::Sine,     48000, .0001f, {.59465456,-.49741781,.91613245,.96652347}},
        {Fold::Triangle, 48000, .0001f, {.59527951,-.49742535,.75939047,.72144502}},
        {Fold::Saw,      48000, .0001f, {.59527963,-.49742547,.48796594,.47637159}}
    }};
    double worst = 0.;
    for (const auto& test : cases) {
        Drive drive;
        drive.prepare(test.rate);
        drive.setFold(test.shape);
        std::array<float,4> readings{};
        std::array<float,2> peaks{};
        std::array<int,2> peakFrames{};
        bool isolated = true;
        for (int n = 0; n <= 2516; ++n) {
            std::array<float,2> signal{};
            if (n == 320) signal[0] = .73f;
            if (n == 723) signal[1] = -.61f;
            if (n >= 1200 && n < 1360) signal = {1.21f, -.84f};
            if (n >= 2500) signal = {-.36f, 1.38f};
            drive.process(signal[0], signal[1], test.strength);
            if (n < 723) isolated &= signal[1] == 0.f;
            if (n < 1200) {
                for (std::size_t ch = 0; ch < peaks.size(); ++ch) {
                    if (std::abs(signal[ch]) > peaks[ch]) {
                        peaks[ch] = std::abs(signal[ch]);
                        peakFrames[ch] = n;
                    }
                }
            }
            if (n == 336) readings[0] = signal[0];
            if (n == 739) readings[1] = signal[1];
            if (n == 1216) readings[2] = signal[0];
            if (n == 2516) readings[3] = signal[1];
        }
        check(isolated, "a left fold marker leaves the right history silent");
        check(peakFrames == std::array{336, 739}, "both independent fold markers peak 16 frames after input");
        for (std::size_t i = 0; i < readings.size(); ++i) {
            check(std::isfinite(readings[i]), "fold marker output is finite");
            worst = std::max(worst, std::abs(readings[i] - test.expected[i]));
        }
    }
    std::printf("Fold signed marker maximum error %.8g\n", worst);
    check(worst < .00003, "fold signed markers at three rates and zero versus first positive strength");
}

void foldContour() {
    // A two-second cycle rises between grid coordinates 2047 and 2048.
    // The first oversampled phase reads one frame earlier than the others.
    const auto level = [](int coordinate) { return coordinate < 2048 ? .12 : .86; };
    constexpr std::array folds{Fold::Sine, Fold::Triangle, Fold::Saw};
    constexpr std::array expected{
        std::array{.04932128, .07201698, .17009312, .20642202},
        std::array{.04695194, .05836519, .18991593, .21032459},
        std::array{.05277730, .05681935, .16513643, .15378042}
    };
    constexpr std::array frames{24006, 24011, 24016, 24021};
    double worst = 0.;
    for (std::size_t m = 0; m < folds.size(); ++m) {
        Drive drive;
        drive.prepare(48000);
        drive.setFold(folds[m]);
        std::size_t reading = 0;
        for (int n = 0; n <= frames.back(); ++n) {
            const double phase = n * (8192. / 96000.);
            const auto i = static_cast<int>(phase);
            const double x = phase - i;
            const double a = level(i-1), b = level(i), c = level(i+1), d = level(i+2);
            const double curve = b + .5*x*(c-a + x*(2*a-5*b+4*c-d + x*(3*(b-c)+d-a)));
            const double t = 2.*std::numbers::pi*n/48000.;
            float left = static_cast<float>(.037*std::cos(278*t) + .015*std::sin(443*t) + .005);
            float right = static_cast<float>(-.039*std::cos(365*t) - .018*std::sin(517*t) - .014);
            drive.process(left, right, static_cast<float>(std::clamp(curve, 0., 1.)));
            if (n == frames[reading]) {
                const float value = reading % 2 == 0 ? left : right;
                check(std::isfinite(value), "fold contour output is finite");
                worst = std::max(worst, std::abs(value - expected[m][reading]));
                ++reading;
            }
        }
    }
    std::printf("Fold rising contour maximum error %.8g\n", worst);
    check(worst < .00003, "fold control arrives at the measured subphase on a sharp rising edge");
}

void sawWrapAndFoldReset() {
    Drive drive;
    drive.prepare(48000);
    const double dcRate = -std::expm1(-2. * std::numbers::pi * 10. / 48000.);
    constexpr std::array expected{.33883445, .00035526};
    for (std::size_t i = 0; i < expected.size(); ++i) {
        drive.setFold(Fold::Saw);
        drive.reset();
        const float level = (159.f + static_cast<float>(i)) / 1024.f;
        double sum = 0.;
        for (int n = 0; n <= 96; ++n) {
            float left = level, right = -level;
            drive.process(left, right, .5f);
            if (n == 96) {
                // Restore the settled DC value from the causal high-pass output.
                const auto settled = (left + dcRate * sum) / (1. - .5 * dcRate);
                check(std::abs(settled - expected[i]) < .00003 && left == -right,
                      "signed saw levels one input step apart straddle the first wrap");
            }
            sum += left;
        }
    }
    for (Fold shape : {Fold::Triangle, Fold::Off, Fold::Saw, Fold::Sine, Fold::Saw}) {
        drive.setFold(shape);
        for (int n = 0; n < 72; ++n) {
            float left = .71f, right = -.23f;
            drive.process(left, right, static_cast<float>(n) / 72.f);
        }
        drive.reset();
        Drive fresh;
        fresh.prepare(48000);
        fresh.setFold(shape);
        bool equal = true;
        for (int n = 0; n < 128; ++n) {
            float left = n == 11 ? .41f : 0.f, right = n == 47 ? -.32f : 0.f;
            float referenceL = left, referenceR = right;
            const float amount = n < 19 ? .83f : .27f;
            drive.process(left, right, amount);
            fresh.process(referenceL, referenceR, amount);
            equal &= left == referenceL && right == referenceR;
        }
        check(equal, "reset restores fresh stereo output after repeated fold selection");
    }
}
} // namespace

int main() {
    impulses();
    harmonicTransfers();
    aliases();
    nearZeroContinuity();
    trimsAndReset();
    movingStereo();
    foldMarkers();
    foldContour();
    sawWrapAndFoldReset();
    return failures==0 ? 0 : 1;
}
