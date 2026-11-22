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
} // namespace

int main() {
    impulses();
    harmonicTransfers();
    aliases();
    nearZeroContinuity();
    trimsAndReset();
    return failures==0 ? 0 : 1;
}
