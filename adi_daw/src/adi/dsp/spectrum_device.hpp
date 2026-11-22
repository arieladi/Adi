// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live 12's Spectrum as a track device, the lane's side (DEV-22; lands after
// 3 December). The signal passes untouched. The analysis is the Mix View's
// shared analyser's (ADR-0183 d7, MX-12, MX-30): it reads the track at the
// device's place and runs dsp::Spectrum and dsp::LinearMagnitudeAverage on
// its worker, with the settings this core derives from the controls. The
// device runs no FFT of its own (CTL-07: producers run none). The contract is
// pd/devices/Spectrum.md.
//
//   Block      1,024 to 32,768 samples (six powers of two): the FFT's size.
//   Channel    L, R or L+R (the mean of the two).
//   Refresh    10 to 2,000 ms: how often an analysis is made (the hop).
//   Avg        1 to 100: each analysis moves the average 1/Avg of the way.
//   Graph, Scale X, Auto, Range Low, Range High: how it is drawn, stored
//              with the set, nothing in the signal or the analysis.
//
// WHAT IS NOT LIVE'S YET, and says so: the controls' ranges and defaults,
// Avg's law and the drawing controls' names wait on a Live 12.4 reading of
// Spectrum (the devices log's ONE list). Allocation-free; no latency.

#pragma once

#include "adi/dsp/spectrum.hpp"
#include "adi/dsp/spectrum_average.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace adi::dsp {

class SpectrumDevice {
public:
    enum Param { Block, Channel, Refresh, Avg, Graph, ScaleX, Auto, RangeLow, RangeHigh, Count };
    enum ChannelKind { Left, Right, Sum };
    struct Range {
        double low, high, initial;
    };
    static constexpr Range ranges[Count] = {{0, 5, 1},      {0, 2, 2}, {10, 2000, 60}, {1, 100, 1},  {0, 2, 0},
                                            {0, 2, 1},      {0, 1, 1}, {-120, 0, -80}, {-120, 12, 0}};
    /// What the shared analyser runs for this device.
    struct Analysis {
        std::int32_t fftSize = 2048;
        Window window = Window::Hann;
        ChannelKind channel = Sum;
        std::int32_t hopSamples = 2880;
        /// The averager's settings: hop and decay, so that each analysis
        /// moves the average 1 / Avg of the way.
        LinearMagnitudeAverage::Settings average{};
    };

    SpectrumDevice() noexcept;
    void prepare(double sampleRate) noexcept;
    void set(Param, double) noexcept;
    [[nodiscard]] double value(Param p) const noexcept { return p_[static_cast<std::size_t>(p)]; }
    [[nodiscard]] int latency() const noexcept { return 0; }
    /// The signal passes untouched (a host may hand an output plane that is
    /// an input plane, even the other channel's).
    void process(const float* inL, const float* inR, float* outL, float* outR, std::size_t frames) noexcept;
    /// Any thread that holds the device: the analysis its controls ask for.
    [[nodiscard]] const Analysis& analysis() const noexcept { return analysis_; }
    /// The analysed signal from a stereo frame, as Channel picks it.
    [[nodiscard]] static float pick(ChannelKind, float left, float right) noexcept;

private:
    void update() noexcept;
    std::array<double, Count> p_{};
    Analysis analysis_{};
    double rate_ = 48000;
};

}  // namespace adi::dsp
