// SPDX-License-Identifier: GPL-3.0-or-later
//
// The scope's audio taps -- ADR-0167, ADR-0175.
//
// A TAP is where the audio thread hands the scope a track's output: the
// strip's, after its fader, the level the track is heard at. It is the SECOND
// path by which the audio thread writes something the UI reads; ADR-0050 d4
// named the meter scalar as the only one, and ADR-0167 d7 amends it, because a
// waveform is a stream, not a scalar.
//
// The ring: one writer (the audio thread), any number of readers, overwrite
// oldest. Every field is atomic and relaxed, and the frame count is published
// with release, so a reader never races the writer in the language's sense.
// A reader copies the newest frames, then checks the writer did not lap it
// while it copied; if it did, the read reports it and the caller tries again.
// The writer never waits.
//
// A STAMP rides with every frame: the timeline position at which that frame
// is HEARD. The tap is told its latency after every rebuild -- the strip's
// input arrival plus the strip's reported latency -- and stamps
// transport position - latency. A plug-in's latency makes the stamps what the
// audio represents; a track delay (ADR-0172), reported as negative latency,
// moves them later, which is when that track is heard. So two taps' frames
// with equal stamps are heard at the same instant, and the scope compares
// tracks as heard (ADR-0167 d3) by matching stamps, with nothing to correct.
//
// A tap nobody watches costs one relaxed atomic load per block: the strip
// holds a null pointer.

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>

namespace adi::engine {

class ScopeTap {
public:
    /// Message thread: `seconds` of stereo at `sampleRate`, allocated here.
    ScopeTap(double sampleRate, double seconds);

    /// Audio thread, wait-free: `frames` frames; `stamp` is the heard timeline
    /// position of the first. `advancing` is false while the transport is
    /// parked: every frame then carries the same stamp.
    void write(const float* l, const float* r, std::int32_t frames, std::int64_t stamp,
               bool advancing) noexcept;

    /// Any thread but the writer: the newest `l.size()` frames and the stamp of
    /// the first. False when fewer have been written, or the writer lapped the
    /// copy -- read again.
    bool read(std::span<float> l, std::span<float> r, std::int64_t& firstStamp) const;

    /// Message thread, after every rebuild: this tap's latency (see above).
    void setLatency(std::int32_t samples) noexcept { latency_.store(samples, std::memory_order_relaxed); }
    [[nodiscard]] std::int32_t latency() const noexcept { return latency_.load(std::memory_order_relaxed); }

    [[nodiscard]] std::int64_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::int64_t written() const noexcept { return written_.load(std::memory_order_acquire); }
    [[nodiscard]] double sampleRate() const noexcept { return sampleRate_; }

private:
    double sampleRate_;
    std::int64_t capacity_;
    std::unique_ptr<std::atomic<float>[]> l_, r_;
    std::unique_ptr<std::atomic<std::int64_t>[]> stamp_;
    std::atomic<std::int64_t> written_{0};    ///< frames complete, published with release
    std::atomic<std::int64_t> pending_{0};    ///< frames complete or being written now
    std::atomic<std::int32_t> latency_{0};
};

// --- the compare (ADR-0167 d2): pure, for the UI thread ---------------------

/// Correlation of two windows: sum(ab) / sqrt(sum(a^2) sum(b^2)), -1 to +1.
/// 0 when either is silent.
[[nodiscard]] double correlation(std::span<const float> a, std::span<const float> b) noexcept;

struct Offset {
    std::int32_t samples = 0;     ///< positive: b is LATE by this much
    double correlation = 0.0;     ///< at that lag
};

/// The lag within +-`maxLag` samples at which b best matches a (the largest
/// correlation, by sign too: a polarity flip is a different finding). The
/// windows are the same length; the overlap shrinks with the lag.
[[nodiscard]] Offset bestOffset(std::span<const float> a, std::span<const float> b,
                                std::int32_t maxLag) noexcept;

}  // namespace adi::engine
