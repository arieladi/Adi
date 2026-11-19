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
static_assert(Shaper::LiquidOn == 208 && Shaper::LiquidLowPolarity == 283 && Shaper::DriveOn == 286);
static_assert(Shaper::CrushOn == 340 && Shaper::TimeOn == 415 && Shaper::PitchOn == 463);
static_assert(Shaper::NoiseOn == 535 && Shaper::ReverbOn == 625 && Shaper::VolumeOrder == 715 && Shaper::Count == 726);
static_assert(Shaper::timeParam(Band::High, Shaper::Time8) + 1 == Shaper::PitchOn);
static_assert(Shaper::modParam(Stage::Pitch, Band::High, Shaper::Secondary8) + 1 == Shaper::NoiseOn);
static_assert(Shaper::noiseParam(Band::High, Shaper::NoiseLowPass) + 1 == Shaper::ReverbOn);
static_assert(Shaper::reverbParam(Band::High, Shaper::RoomClearTails) + 1 == Shaper::VolumeOrder);
static_assert(Shaper::orderParam(Stage::Reverb) + 1 == Shaper::Count);
static_assert(Shaper::driveParam(Band::High, Shaper::OutputTrim) + 1 == Shaper::CrushOn);
static_assert(Shaper::modParam(Stage::Crush, Band::High, Shaper::Secondary8) + 1 == Shaper::TimeOn);
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
    Shaper normal, swapped;
    for (auto* rack : {&normal, &swapped}) {
        rack->set(Shaper::WidthBands, 3);
        rack->set(Shaper::WidthLowSlope, 1);
        rack->set(Shaper::WidthHighSlope, 2);
        flat(*rack, Stage::Width, Band::Mid, 180);
        flat(*rack, Stage::Pan, Band::Mid, 70);
    }
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
    for (const auto stage : {Stage::Filter, Stage::Liquid, Stage::Drive, Stage::Crush}) {
        // First three copies process one wet band each. The fourth runs all
        // three. At 6 dB/octave the shared dry transfer is exactly unity.
        std::array<Shaper, 4> racks;
        const auto on = stage == Stage::Filter ? Shaper::FilterOn : stage == Stage::Liquid ? Shaper::LiquidOn
                      : stage == Stage::Drive ? Shaper::DriveOn : Shaper::CrushOn;
        for (std::size_t slot = 0; slot < racks.size(); ++slot) {
            auto& rack = racks[slot];
            rack.prepare(48000);
            rack.set(on, 1);
            rack.set(static_cast<Shaper::Param>(on + 1), 3);
            for (int b = 0; b < 3; ++b) {
                const auto band = static_cast<Band>(b);
                const float blend = slot == 3 || slot == static_cast<std::size_t>(b) ? 100.0f : 0.0f;
                if (stage == Stage::Drive) {
                    rack.set(Shaper::driveParam(band, Shaper::DriveStyle), static_cast<float>(b));
                    rack.set(Shaper::driveParam(band, Shaper::DriveMix), blend);
                    for (int p = 0; p < 8; ++p)
                        rack.set(Shaper::driveParam(band, static_cast<Shaper::DriveControl>(Shaper::Drive1 + p)),
                                 static_cast<float>(25 + 35 * b));
                    continue;
                }
                const auto type = stage == Stage::Filter ? std::array{0, 3, 4}
                                : stage == Stage::Liquid ? std::array{1, 2, 5} : std::array{0, 0, 0};
                rack.set(Shaper::modParam(stage, band, Shaper::Type), static_cast<float>(type[static_cast<std::size_t>(b)]));
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
        std::array<float, 16> delayed{};
        bool finite = true;
        for (int i = 0; i < 8192; ++i) {
            const float l = static_cast<float>(.25 * std::sin(i * .0065) + .28 * std::sin(i * .095) + .37 * std::sin(i * .94));
            const float r = -.83f * l;
            const auto pos = static_cast<std::size_t>(i) % delayed.size();
            const float dryL = stage == Stage::Drive ? delayed[pos] : l;
            const float dryR = -.83f * dryL;
            delayed[pos] = l;
            std::array<float, 2> total{dryL, dryR};
            for (std::size_t b = 0; b < 3; ++b) {
                const auto solo = one(racks[b], l, r);
                finite &= std::isfinite(solo[0]) && std::isfinite(solo[1]);
                total[0] += solo[0] - dryL;
                total[1] += solo[1] - dryR;
                bandEffect[b] = std::max(bandEffect[b], static_cast<double>(std::abs(solo[0] - dryL)));
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

void driveDryAndLatency() {
    constexpr std::size_t frames = 1021;
    std::array<float, frames> inL{}, inR{}, l{}, r{};
    for (std::size_t i = 0; i < frames; ++i) {
        inL[i] = static_cast<float>(std::sin(.33 * static_cast<double>(i)));
        inR[i] = static_cast<float>(.7 * std::cos(.17 * static_cast<double>(i)));
    }
    inL[3] = -0.0f;
    auto render = [&](Shaper& rack) {
        for (std::size_t offset = 0; offset < frames;) {
            const auto count = std::min<std::size_t>(19, frames - offset);
            rack.process(inL.data() + offset, inR.data() + offset, l.data() + offset,
                         r.data() + offset, count);
            offset += count;
        }
    };
    for (int layout = 0; layout < 4; ++layout) {
        for (const bool masterBypass : {false, true}) {
            Shaper rack;
            rack.set(Shaper::DriveOn, 1);
            rack.set(static_cast<Shaper::Param>(Shaper::DriveOn + 1), static_cast<float>(layout));
            if (masterBypass) rack.set(Shaper::MasterMix, 0);
            else for (auto band : {Band::Low, Band::Mid, Band::High}) rack.set(Shaper::driveParam(band, Shaper::DriveMix), 0);
            check(rack.latency() == 16, "dry and wet bands share the reported drive latency");
            render(rack);
            bool exact = true;
            for (std::size_t i = 0; i < frames; ++i) {
                const float dryL = i < 16 ? 0.0f : inL[i - 16];
                const float dryR = i < 16 ? 0.0f : inR[i - 16];
                exact &= std::bit_cast<std::uint32_t>(l[i]) == std::bit_cast<std::uint32_t>(dryL)
                         && std::bit_cast<std::uint32_t>(r[i]) == std::bit_cast<std::uint32_t>(dryR);
            }
            check(exact, "zero blend delays the original bits on all four first-order layouts");
            rack.set(Shaper::DriveOn, 0);
            check(rack.latency() == 0 && one(rack)[0] == .375f, "disabled Drive removes its physical and reported delay");
        }
    }
    // The dry transfer at steeper slopes is the crossover's allpass. Compare
    // to the existing unity-gain stage followed by a sixteen-frame delay.
    Shaper rack, expected;
    rack.set(Shaper::DriveOn, 1);
    rack.set(Shaper::MasterMix, 0);
    rack.set(static_cast<Shaper::Param>(Shaper::DriveOn + 1), 3);
    rack.set(static_cast<Shaper::Param>(Shaper::DriveOn + 4), 1);
    rack.set(static_cast<Shaper::Param>(Shaper::DriveOn + 5), 2);
    expected.set(Shaper::VolumeBands, 3);
    expected.set(Shaper::VolumeLowSlope, 1);
    expected.set(Shaper::VolumeHighSlope, 2);
    render(rack);
    std::array<float, frames> refL{}, refR{};
    expected.process(inL.data(), inR.data(), refL.data(), refR.data(), frames);
    bool aligned = true;
    for (std::size_t i = 16; i < frames; ++i) aligned &= l[i] == refL[i - 16] && r[i] == refR[i - 16];
    check(aligned, "all three dry Drive bands preserve the 12/24 crossover phase and sixteen-sample delay");
}

void driveAndCrush() {
    Shaper rack;
    rack.set(Shaper::DriveOn, 1);
    rack.set(Shaper::CrushOn, 1);
    rack.set(Shaper::driveParam(Band::Mid, Shaper::DriveStyle), 1);
    rack.set(Shaper::driveParam(Band::Mid, Shaper::DriveMix), 50);
    rack.set(Shaper::driveParam(Band::Mid, Shaper::InputTrim), 75);
    rack.set(Shaper::driveParam(Band::Mid, Shaper::OutputTrim), 25);
    rack.set(Shaper::modParam(Stage::Crush, Band::Mid, Shaper::ModMix), 50);
    for (int i = 0; i < 8; ++i) {
        rack.set(Shaper::driveParam(Band::Mid, static_cast<Shaper::DriveControl>(Shaper::Drive1 + i)), 75);
        rack.set(Shaper::modParam(Stage::Crush, Band::Mid, static_cast<Shaper::ModControl>(Shaper::Primary1 + i)), 25);
        rack.set(Shaper::modParam(Stage::Crush, Band::Mid, static_cast<Shaper::ModControl>(Shaper::Secondary1 + i)), 50);
    }
    adi::dsp::AdiShaperDrive drive;
    adi::dsp::AdiShaperCrush crush;
    drive.prepare(48000);
    drive.setMode(adi::dsp::AdiShaperDrive::Mode::HardClip);
    drive.setTrims(.75f, .25f);
    (void)crush.prepare(48000);
    std::array<float, 2048> l{}, r{};
    double worst = 0.0, plant = 0.0;
    for (std::size_t i = 0; i < l.size(); ++i) {
        l[i] = static_cast<float>(.85 * std::sin(.37 * static_cast<double>(i)));
        r[i] = static_cast<float>(.45 * std::cos(.19 * static_cast<double>(i)));
        const auto actual = one(rack, l[i], r[i]);
        float wetL = l[i], wetR = r[i];
        drive.process(wetL, wetR, .75f);
        const float dryL = i < 16 ? 0.f : l[i - 16], dryR = i < 16 ? 0.f : r[i - 16];
        const float alignedL = dryL + .5f * (wetL - dryL), alignedR = dryR + .5f * (wetR - dryR);
        wetL = alignedL;
        wetR = alignedR;
        crush.processStereo(wetL, wetR, .25f, .5f);
        const float expectedL = alignedL + .5f * (wetL - alignedL);
        const float expectedR = alignedR + .5f * (wetR - alignedR);
        worst = std::max({worst, static_cast<double>(std::abs(actual[0] - expectedL)),
                          static_cast<double>(std::abs(actual[1] - expectedR))});
        plant = std::max(plant, static_cast<double>(std::abs(actual[0] - dryL)));
    }
    check(worst < 1.0e-6 && plant > .05, "Drive then Crush matches separately aligned, half-wet stereo kernels");
    rack.set(Shaper::DriveOn, 0);
    rack.set(Shaper::MasterMix, 0);
    const float negativeZero = -0.0f;
    const auto result = one(rack, negativeZero, .1875f);
    check(rack.latency() == 0 && std::bit_cast<std::uint32_t>(result[0]) == std::bit_cast<std::uint32_t>(negativeZero)
          && result[1] == .1875f, "Crush at zero mix is undelayed and bit exact");
}

void timeHeight(Shaper& rack, Band band, float height) {
    for (int i = 0; i < 8; ++i)
        rack.set(Shaper::timeParam(band, static_cast<Shaper::TimeControl>(Shaper::Time1 + i)), height);
}

void timeRanges() {
    Shaper rack;
    rack.prepare(12000);
    rack.tempo(300);
    rack.set(Shaper::TimeOn, 1);
    timeHeight(rack, Band::Mid, 0);
    auto recover = [&](int range, std::size_t delay, const char* description) {
        rack.set(Shaper::TimeOn, 0);
        rack.set(Shaper::TimeOn, 1);
        rack.set(Shaper::timeParam(Band::Mid, Shaper::TimeRange), static_cast<float>(range));
        std::vector<float> left(delay + 67), right(left.size());
        left[31] = -.75f;
        right[31] = .375f;
        rack.process(left.data(), right.data(), left.data(), right.data(), left.size());
        bool ok = true;
        for (std::size_t i = 0; i < left.size(); ++i) {
            ok &= left[i] == (i == delay + 31 ? -.75f : 0.0f);
            ok &= right[i] == (i == delay + 31 ? .375f : 0.0f);
        }
        check(ok && rack.latency() == 0, description);
    };
    constexpr std::array<std::size_t, 8> delays{9600, 9600, 19200, 38400, 76800, 59, 239, 479};
    for (std::size_t i = 0; i < delays.size(); ++i)
        recover(static_cast<int>(i), delays[i], "fixed and follow ranges reproduce the measured whole-frame impulses");
    rack.set(Shaper::timeParam(Band::Mid, Shaper::TimeCycle), 128);
    recover(0, 307200, "longest supported beat period plays the full 32 bars when it fits in history");
    rack.set(Shaper::timeParam(Band::Mid, Shaper::TimeClock), 1);
    rack.set(Shaper::timeParam(Band::Mid, Shaper::TimeRate), 40);
    recover(0, 300, "free-clock Follow uses the band's hertz period");

    rack.prepare(48000);
    rack.tempo(120);
    rack.set(Shaper::timeParam(Band::Mid, Shaper::TimeRange), 1);
    rack.sync(0);
    std::vector<float> left(200224), right(left.size());
    left[8192] = .5f;
    right[8192] = -.25f;
    rack.process(left.data(), right.data(), left.data(), right.data(), 48000);
    rack.tempo(60);
    rack.sync(2);
    rack.process(left.data() + 48000, right.data() + 48000, left.data() + 48000,
                 right.data() + 48000, left.size() - 48000);
    check(left[200192] == .5f && right[200192] == -.25f &&
          std::count_if(left.begin(), left.end(), [](float v) { return v != 0; }) == 1,
          "tempo change retains older audio and measures the entire bar at the new tempo");

    rack.set(Shaper::timeParam(Band::Mid, Shaper::TimeRange), 5);
    rack.set(Shaper::TimeOn, 0);
    rack.set(Shaper::TimeOn, 1);
    constexpr std::size_t span = 320;
    for (std::uint64_t revision : {1U, 2U}) {
        std::array<float, span> marker{}, markerR{};
        marker[0] = .75f;
        rack.syncAtSample(0, true, 0, 1);
        rack.process(marker.data(), markerR.data(), marker.data(), markerR.data(), 100);
        // The exact sample cursor stays contiguous regardless of beat-map edits.
        rack.syncAtSample(.5, true, 100, revision);
        rack.process(marker.data() + 100, markerR.data() + 100, marker.data() + 100,
                     markerR.data() + 100, span - 100);
        check(marker[239] == (revision == 1 ? .75f : 0.f),
              "native history follows the exact timeline sample and transport revision across tempo-map edits");
    }
}

void timeBandHistory() {
    constexpr std::size_t frames = 1024;
    Shaper time;
    time.prepare(48000);
    time.set(Shaper::TimeOn, 1);
    time.set(static_cast<Shaper::Param>(Shaper::TimeOn + 1), 3);
    std::array<float, frames> source{}, actual{}, actualRight{}, sum{};
    source[11] = -.5f;
    source[26] = .25f;
    constexpr std::array<float, 3> heights{0, 100, 50};
    constexpr std::array<std::size_t, 3> lookback{239, 0, 119};
    for (std::size_t b = 0; b < heights.size(); ++b) {
        const auto band = static_cast<Band>(b);
        time.set(Shaper::timeParam(band, Shaper::TimeRange), 5);
        timeHeight(time, band, heights[b]);
        Shaper isolate;
        isolate.set(Shaper::VolumeBands, 3);
        for (std::size_t k = 0; k < 3; ++k)
            flat(isolate, Stage::Volume, static_cast<Band>(k), b == k ? 100.f : 0.f);
        std::array<float, frames> split{}, right{};
        isolate.process(source.data(), source.data(), split.data(), right.data(), frames);
        for (std::size_t i = lookback[b]; i < frames; ++i) sum[i] += split[i - lookback[b]];
    }
    time.process(source.data(), source.data(), actual.data(), actualRight.data(), frames);
    bool matches = true;
    for (std::size_t i = 0; i < frames; ++i)
        matches &= near(actual[i], sum[i], 1e-6) && near(actualRight[i], sum[i], 1e-6);
    check(matches, "three independent Time histories sum the separately delayed split-band impulses");
}

void reverbReturnMix() {
    constexpr std::size_t frames = 4096;
    std::vector<float> left(frames), right(frames), wetL(frames), wetR(frames), actualL(frames), actualR(frames);
    adi::dsp::AdiShaperReverb room;
    check(room.prepare(48000), "room fixture preparation");
    auto renderWet = [&] {
        room.reset();
        wetL = left;
        wetR = right;
        for (std::size_t i = 0; i < frames; ++i) room.processStereo(wetL[i], wetR[i], 1, 1);
    };
    left[0] = 1.f;
    right[0] = -.5f;
    renderWet();
    const auto peak = static_cast<std::size_t>(std::max_element(wetL.begin(), wetL.end(),
        [](float a, float b) { return std::abs(a) < std::abs(b); }) - wetL.begin());
    check(peak > 0 && std::abs(wetL[peak]) > .001f, "room impulse has an audible delayed reflection");
    left[peak] = std::copysign(1.f, wetL[peak]);
    renderWet();

    Shaper rack;
    rack.prepare(48000);
    for (int layout = 0; layout < 4; ++layout) {
        rack.set(static_cast<Shaper::Param>(Shaper::ReverbOn + 1), static_cast<float>(layout));
        for (const float mix : {0.f, .25f, .5f, .75f, 1.f}) {
            rack.set(Shaper::ReverbOn, 0);
            const float master = mix == .25f ? .5f : 1.f;
            rack.set(Shaper::MasterMix, master * 100.f);
            for (auto band : {Band::Low, Band::Mid, Band::High})
                rack.set(Shaper::modParam(Stage::Reverb, band, Shaper::ModMix), mix * 100.f / master);
            rack.set(Shaper::ReverbOn, 1);
            rack.process(left.data(), right.data(), actualL.data(), actualR.data(), frames);
            const float dry = std::min(1.f, 2.f - 2.f * mix), wet = std::min(1.f, 2.f * mix);
            bool matches = true;
            for (std::size_t i = 0; i < frames; ++i)
                matches &= near(actualL[i], dry * left[i] + wet * wetL[i], 3e-6)
                        && near(actualR[i], dry * right[i] + wet * wetR[i], 3e-6);
            check(matches && rack.latency() == 0, "every layout follows the independently summed Reverb dry/return law");
            if (mix == .5f) check(std::abs(actualL[peak]) > 1.f, "parallel unity dry and wet can add past full scale");
        }
    }
}

void pitchAndNoise() {
    constexpr std::size_t frames = 1600;
    std::array<float, frames> left{}, right{}, actualL{}, actualR{};
    left[0] = .75f;
    right[0] = -.25f;
    Shaper rack;
    rack.prepare(48000);
    rack.set(Shaper::PitchOn, 1);
    rack.set(Shaper::NoiseOn, 1);
    rack.set(Shaper::noiseParam(Band::Mid, Shaper::NoiseOnly), 1);
    rack.set(Shaper::modParam(Stage::Noise, Band::Mid, Shaper::ModMix), 25);
    rack.set(Shaper::modParam(Stage::Pitch, Band::Mid, Shaper::ModMix), 50);
    // With the source off, Noise Only removes the chosen wet share of Pitch.
    adi::dsp::AdiShaperPitch expected;
    check(expected.prepare(48000), "pitch fixture preparation");
    bool matches = true;
    rack.process(left.data(), right.data(), actualL.data(), actualR.data(), frames);
    for (std::size_t i = 0; i < frames; ++i) {
        float l = left[i], r = right[i];
        expected.processStereo(l, r, .5f, .5f);
        matches &= near(actualL[i], .75f * (.5f * left[i] + .5f * l))
                && near(actualR[i], .75f * (.5f * right[i] + .5f * r));
    }
    check(matches && rack.latency() == 0, "cyclic audible delay coexists with zero PDC and linear Noise Only mix");

    rack.set(Shaper::PitchOn, 0);
    rack.set(Shaper::modParam(Stage::Noise, Band::Mid, Shaper::Type), 1);
    rack.set(Shaper::modParam(Stage::Noise, Band::Mid, Shaper::ModMix), 100);
    rack.process(left.data(), right.data(), actualL.data(), actualR.data(), frames);
    float peak = 0;
    for (std::size_t i = 0; i < frames; ++i) peak = std::max(peak, std::abs(actualL[i]));
    check(actualL == actualR && peak > .02f && peak < .2f, "activated white source uses the calibrated mono defaults");
    rack.set(Shaper::PitchOn, 1);
    rack.set(Shaper::ReverbOn, 1);
    rack.set(Shaper::MasterMix, 0);
    left[1] = -0.f;
    rack.process(left.data(), right.data(), actualL.data(), actualR.data(), frames);
    check(actualL == left && actualR == right && std::signbit(actualL[1]),
          "Pitch, Noise and Reverb preserve the exact zero-wet signal");
}

void stagePermutations() {
    Shaper::StageOrder original{};
    for (std::size_t i = 0; i < original.size(); ++i) original[i] = static_cast<Stage>(i);
    Shaper::StageRanks ranks{};
    for (const std::uint8_t rank : {std::uint8_t{0}, std::uint8_t{127}}) {
        ranks.fill(rank);
        check(Shaper::orderFor(ranks) == original, "ties at either extreme restore the complete legacy stage order");
    }
    ranks[static_cast<std::size_t>(Stage::Pitch)] = ranks[static_cast<std::size_t>(Stage::Time)] = 0;
    auto tied = original;
    std::rotate(tied.begin(), tied.begin() + 7, tied.begin() + 9);
    check(Shaper::orderFor(ranks) == tied, "equal-rank groups keep each stage exactly once in stable identity order");

    std::uint32_t state = 9371;
    auto expected = original;
    std::reverse(expected.begin(), expected.end());
    bool matches = true;
    for (int trial = 0; trial < 4096; ++trial) {
        for (std::size_t i = 0; i < expected.size(); ++i)
            ranks[static_cast<std::size_t>(expected[i])] = static_cast<std::uint8_t>(8 + 10 * i);
        matches &= Shaper::orderFor(ranks) == expected;
        for (std::size_t i = expected.size(); i > 1; --i) {
            state = 1664525U * state + 1013904223U;
            std::swap(expected[i - 1], expected[state % i]);
        }
    }
    check(matches, "reverse order and 4095 sampled eleven-stage permutations decode without omissions or duplicates");
}

void nonlinearOrder() {
    constexpr std::size_t frames = 1024;
    std::array<float, frames> left{}, right{}, actualL{}, actualR{};
    Shaper rack;
    flat(rack, Stage::Volume, Band::Mid, 25);
    rack.set(Shaper::DriveOn, 1);
    rack.set(Shaper::driveParam(Band::Mid, Shaper::DriveStyle), 1);
    for (int i = 0; i < 8; ++i)
        rack.set(Shaper::driveParam(Band::Mid, static_cast<Shaper::DriveControl>(Shaper::Drive1 + i)), 50);
    for (std::size_t i = 0; i < frames; ++i) {
        left[i] = static_cast<float>(.42 * std::sin(static_cast<double>(i) * (2. * std::numbers::pi * 400. / 48000.)));
        right[i] = static_cast<float>(.28 * std::sin(static_cast<double>(i) * (2. * std::numbers::pi * 733. / 48000.) + .23));
    }
    float peakDifference = 0;
    std::array<float, frames> original{};
    for (int pass = 0; pass < 3; ++pass) {
        rack.prepare(48000);
        if (pass == 1) {
            rack.set(Shaper::orderParam(Stage::Volume), 1000);
            rack.set(Shaper::orderParam(Stage::Drive), -1);
        } else if (pass == 2) {
            rack.set(Shaper::orderParam(Stage::Drive), 62.6f);
            rack.set(Shaper::orderParam(Stage::Volume), 63);
        }
        adi::dsp::AdiShaperDrive independent;
        independent.prepare(48000);
        independent.setMode(adi::dsp::AdiShaperDrive::Mode::HardClip);
        bool matches = true;
        for (std::size_t at = 0; at < frames;) {
            const auto n = std::min<std::size_t>(73, frames - at);
            rack.process(left.data() + at, right.data() + at, actualL.data() + at, actualR.data() + at, n);
            at += n;
        }
        for (std::size_t i = 0; i < frames; ++i) {
            float l = left[i], r = right[i];
            if (pass != 1) { l *= .25f; r *= .25f; }
            independent.process(l, r, .5f);
            if (pass == 1) { l *= .25f; r *= .25f; }
            matches &= actualL[i] == l && actualR[i] == r;
            if (pass == 0) original[i] = l;
            if (pass == 1) peakDifference = std::max(peakDifference, std::abs(l - original[i]));
        }
        check(matches && rack.latency() == 16, "mixed orders match the separate nonlinear path and retain 16-frame Drive PDC");
    }
    check(peakDifference > .1f, "changing Volume and Drive order produces an audible difference");
}

void historyThroughReorder() {
    Shaper rack;
    rack.prepare(48000);
    flat(rack, Stage::Volume, Band::Mid, 50);
    rack.set(Shaper::TimeOn, 1);
    rack.set(Shaper::timeParam(Band::Mid, Shaper::TimeRange), 5);
    timeHeight(rack, Band::Mid, 0);
    std::array<float, 320> left{}, right{};
    left[0] = 1.f;
    right[0] = -.5f;
    rack.process(left.data(), right.data(), left.data(), right.data(), 128);
    rack.set(Shaper::orderParam(Stage::Time), 0);
    rack.process(left.data() + 128, right.data() + 128, left.data() + 128, right.data() + 128, left.size() - 128);
    check(left[239] == .25f && right[239] == -.125f,
          "moving Time retains the marker recorded under the old upstream gain and sends it through the new downstream gain");
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
    driveDryAndLatency();
    driveAndCrush();
    timeRanges();
    timeBandHistory();
    reverbReturnMix();
    pitchAndNoise();
    stagePermutations();
    nonlinearOrder();
    historyThroughReorder();
    if (failures == 0) std::puts("ADi-SHAPER DSP fixtures passed");
    return failures == 0 ? 0 : 1;
}
