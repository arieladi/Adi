// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live 12's Gate, the core (DEV-22, ADR-0249 d7: after the Compressor).
//
// A gate on the louder of the two channels (stereo-linked): it opens when the
// peak level reaches Threshold and closes when it falls below Threshold less
// Return (the hysteresis), but only after Hold has run out. Its gain moves
// between Floor (closed) and unity (open): toward unity with Attack, toward
// Floor with Release, each a one-pole whose time constant is the control's
// value. The one-pole runs on the gate's opening, 0 closed to 1 open, and the
// gain is Floor plus that opening's share of the way to unity, so a moved
// Floor moves the gain at once and never outside Floor to unity. Flip swaps
// the two: the signal passes while the gate is closed and is held at Floor
// while it is open. Lookahead delays the audio,
// never the detector, by 0, 1 or 10 ms in whole samples, so the gate is open
// before the transient that opens it arrives.
//
// WHAT IS NOT LIVE'S YET, and says so: the ranges and defaults, the shape of
// the attack and release (here a one-pole on the opening, so on the linear
// gain at a fixed Floor), whether Hold
// restarts on every peak above the close level, and the detector (here the
// instantaneous peak) wait on a Live 12.4 reading of the Gate (the devices
// log's ONE list). Until then this core claims no parity.
//
// Allocation-free: the lookahead lines are fixed arrays sized for 10 ms at
// 384 kHz. A non-finite sample is read as silence.

#pragma once

#include <array>
#include <cstddef>

namespace adi::dsp {

class LiveGate {
public:
    enum Param { Threshold, Return, Attack, Hold, Release, Floor, Flip, Lookahead, Count };
    struct Range {
        double low, high, initial;
    };
    // Threshold dBFS; Return dB; Attack, Hold, Release ms; Floor dB (its low
    // end is silence); Flip off/on; Lookahead 0/1/10 ms as 0/1/2. Initial
    // values are ADI's until the reading.
    static constexpr Range ranges[Count] = {{-80, 0, -40}, {0, 24, 3},      {0.02, 150, 0.1}, {0.02, 3000, 50},
                                            {0.01, 3000, 50}, {-80, 0, -80}, {0, 1, 0},        {0, 2, 0}};
    static constexpr int kMaxLookahead = 4096;  // 10 ms at 384 kHz is 3,840

    LiveGate() noexcept;
    void prepare(double sampleRate) noexcept;
    void set(Param, double) noexcept;
    [[nodiscard]] double value(Param p) const noexcept { return p_[static_cast<std::size_t>(p)]; }
    /// The audio's delay: the lookahead in whole samples at the prepared rate.
    [[nodiscard]] int latency() const noexcept { return lookahead_; }
    /// Whether the gate is open now (before Flip), for a meter and tests.
    [[nodiscard]] bool open() const noexcept { return open_; }
    /// The gain applied to the last sample produced (after Flip).
    [[nodiscard]] double gain() const noexcept { return applied_; }
    void process(const float* inL, const float* inR, float* outL, float* outR, std::size_t frames) noexcept;

private:
    void update() noexcept;
    std::array<double, Count> p_{};
    std::array<std::array<double, kMaxLookahead>, 2> line_{};
    std::size_t write_ = 0;
    int lookahead_ = 0;
    double rate_ = 48000, openLevel_ = 0, closeLevel_ = 0, attack_ = 0, release_ = 0, floor_ = 0;
    double opening_ = 0, applied_ = 0;  // opening_: 0 closed to 1 open, smoothed
    long holdSamples_ = 0, holdLeft_ = 0;
    bool open_ = false, flip_ = false;
};

}  // namespace adi::dsp
