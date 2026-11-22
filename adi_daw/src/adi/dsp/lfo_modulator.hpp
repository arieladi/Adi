// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live 12's LFO modulator, the kernel (DEV-07's device side, ADR-0249 d8;
// lands after 3 December). It renders a modulation signal, sample by sample;
// the engine's DEV-07 evaluator routes it to its targets (ADR-0230: depth,
// curve and bipolarity per route live in the host's route tables, not here).
//
//   Shape     Sine, Triangle, Saw Up, Saw Down, Square, Random (a held value
//             per cycle) and Wander (a smooth random line, one value per
//             cycle, cosine-joined).
//   Sync      off: Rate in Hz; on: Division of the song (quarter notes).
//   Rate      0.01 to 40 Hz.
//   Division  the synced period, as a choice of note values (1/64 to 8 bars).
//   Phase     0 to 360 degrees, added to the cycle's position.
//   Offset    -1 to 1, added after Depth.
//   Depth     0 to 1, the waveform's scale.
//   Jitter    0 to 1: each cycle's start moved by a random part of a cycle.
//   Smooth    0 to 1: a one-pole after the waveform, up to 50 ms.
//
// THE OUTPUT AT EVERY BLOCK SIZE IS THE SAME, BIT FOR BIT. Free running, the
// phase advances by the same addition every sample, whatever the blocking.
// Synced, the phase is read from the song's sample position (the host's
// DeviceTransport), so a locate lands on the song's own phase and any
// blocking gives the same samples. Random values come from a hash of the
// cycle's index, not from a running generator, so they are a function of
// the position too.
//
// WHAT IS NOT LIVE'S YET, and says so: the control set, ranges and defaults,
// the Division choices, Jitter's and Smooth's laws, the random shapes'
// distribution, and how a retrigger meets a synced phase wait on a Live 12.4
// reading of the LFO (the devices log's ONE list). Until then it claims no
// parity. Allocation-free; no state but the phase, the smoother, and the
// last cycle's values.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace adi::dsp {

class LfoModulator {
public:
    enum Param { Shape, Sync, Rate, Division, Phase, Offset, Depth, Jitter, Smooth, Count };
    enum Waveform { Sine, Triangle, SawUp, SawDown, Square, Random, Wander, Shapes };
    struct Range {
        double low, high, initial;
    };
    static constexpr Range ranges[Count] = {{0, Shapes - 1, Sine}, {0, 1, 0}, {0.01, 40, 1}, {0, 11, 4},
                                            {0, 360, 0},           {-1, 1, 0}, {0, 1, 1},   {0, 1, 0},
                                            {0, 1, 0}};
    /// Division's choices, in quarter notes: 1/64 1/32 1/16 1/8 1/4 1/2 1 bar
    /// (4/4), 2, 4, 8 bars, and the dotted and triplet eighth.
    static constexpr std::array<double, 12> kDivisionQuarters{1.0 / 16, 1.0 / 8, 1.0 / 4, 1.0 / 2, 1, 2,
                                                              4,        8,       16,      32,      0.75, 1.0 / 3};

    /// Where the song is at the first sample of a block, for a synced LFO:
    /// its sample, and the tempo segment it lies in (the segment's first
    /// sample, its position in quarter notes, its tempo). A sample's position
    /// is the segment's plus its distance at that tempo, so any blocking reads
    /// the same position for the same sample.
    struct Position {
        std::int64_t sample = 0;
        std::int64_t originSample = 0;
        double originQuarterNotes = 0;
        double bpm = 120;
    };

    LfoModulator() noexcept;
    void prepare(double sampleRate) noexcept;
    void set(Param, double) noexcept;
    [[nodiscard]] double value(Param p) const noexcept { return p_[static_cast<std::size_t>(p)]; }
    /// Free running: the next cycle starts now, at Phase.
    void retrigger() noexcept;
    /// Audio thread: `frames` values into `out`. `position` matters only when
    /// synced; a free-running LFO ignores it.
    void render(double* out, std::size_t frames, const Position& position) noexcept;

private:
    [[nodiscard]] double shape(double phase, std::int64_t cycle) const noexcept;
    [[nodiscard]] static double hash(std::int64_t cycle, std::uint64_t salt) noexcept;
    void update() noexcept;
    std::array<double, Count> p_{};
    double rate_ = 48000;
    double freePhase_ = 0;          // cycles, free running; its integer part counts cycles
    double smoothed_ = 0, smoothing_ = 0;
    int waveform_ = Sine;
    bool primed_ = false;
};

}  // namespace adi::dsp
