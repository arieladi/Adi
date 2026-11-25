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
static_assert(Shaper::SweepOn == 208 && Shaper::SweepLowPolarity == 283 && Shaper::DriveOn == 286);
static_assert(Shaper::CrushOn == 340 && Shaper::BaseCount == 415);
static_assert(Shaper::PanLowDelayBlend == 855 && Shaper::PanHighDelayBlend == 857);
static_assert(Shaper::driveParam(Band::High, Shaper::OutputTrim) + 1 == Shaper::CrushOn);
static_assert(Shaper::modParam(Stage::Crush, Band::High, Shaper::Secondary8) + 1 == Shaper::BaseCount);
static_assert(Shaper::TriggerLowMode == 753 && Shaper::TriggerSmoothing == 763);
static_assert(Shaper::followerParam(Band::High, Shaper::FollowerRelease) + 1 == Shaper::WidthFollowerEnd);
static_assert(Shaper::WidthFollowerEnd == Shaper::TriggerLowMode);
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
    shaper.set(Shaper::PanMidDelayBlend, 0);
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
    shaper.set(Shaper::PanMidDelayBlend, 0);
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

void heldPanEdges() {
    constexpr int half = 48000;
    constexpr int count = 3 * half;
    const auto configure = [](Shaper& shaper) {
        shaper.prepare(48000);
        for (int i = 0; i < 8; ++i)
            point(shaper, Stage::Pan, Band::Mid, i, i < 4 ? -100.f : 100.f);
    };
    // Captured gains on opposite-signed DC after compensation. The fading
    // ear first switches delay taps, then plays the end of its old plateau.
    constexpr std::array rising{.797651, 1.130023, 1.250605, 1.335287, 1.385908, 1.408957};
    constexpr std::array falling{1.411764, 1.394800, 1.352765, 1.278064, 1.167472, 1.022765, 1.410795};
    constexpr std::array tail{1.411767, 1.394824, 1.352824, 1.278169, 1.167628, 1.022968,
                              .850699, .661167, .467283, .283179, .123020, .000172};
    const auto expected = [&](int frame, int channel) {
        const int edge = (frame + half / 2) / half;
        const int offset = frame - edge * half;
        const bool opens = edge % 2 != channel;
        if (opens) {
            if (offset < -6) return 0.;
            if (offset < 0) return rising[static_cast<std::size_t>(offset + 6)];
            return std::numbers::sqrt2;
        }
        if (offset >= -11 && offset <= -5) return falling[static_cast<std::size_t>(offset + 11)];
        if (offset >= 85 && offset <= 96) return tail[static_cast<std::size_t>(offset - 85)];
        return offset > 96 ? 0. : std::numbers::sqrt2;
    };
    std::vector<float> previousL, previousR;
    for (const int block : {256, 2048}) {
        Shaper shaper;
        configure(shaper);
        std::vector<float> l(count, .25f), r(count, -.125f);
        for (int i = 0; i < count; i += block) {
            shaper.sync(static_cast<double>(i) / 24000.);
            shaper.process(l.data()+i, r.data()+i, l.data()+i, r.data()+i,
                           static_cast<std::size_t>(std::min(block, count-i)));
        }
        double squares = 0., peak = 0.;
        for (int i = 256; i < count; ++i) {
            const auto at = static_cast<std::size_t>(i);
            const double dl = l[at] - .25 * expected(i, 0);
            const double dr = r[at] + .125 * expected(i, 1);
            squares += dl*dl + dr*dr;
            peak = std::max({peak, std::abs(dl), std::abs(dr)});
        }
        check(std::sqrt(squares / (2. * (count-256))) < 1.e-4 && peak < 1.e-4,
              "held Pan switches ears with the measured pre-edge and two-millisecond tail");
        if (!previousL.empty()) check(l == previousL && r == previousR, "host block size preserves every Pan sample");
        previousL.swap(l);
        previousR.swap(r);
    }

    // A marker heard on the original tap must reappear on the delayed one.
    Shaper marked;
    configure(marked);
    std::vector<float> l(half+160), r(l.size());
    r[half-56] = -.5f;
    marked.process(l.data(), r.data(), l.data(), r.data(), l.size());
    check(std::abs(r[half+40] + .5 * std::numbers::sqrt2) < .002 && l[half+40] == 0.,
          "the moving far ear reads its own audio history");

    // A sparse state can contain a single edited point. Alternating dry and
    // wet on that state must never replay audio collected before bypass.
    Shaper sparse;
    point(sparse, Stage::Pan, Band::Mid, 0, 100.f);
    sparse.sync(2., false);
    for (int i = 0; i < 192; ++i) (void)one(sparse, -.25f, .5f);
    sparse.set(Shaper::MasterMix, 0.f);
    check(one(sparse, -.5f, .25f) == std::array{-.5f, .25f}, "dry sparse Pan state passes both channels exactly");
    sparse.sync(.0625, false);
    sparse.set(Shaper::MasterMix, 100.f);
    bool quiet = true;
    for (int i = 0; i < 192; ++i) quiet &= one(sparse, 0.f, 0.f) == std::array{0.f, 0.f};
    check(quiet && sparse.latency() == 0, "resuming sparse Pan clears stale audio and adds no rack latency");
}

