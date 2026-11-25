// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/adi_shaper_drive.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdio>
#include <numbers>
#include <limits>

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

void driveShapeContour() {
    // A two-second cycle rises between grid coordinates 2047 and 2048.
    // The first oversampled phase reads one frame earlier than the others.
    const auto level = [](int coordinate) { return coordinate < 2048 ? .12 : .86; };
    constexpr std::array folds{Fold::Sine, Fold::Triangle, Fold::Saw, Fold::Off, Fold::Off};
    constexpr std::array expected{
        std::array{.04932128, .07201698, .17009312, .20642202},
        std::array{.04695194, .05836519, .18991593, .21032459},
        std::array{.05277730, .05681935, .16513643, .15378042},
        std::array{.04742835, .08365244, .17877781, .19186419},
        std::array{.05731918, .10924069, .17000119, .18931559}
    };
    constexpr std::array frames{24006, 24011, 24016, 24021};
    double worst = 0.;
    for (std::size_t m = 0; m < folds.size(); ++m) {
        Drive drive;
        drive.prepare(48000);
        drive.setFold(folds[m]);
        if (m == 3) {
            drive.setMode(Mode::HardClip);
            drive.setRoundedClip(true);
        }
        if (m == 4) drive.setPowerSaturation(true);
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
                check(std::isfinite(value), "Drive contour output is finite");
                worst = std::max(worst, std::abs(value - expected[m][reading]));
                ++reading;
            }
        }
    }
    std::printf("Drive rising contour maximum error %.8g\n", worst);
    check(worst < .00003, "Drive control arrives at the measured subphase on a sharp rising edge");
}

