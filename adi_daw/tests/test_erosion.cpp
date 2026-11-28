// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live 12's Erosion, the core (DEV-22). The reference numbers come from
// phase modulation's own law, apart from the code: a tone of f through a
// delay D (1 + sin(2 pi fm t)) is phase-modulated by beta = 2 pi f D, so its
// carrier is J0(beta) and each first sideband, at f +- fm, J1(beta) of the
// tone. Sine mode must give those within 0.05 dB. Noise puts the haze around
// f +- Frequency; Wide Noise differs between the channels and Noise does not;
// Amount 0 is the input itself. Parity with Live waits on a reading.
#include "adi/dsp/erosion.hpp"
#include "adi/dsp/spectrum.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
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
using E = adi::dsp::Erosion;
constexpr double kRate = 48000;

std::vector<float> sine(double hz, std::size_t frames, double amplitude = 0.5) {
    std::vector<float> x(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        x[i] = static_cast<float>(amplitude * std::sin(2 * std::numbers::pi * hz * static_cast<double>(i) / kRate));
    }
    return x;
}

/// The amplitude of the component at `hz` over the last whole second.
double amplitude(const std::vector<float>& y, double hz) {
    std::complex<double> sum = 0;
    const std::size_t n = 48000, from = y.size() - n;
    for (std::size_t i = 0; i < n; ++i) {
        sum += static_cast<double>(y[from + i]) * std::polar(1.0, -2 * std::numbers::pi * hz * static_cast<double>(from + i) / kRate);
    }
    return 2 * std::abs(sum) / static_cast<double>(n);
}

void run(E& e, const std::vector<float>& l, const std::vector<float>& r, std::vector<float>& outL, std::vector<float>& outR,
         std::size_t block = 512) {
    outL.assign(l.size(), 0.f);
    outR.assign(l.size(), 0.f);
    for (std::size_t at = 0; at < l.size(); at += block) {
        const auto n = std::min(block, l.size() - at);
        e.process(l.data() + at, r.data() + at, outL.data() + at, outR.data() + at, n);
    }
}

void besselSidebands() {
    for (double amount : {100.0, 50.0, 20.0}) {
        E e;
        e.set(E::Mode, E::Sine);
        e.set(E::Frequency, 5000);
        e.set(E::Amount, amount);
        e.prepare(kRate);
        const auto x = sine(1000, 96000);
        std::vector<float> l, r;
        run(e, x, x, l, r);
        const double beta = 2 * std::numbers::pi * 1000 * (amount / 100 * E::kDeviationMs / 1000);
        const double carrier = amplitude(l, 1000), upper = amplitude(l, 6000), lower = amplitude(l, 4000);
        const double wantCarrier = 0.5 * std::cyl_bessel_j(0.0, beta), wantSide = 0.5 * std::cyl_bessel_j(1.0, beta);
        const auto db = [](double a, double b) { return std::abs(20 * std::log10(a / b)); };
        check(db(carrier, wantCarrier) < 0.05 && db(upper, wantSide) < 0.05 && db(lower, wantSide) < 0.05,
              "Sine, Amount " + std::to_string(amount) + ": carrier J0 and sidebands J1 of beta " + std::to_string(beta) +
                  " within 0.05 dB (" + std::to_string(carrier) + ", " + std::to_string(upper) + ", " + std::to_string(lower) +
                  " against " + std::to_string(wantCarrier) + ", " + std::to_string(wantSide) + ")");
        check(r == l, "Sine modulates both channels alike");
    }
    // A plant: a depth law 10 % off moves the sideband by over ten times the
    // tolerance.
    const double beta = 2 * std::numbers::pi * 1000 * (E::kDeviationMs / 1000);
    check(std::abs(20 * std::log10(std::cyl_bessel_j(1.0, 1.1 * beta) / std::cyl_bessel_j(1.0, beta))) > 0.5,
          "plant: a depth 10 % off is far outside the tolerance");
}

