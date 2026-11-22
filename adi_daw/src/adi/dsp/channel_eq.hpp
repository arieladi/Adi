// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live 12's Channel EQ, the core (DEV-22; lands after 3 December): a channel
// strip's three bands and a low cut, on the cookbook filters (dsp/biquad).
//
//   Low Cut   an 80 Hz second-order high pass (Butterworth), on or off.
//   Low       -15 to 15 dB, a shelf at 100 Hz.
//   Mid       -12 to 12 dB, a bell of Q 0.7 at Mid Freq.
//   Mid Freq  120 Hz to 7.5 kHz.
//   High      -15 to 15 dB, a shelf at 8 kHz.
//   Output    -12 to 12 dB.
//
// Each band is a direct-form-I biquad in double, stereo, its coefficients
// recomputed when a control moves (on the audio thread: no allocation), its
// states settled on exact 0 below 1e-30 after silence.
//
// WHAT IS NOT LIVE'S YET, and says so: the corner frequencies, the shelf
// slopes, the bell's Q (fixed or gain-dependent), the low cut's slope, the
// ranges and defaults wait on a Live 12.4 reading of Channel EQ (the devices
// log's ONE list). Until then it claims no parity.

#pragma once

#include "adi/dsp/biquad.hpp"
#include "adi/dsp/glide.hpp"

#include <array>
#include <cstddef>

namespace adi::dsp {

class ChannelEq {
public:
    enum Param { LowCut, Low, Mid, MidFreq, High, Output, Count };
    struct Range {
        double low, high, initial;
    };
    static constexpr Range ranges[Count] = {{0, 1, 0}, {-15, 15, 0}, {-12, 12, 0}, {120, 7500, 1500},
                                            {-15, 15, 0}, {-12, 12, 0}};
    static constexpr double kLowCutHz = 80, kLowHz = 100, kHighHz = 8000, kMidQ = 0.7;

    ChannelEq() noexcept;
    void prepare(double sampleRate) noexcept;
    void set(Param, double) noexcept;
    [[nodiscard]] double value(Param p) const noexcept { return p_[static_cast<std::size_t>(p)]; }
    [[nodiscard]] int latency() const noexcept { return 0; }
    /// The design's magnitude at `hz`, in dB: what the bands and Output add
    /// up to (for tests and a display).
    [[nodiscard]] double responseDb(double hz) const noexcept;
    void process(const float* inL, const float* inR, float* outL, float* outR, std::size_t frames) noexcept;

private:
    struct State {
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    };
    enum Band { Cut, LowShelf, Bell, HighShelf, Bands };
    void update() noexcept;
    std::array<double, Count> p_{};
    std::array<Biquad, Bands> band_{};
    std::array<std::array<State, Bands>, 2> state_{};
    double rate_ = 48000;
    Glide output_;   ///< Output glides over about 10 ms once audio runs (dsp/glide.hpp)
};

}  // namespace adi::dsp
