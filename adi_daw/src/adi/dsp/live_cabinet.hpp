// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live 12's Cabinet, the core (DEV-22; lands after 3 December): a guitar or
// bass speaker cabinet heard through a microphone, as a fixed linear filter.
//
//   Cabinet     1x12, 2x12, 4x12, 4x10, 4x10 Bass: each a low cut, a low
//               resonance (the box), a cone peak and a two-section high cut,
//               from kCabinets.
//   Microphone  Near On-Axis, Near Off-Axis (a 3 kHz high shelf of -6 dB:
//               duller), Far (a 200 Hz low shelf of -3 dB and the high cut an
//               octave lower: farther and darker).
//   Mic Type    Condenser (an 8 kHz high shelf of +2 dB) or Dynamic (flat).
//   Output      -36 to +12 dB.
//   Dry/Wet     0 to 100 %.
//   Dual        Mono (the channels' mean through one cabinet, to both) or
//               Dual (a cabinet for each channel).
//
// Output and Dry/Wet glide over about 10 ms once audio runs (dsp/glide.hpp).
// Each filter is a direct-form-I biquad in double, its states settled on
// exact 0 below 1e-30, so the response is the cascade's, which the test
// checks against the documented designs.
//
// WHAT IS NOT LIVE'S YET, and says so: Live's Cabinet convolves measured
// cabinets; these responses, the lists and the defaults are ADI's until a
// Live 12.4 reading of Cabinet (the devices log's ONE list). It claims no
// parity. Allocation-free; no latency.

#pragma once

#include "adi/dsp/biquad.hpp"
#include "adi/dsp/glide.hpp"

#include <array>
#include <cstddef>

namespace adi::dsp {

class LiveCabinet {
public:
    enum Param { Cabinet, Microphone, MicType, Output, DryWet, Dual, Count };
    enum Box { OneByTwelve, TwoByTwelve, FourByTwelve, FourByTen, FourByTenBass, Boxes };
    enum Mic { NearOn, NearOff, Far };
    struct Range {
        double low, high, initial;
    };
    static constexpr Range ranges[Count] = {{0, 4, 0}, {0, 2, 0}, {0, 1, 0}, {-36, 12, 0}, {0, 100, 100}, {0, 1, 0}};
    struct BoxSpec {
        double lowCutHz, resonanceHz, resonanceDb, coneHz, coneDb, highCutHz;
    };
    static constexpr BoxSpec kCabinets[Boxes] = {
        {90, 120, 4, 2500, 4, 5000}, {80, 110, 5, 2200, 5, 5200}, {70, 100, 6, 2000, 6, 5500},
        {75, 130, 4, 2800, 5, 6000}, {40, 80, 5, 1500, 3, 4500},
    };
    static constexpr std::size_t kSections = 8;

    LiveCabinet() noexcept;
    void prepare(double sampleRate) noexcept;
    void set(Param, double) noexcept;
    [[nodiscard]] double value(Param p) const noexcept { return p_[static_cast<std::size_t>(p)]; }
    [[nodiscard]] int latency() const noexcept { return 0; }
    void process(const float* inL, const float* inR, float* outL, float* outR, std::size_t frames) noexcept;

private:
    struct State {
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    };
    void update() noexcept;
    std::array<double, Count> p_{};
    std::array<Biquad, kSections> design_{};
    std::array<std::array<State, kSections>, 2> state_{};
    double rate_ = 48000;
    Glide output_, wet_;
};

}  // namespace adi::dsp
