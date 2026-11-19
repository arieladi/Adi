// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/adi_shaper.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <numbers>
#include <vector>

namespace {
using Shaper = adi::dsp::AdiShaper;
using Band = Shaper::Band;
using Stage = Shaper::Stage;
static_assert(Shaper::bandParam(Stage::Pan, Band::High, Shaper::Point8) + 1 == Shaper::VolumeLowSlope);
static_assert(Shaper::VolumeLowSlope == 127 && Shaper::VolumeHighSlope == 130 && Shaper::FilterOn == 133);
static_assert(Shaper::LiquidOn == 208 && Shaper::LiquidLowPolarity == 283 && Shaper::Count == 286);
int failures = 0;

void check(bool condition, const char* description) {
    if (!condition) {
        std::printf("FAIL %s\n", description);
        ++failures;
    }
}
bool near(float a, double b, double tolerance = 1.0e-6) {
    return std::abs(static_cast<double>(a) - b) < tolerance;
}
void point(Shaper& shaper, Stage stage, Band band, int index, float value) {
    const auto control = static_cast<Shaper::BandControl>(Shaper::Point1 + index);
    shaper.set(Shaper::bandParam(stage, band, control), value);
}
void flat(Shaper& shaper, Stage stage, Band band, float level) {
    for (int i = 0; i < 8; ++i) point(shaper, stage, band, i, level);
}
std::array<float, 2> one(Shaper& shaper, float l = 0.375f, float r = 0.25f) {
    std::array<float, 2> output{};
    shaper.process(&l, &r, &output[0], &output[1], 1);
    return output;
}
void endpoints() {
    Shaper shaper;
    shaper.prepare(48000);
    const std::array percent{0.0f, 25.0f, 50.0f, 75.0f, 100.0f};
    for (const float p : percent) {
        flat(shaper, Stage::Volume, Band::Mid, p);
        auto sample = one(shaper);
        check(near(sample[0], 0.375 * p / 100.0) && near(sample[1], 0.25 * p / 100.0),
              "volume axis is linear in amplitude");
    }
    for (const float p : percent) {
        flat(shaper, Stage::Width, Band::Mid, 2 * p);
        const auto sample = one(shaper);
        check(near(sample[0], 0.3125 + 0.0625 * p / 50.0)
              && near(sample[1], 0.3125 - 0.0625 * p / 50.0),
              "width leaves mid unchanged and scales side");
    }
    flat(shaper, Stage::Width, Band::Mid, 100);
    const std::array balance{-100.0f, -50.0f, 0.0f, 50.0f, 100.0f};
    const std::array left{0.0, 0.550476, 1.0, 1.30308, 1.414213562};
    const std::array right{1.414213562, 1.30308, 1.0, 0.550476, 0.0};
    for (std::size_t i = 0; i < balance.size(); ++i) {
        flat(shaper, Stage::Pan, Band::Mid, balance[i]);
        const auto sample = one(shaper);
        check(near(sample[0], 0.375 * left[i], 1.0e-5)
              && near(sample[1], 0.25 * right[i], 1.0e-5), "pan graph travels from hard right to hard left");
    }
    flat(shaper, Stage::Pan, Band::Mid, 0);
    flat(shaper, Stage::Volume, Band::Mid, 0);
    shaper.set(Shaper::MasterMix, 50);
    shaper.set(Shaper::bandParam(Stage::Volume, Band::Mid, Shaper::Mix), 50);
    auto sample = one(shaper);
    check(near(sample[0], 0.375 * 0.75) && near(sample[1], 0.25 * 0.75),
          "master times band blend");
    shaper.set(Shaper::MasterMix, 100);
    flat(shaper, Stage::Width, Band::Mid, 0);
    flat(shaper, Stage::Pan, Band::Mid, 100);
    sample = one(shaper);
    check(near(sample[0], 0.5 * 0.3125 * std::numbers::sqrt2) && sample[1] == 0.0f,
          "volume, width and pan process in order");
    check(shaper.latency() == 0, "gain, width and balance add no delay");
}

void balanceSweep() {
    // Alternating positions of a 65-point DC sweep held out of the curve fit.
    constexpr std::array gains{
        0.03650149, 0.10881019, 0.18011904, 0.25033918, 0.31938204, 0.38715830,
        0.45357952, 0.51855654, 0.58200061, 0.64382297, 0.70393461, 0.76224679,
        0.81867051, 0.87311715, 0.92549759, 0.97572309, 1.02370477, 1.06935382,
        1.11258149, 1.15329862, 1.19141662, 1.22684646, 1.25949943, 1.28928649,
        1.31611896, 1.33990800, 1.36056459, 1.37799978, 1.39212501, 1.40285146,
        1.41008985, 1.41375160};
    Shaper shaper;
    double worst = 0;
    for (std::size_t i = 0; i < gains.size(); ++i) {
        const auto fraction = static_cast<float>(2 * i + 1) / 64.0f;
        flat(shaper, Stage::Pan, Band::Mid, 200.0f * fraction - 100.0f);
        const auto out = one(shaper, 1, 1);
        worst = std::max(worst, std::abs(out[0] - gains[i]));
        worst = std::max(worst, std::abs(out[1] - gains[gains.size() - 1 - i]));
    }
    check(worst < 2.0e-7, "balance law reproduces independent fractional heights on both channels");
}

void beatGate() {
    constexpr std::size_t count = 192000;
    std::vector<float> inL(count, 0.375f), inR(count, 0.25f), a(count), b(count), c(count), d(count);
    Shaper large, split;
    for (auto* shaper : {&large, &split}) {
        shaper->prepare(48000);
        for (int i = 0; i < 4; ++i) point(*shaper, Stage::Volume, Band::Mid, i, 0);
    }
    large.process(inL.data(), inR.data(), a.data(), b.data(), count);
    constexpr std::array<std::size_t, 4> blocks{1, 63, 256, 1024};
    for (std::size_t i = 0, k = 0; i < count; ++k) {
        const auto n = std::min(blocks[k % blocks.size()], count - i);
        split.process(inL.data() + i, inR.data() + i, c.data() + i, d.data() + i, n);
        i += n;
    }
    check(a == c && b == d, "block partition preserves exact gate timing");
    bool shape = true;
    for (std::size_t i = 0; i < count; ++i) {
        // The reference transitions within 16 samples of each half-bar; the
        // hold generator changes at the exact boundary to integer precision.
        if (i && i % 48000 <= 16) continue;
        const bool open = (i % 96000) >= 48000;
        shape &= a[i] == (open ? inL[i] : 0.0f) && b[i] == (open ? inR[i] : 0.0f);
    }
    check(shape, "48 kHz, 120 BPM: full-scale half gate has a 96000-sample period");
    split.sync(3.0, false);
    split.process(inL.data(), inR.data(), c.data(), d.data(), count);
    check(c == inL && d == inR, "stopped beat holds its phase over many blocks");
    split.sync(-3.0, false);
    check(one(split)[0] == 0.0f, "backwards seek and negative preroll wrap the beat phase");
    split.sync(0.0, true);
    split.tempo(240);
    split.process(inL.data(), inR.data(), c.data(), d.data(), count);
    check(c[23998] == 0.0f && c[24002] == inL[24002] && c[48002] == 0.0f,
          "tempo doubles the gate rate on resume");
}

void clocksAndLinearCurve() {
    Shaper shaper;
    shaper.prepare(48000);
    flat(shaper, Stage::Volume, Band::Mid, 0);
    point(shaper, Stage::Volume, Band::Mid, 1, 100);
    shaper.set(Shaper::bandParam(Stage::Volume, Band::Mid, Shaper::Curve), 1);
    shaper.sync(0.25, false);
    check(near(one(shaper, 1, 1)[0], 0.5), "linear interpolation halfway between two points");
    shaper.sync(0.5, false);
    check(one(shaper, 1, 1)[0] == 1.0f, "curve passes through each point");
    shaper.sync(0.75, false);
    check(near(one(shaper, 1, 1)[0], 0.5), "linear descending segment");
    point(shaper, Stage::Volume, Band::Mid, 1, 0);
    point(shaper, Stage::Volume, Band::Mid, 7, 100);
    shaper.sync(3.75, false);
    check(near(one(shaper, 1, 1)[0], 0.5), "linear last-to-first wrap");
    shaper.sync(0.0, false);
    shaper.set(Shaper::bandParam(Stage::Volume, Band::Mid, Shaper::Clock), 1);
    shaper.set(Shaper::bandParam(Stage::Volume, Band::Mid, Shaper::Rate), 2);
    std::vector<float> in(18000, 1.0f), l(in.size()), r(in.size());
    shaper.process(in.data(), in.data(), l.data(), r.data(), in.size());
    shaper.sync(100.0, false);
    shaper.process(in.data(), in.data(), l.data(), r.data(), in.size());
    check(near(l[3000], 1.0, 1.0e-5) && near(l[4500], 0.5, 1.0e-5),
          "free clock advances while stopped and survives transport seeks");
}

void splitBands() {
    Shaper shaper;
    shaper.prepare(48000);
    shaper.set(Shaper::VolumeBands, 1);
    shaper.set(static_cast<Shaper::Param>(Shaper::VolumeBands + 1), 1000);
    flat(shaper, Stage::Volume, Band::Mid, 0);
    std::vector<float> in(48000), l(in.size()), r(in.size());
    double powers[3]{};
    constexpr std::array hz{100.0, 1000.0, 10000.0};
    for (std::size_t k = 0; k < hz.size(); ++k) {
        for (std::size_t i = 0; i < in.size(); ++i)
            in[i] = static_cast<float>(std::sin(2.0 * std::numbers::pi * hz[k]
                                               * static_cast<double>(i) / 48000.0));
        shaper.process(in.data(), in.data(), l.data(), r.data(), in.size());
        for (std::size_t i = 24000; i < in.size(); ++i)
            powers[k] += static_cast<double>(l[i]) * l[i];
        powers[k] /= 12000.0;
    }
    check(powers[0] > 0.98 && std::abs(powers[1] - 0.5) < 0.001 && powers[2] < 0.011,
          "first-order low band is -3 dB at the crossover");
    shaper.set(Shaper::VolumeBands, 3);
    shaper.set(static_cast<Shaper::Param>(Shaper::VolumeBands + 2), 5000);
    flat(shaper, Stage::Volume, Band::Mid, 50);
    flat(shaper, Stage::Volume, Band::Low, 50);
    flat(shaper, Stage::Volume, Band::High, 50);
    for (std::size_t i = 0; i < in.size(); ++i)
        in[i] = static_cast<float>(std::sin(static_cast<double>(i) * 0.237));
    shaper.process(in.data(), in.data(), l.data(), r.data(), in.size());
    bool recombines = true;
    for (std::size_t i = 0; i < in.size(); ++i) recombines &= near(l[i], 0.5 * in[i]);
    check(recombines, "three independently scaled bands reconstruct the source at uniform gain");
    flat(shaper, Stage::Volume, Band::Low, 0);
    flat(shaper, Stage::Volume, Band::High, 100);
    std::fill(in.begin(), in.end(), 1.0f);
    shaper.process(in.data(), in.data(), l.data(), r.data(), in.size());
    check(std::abs(l.back()) < 1.0e-6f, "low-band curve can mute DC with the other bands active");
    shaper.set(Shaper::VolumeBands, 2);
    shaper.process(in.data(), in.data(), l.data(), r.data(), in.size());
    check(near(l.back(), 0.5), "high-only split gives the lower remainder to the middle band");
}

void independentBandClocks() {
    Shaper shaper;
    shaper.prepare(48000);
    shaper.set(Shaper::VolumeBands, 3);
    flat(shaper, Stage::Volume, Band::Mid, 0);
    for (int i = 0; i < 4; ++i) {
        point(shaper, Stage::Volume, Band::Low, i, 0);
        point(shaper, Stage::Volume, Band::High, i, 0);
    }
    shaper.set(Shaper::bandParam(Stage::Volume, Band::High, Shaper::Clock), 1);
    shaper.set(Shaper::bandParam(Stage::Volume, Band::High, Shaper::Rate), 2);
    // DC occupies the left low band. Nyquist occupies the right high band.
    // The 2 Hz high-band gate opens four times per low band's beat-locked bar.
    constexpr std::size_t count = 72000;
    std::vector<float> inL(count, 0.375f), inR(count), outL(count), outR(count);
    for (std::size_t i = 0; i < count; ++i) inR[i] = (i % 2 == 0) ? 0.25f : -0.25f;
    shaper.process(inL.data(), inR.data(), outL.data(), outR.data(), count);
    bool independent = true;
    for (const std::size_t frame : std::array<std::size_t, 4>{6000, 18000, 54000, 66000}) {
        const auto low = frame >= 48000 ? inL[frame] : 0.0f;
        const auto high = (frame % 24000) >= 12000 ? inR[frame] : 0.0f;
        independent &= near(outL[frame], low) && near(outR[frame], high);
    }
    check(independent, "low and high clocks, independent of middle, reach all four gate combinations");
}

using Complex = std::complex<double>;
struct Response { Complex low, high; };
Response crossover(int slope, double cutoff, double f) {
    const Complex s(0.0, std::tan(std::numbers::pi * f / 48000.0)
                         / std::tan(std::numbers::pi * cutoff / 48000.0));
    if (slope == 0) return {1.0 / (1.0 + s), s / (1.0 + s)};
    if (slope == 1) {
        const auto denominator = (1.0 + s) * (1.0 + s);
        const auto all = (1.0 - s) / (1.0 + s);
        return {all / denominator, -all * s * s / denominator};
    }
    const auto d = 1.0 + std::numbers::sqrt2 * s + s * s;
    return {1.0 / (d * d), s * s * s * s / (d * d)};
}

void steepCrossovers() {
    constexpr float lowerHz = 659.2551f, upperHz = 3729.3101f;
    std::array<float, 2048> impulse{}, l{}, r{};
    impulse[0] = 1;
    for (int lower = 0; lower <= 2; ++lower) {
        for (int upper = 0; upper <= 2; ++upper) {
            for (int solo = 0; solo <= 3; ++solo) {
                Shaper shaper;
                shaper.set(Shaper::VolumeBands, 3);
                shaper.set(static_cast<Shaper::Param>(Shaper::VolumeBands + 1), lowerHz);
                shaper.set(static_cast<Shaper::Param>(Shaper::VolumeBands + 2), upperHz);
                shaper.set(Shaper::VolumeLowSlope, static_cast<float>(lower));
                shaper.set(Shaper::VolumeHighSlope, static_cast<float>(upper));
                for (int band = 0; band < 3; ++band)
                    flat(shaper, Stage::Volume, static_cast<Band>(band), solo == 3 || solo == band ? 100.0f : 0.0f);
                shaper.process(impulse.data(), impulse.data(), l.data(), r.data(), l.size());
                for (const double f : {160.0, 658.0, 1200.0, 3729.0, 5000.0}) {
                    const auto a = crossover(lower, lowerHz, f), b = crossover(upper, upperHz, f);
                    const std::array expected{a.low * (b.low + b.high), a.high * b.low,
                                              a.high * b.high, (a.low + a.high) * (b.low + b.high)};
                    const auto rotation = std::polar(1.0, -2.0 * std::numbers::pi * f / 48000.0);
                    Complex phase{1, 0}, actual{};
                    for (const auto x : l) {
                        actual += static_cast<double>(x) * phase;
                        phase *= rotation;
                    }
                    check(std::abs(actual - expected[static_cast<std::size_t>(solo)]) < 1.0e-6,
                          "each branch retains its measured crossover magnitude and phase");
                    if (solo == 3) check(std::abs(std::abs(actual) - 1.0) < 1.0e-6,
                                         "unity band gains sum to the shared allpass");
                }
            }
        }
    }
    // Lower split only, 0.5 impulse. First samples are especially sensitive
    // to the extra 12 dB/octave allpass and the 24 dB/octave section order.
    constexpr std::array firstTaps{0.020694008, -0.0007855875, 0.000001538};
    for (int slope = 0; slope <= 2; ++slope) {
        Shaper shaper;
        shaper.set(Shaper::VolumeBands, 1);
        shaper.set(static_cast<Shaper::Param>(Shaper::VolumeBands + 1), lowerHz);
        shaper.set(Shaper::VolumeLowSlope, static_cast<float>(slope));
        flat(shaper, Stage::Volume, Band::Mid, 0);
        check(near(one(shaper, 0.5f, 0.5f)[0], firstTaps[static_cast<std::size_t>(slope)], 2.0e-8),
              "isolated low branch starts at the measured impulse coefficient");
    }
}

void bypassAndInvalid() {
    Shaper shaper;
    const float minusZero = -0.0f;
    auto sample = one(shaper, minusZero, 0.3f);
    check(std::bit_cast<std::uint32_t>(sample[0]) == std::bit_cast<std::uint32_t>(minusZero)
          && sample[1] == 0.3f, "neutral rack preserves input bits");
    shaper.set(Shaper::bandParam(Stage::Width, Band::Mid, Shaper::Point3),
               std::numeric_limits<float>::quiet_NaN());
    shaper.set(Shaper::Count, 20);
    shaper.sync(std::numeric_limits<double>::infinity());
    shaper.tempo(0);
    shaper.prepare(-1);
    sample = one(shaper);
    check(sample[0] == 0.375f && sample[1] == 0.25f, "invalid messages leave the current settings intact");
    for (auto stage : {Shaper::VolumeBands, Shaper::WidthBands, Shaper::PanBands}) shaper.set(stage, 3);
    flat(shaper, Stage::Width, Band::Mid, 0);
    shaper.set(Shaper::MasterMix, 0);
    float left = minusZero, right = 0.01f;
    shaper.process(&left, &right, &right, &left, 1);
    check(std::bit_cast<std::uint32_t>(right) == std::bit_cast<std::uint32_t>(minusZero)
          && left == 0.01f, "zero master mix is bit transparent across live split stages");
}

void clockLimitsAndAliases() {
    constexpr std::size_t count = 100003;
    std::vector<float> inL(count, 1.0f), inR(count, -0.25f), outL(count), outR(count);
    for (const float hz : {0.02f, 5240.0f}) {
        Shaper shaper;
        shaper.set(Shaper::bandParam(Stage::Volume, Band::Mid, Shaper::Clock), 1);
        shaper.set(Shaper::bandParam(Stage::Volume, Band::Mid, Shaper::Rate), hz);
        shaper.set(Shaper::bandParam(Stage::Volume, Band::Mid, Shaper::Curve), 1);
        for (int i = 0; i < 8; ++i) point(shaper, Stage::Volume, Band::Mid, i, static_cast<float>(i) * 12.5f);
        for (std::size_t i = 0; i < count;) {
            const auto n = std::min<std::size_t>(127, count - i);
            shaper.process(inL.data() + i, inR.data() + i, outL.data() + i, outR.data() + i, n);
            i += n;
        }
        bool accurate = true;
        for (std::size_t i = 0; i < count; i += 23) {
            const double cycles = static_cast<double>(i) * hz / 48000.0;
            const double phase = cycles - std::floor(cycles);
            const double expected = phase < 0.875 ? phase : (1.0 - phase) * 7.0;
            accurate &= near(outL[i], expected, 1.0e-6) && near(outR[i], -0.25 * expected);
        }
        check(accurate, "free-clock limits follow absolute sample phase over uneven block partitions");
    }
    Shaper normal;
    normal.set(Shaper::WidthBands, 3);
    normal.set(Shaper::WidthLowSlope, 1);
    normal.set(Shaper::WidthHighSlope, 2);
    flat(normal, Stage::Width, Band::Mid, 180);
    flat(normal, Stage::Pan, Band::Mid, 70);
    Shaper swapped = normal;
    for (std::size_t i = 0; i < count; ++i) {
        inL[i] = static_cast<float>(std::sin(0.1 * static_cast<double>(i)));
        inR[i] = static_cast<float>(std::sin(0.013 * static_cast<double>(i)));
    }
    normal.process(inL.data(), inR.data(), outL.data(), outR.data(), count);
    swapped.process(inL.data(), inR.data(), inR.data(), inL.data(), count);
    check(inR == outL && inL == outR, "crossed input and output planes survive split and in-place stages");
}

void modulationStack() {
    Shaper rack;
    rack.prepare(48000);
    rack.set(Shaper::FilterOn, 1);
    rack.set(Shaper::LiquidOn, 1);
    rack.set(Shaper::MasterMix, 50);
    rack.set(Shaper::modParam(Stage::Filter, Band::Mid, Shaper::ModMix), 80);
    rack.set(Shaper::modParam(Stage::Liquid, Band::Mid, Shaper::ModMix), 60);
    rack.set(Shaper::LiquidMidPolarity, 1);
    // Cutoff interpolates while resonance holds. The Liquid stage does the reverse.
    rack.set(Shaper::modParam(Stage::Filter, Band::Mid, Shaper::PrimaryCurve), 1);
    rack.set(Shaper::modParam(Stage::Liquid, Band::Mid, Shaper::SecondaryCurve), 1);
    constexpr std::array<float, 8> cut{30, 70, 80, 50, 65, 70, 35, 40};
    constexpr std::array<float, 8> res{20, 65, 50, 15, 40, 25, 10, 45};
    constexpr std::array<float, 8> center{40, 60, 80, 90, 55, 30, 50, 75};
    constexpr std::array<float, 8> feedback{10, 25, 60, 30, 40, 20, 65, 50};
    for (int i = 0; i < 8; ++i) {
        auto first = static_cast<Shaper::ModControl>(Shaper::Primary1 + i);
        auto second = static_cast<Shaper::ModControl>(Shaper::Secondary1 + i);
        const auto j = static_cast<std::size_t>(i);
        rack.set(Shaper::modParam(Stage::Filter, Band::Mid, first), cut[j]);
        rack.set(Shaper::modParam(Stage::Filter, Band::Mid, second), res[j]);
        rack.set(Shaper::modParam(Stage::Liquid, Band::Mid, first), center[j]);
        rack.set(Shaper::modParam(Stage::Liquid, Band::Mid, second), feedback[j]);
    }
    adi::dsp::AdiShaperFilter filter;
    adi::dsp::AdiShaperLiquid liquid;
    filter.prepare(48000);
    filter.setMode(adi::dsp::AdiShaperFilter::Mode::LowPass6);
    (void)liquid.prepare(48000);
    liquid.setMode(adi::dsp::AdiShaperLiquid::Mode::Flange);
    liquid.setPolarity(true);
    double maxError = 0.0, effectPeak = 0.0;
    for (std::size_t step = 0; step < 8; ++step) {
        const auto next = (step + 1) % 8;
        const float cutoff = cut[step] * .01f + .5f * (cut[next] * .01f - cut[step] * .01f);
        const float fb = feedback[step] * .01f + .5f * (feedback[next] * .01f - feedback[step] * .01f);
        rack.sync(0.5 * static_cast<double>(step) + 0.25, false);
        for (int frame = 0; frame < 301; ++frame) {
            const float a = static_cast<float>(std::sin(0.034 * frame));
            const float b = static_cast<float>(std::cos(0.079 * frame) * 0.25);
            const auto out = one(rack, a, b);
            float l = a, r = b;
            filter.process(l, r, cutoff, res[step] * .01f);
            const float dryL = a + (0.5f * (80.f * .01f)) * (l - a);
            const float dryR = b + (0.5f * (80.f * .01f)) * (r - b);
            l = dryL;
            r = dryR;
            liquid.processStereo(l, r, center[step] * .01f, fb);
            l = dryL + (0.5f * (60.f * .01f)) * (l - dryL);
            r = dryR + (0.5f * (60.f * .01f)) * (r - dryR);
            maxError = std::max({maxError, static_cast<double>(std::abs(out[0] - l)),
                                 static_cast<double>(std::abs(out[1] - r))});
            effectPeak = std::max(effectPeak, static_cast<double>(std::abs(l - a)));
        }
    }
    check(maxError < 2.0e-6 && effectPeak > 0.1, "two independent contours feed each serial kernel at master times band blend");
    rack.set(Shaper::FilterOn, 0);
    rack.set(Shaper::LiquidOn, 0);
    rack.set(static_cast<Shaper::Param>(Shaper::LiquidOn + 1), 3);
    rack.set(static_cast<Shaper::Param>(Shaper::LiquidOn + 4), 2);
    const auto bypass = one(rack);
    check(bypass[0] == 0.375f && bypass[1] == 0.25f, "switching stages off bypasses their crossover and tail");
}

void modulationBands() {
    for (const auto stage : {Stage::Filter, Stage::Liquid}) {
        // First three copies process one wet band each. The fourth runs all
        // three. At 6 dB/octave the shared dry transfer is exactly unity.
        std::array<Shaper, 4> racks;
        const auto on = stage == Stage::Filter ? Shaper::FilterOn : Shaper::LiquidOn;
        for (std::size_t slot = 0; slot < racks.size(); ++slot) {
            auto& rack = racks[slot];
            rack.prepare(48000);
            rack.set(on, 1);
            rack.set(static_cast<Shaper::Param>(on + 1), 3);
            for (int b = 0; b < 3; ++b) {
                const auto band = static_cast<Band>(b);
                const auto type = stage == Stage::Filter ? std::array{0, 3, 4} : std::array{1, 2, 5};
                rack.set(Shaper::modParam(stage, band, Shaper::Type), static_cast<float>(type[static_cast<std::size_t>(b)]));
                const float blend = slot == 3 || slot == static_cast<std::size_t>(b) ? 100.0f : 0.0f;
                rack.set(Shaper::modParam(stage, band, Shaper::ModMix), blend);
                for (int p = 0; p < 8; ++p) {
                    const auto a = static_cast<Shaper::ModControl>(Shaper::Primary1 + p);
                    const auto s = static_cast<Shaper::ModControl>(Shaper::Secondary1 + p);
                    rack.set(Shaper::modParam(stage, band, a), static_cast<float>(50 * b));
                    rack.set(Shaper::modParam(stage, band, s), static_cast<float>(100 - 50 * b));
                }
            }
        }
        double maxError = 0.0;
        std::array<double, 3> bandEffect{};
        bool finite = true;
        for (int i = 0; i < 8192; ++i) {
            const float l = static_cast<float>(.25 * std::sin(i * .0065) + .28 * std::sin(i * .095) + .37 * std::sin(i * .94));
            const float r = -.83f * l;
            std::array<float, 2> total{l, r};
            for (std::size_t b = 0; b < 3; ++b) {
                const auto solo = one(racks[b], l, r);
                finite &= std::isfinite(solo[0]) && std::isfinite(solo[1]);
                total[0] += solo[0] - l;
                total[1] += solo[1] - r;
                bandEffect[b] = std::max(bandEffect[b], static_cast<double>(std::abs(solo[0] - l)));
            }
            const auto wet = one(racks[3], l, r);
            finite &= std::isfinite(wet[0]) && std::isfinite(wet[1]);
            maxError = std::max({maxError, static_cast<double>(std::abs(total[0] - wet[0])),
                                 static_cast<double>(std::abs(total[1] - wet[1]))});
        }
        check(finite && maxError < 2.0e-6 && *std::min_element(bandEffect.begin(), bandEffect.end()) > 0.01,
              "three modulation bands retain independent stereo histories, modes, curves and wet amounts at extrema");
    }
}
} // namespace

int main() {
    endpoints();
    balanceSweep();
    beatGate();
    clocksAndLinearCurve();
    splitBands();
    independentBandClocks();
    steepCrossovers();
    bypassAndInvalid();
    clockLimitsAndAliases();
    modulationStack();
    modulationBands();
    if (failures == 0) std::puts("ADi-SHAPER DSP fixtures passed");
    return failures == 0 ? 0 : 1;
}
