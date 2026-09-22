// SPDX-License-Identifier: GPL-3.0-or-later
//
// Hearing what a real plugin did: render a device, then measure the pitch that
// actually sounds. For the probes only (ADR-0098) -- not part of adi_core.
//
// "The plugin produced audio" proves an event arrived. It does not prove the
// event MEANT what we intended: a bend on the wrong channel, at the wrong range,
// or applied to every note instead of one still makes sound. So the MPE
// acceptance tests measure frequencies, and this file is how.
//
// Both measurements are checked against synthetic signals by `selfTest()`
// before any plugin is judged by them.

#pragma once

#include "adi/engine/events.hpp"
#include "adi/engine/graph.hpp"
#include "juce/device_model.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace adi::probe {

inline constexpr double kPi = 3.14159265358979323846;

/// Equal temperament at A4 = 440 Hz.
inline double midiHz(double key) { return 440.0 * std::pow(2.0, (key - 69.0) / 12.0); }

inline double centsBetween(double hz, double refHz) {
    return (hz > 0.0 && refHz > 0.0) ? 1200.0 * std::log2(hz / refHz) : 1e9;
}

/// Render `blocks` blocks of `blockSize` from a prepared device, handing it
/// `atStart` in the first block (frames block-relative, ADR-0081). The left
/// channel, concatenated.
inline std::vector<float> render(device::DeviceInstance& dev, const std::vector<engine::Event>& atStart,
                                 int blocks, int blockSize = 512, double sampleRate = 48000.0) {
    const auto bs = static_cast<std::size_t>(blockSize);
    std::vector<float> l(bs, 0.0f), r(bs, 0.0f), all;
    all.reserve(bs * static_cast<std::size_t>(blocks));
    float* out[2] = {l.data(), r.data()};
    for (int b = 0; b < blocks; ++b) {
        engine::NodeIo io;
        io.out = out;
        io.channels = 2;
        io.frames = blockSize;
        io.sampleRate = sampleRate;
        io.inputSilent = true;
        if (b == 0 && !atStart.empty())
            io.events = engine::EventSpan{atStart.data(), static_cast<std::int32_t>(atStart.size())};
        dev.process(io);
        all.insert(all.end(), l.begin(), l.end());
    }
    return all;
}

/// The fundamental of `n` samples starting at `from`, by YIN's cumulative
/// mean normalised difference with parabolic interpolation. `lo`..`hi` bound
/// the search. 0 when nothing periodic is found.
inline double estimateHz(const std::vector<float>& x, std::size_t from, std::size_t n,
                         double sampleRate, double lo, double hi) {
    const auto tauMin = static_cast<std::size_t>(sampleRate / hi);
    const auto tauMax = static_cast<std::size_t>(sampleRate / lo);
    if (from + n > x.size() || n <= tauMax + 2 || tauMin < 2) return 0.0;
    const std::size_t w = n - tauMax - 2;
    std::vector<double> d(tauMax + 2, 0.0), c(tauMax + 2, 1.0);
    for (std::size_t tau = 1; tau < tauMax + 2; ++tau) {
        double s = 0.0;
        for (std::size_t i = 0; i < w; ++i) {
            const double diff = static_cast<double>(x[from + i]) - static_cast<double>(x[from + i + tau]);
            s += diff * diff;
        }
        d[tau] = s;
    }
    double run = 0.0;
    for (std::size_t tau = 1; tau < tauMax + 2; ++tau) {
        run += d[tau];
        c[tau] = run > 0.0 ? d[tau] * static_cast<double>(tau) / run : 1.0;
    }
    std::size_t best = 0;
    for (std::size_t tau = tauMin; tau <= tauMax; ++tau) {
        if (c[tau] < 0.15) {
            while (tau + 1 <= tauMax && c[tau + 1] < c[tau]) ++tau;
            best = tau;
            break;
        }
    }
    if (best == 0) {
        best = tauMin;
        for (std::size_t tau = tauMin; tau <= tauMax; ++tau) if (c[tau] < c[best]) best = tau;
        if (c[best] > 0.5) return 0.0;
    }
    const double a = d[best - 1], b = d[best], cc = d[best + 1];
    const double denom = a - 2.0 * b + cc;
    const double shift = denom != 0.0 ? 0.5 * (a - cc) / denom : 0.0;
    return sampleRate / (static_cast<double>(best) + shift);
}

/// The magnitude at exactly `hz` over `n` samples from `from`, Hann-windowed
/// (Goertzel). Compare levels with each other, not with a constant.
inline double toneLevel(const std::vector<float>& x, std::size_t from, std::size_t n,
                        double sampleRate, double hz) {
    if (from + n > x.size() || n < 2) return 0.0;
    const double k = 2.0 * std::cos(2.0 * kPi * hz / sampleRate);
    double s1 = 0.0, s2 = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double win = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) /
                                                static_cast<double>(n - 1));
        const double s = static_cast<double>(x[from + i]) * win + k * s1 - s2;
        s2 = s1;
        s1 = s;
    }
    const double p = s1 * s1 + s2 * s2 - k * s1 * s2;
    return std::sqrt(p > 0.0 ? p : 0.0) / static_cast<double>(n);
}

inline double dbRel(double level, double ref) {
    return (level > 0.0 && ref > 0.0) ? 20.0 * std::log10(level / ref) : -200.0;
}

/// A band-limited sawtooth: the shape a synth's init patch usually is.
inline std::vector<float> saw(double hz, double sampleRate, std::size_t n, double gain = 0.3) {
    std::vector<float> v(n, 0.0f);
    for (int h = 1; hz * h < sampleRate / 2.0; ++h)
        for (std::size_t i = 0; i < n; ++i)
            v[i] += static_cast<float>(gain / h * std::sin(2.0 * kPi * hz * h *
                                                          static_cast<double>(i) / sampleRate));
    return v;
}

/// The measurements, checked on signals whose answer is known. False means
/// no plugin result may be trusted.
inline bool selfTest(double* worstCents = nullptr) {
    const double sr = 48000.0;
    double worst = 0.0;
    for (double hz : {261.6256, 329.6276, 392.0, 523.2511}) {
        const auto v = saw(hz, sr, 16384);
        const double got = estimateHz(v, 0, 16384, sr, 80.0, 1200.0);
        worst = std::max(worst, std::abs(centsBetween(got, hz)));
    }
    if (worstCents != nullptr) *worstCents = worst;

    // Two notes at once: both present, and a third, absent, frequency is not.
    auto a = saw(392.0, sr, 16384);
    const auto b = saw(329.6276, sr, 16384);
    for (std::size_t i = 0; i < a.size(); ++i) a[i] += b[i];
    const double l392 = toneLevel(a, 0, 16384, sr, 392.0);
    const double l330 = toneLevel(a, 0, 16384, sr, 329.6276);
    const double l494 = toneLevel(a, 0, 16384, sr, 493.8833);
    const double l262 = toneLevel(a, 0, 16384, sr, 261.6256);
    const double top = std::max(l392, l330);
    return worst < 1.0 && dbRel(l392, top) > -3.0 && dbRel(l330, top) > -3.0 &&
           dbRel(l494, top) < -40.0 && dbRel(l262, top) < -40.0;
}

}  // namespace adi::probe
