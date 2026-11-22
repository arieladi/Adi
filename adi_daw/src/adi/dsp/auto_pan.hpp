// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live 12.4's Auto Pan-Tremolo, the core (DEV-22; lands after 3 December):
// each channel's gain moved by an LFO, on the modulators' clock
// (modulator_clock.hpp).
//
//   Mode      Pan: the two channels' LFOs are Phase apart (at 180 degrees one
//             is loud while the other is quiet: the sound moves); Tremolo:
//             both channels follow one LFO.
//   Amount    0 to 100 %: how far the gain falls at the LFO's trough.
//   Rate      0.01 to 20 Hz, or Sync to a Division at the host's tempo.
//   Phase     the right channel's LFO ahead of the left's, 0 to 360 degrees.
//   Offset    both LFOs' start, 0 to 360 degrees.
//   Wave      Sine, Triangle, Saw Down, Random (a held value per cycle).
//   Shape     0 to 100 %: the wave hardened toward a square.
//   Invert    the wave upside down.
//
// The gain is 1 - Amount x (1 - (1 + w) / 2) for the wave w in [-1, 1]: unity
// at the crest, 1 - Amount at the trough. Free or synced, the phase advances
// by the same addition every sample, so every block size gives the same
// samples. Synced, it follows the host's tempo from the device's start, not
// the song's bar line: the native adapter passes the song's position to
// ADI-Shaper alone today (a need logged for the engine).
//
// WHAT IS NOT LIVE'S YET, and says so: every range and default, the pan law,
// the Shape law, the wave set, and how Tremolo meets Phase wait on a Live 12.4
// reading of Auto Pan-Tremolo (the devices log's ONE list). Until then this
// core claims no parity. Allocation-free; no latency.

#pragma once

#include "adi/dsp/glide.hpp"
#include "adi/dsp/modulator_clock.hpp"

#include <array>
#include <cstddef>

namespace adi::dsp {

class AutoPan {
public:
    enum Param { Mode, Amount, Rate, Sync, Division, Phase, Offset, Wave, Shape, Invert, Count };
    enum Motion { Pan, Tremolo };
    enum Waveform { Sine, Triangle, SawDown, Random };
    struct Range {
        double low, high, initial;
    };
    static constexpr Range ranges[Count] = {{0, 1, Pan},  {0, 100, 100}, {0.01, 20, 1}, {0, 1, 0},  {0, 11, 4},
                                            {0, 360, 180}, {0, 360, 0},   {0, 3, Sine},  {0, 100, 0}, {0, 1, 0}};
    /// Division's choices, in quarter notes (as the modulators').
    static constexpr std::array<double, 12> kDivisionQuarters{1.0 / 16, 1.0 / 8, 1.0 / 4, 1.0 / 2, 1, 2,
                                                              4,        8,       16,      32,      0.75, 1.0 / 3};

    AutoPan() noexcept;
    void prepare(double sampleRate) noexcept;
    void set(Param, double) noexcept;
    void tempo(double bpm) noexcept;
    [[nodiscard]] double value(Param p) const noexcept { return p_[static_cast<std::size_t>(p)]; }
    [[nodiscard]] int latency() const noexcept { return 0; }
    /// The gain each channel took on the last sample, for tests and a meter.
    [[nodiscard]] double gain(int channel) const noexcept { return gain_[channel == 0 ? 0 : 1]; }
    void process(const float* inL, const float* inR, float* outL, float* outR, std::size_t frames) noexcept;

private:
    [[nodiscard]] double wave(double cycles) const noexcept;
    std::array<double, Count> p_{};
    std::array<double, 2> gain_{1, 1};
    Glide amount_;   ///< Amount glides over about 10 ms once audio runs (dsp/glide.hpp)
    ModulatorClock clock_;
    double bpm_ = 120;
};

}  // namespace adi::dsp
