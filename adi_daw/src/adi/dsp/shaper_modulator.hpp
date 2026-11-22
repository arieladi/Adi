// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live 12's Shaper modulator, the kernel (DEV-07's device side, ADR-0249 d8;
// lands after 3 December). A breakpoint envelope that repeats: up to 64
// points, each a time in the cycle, a value, and the curve of the segment it
// starts. It renders a modulation signal sample by sample for the engine's
// DEV-07 evaluator to route (ADR-0230), on the LFO's clock
// (modulator_clock.hpp): free or synced, the same bits at every block size,
// a locate landing on the song's own phase.
//
//   Points    time in [0, 1), value in [-1, 1], curve in [-1, 1]: 0 is a
//             straight line; above 0 the segment starts slowly and ends fast,
//             below 0 the other way (an exponential of 8 times the curve).
//             The last point's segment runs to the first point of the next
//             cycle. Two points at one time make a step.
//   Sync, Rate, Division, Phase, Offset, Depth, Jitter, Smooth: as the LFO's.
//
// WHAT IS NOT LIVE'S YET, and says so: the default shape (here a triangle
// through 0, 1 and -1, ADI's until reading 11), the curve law, the point
// limit, and every range and default wait on a Live 12.4 reading of the
// Shaper (reading 11 on the devices log's ONE list). Until then it claims no
// parity. Allocation-free: setPoints() copies into a fixed array.

#pragma once

#include "adi/dsp/modulator_clock.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace adi::dsp {

class ShaperModulator {
public:
    enum Param { Sync, Rate, Division, Phase, Offset, Depth, Jitter, Smooth, Count };
    struct Range {
        double low, high, initial;
    };
    static constexpr Range ranges[Count] = {{0, 1, 0}, {0.01, 40, 1}, {0, 11, 4}, {0, 360, 0},
                                            {-1, 1, 0}, {0, 1, 1},    {0, 1, 0},  {0, 1, 0}};
    /// The synced period's choices, in quarter notes (as the LFO's).
    static constexpr std::array<double, 12> kDivisionQuarters{1.0 / 16, 1.0 / 8, 1.0 / 4, 1.0 / 2, 1, 2,
                                                              4,        8,       16,      32,      0.75, 1.0 / 3};
    struct Point {
        double time = 0, value = 0, curve = 0;
    };
    static constexpr std::size_t kMaxPoints = 64;
    using Position = ModulatorClock::Position;

    ShaperModulator() noexcept;
    void prepare(double sampleRate) noexcept;
    void set(Param, double) noexcept;
    [[nodiscard]] double value(Param p) const noexcept { return p_[static_cast<std::size_t>(p)]; }
    /// The shape: `count` points copied, each clamped to its range, sorted by
    /// time (at most kMaxPoints; the rest are dropped). False, and the shape
    /// kept, when there is none. Called on the audio thread or serialised
    /// with render() by the caller; it allocates nothing.
    bool setPoints(const Point* points, std::size_t count) noexcept;
    [[nodiscard]] std::size_t pointCount() const noexcept { return count_; }
    /// The shape at a phase in [0, 1), before Depth and Offset.
    [[nodiscard]] double shapeAt(double phase) const noexcept;
    void retrigger() noexcept { clock_.retrigger(); }
    void render(double* out, std::size_t frames, const Position& position) noexcept;

private:
    void update() noexcept;
    std::array<double, Count> p_{};
    std::array<Point, kMaxPoints> points_{};
    std::size_t count_ = 0;
    ModulatorClock clock_;
    double rate_ = 48000, smoothed_ = 0, smoothing_ = 1;
    bool primed_ = false;
};

}  // namespace adi::dsp
