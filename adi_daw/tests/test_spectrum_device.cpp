// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live 12's Spectrum as a track device (DEV-22), the lane's side. The signal
// passes untouched; the controls become the shared analyser's settings. The
// reference numbers are the controls' own meanings and the analyser's
// calibration: driven with the device's settings, dsp::Spectrum and
// dsp::LinearMagnitudeAverage read a bin-centred sine of 0.5 at -6.02 dBFS,
// each analysis moves the average 1/Avg of the way, and Channel picks L, R or
// their mean. Parity with Live waits on a reading.
#include "adi/dsp/spectrum_device.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <numbers>
#include <string>
#include <vector>

namespace {
bool counting = false;
std::size_t allocations = 0;
int checks = 0, failures = 0;
void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}
}  // namespace
void* operator new(std::size_t n) {
    if (counting) {
        ++allocations;
    }
    if (void* p = std::malloc(n ? n : 1)) {
        return p;
    }
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {
using D = adi::dsp::SpectrumDevice;

/// The shared analyser, as the Mix View's worker would run it for `d`: the
/// device's output, picked by Channel, a frame every hop, averaged. Returns
/// the averaged magnitudes after `analyses` frames.
std::vector<double> analyse(const D& d, const std::vector<float>& l, const std::vector<float>& r, int analyses) {
    const auto& a = d.analysis();
    adi::dsp::Spectrum fft(a.fftSize, a.window);
    adi::dsp::LinearMagnitudeAverage average;
    if (!fft.valid() || !average.prepare(a.average)) {
        return {};
    }
    std::vector<float> frame(static_cast<std::size_t>(a.fftSize)), mags(static_cast<std::size_t>(fft.bins()));
    for (int k = 0; k < analyses; ++k) {
        const auto end = static_cast<std::size_t>(a.fftSize) + static_cast<std::size_t>(k) * static_cast<std::size_t>(a.hopSamples);
        for (std::size_t i = 0; i < frame.size(); ++i) {
            const std::size_t at = end - frame.size() + i;
            frame[i] = D::pick(a.channel, l[at], r[at]);
        }
        fft.analyse(frame, mags);
        (void)average.processTrustedFinite(std::span<const float>(mags));
    }
    const auto m = average.magnitudes();
    return {m.begin(), m.end()};
}

std::vector<float> sine(double hz, double rate, std::size_t frames, double amplitude) {
    std::vector<float> x(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        x[i] = static_cast<float>(amplitude * std::sin(2 * std::numbers::pi * hz * static_cast<double>(i) / rate));
    }
    return x;
}

void settings() {
    D d;
    for (int block = 0; block <= 5; ++block) {
        d.set(D::Block, block);
        check(d.analysis().fftSize == (1024 << block) && d.analysis().average.bins == static_cast<std::size_t>((1024 << block) / 2 + 1),
              "Block " + std::to_string(block) + ": a " + std::to_string(1024 << block) + "-point analysis");
    }
    d.set(D::Refresh, 60);
    check(d.analysis().hopSamples == 2880, "Refresh 60 ms at 48 kHz: a hop of 2,880 samples");
    d.prepare(44100);
    check(d.analysis().hopSamples == 2646, "and at 44.1 kHz, 2,646");
    // From Left, so a rejected value (which would leave Left) is told apart
    // from a clamped one (the second review: from the default, Sum, both
    // read the same).
    d.set(D::Channel, D::Left);
    d.set(D::Channel, 7);
    check(d.analysis().channel == D::Sum, "Channel is clamped to its three choices");
    // Refresh is the hop at every rate, over its whole range: 2 s at 96 kHz
    // is 192,000 samples, past the averager's own 65,536, and each analysis
    // still moves the average 1/Avg of the way.
    d.prepare(96000);
    d.set(D::Refresh, 2000);
    d.set(D::Avg, 4);
    check(d.analysis().hopSamples == 192000, "Refresh 2 s at 96 kHz: a hop of 192,000 samples (" +
                                                 std::to_string(d.analysis().hopSamples) + ")");
    adi::dsp::LinearMagnitudeAverage avg;
    check(avg.prepare(d.analysis().average) && std::abs(avg.coefficient() - 0.25) < 1e-12,
          "and the averager moves 1/Avg an analysis (" + std::to_string(avg.coefficient()) + ")");
    d.set(D::Refresh, 1500);
    d.prepare(48000);
    check(d.analysis().hopSamples == 72000, "Refresh 1.5 s at 48 kHz: 72,000 samples, not held at 65,536");
    check(D::pick(D::Left, 0.25f, -0.5f) == 0.25f && D::pick(D::Right, 0.25f, -0.5f) == -0.5f && D::pick(D::Sum, 0.25f, -0.5f) == -0.125f,
          "Channel picks L, R, or their mean");
}

void calibrated() {
    const double rate = 48000;
    for (int block : {0, 2, 5}) {
        D d;
        d.set(D::Block, block);
        d.set(D::Avg, 1);
        d.prepare(rate);
        const int n = 1024 << block, bin = n / 16;
        const double hz = rate * bin / n;
        const auto x = sine(hz, rate, static_cast<std::size_t>(n) + 4 * static_cast<std::size_t>(d.analysis().hopSamples), 0.5);
        std::vector<float> l(x.size()), r(x.size());
        d.process(x.data(), x.data(), l.data(), r.data(), x.size());
        const auto m = analyse(d, l, r, 4);
        const double db = m.empty() ? -999 : 20 * std::log10(m[static_cast<std::size_t>(bin)]);
        check(std::abs(db - 20 * std::log10(0.5)) < 0.01, std::to_string(n) + "-point: a bin-centred sine of 0.5 reads -6.02 dBFS (" +
                                                              std::to_string(db) + ")");
    }
    // Channel L+R: a sine on the left alone reads half its level.
    D sum;
    sum.prepare(rate);
    const auto x = sine(rate * 128 / 2048, rate, 2048 + 4 * 2880, 0.5);
    const std::vector<float> zero(x.size(), 0.f);
    std::vector<float> l(x.size()), r(x.size());
    sum.process(x.data(), zero.data(), l.data(), r.data(), x.size());
    auto m = analyse(sum, l, r, 4);
    check(std::abs(20 * std::log10(m[128]) - 20 * std::log10(0.25)) < 0.01, "L+R: a sine on the left alone reads at half its level");
    sum.set(D::Channel, D::Right);
    m = analyse(sum, l, r, 4);
    check(m[128] < 1e-6, "Channel R does not see the left");
}

void averaging() {
    // Avg 4: from silence, the first analysis gives a quarter of the level,
    // the second 1 - (3/4)^2 of it, the third 1 - (3/4)^3.
    const double rate = 48000;
    D d;
    d.set(D::Avg, 4);
    d.prepare(rate);
    const auto x = sine(rate * 64 / 2048, rate, 2048 + 4 * 2880, 0.5);
    for (int k = 1; k <= 3; ++k) {
        const auto m = analyse(d, x, x, k);
        const double want = 0.5 * (1 - std::pow(0.75, k));
        check(std::abs(m[64] / want - 1) < 1e-6, "Avg 4, analysis " + std::to_string(k) + ": the average is 1 - (3/4)^" + std::to_string(k) +
                                                     " of the level (" + std::to_string(m[64]) + ")");
    }
    // Avg 1 replaces the average with each analysis.
    d.set(D::Avg, 1);
    auto m = analyse(d, x, x, 1);
    check(std::abs(m[64] / 0.5 - 1) < 1e-6, "Avg 1: each analysis replaces the last");
}

void passThrough() {
    D d;
    std::vector<float> x(4096), y(4096), l(4096), r(4096);
    for (std::size_t i = 0; i < x.size(); ++i) {
        x[i] = static_cast<float>(std::sin(0.01 * static_cast<double>(i)));
        y[i] = static_cast<float>(std::cos(0.3 * static_cast<double>(i)));
    }
    x[7] = std::numeric_limits<float>::quiet_NaN();
    counting = true;
    d.process(x.data(), y.data(), l.data(), r.data(), x.size());
    d.set(D::Block, 5);
    d.set(D::Avg, 30);
    d.set(D::Refresh, 1000);
    d.set(D::Graph, 2);
    (void)d.analysis();
    counting = false;
    check(allocations == 0, "processing and moved controls allocate nothing");
    check(std::equal(l.begin(), l.end(), x.begin(), [](float a, float b) { return a == b || (std::isnan(a) && std::isnan(b)); }) && r == y,
          "the signal passes untouched, a NaN as it came");
    std::vector<float> left(4800, 0.25f), shared(4800, -0.5f), rightOut(4800);
    d.process(left.data(), shared.data(), shared.data(), rightOut.data(), 4800);
    check(shared.back() == 0.25f && rightOut.back() == -0.5f, "an output plane that is the other channel's input is read first");
}

}  // namespace

int main() {
    settings();
    calibrated();
    averaging();
    passThrough();
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAILED" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