void optionalShapeAtZero() {
    // At zero the transfer is linear through the two reconstruction filters.
    // The first positive strength already rounds an input above the knee.
    struct Reading { int rate; double impulseL, impulseR, zeroPlateau, roundedPlateau; };
    constexpr std::array<Reading,2> readings{{
        {48000, .59527951, -.49742535, 1.15319026, .91046429},
        {96000, .65236324, -.54512548, 1.18121147, .93260366}}};
    for (const auto& ref : readings) {
        for (int form = 0; form < 3; ++form) {
            Drive drive;
            drive.prepare(ref.rate);
            drive.setMode(form == 1 ? Mode::Saturate : Mode::HardClip);
            drive.setPowerSaturation(true);
            drive.setRoundedClip(true);
            const float strength = form == 2 ? .0001f : 0.f;
            for (int n = 0; n <= 1252; ++n) {
                float l = n == 320 ? .73f : n >= 1200 ? 1.21f : 0.f;
                float r = n == 723 ? -.61f : n >= 1200 ? -.84f : 0.f;
                drive.process(l, r, strength);
                if (!strength && n == 336) check(std::abs(l-ref.impulseL) < 2e-6, "linear optional left impulse");
                if (!strength && n == 739) check(std::abs(r-ref.impulseR) < 2e-6, "linear optional right impulse");
                if (n == 1252)
                    check(std::abs(l-(strength == 0.f ? ref.zeroPlateau : ref.roundedPlateau)) < 3e-5,
                          "zero and the first positive rounded setting retain their separate transfers");
            }
        }
    }
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
void historyAcrossWrap() {
    // Leading silence changes the write cursor without changing the filter state.
    // The impulse and moving stereo input must then hit the same output frames.
    for (const double rate : {48000., 96000.}) {
        for (int offset = 1; offset <= 17; ++offset) {
            Drive reference, shifted;
            reference.prepare(rate);
            shifted.prepare(rate);
            for (int i = 0; i < offset; ++i) {
                float l = 0, r = 0;
                shifted.process(l, r, .5f);
            }
            bool equal = true;
            for (int i = 0; i < 129; ++i) {
                float l = i == 0 ? -.875f : static_cast<float>(i % 29 - 14) / 21.f;
                float r = i == 13 ? .625f : static_cast<float>(17 - i % 37) / 27.f;
                float a = l, b = r;
                const float amount = static_cast<float>(i % 17) / 16.f;
                reference.process(l, r, amount);
                shifted.process(a, b, amount);
                equal &= a == l && b == r;
            }
            check(equal, "oversampled history is independent of the position of its write cursor");
        }
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
            check(firstDifference(drive, held, pulse) == 9,
                  "the first rendered change includes the edited rounded subphases one frame after the eight-frame delay");
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
            const int firstFresh = firstDifference(drive, held, pulse);
            check(firstFresh >= 0 && firstFresh <= 1,
                  "re-enabling rounded clipping primes the first fresh strength before the leading FIR taps");
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
    // Two independent channels include quiet signals in the same transfer as loud ones.
    constexpr std::array measured{
        Step{44100, .125f, .013942f, -.68131f, .018392508, -.643977405},
        Step{44100, .5f, .4872f, -.74831f, .339886479, -.403588892},
        Step{48000, .37f, .029381f, -1.23857f, .082463737, -.643232686},
        Step{48000, 1.f, -0x1p-42f, .6737f, -.004716909, .285351358},
        Step{48000, 1.f, .098317f, -0x1p-55f, .216735122, -.001302166},
        Step{96000, .125f, .31731f, -.023168f, .320281083, -.029265670}};
    for (const auto& step : measured) {
        const auto out = powerPlateau(step.rate, step.amount, step.left, step.right);
        check(std::abs(out[0] - step.outLeft) < 5.e-7 && std::abs(out[1] - step.outRight) < 5.e-7,
              "signed transfer follows the measured per-rate response at unequal stereo levels");
    }
    for (const int rate : {48000, 96000}) {
        struct Ratio { float amount; double fourfold; };
        constexpr std::array quiet{
            Ratio{1.f / 256.f, .062515918}, Ratio{1.f / 64.f, .062754957}, Ratio{3.f / 128.f, .063073955}};
        for (const auto& row : quiet) {
            const auto out = powerPlateau(rate, row.amount, -.0625f, 1.f);
            const double exact = std::pow(.0625, 1. / (1. + 6. * row.amount * row.amount));
            const double expected = rate == 48000 ? row.fourfold : exact;
            check(std::abs(-out[0]/out[1] - expected) < 1.e-7,
                  "power saturation follows the measured low-strength ratio at each oversampling factor");
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

    // A very small error in the leading interpolation tap becomes audible at
    // full strength because the exponent is one seventh.
    struct Full { int rate; double early, late; };
    constexpr std::array<Full,3> full{{
        {44100, .06654526, .06623533}, {48000, .06654736, .06626231}, {96000, -.03640781, -.03669132}}};
    for (const auto& ref : full) {
        Drive drive;
        drive.prepare(ref.rate);
        drive.setPowerSaturation(true);
        for (int n = 0; n <= 24; ++n) {
            float l = n == 0 ? .97f : 0.f, r = 0.f;
            drive.process(l, r, 1.f);
            if (n == 8 || n == 24)
                check(std::abs(l-(n == 8 ? ref.early : ref.late)) < .00004 && r == 0.f,
                      "full-strength signed power preserves both measured impulse shoulders");
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
        if (mode != Mode::Saturate)
            check(zeroExact, "selecting power on an unrelated mode preserves its zero-strength response");
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
            check(firstDifference(warm, held, pulse) == 9,
                  "the first rendered change includes the edited power subphases one frame after the eight-frame delay");
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
                const int firstFresh = firstDifference(live, held, pulse);
                check(firstFresh >= 0 && firstFresh <= 1,
                      "entering signed power primes the fresh strength before the leading FIR taps");
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
    historyAcrossWrap();
    roundedSignedSteps();
    roundedImpulses();
    roundedScopeAndSwitching();
    roundedStrengthTiming();
    roundedTrimsAndRecovery();
    powerTransfers();
    powerImpulses();
    powerScopeAndTrims();
    powerStrengthTiming();
    powerResetAndRecovery();
    foldMarkers();
    driveShapeContour();
    optionalShapeAtZero();
    sawWrapAndFoldReset();
    return failures==0 ? 0 : 1;
}
