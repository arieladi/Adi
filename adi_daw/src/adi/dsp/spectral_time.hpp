// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live 12's Spectral Time, the core (DEV-22; lands after 3 December): a
// delay and a freeze that work on the spectrum, frame by frame.
//
//   Freeze     off or on: on, the spectrum at that moment is held, each bin
//              ringing on at its own measured frequency (a phase vocoder),
//              until Freeze is let go.
//   Time       20 to 2,000 ms: the delay, whole frames (a hop is 512
//              samples) per bin.
//   Feedback   0 to 95 %: each bin's repeats, each Feedback times the last.
//   Tilt       -100 to +100 %: the high bins delayed up to twice as long (+)
//              or half (-), the low ones the other way, the middle at Time.
//   Spray      0 to 100 %: each bin's delay lengthened by up to that share
//              of Time, at random (fixed per bin, from a seed).
//   Mask       -100 to +100 %: with a positive Mask only the bins above a
//              split are delayed, with a negative only those below; the
//              split moves from the bottom (or top) to the middle with |Mask|.
//              Bins outside it come straight through.
//   Dry/Wet    0 to 100 %.
//
// The short-time transform is a 2,048-point periodic Hann window at a hop
// of 512, analysis and synthesis, rebuilt exactly (the squared windows of
// four overlaps sum to 1.5). A bin delayed by d frames is the input delayed
// by d x 512 samples, exactly. The device's latency is one window, 2,048
// samples, reported, and the dry signal is delayed with it. prepare()
// allocates the frames and never throws; processing does not.
//
// WHAT IS NOT LIVE'S YET, and says so: Spectral Time's resolution choices,
// its pitch shift, stereo spread, sync and the freeze's retrigger and fade,
// the laws of Tilt, Spray and Mask, and every range and default wait on a
// Live 12.4 reading of Spectral Time (the devices log's ONE list). It
// claims no parity.

#pragma once

#include "adi/dsp/spectrum.hpp"

#include <array>
#include <complex>
#include <cstddef>
#include <memory>
#include <vector>

namespace adi::dsp {

class SpectralTime {
public:
    enum Param { Freeze, Time, Feedback, Tilt, Spray, Mask, DryWet, Count };
    struct Range {
        double low, high, initial;
    };
    static constexpr Range ranges[Count] = {{0, 1, 0}, {20, 2000, 250}, {0, 95, 30}, {-100, 100, 0}, {0, 100, 0}, {-100, 100, 0}, {0, 100, 50}};
    static constexpr std::size_t kSize = 2048, kHop = 512, kBins = kSize / 2 + 1;

    SpectralTime();
    void prepare(double sampleRate) noexcept;
    void set(Param, double) noexcept;
    [[nodiscard]] double value(Param p) const noexcept { return p_[static_cast<std::size_t>(p)]; }
    [[nodiscard]] int latency() const noexcept { return static_cast<int>(kSize); }
    void process(const float* inL, const float* inR, float* outL, float* outR, std::size_t frames) noexcept;
    /// Bin k's delay as set, in frames, for tests.
    [[nodiscard]] std::size_t delay(std::size_t k) const noexcept { return delays_[k]; }

private:
    struct Lane {
        std::vector<float> in, acc, dry;   ///< the input window, the overlap-add, the delayed dry
        std::vector<std::complex<float>> history;   ///< frames x bins: input plus feedback, per bin
        std::vector<float> magnitude, phase, advance, lastPhase;   ///< the freeze's held state
    };
    void update() noexcept;
    void frame(Lane&) noexcept;
    std::array<double, Count> p_{};
    std::unique_ptr<RealFft> fft_;
    std::array<Lane, 2> lanes_;
    std::vector<float> window_, scratch_;
    std::vector<std::complex<float>> bins_, out_;
    std::vector<std::size_t> delays_;
    std::vector<float> sprayDraw_;
    std::size_t frames_ = 0, written_ = 0, frameIndex_ = 0;
    double rate_ = 48000, feedback_ = 0, wet_ = 0.5;
    bool frozen_ = false, ready_ = false;
};

}  // namespace adi::dsp