void noiseBands() {
    const auto x = sine(1000, 65536);
    // The haze's spectrum, the output less its carrier and m squared's
    // products beside it, in 100 Hz bands: m's band (a two-pole band pass)
    // shifted by the tone, so its strongest band sits at centre + 1 kHz or
    // centre - 1 kHz, and a narrower Width gathers more of it there.
    const auto haze = [&](double centre, double width, double& peakHz) {
        E e;
        e.set(E::Mode, E::Noise);
        e.set(E::Frequency, centre);
        e.set(E::Width, width);
        e.set(E::Amount, 25);   // small beta: the second-order products stay low
        e.prepare(kRate);
        std::vector<float> l, r;
        run(e, x, x, l, r, 333);
        check(l == r, "Noise modulates both channels alike");
        adi::dsp::RealFft fft(32768);
        std::vector<float> frame(l.end() - 32768, l.end());
        std::vector<std::complex<float>> bins(16385);
        for (std::size_t i = 0; i < frame.size(); ++i) {
            frame[i] *= static_cast<float>(0.5 - 0.5 * std::cos(2 * std::numbers::pi * static_cast<double>(i) / 32768));
        }
        fft.forward(frame, bins);
        std::vector<double> bands(240, 0.0);
        double near = 0, rest = 0;
        for (std::size_t k = 1; k < bins.size(); ++k) {
            const double hz = static_cast<double>(k) * kRate / 32768, p = std::norm(bins[k]);
            if (std::abs(hz - 1000) < 1500) {
                continue;
            }
            bands[std::min<std::size_t>(239, static_cast<std::size_t>(hz / 100))] += p;
            (std::abs(hz - (centre + 1000)) < 1200 || std::abs(hz - (centre - 1000)) < 1200 ? near : rest) += p;
        }
        peakHz = 100 * static_cast<double>(std::max_element(bands.begin(), bands.end()) - bands.begin()) + 50;
        return 10 * std::log10(near / rest);
    };
    for (double centre : {5000.0, 12000.0}) {
        double peak = 0;
        const double narrow = haze(centre, 0.1, peak);
        check(std::abs(peak - (centre + 1000)) <= 150 || std::abs(peak - (centre - 1000)) <= 150,
              "Noise at " + std::to_string(centre) + " Hz: the haze peaks at the tone +- Frequency (" + std::to_string(peak) + " Hz)");
        double widePeak = 0;
        const double broad = haze(centre, 2, widePeak);
        check(narrow > 5 && narrow > broad + 3, "Noise at " + std::to_string(centre) + " Hz: Width 0.1 gathers the haze (" +
                                                    std::to_string(narrow) + " dB against " + std::to_string(broad) + " at Width 2)");
    }
    E wide;
    wide.set(E::Mode, E::WideNoise);
    wide.set(E::Amount, 100);
    wide.prepare(kRate);
    std::vector<float> l, r;
    run(wide, x, x, l, r);
    double diff = 0;
    for (std::size_t i = 0; i < l.size(); ++i) {
        diff = std::max(diff, static_cast<double>(std::abs(l[i] - r[i])));
    }
    check(diff > 1e-3, "Wide Noise modulates the channels apart");
}

void identityAndRobustness() {
    std::vector<float> x(20000), y(20000);
    for (std::size_t i = 0; i < x.size(); ++i) {
        x[i] = static_cast<float>(0.3 * std::sin(0.013 * static_cast<double>(i)) + 0.2 * std::sin(1.9 * static_cast<double>(i)));
        y[i] = static_cast<float>(0.1 * std::sin(0.7 * static_cast<double>(i)));
    }
    E zero;
    zero.set(E::Amount, 0);
    zero.prepare(kRate);
    std::vector<float> l, r;
    run(zero, x, y, l, r);
    check(l == x && r == y, "Amount 0 is the input itself");
    std::vector<float> first;
    bool identical = true;
    for (std::size_t block : {1u, 64u, 333u, 4096u}) {
        E e;
        e.set(E::Mode, E::WideNoise);
        e.set(E::Amount, 80);
        e.prepare(kRate);
        run(e, x, y, l, r, block);
        if (first.empty()) {
            first = l;
        } else {
            identical = identical && l == first;
        }
    }
    check(identical, "every block size gives the same samples");
    E e;
    e.set(E::Amount, 100);
    // Each block is checked as it comes out (the review of a1f60f9b: one
    // check after three blocks saw only the last).
    std::vector<float> bad(512, std::numeric_limits<float>::quiet_NaN()), o(512), p(512);
    const auto silent = [&] { return std::all_of(o.begin(), o.end(), [](float v) { return v == 0.f; }); };
    const auto bounded = [&] { return std::all_of(o.begin(), o.end(), [](float v) { return std::isfinite(v) && std::abs(v) < 1; }); };
    counting = true;
    e.process(bad.data(), bad.data(), o.data(), p.data(), bad.size());
    const bool nanSilent = silent();
    e.set(E::Mode, E::Sine);
    e.set(E::Frequency, 17000);
    e.process(x.data(), x.data(), o.data(), p.data(), 512);
    const bool sineBounded = bounded();
    e.set(E::Mode, E::WideNoise);
    e.set(E::Width, 10);
    e.process(x.data(), x.data(), o.data(), p.data(), 512);
    const bool wideBounded = bounded();
    counting = false;
    check(allocations == 0, "processing and moved controls allocate nothing");
    check(nanSilent, "a NaN input is silence");
    check(sineBounded && wideBounded, "after it, Sine at 17 kHz and Wide Noise at Width 10 stay finite and bounded");
    E z;
    z.set(E::Amount, 0);
    std::vector<float> left(4800, 0.25f), shared(4800, -0.5f), rightOut(4800);
    z.process(left.data(), shared.data(), shared.data(), rightOut.data(), 4800);
    check(rightOut.back() == -0.5f && shared.back() == 0.25f, "an output plane that is the other channel's input is read first");
}

}  // namespace

