// SPDX-License-Identifier: GPL-3.0-or-later
//
// The clock Live's modulator kernels run on (DEV-07's device side, ADR-0249
// d8): the LFO's and the Shaper's. Free running, its phase advances by the
// same addition every sample, whatever the blocking; synced, a sample's phase
// is read from the song's position within its tempo segment, so any blocking
// and a locate give the same value. Header-only, allocation-free.

#pragma once

#include <cstddef>
#include <cstdint>

namespace adi::dsp {

class ModulatorClock {
public:
    /// Where the song is at the first sample of a block: its sample, and the
    /// tempo segment it lies in (the segment's first sample, its position in
    /// quarter notes, its tempo). A sample's position is the segment's plus
    /// its distance at that tempo, so any blocking reads the same position.
    struct Position {
        std::int64_t sample = 0;
        std::int64_t originSample = 0;
        double originQuarterNotes = 0;
        double bpm = 120;
    };

    void prepare(double sampleRate) noexcept {
        rate_ = sampleRate > 0 ? sampleRate : 48000;
        free_ = 0;
    }
    /// Free running: the next cycle starts now.
    void retrigger() noexcept { free_ = 0; }
    [[nodiscard]] double rate() const noexcept { return rate_; }

    /// A value in [-1, 1) that is a function of a cycle's index alone
    /// (SplitMix64), so a random shape or a jitter needs no running generator.
    [[nodiscard]] static double hash(std::int64_t cycle, std::uint64_t salt) noexcept {
        std::uint64_t z = static_cast<std::uint64_t>(cycle) + salt;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        return static_cast<double>(z >> 11) / 4503599627370496.0 - 1.0;   // 2^52
    }

    /// Sample `i` of a block, in cycles (before any phase offset): free
    /// running, the summed phase; synced, the song's quarter notes over
    /// `quarters` a cycle.
    [[nodiscard]] double cycles(std::size_t i, bool synced, double quarters, const Position& p) const noexcept {
        if (!synced) {
            return free_;
        }
        const double perSample = p.bpm > 0 && p.bpm < 1e6 ? p.bpm / (60 * rate_) : 0;
        const auto at = p.sample + static_cast<std::int64_t>(i) - p.originSample;
        return (p.originQuarterNotes + static_cast<double>(at) * perSample) / quarters;
    }
    /// After each sample: free running, the phase advances by `hz` a second.
    void step(bool synced, double hz) noexcept {
        if (!synced) {
            free_ += hz / rate_;
            if (free_ >= kWrap) {
                free_ -= kWrap;
            }
        }
    }

private:
    // Kept under this many cycles: subtracted at the same sample whatever
    // the blocking, so where the host cut never changes a value's bits.
    static constexpr double kWrap = 1048576;
    double rate_ = 48000, free_ = 0;
};

}  // namespace adi::dsp