void panDelayBlend() {
    // Opposite-signed markers distinguish an attenuated current sample, a
    // delayed copy and any leakage into the other input's channel.
    struct Reading { float blend, direct, delayed, nearGain; };
    constexpr std::array readings{
        Reading{0, .55047578f, 0.f, 1.30307758f},
        Reading{25, .06880947f, .48166630f, 1.30307758f},
        Reading{50, 0.f, .55047578f, 1.30307758f},
        Reading{62.5f, 0.f, .66285682f, 1.22730815f},
        Reading{100, 0.f, 1.f, 1.f}};
    for (const auto reading : readings) {
        for (const float mix : {1.f, .5f}) {
            Shaper shaper;
            flat(shaper, Stage::Pan, Band::Mid, -50.f);
            shaper.set(Shaper::PanMidDelayBlend, reading.blend);
            shaper.set(Shaper::bandParam(Stage::Pan, Band::Mid, Shaper::Mix), 100.f * mix);
            std::array<float, 64> l{}, r{};
            l[12] = -.5f;
            r[40] = .25f;
            shaper.process(l.data(), r.data(), l.data(), r.data(), l.size());
            bool correct = true;
            for (std::size_t i = 0; i < l.size(); ++i) {
                const float left = i == 12 ? -.5f * (1.f-mix + mix*reading.direct)
                                   : i == 33 ? -.5f * mix * reading.delayed : 0.f;
                const float right = i == 40 ? .25f * (1.f-mix + mix*reading.nearGain) : 0.f;
                correct &= near(l[i], left) && near(r[i], right);
            }
            check(correct, "static Pan marker has measured current and integer-delay taps");
        }
    }
    struct RateReading { double rate; float pan, blend; std::size_t age; float direct, tailGain; };
    constexpr std::array rates{
        RateReading{44100., -37.f, 25.f, 12, .08486272f, .59403902f},
        RateReading{48000., 81.f, 75.f, 57, 0.f, .60908943f},
        RateReading{96000., -73.f, 100.f, 90, 0.f, 1.f}};
    for (const auto& reading : rates) {
        Shaper shaper;
        shaper.prepare(reading.rate);
        flat(shaper, Stage::Pan, Band::Mid, reading.pan);
        shaper.set(Shaper::PanMidDelayBlend, reading.blend);
        shaper.set(Shaper::PanHighDelayBlend, 0.f);
        shaper.set(Shaper::PanLowDelayBlend, 0.f);
        std::array<float, 160> l{}, r{};
        auto& far = reading.pan < 0.f ? l : r;
        far[19] = -.5f;
        shaper.process(l.data(), r.data(), l.data(), r.data(), 31);
        shaper.process(l.data()+31, r.data()+31, l.data()+31, r.data()+31, l.size()-31);
        bool matches = true;
        for (std::size_t i = 0; i < l.size(); ++i) {
            const float expected = i == 19 ? -.5f * reading.direct :
                                   i == 19+reading.age ? -.5f * reading.tailGain : 0.f;
            matches &= near(far[i], expected);
        }
        check(matches, "stationary Pan delay scales across rates and survives an uneven callback edge");
    }
    for (const auto blend : {0.f, 50.f, 100.f}) {
        Shaper shaper;
        for (int i = 0; i < 8; ++i)
            point(shaper, Stage::Pan, Band::Mid, i, i < 4 ? -100.f : 100.f);
        shaper.set(Shaper::PanMidDelayBlend, blend);
        std::vector<float> l(48200), r(l.size());
        r[47917] = .625f;
        r[48003] = -.6875f;
        shaper.process(l.data(), r.data(), l.data(), r.data(), l.size());
        const double old = blend == 0.f ? 0. : (blend == 50.f ? .882643 : .624123);
        const double newMarker = blend == 100.f ? -.686535 : 0.;
        check(near(r[48013], old, .002) && near(r[48099], newMarker, .002),
              "a hard Pan switch scales and delays each signed marker at the selected blend");
    }
    Shaper moving;
    moving.set(Shaper::PanMidDelayBlend, 100);
    moving.set(Shaper::bandParam(Stage::Pan, Band::Mid, Shaper::Curve), 1);
    for (int i = 0; i < 8; ++i) point(moving, Stage::Pan, Band::Mid, i, i < 2 ? -50.f : 50.f);
    std::array<float, 128> l{}, r{};
    l[48] = .5f;
    r[65] = -.25f;
    moving.process(l.data(), r.data(), l.data(), r.data(), l.size());
    check(l[48] == 0.f && near(l[69]+l[70], .5) && r[65] == -.25f,
          "a linear Pan contour applies the far-ear delay independently of gain");
    Shaper gaps;
    for (int id : {415, 720, 854, 858, 2000}) gaps.set(static_cast<Shaper::Param>(id), 100.f);
    check(one(gaps) == std::array{.375f, .25f}, "unassigned parameter numbers leave the rack dry");
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
                    const auto total = (a.low + a.high) * (b.low + b.high);
                    const auto high = a.high * b.high;
                    const auto low = lower == 0 && upper == 0 ? a.low * b.low : a.low * (b.low + b.high);
                    const std::array expected{low, total - low - high, high, total};
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

void firstOrderBandImpulses() {
    // Measured unit-impulse taps with the two split controls at 20% and 58%.
    // Each row isolates low, middle or high with the other gain curves at zero.
    constexpr std::array<std::size_t, 4> taps{0, 1, 17, 63};
    constexpr double response[2][3][4]{
        {{.00044206684, .00169613920, .00957904682, .00603571869},
         {.08069658279, .14822648764, .00056757480, -.00646242127},
         {.91886138916, -.14992262125, -.01014662162, .00042670271}},
        {{.00037551573, .00144546293, .00877333507, .00588846505},
         {.07461194992, .13797796965, .00309078321, -.00629830435},
         {.92501249313, -.13942342997, -.01186411828, .00040983907}}
    };
    constexpr std::array rates{44100.0, 48000.0};
    for (std::size_t rate = 0; rate < rates.size(); ++rate) {
        for (std::size_t band = 0; band < 3; ++band) {
            Shaper shaper;
            shaper.prepare(rates[rate]);
            shaper.set(Shaper::VolumeBands, 3);
            shaper.set(static_cast<Shaper::Param>(Shaper::VolumeBands + 1), 82.40689f);
            shaper.set(static_cast<Shaper::Param>(Shaper::VolumeBands + 2), 1147.8298f);
            for (std::size_t i = 0; i < 3; ++i)
                flat(shaper, Stage::Volume, static_cast<Band>(i), i == band ? 100.0f : 0.0f);
            std::array<float, 160> left{}, right{}, outL{}, outR{};
            left[3] = .625f;
            right[56] = -.375f;
            shaper.process(left.data(), right.data(), outL.data(), outR.data(), 64);
            shaper.process(left.data() + 64, right.data() + 64, outL.data() + 64, outR.data() + 64, 96);
            for (std::size_t i = 0; i < taps.size(); ++i) {
                const double tap = response[rate][band][i];
                check(near(outL[3 + taps[i]], .625 * tap, 1.0e-7)
                          && near(outR[56 + taps[i]], -.375 * tap, 1.0e-7),
                      "isolated first-order band has the measured stereo impulse response across a block boundary");
            }
        }
    }
}

void movingFirstOrderSplits() {
    // A pair of independent two-way devices gives the correct response while
    // each edge is moving, including the device's own cutoff timing.
    const auto isolate = [](const std::vector<float>& input, int layout, Band selected) {
        Shaper shaper;
        shaper.prepare(48000);
        shaper.set(Shaper::VolumeBands, static_cast<float>(layout));
        const auto lower = static_cast<Shaper::Param>(Shaper::VolumeBands + 1);
        const auto upper = static_cast<Shaper::Param>(Shaper::VolumeBands + 2);
        shaper.set(lower, 160);
        shaper.set(upper, 8000);
        for (const auto b : {Band::Low, Band::Mid, Band::High})
            flat(shaper, Stage::Volume, b, b == selected ? 100.f : 0.f);
        std::vector<float> left(input.size()), right(input.size());
        const auto render = [&](std::size_t first, std::size_t end) {
            shaper.process(input.data() + first, input.data() + first, left.data() + first,
                           right.data() + first, end - first);
        };
        render(0, 733);
        shaper.set(lower, 3200);
        render(733, 2783);
        shaper.set(upper, 5200);
        render(2783, input.size());
        return left;
    };
    std::vector<float> input(8192);
    for (std::size_t i = 0; i < input.size(); ++i) {
        const double t = static_cast<double>(i);
        input[i] = static_cast<float>(.38 * std::sin(.033 * t) + .19 * std::cos(.23 * t));
    }
    const auto bass = isolate(isolate(input, 1, Band::Low), 2, Band::Mid);
    const auto treble = isolate(isolate(input, 2, Band::High), 1, Band::Mid);
    const auto reversedBass = isolate(isolate(input, 2, Band::Mid), 1, Band::Low);
    const auto low = isolate(input, 3, Band::Low);
    const auto mid = isolate(input, 3, Band::Mid);
    const auto high = isolate(input, 3, Band::High);
    bool discriminates = false;
    float peak = 0;
    for (std::size_t i = 0; i < input.size(); ++i) {
        const float middle = input[i] - bass[i] - treble[i];
        peak = std::max({peak, std::abs(low[i] - bass[i]), std::abs(mid[i] - middle), std::abs(high[i] - treble[i])});
        discriminates |= std::abs(bass[i] - reversedBass[i]) > .0001f;
    }
    check(peak < 4e-7f, "each moving outer band crosses its own edge first and the middle is complementary");
    check(discriminates, "the moving input distinguishes reversed crossover order");
}

void crossoverSpacing() {
    constexpr auto lowHz = static_cast<Shaper::Param>(Shaper::VolumeBands + 1);
    constexpr auto highHz = static_cast<Shaper::Param>(Shaper::VolumeBands + 2);
    const auto impulse = [&](int layout, float lower, float upper, Band selected, double rate) {
        Shaper shaper;
        shaper.prepare(rate);
        shaper.set(Shaper::VolumeBands, static_cast<float>(layout));
        shaper.set(highHz, upper);
        shaper.set(lowHz, lower);
        if (layout == 3) {
            shaper.set(Shaper::VolumeLowSlope, 1);
            shaper.set(Shaper::VolumeHighSlope, 2);
        }
        for (const auto band : {Band::Low, Band::Mid, Band::High})
            flat(shaper, Stage::Volume, band, band == selected ? 100.f : 0.f);
        std::array<float, 32> in{}, left{}, right{};
        in[0] = 1;
        shaper.process(in.data(), in.data(), left.data(), right.data(), in.size());
        return left;
    };
    // Isolate the high band with 12/24 dB slopes. Equal 48% positions give
    // 573.915 / 811.638 Hz, and lowering the requested upper has the same result.
    constexpr std::array rates{44100.0, 48000.0, 96000.0};
    constexpr std::array first{.7220980644, .7413837433, .8607804298};
    constexpr std::array later{.0241557270, .0351716161, .0252627730};
    for (std::size_t i = 0; i < rates.size(); ++i) {
        const auto equal = impulse(3, 573.9149f, 573.9149f, Band::High, rates[i]);
        const auto crossed = impulse(3, 573.9149f, 400, Band::High, rates[i]);
        check(near(equal[0], first[i], 1e-6) && near(equal[16], later[i], 1e-6) && equal == crossed,
              "three-band cutoffs keep half an octave of spacing with the lower edge taking priority");
    }
    const auto bass = impulse(1, 20000, 20000, Band::Low, 48000);
    const auto middle = impulse(2, 20, 20, Band::Mid, 48000);
    check(near(bass[0], .59664369, 2e-7) && near(bass[1], .4813200, 2e-7),
          "highest lower split leaves the measured middle-band remainder");
    check(near(middle[0], .00190327, 2e-7) && near(middle[1], .00379929, 2e-7),
          "lowest upper split retains the measured narrow middle band");
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

bool sameSampleBits(float actual, float expected) {
    return std::bit_cast<std::uint32_t>(actual) == std::bit_cast<std::uint32_t>(expected);
}

void nonfiniteAudio() {
    struct Case {
        const char* name;
        void (*configure)(Shaper&);
    };
    const std::array cases{
        Case{"neutral rack substitutes one zero for nonfinite audio", [](Shaper&) {}},
        Case{"Volume split substitutes one zero for nonfinite audio", [](Shaper& rack) {
            rack.set(Shaper::VolumeBands, 1);
            flat(rack, Stage::Volume, Band::Mid, 50.f);
        }},
        Case{"Width compensation stays exact after nonfinite audio", [](Shaper& rack) {
            rack.set(Shaper::WidthBands, 3);
            rack.set(Shaper::WidthLowSlope, 1);
            rack.set(Shaper::WidthHighSlope, 2);
        }},
        Case{"Pan compensation stays exact after nonfinite audio", [](Shaper& rack) {
            rack.set(Shaper::PanBands, 3);
            rack.set(Shaper::PanLowSlope, 2);
            rack.set(Shaper::PanHighSlope, 1);
        }},
        Case{"Filter stays exact after nonfinite audio", [](Shaper& rack) {
            rack.set(Shaper::FilterOn, 1);
            rack.set(Shaper::modParam(Stage::Filter, Band::Mid, Shaper::Type), 1);
        }},
        Case{"Sweep flanger delay stays exact after nonfinite audio", [](Shaper& rack) {
            rack.set(Shaper::SweepOn, 1);
        }},
        Case{"Sweep phaser feedback stays exact after nonfinite audio", [](Shaper& rack) {
            rack.set(Shaper::SweepOn, 1);
            rack.set(Shaper::modParam(Stage::Sweep, Band::Mid, Shaper::Type), 5);
        }},
        Case{"Drive wet path stays exact after nonfinite audio", [](Shaper& rack) {
            rack.set(Shaper::DriveOn, 1);
            for (int i = 0; i < 8; ++i)
                rack.set(Shaper::driveParam(Band::Mid, static_cast<Shaper::DriveControl>(Shaper::Drive1 + i)), 50);
        }},
        Case{"Drive split latency and warm bypass stay exact after nonfinite audio", [](Shaper& rack) {
            rack.set(Shaper::DriveOn, 1);
            rack.set(static_cast<Shaper::Param>(Shaper::DriveOn + 1), 3);
            rack.set(Shaper::MasterMix, 0);
        }},
        Case{"Crush hold and dither stay exact after nonfinite audio", [](Shaper& rack) {
            rack.set(Shaper::CrushOn, 1);
            rack.set(Shaper::modParam(Stage::Crush, Band::Mid, Shaper::Type), 1);
            for (int i = 0; i < 8; ++i) {
                rack.set(Shaper::modParam(Stage::Crush, Band::Mid, static_cast<Shaper::ModControl>(Shaper::Primary1 + i)), 50);
                rack.set(Shaper::modParam(Stage::Crush, Band::Mid, static_cast<Shaper::ModControl>(Shaper::Secondary1 + i)), 50);
            }
        }}
    };
    struct Fault {
        float sample;
        bool right;
    };
    constexpr auto inf = std::numeric_limits<float>::infinity();
    const std::array faults{Fault{std::numeric_limits<float>::quiet_NaN(), false},
                            Fault{inf, true}, Fault{-inf, false}};
    constexpr std::size_t block = 128, frames = 12 * block, badFrame = 2 * block - 1;
    std::array<float, frames> inL{}, inR{}, actualL{}, actualR{}, expectedL{}, expectedR{};
    for (std::size_t i = 0; i < frames; ++i) {
        const auto time = static_cast<double>(i);
        inL[i] = static_cast<float>(.35 * std::sin(.13 * time));
        inR[i] = static_cast<float>(.22 * std::cos(.09 * time));
    }
    inL.front() = -0.f;

    for (const auto& test : cases) {
        for (const auto rate : {48000., 96000.}) {
            for (const auto& fault : faults) {
                Shaper actual, expected;
                actual.prepare(rate);
                expected.prepare(rate);
                test.configure(actual);
                test.configure(expected);
                actualL = inL;
                actualR = inR;
                auto& invalid = fault.right ? actualR : actualL;
                auto& clean = fault.right ? inR : inL;
                const auto original = clean[badFrame];
                invalid[badFrame] = fault.sample;
                clean[badFrame] = 0.f;
                for (std::size_t frame = 0; frame < frames; frame += block) {
                    auto* left = actualL.data() + frame;
                    auto* right = actualR.data() + frame;
                    actual.process(left, right, left, right, block);
                    expected.process(inL.data() + frame, inR.data() + frame,
                                     expectedL.data() + frame, expectedR.data() + frame, block);
                }
                clean[badFrame] = original;
                check(actual.latency() == expected.latency() &&
                          std::equal(actualL.begin(), actualL.end(), expectedL.begin(), sameSampleBits) &&
                          std::equal(actualR.begin(), actualR.end(), expectedR.begin(), sameSampleBits),
                      test.name);
            }
        }
    }

    Shaper crossed;
    float left = inf, right = -.25f;
    crossed.process(&left, &right, &right, &left, 1);
    check(right == 0.f && left == -.25f, "nonfinite stereo copy supports crossed input and output planes");
}

void finiteDriveOverflow() {
    constexpr std::size_t frames = 128;
    std::array<float, frames> inL{}, inR{}, actualL{}, actualR{}, expectedL{}, expectedR{};
    for (std::size_t i = 0; i < frames; ++i) {
        const auto time = static_cast<double>(i);
        inL[i] = static_cast<float>(.35 * std::sin(.13 * time));
        inR[i] = static_cast<float>(.22 * std::cos(.09 * time));
    }
    for (const auto rate : {48000., 96000.}) {
        Shaper actual, expected;
        for (auto* rack : {&actual, &expected}) {
            rack->prepare(rate);
            rack->set(Shaper::DriveOn, 1);
            rack->set(Shaper::driveParam(Band::Mid, Shaper::InputTrim), 100);
            rack->set(Shaper::driveParam(Band::Mid, Shaper::OutputTrim), 100);
        }
        auto run = [&](const std::array<float, frames>& left) {
            actual.process(left.data(), inR.data(), actualL.data(), actualR.data(), frames);
            expected.process(inL.data(), inR.data(), expectedL.data(), expectedR.data(), frames);
        };
        run(inL);
        auto fault = inL;
        // Finite ingress with extreme trims at exactly zero Drive overflows
        // the internal wet float ring, independently of NaN/Inf input handling.
        fault[frames / 2] = std::numeric_limits<float>::max();
        run(fault);
        bool rightExact = std::equal(actualR.begin(), actualR.end(), expectedR.begin(), sameSampleBits);
        bool leftFinite = true, leftAudible = false;
        for (int block = 0; block < 3; ++block) {
            run(inL);
            rightExact &= std::equal(actualR.begin(), actualR.end(), expectedR.begin(), sameSampleBits);
            for (float sample : actualL) {
                leftFinite &= std::isfinite(sample);
                leftAudible |= std::abs(sample) > .1f;
            }
        }
        check(leftFinite && leftAudible, "Drive recovers finite audio on the next callback after internal overflow");
        check(rightExact, "Drive overflow recovery preserves the unaffected channel's filter and delay state");
    }
}

void driveDryOverflowAtBlockEdge() {
    constexpr std::size_t frames = 64;
    for (int layout : {0, 3}) {
        Shaper rack;
        rack.set(Shaper::DriveOn, 1);
        rack.set(static_cast<Shaper::Param>(Shaper::DriveOn + 1), static_cast<float>(layout));
        for (const auto band : {Band::Low, Band::Mid, Band::High})
            rack.set(Shaper::driveParam(band, Shaper::DriveMix), 0);
        flat(rack, Stage::Pan, Band::Mid, 100.f);
        std::array<float, frames> input{}, left{}, right{};
        input.fill(.25f);
        input.back() = std::numeric_limits<float>::max();
        rack.process(input.data(), right.data(), left.data(), right.data(), frames);
        input.back() = .25f;
        rack.process(input.data(), right.data(), left.data(), right.data(), frames);
        check(std::ranges::all_of(left, [](float value) { return std::isfinite(value); }) &&
                  near(left[32], .25 * std::numbers::sqrt2),
              "an overflowing upstream stage cannot poison Drive's delayed dry path across a callback edge");
    }
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
    rack.set(Shaper::SweepOn, 1);
    rack.set(Shaper::MasterMix, 50);
    rack.set(Shaper::modParam(Stage::Filter, Band::Mid, Shaper::ModMix), 80);
    rack.set(Shaper::modParam(Stage::Sweep, Band::Mid, Shaper::ModMix), 60);
    rack.set(Shaper::SweepMidPolarity, 1);
    // Cutoff interpolates while resonance holds. The Sweep stage does the reverse.
    rack.set(Shaper::modParam(Stage::Filter, Band::Mid, Shaper::PrimaryCurve), 1);
    rack.set(Shaper::modParam(Stage::Sweep, Band::Mid, Shaper::SecondaryCurve), 1);
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
        rack.set(Shaper::modParam(Stage::Sweep, Band::Mid, first), center[j]);
        rack.set(Shaper::modParam(Stage::Sweep, Band::Mid, second), feedback[j]);
    }
    adi::dsp::AdiShaperFilter filter;
    adi::dsp::AdiShaperSweep sweep;
    filter.prepare(48000);
    filter.setMode(adi::dsp::AdiShaperFilter::Mode::LowPass6);
    (void)sweep.prepare(48000);
    sweep.setMode(adi::dsp::AdiShaperSweep::Mode::Flange);
    sweep.setPolarity(true);
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
            sweep.processStereo(l, r, center[step] * .01f, fb);
            l = dryL + (0.5f * (60.f * .01f)) * (l - dryL);
            r = dryR + (0.5f * (60.f * .01f)) * (r - dryR);
            maxError = std::max({maxError, static_cast<double>(std::abs(out[0] - l)),
                                 static_cast<double>(std::abs(out[1] - r))});
            effectPeak = std::max(effectPeak, static_cast<double>(std::abs(l - a)));
        }
    }
    check(maxError < 2.0e-6 && effectPeak > 0.1, "two independent contours feed each serial kernel at master times band blend");
    rack.set(Shaper::FilterOn, 0);
    rack.set(Shaper::SweepOn, 0);
    rack.set(static_cast<Shaper::Param>(Shaper::SweepOn + 1), 3);
    rack.set(static_cast<Shaper::Param>(Shaper::SweepOn + 4), 2);
    const auto bypass = one(rack);
    check(bypass[0] == 0.375f && bypass[1] == 0.25f, "switching stages off bypasses their crossover and tail");
}