/// The noise modulator's level, read from the output (the review of
/// a1f60f9b: the scale assumed a small-angle noise bandwidth, and the noise
/// fell up to 12 dB short at a high, wide band). For a tone A at f and a
/// small swing, y = x(t - D) - A w D m cos(...), so the haze, the output less
/// its carrier, has a power of A^2 (w D)^2 E[m^2] / 2: m's RMS follows. Band
/// passed noise of RMS 0.5 clipped at +-1 (two sigma) keeps an RMS of 0.48.
void noiseLevel() {
    struct Band {
        double hz, width;
    };
    for (const Band b : {Band{1000, 1}, Band{5000, 10}, Band{12000, 1}, Band{18000, 0.08}, Band{18000, 10}}) {
        E e;
        e.set(E::Mode, E::Noise);
        e.set(E::Frequency, b.hz);
        e.set(E::Width, b.width);
        e.set(E::Amount, 20);
        e.prepare(kRate);
        const double a = 0.4;
        const auto x = sine(1000, 96000, a);
        std::vector<float> l, r;
        run(e, x, x, l, r);
        double c = 0, s = 0, total = 0;
        const std::size_t n = 48000, from = l.size() - n;
        for (std::size_t i = 0; i < n; ++i) {
            const double phase = 2 * std::numbers::pi * 1000 * static_cast<double>(from + i) / kRate, y = l[from + i];
            c += y * std::cos(phase);
            s += y * std::sin(phase);
            total += y * y;
        }
        c *= 2.0 / static_cast<double>(n);
        s *= 2.0 / static_cast<double>(n);
        const double haze = total / static_cast<double>(n) - (c * c + s * s) / 2;
        const double wd = 2 * std::numbers::pi * 1000 / kRate * (0.2 * E::kDeviationMs / 1000 * kRate);
        const double rms = std::sqrt(2 * haze / (a * a * wd * wd));
        check(std::abs(rms - 0.48) < 0.04, "Noise at " + std::to_string(b.hz) + " Hz, Width " + std::to_string(b.width) +
                                               ": the modulator's RMS is 0.48 (" + std::to_string(rms) + ")");
    }
}

/// At 768 kHz the swing at Amount 100 (153.6 samples) would leave the ring:
/// it is held at kMaxDepth, and the output is still that phase modulation.
void highRate() {
    const double rate = 768000;
    E e;
    e.set(E::Mode, E::Sine);
    e.set(E::Frequency, 5000);
    e.set(E::Amount, 100);
    e.prepare(rate);
    std::vector<float> x(static_cast<std::size_t>(rate * 1.5)), l(x.size()), r(x.size());
    for (std::size_t i = 0; i < x.size(); ++i) {
        x[i] = static_cast<float>(0.5 * std::sin(2 * std::numbers::pi * 1000 * static_cast<double>(i) / rate));
    }
    e.process(x.data(), x.data(), l.data(), r.data(), x.size());
    const auto at = [&](double hz) {
        std::complex<double> sum = 0;
        const auto n = static_cast<std::size_t>(rate), from = l.size() - n;
        for (std::size_t i = 0; i < n; ++i) {
            sum += static_cast<double>(l[from + i]) * std::polar(1.0, -2 * std::numbers::pi * hz * static_cast<double>(from + i) / rate);
        }
        return 2 * std::abs(sum) / static_cast<double>(n);
    };
    const double beta = 2 * std::numbers::pi * 1000 / rate * E::kMaxDepth;
    const double carrier = at(1000), side = at(6000);
    check(std::abs(20 * std::log10(carrier / (0.5 * std::cyl_bessel_j(0.0, beta)))) < 0.05 &&
              std::abs(20 * std::log10(side / (0.5 * std::cyl_bessel_j(1.0, beta)))) < 0.05,
          "768 kHz, Amount 100: the swing is held inside the ring, J0 and J1 of its beta (" + std::to_string(carrier) + ", " +
              std::to_string(side) + ")");
}

int main() {
    noiseLevel();
    highRate();
    besselSidebands();
    noiseBands();
    identityAndRobustness();
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAILED" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
