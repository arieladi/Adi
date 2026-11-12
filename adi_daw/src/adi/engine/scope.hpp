// SPDX-License-Identifier: GPL-3.0-or-later
//
// The scope's audio taps -- ADR-0167, ADR-0175.
//
// A TAP is where the audio thread hands a consumer one point on a track.
// ADR-0175 defaults to the strip output; ADR-0195 adds chain input and the
// pre-fader strip input for the analyser. It is the SECOND
// path by which the audio thread writes something the UI reads; ADR-0050 d4
// named the meter scalar as the only one, and ADR-0167 d7 amends it, because a
// waveform is a stream, not a scalar.
//
// The ring: one writer (the audio thread), any number of readers, overwrite
// oldest. Sample fields are atomic and relaxed, and the frame count is published
// with release, so a reader never races the writer in the language's sense.
// A reader copies the newest frames, then checks the writer did not lap it
// while it copied; if it did, the read reports it and the caller tries again.
// The writer never waits.
//
// Bounded timestamp spans describe the timeline position at which each frame
// is HEARD. Contiguous event/transport segments coalesce without a per-sample
// timestamp store. The graph uses its cached input arrival, plus the node's own
// configured latency for an output. A plug-in's latency makes the stamps what the
// audio represents; a track delay (ADR-0172), reported as negative latency,
// moves them later, which is when that track is heard. So two taps' frames
// with equal stamps are heard at the same instant, and the scope compares
// tracks as heard (ADR-0167 d3) by matching stamps, with nothing to correct.
//
// A tap nobody watches copies no samples. Session publishes capture-port bindings
// separately from DSP graphs and retires buffers only after audio acknowledges
// a newer publication; a slow UI consumer cannot block the writer.

#pragma once

#include <atomic>
#include <compare>
#include <cstdint>
#include <memory>
#include <span>

namespace adi::engine {

/// ADR-0195 d5: spectrum.pre is ChainInput; spectrum.post (and spectrum)
/// is PreFader. PostFader remains ADR-0175's default waveform scope.
enum class ScopePoint { PostFader, PreFader, ChainInput, DeviceInput, DeviceOutput };

/// Device endpoints also name their authored device, so a chain reorder never changes source.
struct ScopeSource {
    std::int64_t track = 0;
    ScopePoint point = ScopePoint::PostFader;
    std::int64_t device = 0;
    auto operator<=>(const ScopeSource&) const = default;
};
struct ScopeRequest {
    ScopeSource source;
    double seconds = 10.0;
};

class ScopeTap {
public:
    enum class ReadStatus { Ready, NotReady, Overrun, GenerationChanged, Discontinuity, Invalid };
    struct Cursor {
        std::uint64_t generation = 0;
        std::int64_t next = 0;
        std::int64_t expectedStamp = 0;
        bool expectedAdvancing = false, hasExpected = false;
    };
    struct ReadResult {
        ReadStatus status = ReadStatus::NotReady;
        std::int32_t frames = 0;
        std::int64_t firstStamp = 0, firstSequence = 0;
        bool advancing = false;
    };
    /// Message thread: `seconds` of stereo at `sampleRate`, allocated here.
    ScopeTap(double sampleRate, double seconds);

    /// Audio thread, wait-free: `frames` frames; `stamp` is the heard timeline
    /// position of the first. `advancing` is false while the transport is
    /// parked: every frame then carries the same stamp. A null left channel
    /// writes silence; a null right channel duplicates the left.
    void write(const float* l, const float* r, std::int32_t frames, std::int64_t stamp,
               bool advancing) noexcept;
    /// The same from a graph at 64 (ADR-0207 d2): the ring is float, a
    /// picture of the signal, so each frame is narrowed as it is stored.
    void write(const double* l, const double* r, std::int32_t frames, std::int64_t stamp,
               bool advancing) noexcept;

    /// Any thread but the writer: the newest `l.size()` frames and the stamp of
    /// the first. False when fewer have been written, or the writer lapped the
    /// copy -- read again.
    bool read(std::span<float> l, std::span<float> r, std::int64_t& firstStamp) const;

    /// Independent non-destructive consumers. Reads are bounded by both sample and timestamp
    /// history and stop at a timeline boundary. Discontinuity includes the first new span;
    /// reset an overlapping analysis window before ingesting it. Overrun copies no valid data
    /// and advances to oldest(); GenerationChanged resets to that generation's oldest().
    [[nodiscard]] ReadResult read(Cursor&, std::span<float> l, std::span<float> r) const noexcept;
    [[nodiscard]] Cursor cursorAtEnd() const noexcept { return {generation_, written()}; }
    [[nodiscard]] Cursor cursorAtOldest() const noexcept { return {generation_, oldest()}; }
    [[nodiscard]] std::int64_t oldest() const noexcept;
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }

    /// Message thread, after every rebuild: this tap's latency (see above).
    void setLatency(std::int32_t samples) noexcept { latency_.store(samples, std::memory_order_relaxed); }
    [[nodiscard]] std::int32_t latency() const noexcept { return latency_.load(std::memory_order_relaxed); }

    [[nodiscard]] std::int64_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::int64_t written() const noexcept { return written_.load(std::memory_order_acquire); }
    [[nodiscard]] double sampleRate() const noexcept { return sampleRate_; }

private:
    struct StampSpan {
        std::atomic<std::uint64_t> version{0};
        std::atomic<std::int64_t> begin{0}, end{0}, stamp{0};
        std::atomic<bool> advancing{false};
    };
    struct Stamp { std::int64_t begin = 0, end = 0, stamp = 0; bool advancing = false; };
    bool spanAt(std::int64_t sequence, Stamp&) const noexcept;
    bool loadSpan(std::int64_t record, Stamp&) const noexcept;
    template <typename T>
    void store(const T* l, const T* r, std::int32_t frames, std::int64_t stamp,
               bool advancing) noexcept;

    double sampleRate_;
    std::int64_t capacity_;
    std::uint64_t generation_;
    std::unique_ptr<std::atomic<float>[]> l_, r_;
    std::int64_t spanCapacity_;
    std::unique_ptr<StampSpan[]> spans_;
    std::atomic<std::int64_t> spanCount_{0};
    // Single writer only. Contiguous writes, including one-sample event segments, share a span.
    std::int64_t lastSpan_ = -1, lastBegin_ = 0, lastEnd_ = 0, lastStamp_ = 0;
    bool lastAdvancing_ = false;
    std::atomic<std::int64_t> written_{0};    ///< frames complete, published with release
    std::atomic<std::int64_t> pending_{0};    ///< frames complete or being written now
    std::atomic<std::int32_t> latency_{0};
};

/// A Session-owned subscription. Acquire/reset/destruction belong to the Session owner thread;
/// tap() may be read off-thread and follows prepared history/rate/source generations. A lease
/// outliving its Session becomes unavailable. Dropping a UI consumer never disables a processor.
class ScopeLease {
public:
    ScopeLease() = default;
    ~ScopeLease();
    ScopeLease(ScopeLease&&) noexcept;
    ScopeLease& operator=(ScopeLease&&) noexcept;
    ScopeLease(const ScopeLease&) = delete;
    ScopeLease& operator=(const ScopeLease&) = delete;
    void reset() noexcept;
    explicit operator bool() const noexcept;
    [[nodiscard]] std::shared_ptr<const ScopeTap> tap() const;
private:
    struct State;
    explicit ScopeLease(std::shared_ptr<State>);
    std::shared_ptr<State> state_;
    friend class Session;
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