void modulationBands() {
    for (const auto stage : {Stage::Filter, Stage::Sweep, Stage::Drive, Stage::Crush}) {
        // First three copies process one wet band each. The fourth runs all
        // three. At 6 dB/octave the shared dry transfer is exactly unity.
        std::array<Shaper, 4> racks;
        const auto on = stage == Stage::Filter ? Shaper::FilterOn : stage == Stage::Sweep ? Shaper::SweepOn
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
                                : stage == Stage::Sweep ? std::array{1, 2, 5} : std::array{0, 0, 0};
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

void crushHoldClockBlocks() {
    constexpr std::size_t frames = 144257;
    std::vector<float> input(frames);
    std::uint32_t seed = 0x912ab43du;
    for (auto& sample : input) {
        seed = 1664525u * seed + 1013904223u;
        sample = static_cast<float>(seed >> 8) * 0x1p-24f - .5f;
    }
    const auto render = [&](std::array<std::size_t, 4> blocks, bool linear,
                            float low, float high, double startBeat) {
        Shaper rack;
        rack.set(Shaper::CrushOn, 1);
        const auto set = [&](Shaper::ModControl control, float value) {
            rack.set(Shaper::modParam(Stage::Crush, Band::Mid, control), value);
        };
        set(Shaper::SecondaryCurve, linear ? 1.f : 0.f);
        for (int i = 0; i < 8; ++i)
            set(static_cast<Shaper::ModControl>(Shaper::Secondary1 + i), i == 0 ? low : high);
        rack.prepare(48000);
        rack.tempo(120);
        auto left = input, right = input;
        std::size_t pos = 0, block = 0;
        while (pos < frames) {
            const auto n = std::min(blocks[block++ % blocks.size()], frames - pos);
            rack.sync(startBeat + static_cast<double>(pos) / 24000.0, true);
            rack.process(left.data() + pos, right.data() + pos,
                         left.data() + pos, right.data() + pos, n);
            pos += n;
        }
        check(rack.latency() == 0, "Crush hold clock retains zero latency");
        return left;
    };
    const std::array<std::size_t, 4> blocks256{256, 256, 256, 256};
    const std::array<std::size_t, 4> blocks2048{2048, 2048, 2048, 2048};
    const std::array<std::size_t, 4> irregular{63, 411, 1021, 19};
    for (bool linear : {false, true}) {
        for (auto rates : {std::array{20.f, 100.f}, std::array{20.f, 99.f},
                           std::array{55.f, 55.f}, std::array{100.f, 100.f}}) {
            for (double start : {0.0, .171875}) {
                const auto reference = render({frames, frames, frames, frames}, linear, rates[0], rates[1], start);
                check(render(blocks256, linear, rates[0], rates[1], start) == reference,
                      "Crush hold clock matches a whole buffer at 256 frames");
                check(render(blocks2048, linear, rates[0], rates[1], start) == reference,
                      "Crush hold clock matches a whole buffer at 2048 frames");
                check(render(irregular, linear, rates[0], rates[1], start) == reference,
                      "Crush hold clock matches a whole buffer at irregular frame counts");
            }
        }
    }
    Shaper located;
    located.set(Shaper::CrushOn, 1);
    located.set(Shaper::modParam(Stage::Crush, Band::Mid, Shaper::Secondary1), 0);
    located.prepare(48000);
    located.sync(0, true);
    std::array<float, 129> alternating{}, right{};
    for (std::size_t i = 0; i < alternating.size(); ++i) alternating[i] = (i % 2 ? -.25f : .25f);
    auto left = alternating;
    located.process(left.data(), alternating.data(), left.data(), right.data(), left.size());
    located.sync(2.0, true);
    left = alternating;
    located.process(left.data(), alternating.data(), left.data(), right.data(), left.size());
    check(std::abs(left[64] - left[65]) > .49f,
          "A transport locate moves the Crush hold contour into its full-rate segment");
}

void audioTrigger() {
    // These are stable public parameter IDs; sparse sessions only contain active controls.
    const auto parameter = [](int publicId) {
        return publicId <= Shaper::Count ? static_cast<Shaper::Param>(publicId - 1) : Shaper::Count;
    };
    constexpr std::array rates{44100, 48000, 96000};
    constexpr int expected[][4] = {{617, 882, 1058, 1323}, {672, 961, 1152, 1441}, {1344, 1921, 2304, 2881}};
    for (std::size_t rate = 0; rate < rates.size(); ++rate) {
        for (int cell = 0; cell < 4; ++cell) {
            Shaper rack;
            rack.prepare(rates[rate]);
            rack.set(parameter(755), 2); // Mid band, one shot
            rack.set(parameter(762), static_cast<float>(cell < 2 ? 0 : 2));
            rack.set(parameter(764), static_cast<float>(cell % 2));
            const int latency = expected[rate][cell];
            std::vector<float> l(4096), r(4096);
            l[31] = .5f;
            r[79] = -.25f;
            rack.process(l.data(), r.data(), l.data(), r.data(), l.size());
            check(rack.latency() == latency && l[31 + latency] == .5f && r[79 + latency] == -.25f &&
                  std::count(l.begin(), l.end(), 0.f) == 4095 && std::count(r.begin(), r.end(), 0.f) == 4095,
                  "reported detector latency equals both observed stereo impulse offsets");
            rack.set(Shaper::DriveOn, 1);
            check(rack.latency() == latency + 16, "Drive compensation is added to detector latency");
            rack.set(parameter(755), 0);
            check(rack.latency() == 16, "synchronized Volume releases only its own compensation");
        }
    }

    for (const auto& setting : std::array<std::array<int, 3>, 4>{{
             {0, 0, 479}, {0, 100, 863}, {1, 50, 671}, {2, 50, 1153}}}) {
        Shaper rack;
        rack.prepare(48000);
        rack.set(parameter(755), 2);
        rack.set(parameter(758), 75);
        rack.set(parameter(762), static_cast<float>(setting[0]));
        rack.set(parameter(763), static_cast<float>(setting[1]) * .01f);
        point(rack, Stage::Volume, Band::Mid, 0, 0);
        std::vector<float> l(6000), r(6000, .0125f);
        l[3000] = .7f;
        rack.process(l.data(), r.data(), l.data(), r.data(), l.size());
        const auto first = static_cast<std::size_t>(3000 + setting[2]);
        check(r[first - 1] == .0125f && r[first] == 0,
              "onset shift and profile set the restart independently of dry delay");
    }
    for (int stop : {25, 50, 75}) {
        Shaper rack;
        rack.prepare(48000);
        rack.set(parameter(755), 2);
        rack.set(parameter(758), static_cast<float>(stop));
        for (int i = 0; i < 8; ++i) point(rack, Stage::Volume, Band::Mid, i, 12.5f * static_cast<float>(i));
        std::vector<float> l(2048), r(2048, .0125f);
        rack.process(l.data(), r.data(), l.data(), r.data(), l.size());
        check(near(r.back(), .0125 * stop * .01), "one-shot without hits holds the selected contour endpoint");
    }

    constexpr int length = 96000;
    const std::array<int, 4> starts{14000, 31300, 53500, 81400};
    const std::array<float, 4> amplitudes{.7f, .045f, .7f, .7f};
    const std::array<int, 3> restarts{14671, 54171, 82071};
    for (int mode : {1, 2}) {
        Shaper rack;
        rack.prepare(48000);
        rack.set(parameter(755), static_cast<float>(mode));
        rack.set(parameter(758), 75.f);
        rack.set(Shaper::bandParam(Stage::Volume, Band::Mid, Shaper::Cycle), .5f);
        point(rack, Stage::Volume, Band::Mid, 0, 0);
        std::vector<float> l(length), r(length, .0125f);
        for (std::size_t burst = 0; burst < starts.size(); ++burst)
            for (int j = 0; j < 1800; ++j)
                l[starts[burst] + j] = amplitudes[burst] * static_cast<float>(
                    std::cos(2 * std::numbers::pi * 920 * j / 48000.0) * std::exp(-j / 420.0));
        constexpr std::array blocks{31, 256, 7, 2048, 19};
        for (int at = 0, block = 0; at < length; ++block) {
            const int n = std::min(blocks[static_cast<std::size_t>(block) % blocks.size()], length - at);
            rack.sync(at / 24000.0);
            rack.process(l.data() + at, r.data() + at, l.data() + at, r.data() + at, n);
            at += n;
        }
        for (int restart : restarts)
            check(r[restart - 1] == .0125f && r[restart] == 0 && r[restart + 1499] == 0 &&
                  r[restart + 1501] == .0125f, "strong stereo onset restarts at the measured frame");
        check(r[31970] == .0125f && r[31971] == .0125f, "quiet burst leaves the phase undisturbed");
        check(mode == 1 ? r[26672] == 0 : r[26672] == .0125f, "loop continues while one-shot holds");
        check(mode == 1 ? r[12001] == 0 : r[12001] == .0125f, "pre-trigger loop and one-shot starting phase");
    }
}



void setFollower(Shaper& rack, Band band, Shaper::FollowerControl control, float value) {
    rack.set(Shaper::followerParam(band, control), value);
}

void followerSignedPeaks() {
    Shaper rack;
    flat(rack, Stage::Width, Band::Mid, 0);
    auto set = [&](Shaper::FollowerControl id, float v) { setFollower(rack, Band::Mid, id, v); };
    set(Shaper::FollowerOn, 1);
    set(Shaper::FollowerAmount, 75);
    set(Shaper::FollowerAttack, 0);
    set(Shaper::FollowerRelease, 0);
    set(Shaper::FollowerHold, 3.5f);
    auto read = [&](int frame, float level) {
        const float left = frame % 3 == 0 ? -level : level;
        return one(rack, left, -left)[0] / (2.f * left);
    };
    // A signed plateau followed by a one-sample peak. At 96 kHz the
    // 3.5-percent hold spans 58 previous frames, before the envelope falls.
    struct Capture { double rate; std::array<float, 6> envelope; };
    constexpr std::array<int, 6> frames{385, 387, 610, 611, 640, 669};
    constexpr Capture reads[]{
        {44100, {.24998784f, .25f, .61946333f, .62494051f, .61655682f, .54059649f}},
        {48000, {.24896085f, .24999976f, .61712301f, .62487769f, .62236190f, .55154723f}},
        {96000, {.25f, .24947989f, .56986404f, .61813009f, .62483800f, .62353802f}}
    };
    for (const auto& row : reads) {
        rack.prepare(row.rate);
        std::size_t mark = 0;
        for (int i = 0; i <= frames.back(); ++i) {
            const float level = i <= 400 ? .25f : i == 610 ? .625f : .0625f;
            const auto actual = read(i, level);
            if (i == frames[mark])
                check(near(actual, row.envelope[mark++], 5e-6), "signed plateau and single-frame peak hold");
        }
    }

    // Lengthening the window can recover an older peak. Frames delivered
    // during zero hold drive the envelope without advancing this history.
    rack.prepare(48000);
    constexpr std::array<int, 10> at{167, 168, 169, 639, 640, 642, 767, 768, 1168, 1169};
    constexpr std::array<double, 10> captured{.32955289, .37429443, .37498903, .38544315, .38384101,
                                               .38065666, .37497580, .37341717, .49755105, .49996197};
    std::size_t mark = 0;
    for (int i = 0; i <= at.back(); ++i) {
        if (i == 168) set(Shaper::FollowerHold, 10);
        if (i == 310) set(Shaper::FollowerHold, 0);
        if (i == 640) set(Shaper::FollowerHold, 16);
        if (i == 768) set(Shaper::FollowerHold, 2.5f);
        if (i == 1168) set(Shaper::FollowerHold, 17);
        float level = .03125f;
        if (i >= 102 && i < 108) level = .375f;
        if (i == 522 || i == 523) level = .625f;
        if (i == 1060 || i == 1061) level = .5f;
        const auto actual = read(i, level);
        if (i == at[mark])
            check(near(actual, captured[mark++], 5e-6), "changing hold recalls only positive-window input");
    }

    // The oldest input still belongs to the half-second window at the
    // highest supported sample rate, including after the circular queue wraps.
    rack.prepare(768000);
    set(Shaper::FollowerHold, 100);
    std::array<float, 2048> l{}, r{}, outL{}, outR{};
    l.fill(.03125f);
    r.fill(-.03125f);
    auto feedLow = [&](int n) {
        while (n > 0) {
            const auto framesThisTime = std::min(n, static_cast<int>(l.size()));
            rack.process(l.data(), r.data(), outL.data(), outR.data(), static_cast<std::size_t>(framesThisTime));
            n -= framesThisTime;
        }
    };
    feedLow(384019);
    (void)read(0, .625f);
    feedLow(383999);
    check(near(read(0, .03125f), .625, 2e-6), "768 kHz retains the oldest peak at the 500 ms limit");
    check(near(read(0, .03125f), .6248373, 2e-6), "the oldest peak expires on the next frame");

    struct Release { double rate; float percent, sample800, sample1200; };
    constexpr Release releases[]{
        {48000, 2, .33241251f, .14692384f}, {48000, 7.25f, .42682707f, .31103966f},
        {96000, 2, .40769157f, .27105471f}, {96000, 11, .47488791f, .42838410f}
    };
    set(Shaper::FollowerHold, 0);
    for (const auto& release : releases) {
        rack.prepare(release.rate);
        set(Shaper::FollowerRelease, release.percent);
        for (int i = 0; i <= 1200; ++i) {
            const auto actual = read(i, i <= 600 ? .5f : .0625f);
            if (i == 800) check(near(actual, release.sample800, 3e-5), "early measured decay between release anchors");
            // After 600 decay steps a two-ULP pole error can reach 0.000036.
            if (i == 1200) check(near(actual, release.sample1200, 4e-5), "later measured decay between release anchors");
        }
    }
}

void followerLevels() {
    Shaper rack;
    flat(rack, Stage::Width, Band::Mid, 50);
    auto set = [&](Shaper::FollowerControl id, float value) { setFollower(rack, Band::Mid, id, value); };
    set(Shaper::FollowerOn, 1);
    set(Shaper::FollowerAmount, 75);
    set(Shaper::FollowerAttack, 0);
    set(Shaper::FollowerRelease, 0);
    auto measure = [&](float left, float right, bool narrows = false) {
        std::array<float, 4096> l, r;
        l.fill(left);
        r.fill(right);
        rack.process(l.data(), r.data(), l.data(), r.data(), l.size());
        // Compare the maximum modulation over a complete plateau ripple.
        float extreme = (l.back() - r.back()) / (left - right);
        for (std::size_t i = l.size() - 16; i < l.size(); ++i) {
            const float gain = (l[i] - r[i]) / (left - right);
            extreme = narrows ? std::min(extreme, gain) : std::max(extreme, gain);
        }
        return extreme;
    };
    for (const auto& pair : {std::array{-.2f,.02f}, std::array{.02f,-.2f}, std::array{.15f,-.15f},
                             std::array{-.03f,-.09f}, std::array{.01f,.35f}}) {
        const float e = std::max(std::abs(pair[0]), std::abs(pair[1]));
        check(near(measure(pair[0], pair[1]), .5f + 2.f * e), "Width detects the larger absolute channel level");
    }
    set(Shaper::FollowerDepth, 1);
    check(near(measure(.11f,.09f), .94), "second Depth setting doubles the measured positive modulation");
    set(Shaper::FollowerDepth, 2);
    check(near(measure(.11f,.09f), 1.38), "third Depth setting quadruples the measured modulation");
    set(Shaper::FollowerThreshold, 25);
    check(near(measure(.21f,.19f), .5, 1e-4) && near(measure(.41f,.39f), 2.0, 1e-5),
          "threshold gates quieter plateaus and larger peaks can saturate the width");
    set(Shaper::FollowerThreshold, 0);
    set(Shaper::FollowerDepth, 0);
    set(Shaper::FollowerAmount, 25);
    check(near(measure(.21f,.19f,true), .08, 1e-5), "negative Add amount narrows the stereo signal");
    set(Shaper::FollowerMode, 1);
    set(Shaper::FollowerAmount, 75);
    set(Shaper::FollowerOffset, 75);
    check(near(measure(-.2f,.02f), 1.15), "Multiply uses a unipolar amount and applies the offset afterwards");
    rack.set(Shaper::MasterMix, 50);
    rack.set(Shaper::bandParam(Stage::Width, Band::Mid, Shaper::Mix), 50);
    check(near(measure(-.2f,.02f), 1.0375), "the follower feeds the existing master and band dry/wet mix");
    set(Shaper::FollowerOn, 0);
    check(near(measure(-.2f,.02f), .875), "disabling the detector restores the independent Width contour");

    flat(rack, Stage::Width, Band::Mid, 25);
    rack.set(Shaper::MasterMix, 100);
    rack.set(Shaper::bandParam(Stage::Width, Band::Mid, Shaper::Mix), 100);
    set(Shaper::FollowerOn, 1);
    set(Shaper::FollowerAmount, 50);
    set(Shaper::FollowerOffset, 0);
    check(near(measure(.25f,.125f), -1.9375, 1e-6),
          "negative Multiply offset reverses the stereo difference");
    std::array<float, 64> left, right;
    left.fill(.25f);
    right.fill(.125f);
    rack.process(left.data(), right.data(), left.data(), right.data(), left.size());
    for (std::size_t i = 0; i < left.size(); ++i)
        check(near(left[i] + right[i], .375, 1e-7), "signed Width preserves the sum");
    set(Shaper::FollowerMode, 0);
    check(near(measure(.25f,.125f), 0, 1e-6), "Add stops at mono at the same minimum offset");
    set(Shaper::FollowerMode, 1);
    set(Shaper::FollowerAmount, 100);
    set(Shaper::FollowerDepth, 2);
    set(Shaper::FollowerOffset, 100);
    check(near(measure(.75f,.375f), 2, 1e-6), "Multiply limits a positive side gain to two");
}

void followerTimeConstants() {
    Shaper rack;
    flat(rack, Stage::Width, Band::Mid, 50);
    auto set = [&](Shaper::FollowerControl id, float value) { setFollower(rack, Band::Mid, id, value); };
    set(Shaper::FollowerOn, 1);
    set(Shaper::FollowerAmount, 75);
    set(Shaper::FollowerAttack, 0);
    set(Shaper::FollowerRelease, 0);
    // The side pilot stays at .01 across the common-mode level step.
    auto baseline = [&] {
        set(Shaper::FollowerOn, 0);
        set(Shaper::FollowerOn, 1);
        std::array<float, 4096> l, r;
        l.fill(.01f);
        r.fill(-.01f);
        rack.process(l.data(), r.data(), l.data(), r.data(), l.size());
    };
    auto gainAfter = [&](std::size_t frames, float common) {
        std::vector<float> l(frames, common + .01f), r(frames, common - .01f);
        rack.process(l.data(), r.data(), l.data(), r.data(), frames);
        return (l.back() - r.back()) / .02f;
    };
    baseline();
    check(near(gainAfter(1, .18f), .87441205978, 3e-6), "minimum attack matches the first measured step sample at 48 kHz");
    rack.prepare(96000);
    baseline();
    check(near(gainAfter(1, .18f), .83514451981, 3e-6), "minimum attack matches the first measured step sample at 96 kHz");
    rack.prepare(48000);
    const std::array attackMs{1.562,12.500,42.188,99.938};
    for (std::size_t i = 0; i < attackMs.size(); ++i) {
        set(Shaper::FollowerAttack, 0);
        baseline();
        set(Shaper::FollowerAttack, 25.f * static_cast<float>(i + 1));
        const auto count = static_cast<std::size_t>(std::round(attackMs[i] * 48)) + 1;
        check(near(gainAfter(count, .18f), .52 + .36 * .63212, .002),
              "all four measured Attack positions reach their 63.2 percent crossing");
    }
    set(Shaper::FollowerAttack, 0);
    for (const float h : {25.f, 50.f, 75.f, 100.f}) {
        set(Shaper::FollowerHold, h);
        baseline();
        (void)gainAfter(100, .18f);
        const auto held = static_cast<std::size_t>(std::round(24000 * h * h / 10000));
        check(near(gainAfter(held, 0), .88, 2e-6) && gainAfter(1, 0) < .879,
              "hold freezes the plateau for precisely 31.25, 125, 281.25 or 500 ms");
    }
    set(Shaper::FollowerHold, 0);
    constexpr std::array<float, 6> release{0, 20, 25, 50, 75, 100};
    constexpr std::array<double, 6> measuredHalfMs{3.208, 55.083, 77.208, 306.438, 1019.708, 3251.188};
    for (std::size_t i = 0; i < release.size(); ++i) {
        set(Shaper::FollowerRelease, release[i]);
        baseline();
        (void)gainAfter(100, .18f);
        const auto n = static_cast<std::size_t>(std::round(measuredHalfMs[i] * 48)) + 1;
        check(near(gainAfter(n, 0), .7, .001), "absolute peak decay reproduces the six measured release crossings");
    }
}

void followerPoleLandmarks() {
    using Follower = adi::dsp::AdiShaperFollower;
    using Control = Follower::Control;
    Follower meter;
    meter.set(Control::On, 1);
    meter.set(Control::Amount, 75);
    struct Reading { double rate; float percent, rise, fall, fourth, twelfth; };
    constexpr Reading readings[]{
        {44100, 12.5f, .10700142384f, .99951541424f, .18203955889f, .37141829729f},
        {48000, 20, .02554094791f, .99975728989f, .04915797710f, .13345009089f},
        {48000, 11, .13999295235f, .99948495626f, .22648695111f, .41815462708f},
        {88200, 17.6f, .02039110661f, .99984222651f, .03955170512f, .10951656103f},
        {96000, 18.75f, .01555687189f, .99986714125f, .03039520979f, .08575469255f},
        {192000, .5f, .64609569311f, .99916821718f, .49215644598f, .49999806285f}
    };
    for (const auto& row : readings) {
        meter.prepare(row.rate);
        meter.set(Control::Attack, row.percent);
        meter.set(Control::Release, 0);
        for (int i = 0; i < 12; ++i) {
            const float sign = i % 2 ? 1.f : -1.f;
            const float e = meter.apply(sign * .5f, -sign * .5f, 0);
            if (i == 0) {
                // The fastest held-out 192 kHz attack differs by one float step.
                const float tolerance = row.rate == 192000 ? 0x1p-24f : 0.f;
                check(std::abs(2.f * e - row.rise) <= tolerance, "measured first attack coefficient");
            }
            if (i == 3 || i == 11) {
                const float value = .5f + (e - .5f); // Read with the signed half-scale reference's rounding.
                check(std::abs(value - (i == 3 ? row.fourth : row.twelfth)) <= 0x1p-25f,
                      "independent signed attack trajectory at the fourth and twelfth frames");
            }
        }
        meter.reset();
        meter.set(Control::Attack, 0);
        meter.set(Control::Release, row.percent);
        float e = 0;
        for (int i = 0; i < 256 && e != .5f; ++i) e = meter.apply(-.5f, .25f, 0);
        check(e == .5f && 2.f * meter.apply(0, 0, 0) == row.fall, "measured first falling coefficient");
    }

    meter.prepare(48000);
    meter.set(Control::Threshold, 10);
    check(meter.apply(0, -std::nextafter(.1f, 0.f), 0) == 0 && meter.apply(0, -.1f, 0) > 0,
          "a physical threshold is normalized by division and includes the exact boundary");
    meter.set(Control::Threshold, 0);
    meter.set(Control::Release, 0);
    meter.prepare(1);
    check(meter.apply(1, 0, 0) == 1 && meter.apply(0, 0, 0) == 0,
          "an underflowed pole settles immediately at the slowest supported rate");
    meter.prepare(std::numeric_limits<double>::max());
    check(meter.apply(1, 0, 0) == 0, "the rising increment rounds to zero at extreme finite rates");
}

void followerSplitInputs() {
    constexpr std::size_t frames = 3072;
    std::array<float, frames> left{}, right{}, l{}, r{}, sumL{}, sumR{};
    for (std::size_t i = 0; i < frames; ++i) {
        const auto t = static_cast<double>(i) / 48000;
        left[i] = static_cast<float>(.41 * std::sin(2 * std::numbers::pi * 65 * t) + .13 * std::sin(2 * std::numbers::pi * 4100 * t));
        right[i] = static_cast<float>(.23 * std::cos(2 * std::numbers::pi * 870 * t));
    }
    Shaper rack;
    rack.set(Shaper::WidthBands, 3);
    for (int k = 0; k < 3; ++k) {
        const auto band = static_cast<Band>(k);
        const auto configure = [&](Shaper& processor, Band target) {
            flat(processor, Stage::Width, target, 70.f);
            setFollower(processor, target, Shaper::FollowerOn, 1);
            setFollower(processor, target, Shaper::FollowerMode, k == 0 ? 1.f : 0.f);
            setFollower(processor, target, Shaper::FollowerAmount, 70.f + static_cast<float>(k) * 10.f);
            setFollower(processor, target, Shaper::FollowerAttack, static_cast<float>(k) * 14.f);
            setFollower(processor, target, Shaper::FollowerHold, static_cast<float>(k) * 20.f);
        };
        configure(rack, band);
        Shaper isolate, full;
        isolate.set(Shaper::VolumeBands, 3);
        for (int j = 0; j < 3; ++j) flat(isolate, Stage::Volume, static_cast<Band>(j), j == k ? 100.f : 0.f);
        configure(full, Band::Mid);
        isolate.process(left.data(), right.data(), l.data(), r.data(), frames);
        full.process(l.data(), r.data(), l.data(), r.data(), frames);
        for (std::size_t i = 0; i < frames; ++i) {
            sumL[i] += l[i];
            sumR[i] += r[i];
        }
    }
    rack.process(left.data(), right.data(), l.data(), r.data(), frames);
    bool matches = true;
    for (std::size_t i = 0; i < frames; ++i)
        matches &= near(l[i], sumL[i], 2e-6) && near(r[i], sumR[i], 2e-6);
    check(matches && rack.latency() == 0, "three follower bands independently track their split stereo inputs");
}
void externalWidthKey() {
    constexpr std::size_t block = 256, plateau = 245760;
    constexpr std::array keys{std::array{0.f, 0.f}, std::array{.2f, 0.f}, std::array{0.f, -.4f},
                              std::array{.15f, -.15f}, std::array{0.f, 0.f}};
    constexpr std::array reference{.49999999, .89999102, 1.29998205, .79999100, .50000690};
    std::array<float, block> inL{}, inR{}, keyL{}, keyR{}, outL{}, outR{};
    inL.fill(.01f);
    inR.fill(-.01f);
    for (bool external : {false, true}) {
        Shaper s;
        flat(s, Stage::Width, Band::Mid, 50.f);
        for (auto setting : {std::pair{Shaper::FollowerOn, 1.f}, {Shaper::FollowerAmount, 75.f},
                             {Shaper::FollowerAttack, 50.f}, {Shaper::FollowerHold, 0.f},
                             {Shaper::FollowerRelease, 50.f}, {Shaper::FollowerThreshold, 0.f}})
            setFollower(s, Band::Mid, setting.first, setting.second);
        s.useExternalWidthKey(Band::Mid, external);
        for (std::size_t j = 0; j < keys.size(); ++j) {
            keyL.fill(keys[j][0]);
            keyR.fill(keys[j][1]);
            const Shaper::KeyInput key{keys[j][0] ? keyL.data() : nullptr, keys[j][1] ? keyR.data() : nullptr};
            double gain = 0.;
            bool opposite = true;
            for (std::size_t frame = 0; frame < plateau; frame += block) {
                s.process(inL.data(), inR.data(), outL.data(), outR.data(), block, key);
                if (frame < plateau - 2048) continue;
                for (std::size_t i = 0; i < block; ++i) {
                    gain += outL[i] / .01;
                    opposite &= outL[i] == -outR[i];
                }
            }
            check(std::abs(gain / 2048. - (external ? reference[j] : .51999944)) < .00005 && opposite,
                  "input or independent stereo key drives the measured Width plateau");
        }
        check(s.latency() == 0, "the external Width detector adds no delay");
    }
}

void keyedWidthAcrossRetriggers() {
    constexpr std::size_t length = 12017;
    std::array<std::vector<float>, 2> program, key;
    for (auto* buffer : {&program, &key})
        for (auto& channel : *buffer) channel.resize(length);
    for (std::size_t i = 0; i < length; ++i) {
        const float x = i % 1957 < 11 ? .95f : .02f;
        program[0][i] = x;
        program[1][i] = -.8f * x;
        key[0][i] = static_cast<float>((i / 43) % 31) * .025f;
        key[1][i] = -static_cast<float>((i / 67) % 19) * .043f;
    }
    const auto render = [&](std::size_t block, bool external, bool sendNotes) {
        auto out = program;
        Shaper rack;
        flat(rack, Stage::Width, Band::Mid, 50.f);
        setFollower(rack, Band::Mid, Shaper::FollowerOn, 1.f);
        setFollower(rack, Band::Mid, Shaper::FollowerAmount, 75.f);
        setFollower(rack, Band::Mid, Shaper::FollowerAttack, 0.f);
        setFollower(rack, Band::Mid, Shaper::FollowerHold, 0.f);
        setFollower(rack, Band::Mid, Shaper::FollowerRelease, 0.f);
        rack.useExternalWidthKey(Band::Mid, external);
        rack.useVolumeNotes(Band::Mid, true);
        rack.set(Shaper::TriggerLowMode, 1.f);
        rack.set(Shaper::TriggerMidMode, 1.f);
        rack.set(Shaper::bandParam(Stage::Volume, Band::Mid, Shaper::Curve), 1.f);
        for (int j = 0; j < 8; ++j) point(rack, Stage::Volume, Band::Mid, j, 12.5f * static_cast<float>(j));
        check(rack.latency() == 672, "an audio-triggered band retains its detector delay beside a note-triggered band");
        constexpr std::array<std::size_t, 3> notes{1507, 5581, 9803};
        for (std::size_t i = 0; i < length;) {
            auto end = std::min(length, i + block);
            for (auto at : notes) {
                if (i == at && sendNotes) rack.noteOn(127, 1);
                if (at > i) end = std::min(end, at);
            }
            rack.process(program[0].data() + i, program[1].data() + i,
                         out[0].data() + i, out[1].data() + i, end - i,
                         {key[0].data() + i, key[1].data() + i});
            i = end;
        }
        return out;
    };
    const auto oracle = render(1, true, true);
    check(oracle != render(256, false, true) && oracle != render(256, true, false),
          "the key planes and delivered notes independently affect the mixed fixture");
    for (std::size_t block : {37u, 256u, 2048u})
        check(oracle == render(block, true, true), "audio retriggers keep key offsets and note phases frame-exact");
}

void midiVolumeRetrigger() {
    constexpr int length = 96000, period = 12000;
    constexpr std::array notes{14000, 31300, 53500, 81400};
    for (int block : {256, 2048}) for (int mode : {1, 2}) {
        Shaper s;
        s.useVolumeNotes(Band::Mid, true);
        s.set(Shaper::TriggerMidMode, static_cast<float>(mode));
        s.set(Shaper::TriggerMidEnd, 75.f);
        s.set(Shaper::bandParam(Stage::Volume, Band::Mid, Shaper::Cycle), .5f);
        s.set(Shaper::bandParam(Stage::Volume, Band::Mid, Shaper::Curve), 1.f);
        for (int i = 0; i < 8; ++i) point(s, Stage::Volume, Band::Mid, i, 12.5f * static_cast<float>(i));
        std::vector<float> left(length, .25f), right(length, -.125f);
        int frame = 0;
        while (frame < length) {
            int end = std::min(length, (frame / block + 1) * block);
            for (int on : notes) {
                if (frame == on) s.noteOn(60, 100);
                if (frame == on + 1440) s.noteOn(60, 0);
                for (int event : {on, on + 1440}) if (event > frame) end = std::min(end, event);
            }
            s.sync(static_cast<double>(frame) / 24000.);
            s.process(left.data()+frame, right.data()+frame, left.data()+frame, right.data()+frame, end-frame);
            frame = end;
        }
        bool exact = true;
        int last = -1;
        double worst = 0.;
        for (int i = 0; i < length; ++i) {
            if (std::find(notes.begin(), notes.end(), i) != notes.end()) {
                last = i;
                exact &= left[i] == 0.f && right[i] == 0.f && left[i-1] > .001f && left[i+1] > 0.f;
            }
            const int age = last < 0 ? i : i-last;
            const double pos = mode == 2 ? (last < 0 ? .75 : std::min(.75, static_cast<double>(age)/period))
                                         : static_cast<double>(age % period)/period;
            const double gain = pos < .875 ? pos : 7. * (1. - pos);
            worst = std::max({worst, std::abs(left[i] - .25*gain), std::abs(right[i] + .125*gain)});
        }
        check(exact && worst < 1.e-6 && s.latency() == 0,
              "four sample-addressed notes restart loop and one-shot contours and ignore note-off velocity");
    }
}

} // namespace

int main() {
    endpoints();
    balanceSweep();
    heldPanEdges();
    panDelayBlend();
    beatGate();
    clocksAndLinearCurve();
    audioTrigger();
    externalWidthKey();
    midiVolumeRetrigger();
    keyedWidthAcrossRetriggers();
    splitBands();
    independentBandClocks();
    steepCrossovers();
    firstOrderBandImpulses();
    movingFirstOrderSplits();
    crossoverSpacing();
    bypassAndInvalid();
    nonfiniteAudio();
    finiteDriveOverflow();
    driveDryOverflowAtBlockEdge();
    clockLimitsAndAliases();
    modulationStack();
    modulationBands();
    driveDryAndLatency();
    driveAndCrush();
    crushHoldClockBlocks();

    followerSignedPeaks();
    followerLevels();
    followerTimeConstants();
    followerPoleLandmarks();
    followerSplitInputs();
    if (failures == 0) std::puts("ADI-Shaper DSP fixtures passed");
    return failures == 0 ? 0 : 1;
}
