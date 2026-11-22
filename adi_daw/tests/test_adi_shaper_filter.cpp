// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/adi_shaper_filter.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdio>
#include <limits>
#include <numbers>

namespace {
using Filter = adi::dsp::AdiShaperFilter;
using Mode = Filter::Mode;
int failures = 0;

void check(bool passed, const char* message) {
    if (!passed) { std::printf("FAIL %s\n", message); ++failures; }
}

void bandwidth() {
    // Transform the unit impulse at two test frequencies. The reference
    // readings came from an independent stereo render at 48 kHz.
    constexpr std::array cutoffs{0.0f, 0.5f, 1.0f};
    constexpr std::array<double, 3> at400{0.051429, 0.854891, 0.999930};
    constexpr std::array<double, 3> at8000{0.002431, 0.077676, 0.974473};
    for (std::size_t i = 0; i < cutoffs.size(); ++i) {
        Filter filter;
        std::complex<double> a{}, b{};
        for (int n = 0; n < 16000; ++n) {
            float l = n == 0 ? 0.5f : 0.0f, r = 0.0f;
            filter.process(l, r, cutoffs[i], 0.0f);
            const double phase = -2.0 * std::numbers::pi * n / 48000.0;
            a += 2.0 * l * std::polar(1.0, 400.0 * phase);
            b += 2.0 * l * std::polar(1.0, 8000.0 * phase);
        }
        std::printf("Cutoff %.2f, gains at 400 Hz and 8 kHz %.7f %.7f\n",
                    static_cast<double>(cutoffs[i]), std::abs(a), std::abs(b));
        check(std::abs(std::abs(a) - at400[i]) < 0.00003, "400 Hz gain at the three cutoff anchors");
        check(std::abs(std::abs(b) - at8000[i]) < 0.00003, "8 kHz gain at the three cutoff anchors");
    }
}

void heldOutResonantImpulses() {
    struct Response { Mode mode; float cutoff, resonance; std::array<double,5> taps; };
    constexpr std::array<int,5> indices{0,1,4,37,137};
    constexpr std::array cases{
        Response{Mode::LowPass6, .363f,.72f,{.02510610,.03364079,.03413621,.02617717,-.01417630}},
        Response{Mode::LowPass12,.363f,.72f,{.00034746,.00138600,.00465027,.02780101,-.02041005}},
        Response{Mode::HighPass6,.363f,.72f,{.97489393,-.03364079,-.03413621,-.02617717,.01417630}},
        Response{Mode::HighPass12,.363f,.72f,{.99626356,-.00813031,-.01119727,-.02920465,.02065383}},
        Response{Mode::BandPass12,.363f,.72f,{.02496335,.03306190,.03206702,.00665126,-.00102514}},
        Response{Mode::LowPass6,.86f,.73f,{.79847366,.75420451,-.48189661,.01831382,-.00000047}},
        Response{Mode::LowPass12,.86f,.73f,{.29587865,.82056397,-.60155189,.01940724,-.00000082}},
        Response{Mode::HighPass6,.86f,.73f,{.20152636,-.75420451,.48189661,-.01831382,.00000047}},
        Response{Mode::HighPass12,.86f,.73f,{.73190087,-.79673201,.58342648,-.01857230,.00000082}},
        Response{Mode::BandPass12,.86f,.73f,{.62964565,.15893301,-.12690689,.00806174,.00000012}}};
    double worst = 0;
    for (const auto& test : cases) {
        Filter filter;
        filter.setMode(test.mode);
        std::size_t point = 0;
        for (int n = 0; n <= indices.back(); ++n) {
            constexpr float amplitude = 0.025f;
            float l = n == 0 ? amplitude : 0.0f, r = -0.25f * l;
            filter.process(l, r, test.cutoff, test.resonance);
            if (n == indices[point]) {
                worst = std::max(worst, std::abs(l / amplitude - test.taps[point]));
                worst = std::max(worst, std::abs(r / amplitude + 0.25 * test.taps[point]));
                ++point;
            }
        }
    }
    std::printf("Held-out filter impulse maximum error %.9g\n", worst);
    check(worst < 5e-5, "five small-signal filter responses agree within 0.005% of a unit impulse");
}

void peakImpulseResponse() {
    struct Reading {
        double rate;
        float cutoff, resonance;
        std::array<double, 6> taps;
    };
    constexpr std::array frames{0, 1, 3, 11, 63, 127};
    constexpr std::array readings{
        Reading{44100., .5f, 0.f, {1.06520438, .07391887, .04823056, -.00182336, -.00121960, -.00000661}},
        Reading{48000., .845f, .91f, {1.61642444, .30349815, -.80005819, -.22326441, -.12978590, .01543924}},
        Reading{96000., 1.f, 1.f, {1.80734241, -.18637720, -.26273555, -1.13907290, .79100227, .77836698}}
    };
    constexpr float impulse = 1.f / 256.f;
    double worst = 0.;
    for (const auto& reading : readings) {
        Filter filter;
        filter.prepare(reading.rate);
        filter.setMode(Mode::Peak12);
        std::size_t tap = 0;
        for (int i = 0; i <= frames.back(); ++i) {
            float left = i == 0 ? impulse : 0.f;
            float right = -left;
            filter.process(left, right, reading.cutoff, reading.resonance);
            if (i != frames[tap]) continue;
            worst = std::max({worst, std::abs(left / impulse - reading.taps[tap]),
                              std::abs(right / impulse + reading.taps[tap])});
            ++tap;
        }
    }
    std::printf("Signed peak impulse maximum error %.9g\n", worst);
    check(worst < 5e-5, "peak impulses match the measured response at three sample rates");
}

void movingPeakResponse() {
    Filter peak, pass;
    peak.setMode(Mode::Peak12);
    pass.setMode(Mode::BandPass12);
    bool exact = true;
    for (int frame = 0; frame < 24000; ++frame) {
        const float cutoff = static_cast<float>(std::abs(frame % 4096 - 2048)) / 2048.f;
        const float resonance = static_cast<float>(frame % 1021) / 1020.f;
        const float dryL = static_cast<float>(frame % 173 - 86) / 128.f;
        const float dryR = static_cast<float>(61 - frame % 127) / 97.f;
        float left = dryL, right = dryR, bandL = dryL, bandR = dryR;
        peak.process(left, right, cutoff, resonance);
        pass.process(bandL, bandR, cutoff, resonance);
        exact = exact && left == dryL + bandL && right == dryR + bandR;
    }
    check(exact, "moving peak adds each input sample to its own band-pass output");
}

void sampleRatesAndReset() {
    constexpr std::array rates{32000.,44100.,96000.};
    constexpr std::array first{1.43091667,1.30600560,.47358054};
    constexpr std::array second{-.68922192,-.48839962,1.00348473};
    Filter filter;
    filter.setMode(Mode::LowPass12);
    for (std::size_t i = 0; i < rates.size(); ++i) {
        filter.prepare(rates[i]);
        constexpr float excitation = 0.025f;
        float l = excitation, r = 0.0f;
        filter.process(l,r,1.0f,.7f);
        check(std::abs(l/excitation-first[i]) < 5e-5 && r == 0.0f, "rate-specific upper corner and stereo isolation");
        l = 0.0f;
        filter.process(l,r,1.0f,.7f);
        check(std::abs(l/excitation-second[i]) < 5e-5, "upper corner impulse phase at a different sample rate");
        filter.reset();
        l = 0.0f;
        filter.process(l,r,std::numeric_limits<float>::quiet_NaN(),-2.0f);
        check(l == 0.0f && r == 0.0f, "reset clears feedback and malformed control leaves silence");
    }
}

void resonantLevel() {
    // A half-scale impulse at maximum resonance distinguishes the measured
    // level-dependent damping from a linear high-Q filter.
    constexpr std::array indices{0,1,4,9,31,64};
    constexpr std::array expected{.15795082,.46266505,-.44627717,-.11943652,.34469840,-.27456656};
    Filter filter;
    filter.setMode(Mode::LowPass12);
    double worst = 0;
    std::size_t i = 0;
    for (int n = 0; n <= indices.back(); ++n) {
        float l = n == 0 ? .5f : 0.0f;
        float r = -l;
        filter.process(l, r, .86f, 1.0f);
        if (n == indices[i]) {
            worst = std::max(worst, std::abs(l - expected[i]));
            worst = std::max(worst, std::abs(r + expected[i]));
            ++i;
        }
    }
    std::printf("Full-resonance level test maximum error %.9g\n", worst);
    check(worst < 0.0002, "half-scale impulse excites the measured damped resonance");
}

void movingContours() {
    // Three seconds at 120 BPM. A stereo saw input, an ascending/descending
    // cutoff and an opposing resonance contour exercise both filter states.
    constexpr std::array<int,9> frames{12000,24000,36000,60000,72000,84000,108000,120000,132000};
    constexpr std::array modes{Mode::LowPass6, Mode::LowPass12, Mode::HighPass6,
                               Mode::HighPass12, Mode::BandPass12};
    constexpr std::array<std::array<double,9>,5> reference{{
        {.01045448,-.03583412,-.01326559,-.00218493,.01204461,-.00835055,.00309323,.02020766,.00097753},
        {.00700892,-.02225187,-.01347150,-.00296334,.00857418,-.00663886,-.00586140,.01602494,.01985892},
        {-.03193886,.01923256,.00154684,.00023180,-.00911492,.01616305,.01448489,.00225328,-.02685644},
        {-.03116858,.01780385,.00171912,.00008270,-.01088717,.01624106,.02085670,.00379041,-.03393587},
        {.00755479,-.02419932,.00018038,.00141240,.01036288,-.00509346,.00771029,.00545873,-.02044512}
    }};
    double worst = 0;
    for (std::size_t m = 0; m < modes.size(); ++m) {
        Filter filter;
        filter.setMode(modes[m]);
        std::size_t index = 0;
        for (int n = 0; n <= frames.back(); ++n) {
            const int cycle = n % 96000;
            const auto triangle = static_cast<float>(std::min(cycle, 96000 - cycle)) / 48000.0f;
            float l = static_cast<float>((n % 109) - 54) / 2048.0f;
            float r = static_cast<float>((n % 151) - 75) / 2048.0f;
            filter.process(l, r, .2f + .64f * triangle, .72f - .62f * triangle);
            if (n == frames[index]) {
                worst = std::max(worst, std::abs(l - reference[m][index]));
                ++index;
            }
        }
    }
    std::printf("Moving filter contour maximum error %.9g\n", worst);
    check(worst < 0.0003, "all five modes track independently modulated cutoff and resonance");
}
} // namespace

int main() {
    bandwidth();
    heldOutResonantImpulses();
    peakImpulseResponse();
    movingPeakResponse();
    sampleRatesAndReset();
    resonantLevel();
    movingContours();
    return failures == 0 ? 0 : 1;
}
