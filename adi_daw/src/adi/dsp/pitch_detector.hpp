// SPDX-License-Identifier: GPL-3.0-or-later
//
// A monophonic pitch detector (DEV-22's Tuner; Auto Shift shares it later):
// YIN's cumulative-mean-normalised difference, refined to a fraction of a
// cent.
//
//   1. The coarse lag: the difference function from an FFT autocorrelation
//      (one transform pair an analysis, not window x lags products), its
//      first dip under 0.15 followed to its bottom, or else its least value.
//   2. The fine period: the difference recomputed exactly, in double, at the
//      lag's highest multiple inside half the window, walked to its least
//      value, and a parabola through that and its neighbours. The vertex's
//      error is divided by the multiple, so a 4 kHz period (12 samples at
//      48 kHz) is resolved as finely as a 40 Hz one.
//
// The window is the least power of two that holds two of the longest period
// (4,096 samples at 44.1 and 48 kHz, 8,192 at 88.2 and 96 kHz); an estimate
// is made every quarter window on the last window, its mean removed, and a
// window under -80 dBFS RMS reads nothing. prepare() allocates on the message
// thread and never throws; push() and analyse() do not allocate. 30 Hz to
// 5 kHz.

#pragma once

#include "adi/dsp/spectrum.hpp"

#include <complex>
#include <cstddef>
#include <memory>
#include <vector>

namespace adi::dsp {

class PitchDetector {
public:
    static constexpr double kLowestHz = 30, kHighestHz = 5000, kThreshold = 0.15;
    /// The least RMS level read, once the window's mean is removed: -80 dBFS.
    static constexpr double kFloor = 1e-4;

    PitchDetector();
    /// Message thread: the window, the transform and its scratch. It never
    /// throws: when memory or a plan cannot be had the detector is inert.
    void prepare(double sampleRate) noexcept;
    /// Audio thread: mono samples in; an estimate every hop() samples.
    void push(const float* x, std::size_t frames) noexcept;
    /// The last estimate: its frequency, and its clarity (1 minus YIN's
    /// normalised difference at the period: near 1 for a clean tone). False
    /// while there is none, or when the last window held no tone.
    [[nodiscard]] bool estimate(double& hz, double& clarity) const noexcept;
    /// One window's estimate, for tests: `window` holds window() samples.
    [[nodiscard]] bool analyse(const float* window, double& hz, double& clarity) noexcept;
    [[nodiscard]] std::size_t window() const noexcept { return size_; }
    [[nodiscard]] std::size_t hop() const noexcept { return size_ / 4; }
    /// Forget the held samples and the estimate (a transport stop).
    void reset() noexcept;

private:
    [[nodiscard]] static double difference(const float* w, std::size_t lag, std::size_t length) noexcept;
    std::unique_ptr<RealFft> fft_;
    std::vector<float> ring_, window_, padded_;
    std::vector<std::complex<float>> bins_;
    std::vector<double> d_, energy_;
    std::size_t size_ = 0, write_ = 0, sinceLast_ = 0, filled_ = 0;
    double rate_ = 48000, hz_ = 0, clarity_ = 0;
    bool found_ = false;
};

}  // namespace adi::dsp
