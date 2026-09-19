// SPDX-License-Identifier: GPL-3.0-or-later
//
// The seam between an audio device and the graph.
//
// `src/juce/` owns devices and plugin hosting; `src/adi/engine/` owns the graph
// and has no JUCE in it (ADR-0036). This header is the one place they meet, and
// it deliberately lives on the engine side: the graph is what gets driven, so
// the graph declares how it is driven. It pulls in nothing but `<cstdint>`.
//
// It exists early and on its own so that neither side waits for the other. The
// device can be built and tested against `SilenceProcessor` before a graph
// exists, and the graph can be built and tested against a synthetic driver
// before a device exists — which is the same property ADR-0036 bought for the
// snapshot handoff, applied one layer out.

#pragma once

#include <cstdint>

namespace adi::engine {

/// One callback's worth of audio, as the device hands it over.
///
/// Raw pointers rather than a buffer type, because the buffer type on the other
/// side is JUCE's and this header must not know that. Nothing here owns
/// anything; every pointer is valid for the duration of one `process` call and
/// not one sample longer.
struct AudioIo {
    float* const* out = nullptr;         ///< `numOut` channels of `frames`
    const float* const* in = nullptr;    ///< `numIn` channels, may be null
    std::int32_t numOut = 0;
    std::int32_t numIn = 0;

    /// Frames in THIS callback. Anywhere from 1 to the `maxFrames` last
    /// prepared — a host may hand over fewer than the maximum and routinely
    /// does, so sizing anything from `maxFrames` at call time is a bug that
    /// only shows on the one driver that varies it.
    std::int32_t frames = 0;

    /// Frames since the stream started. Monotonic, never reset by a dropout,
    /// and the only clock the audio thread should trust: wall-clock time on the
    /// audio thread is a syscall and a lie in roughly that order.
    std::int64_t streamTimeSamples = 0;
};

/// What an audio device drives. The graph implements it.
class BlockProcessor {
public:
    virtual ~BlockProcessor() = default;

    BlockProcessor(const BlockProcessor&) = delete;
    BlockProcessor& operator=(const BlockProcessor&) = delete;

    /// Message thread, before the stream starts and again after any device or
    /// buffer-size change.
    ///
    /// `maxFrames` is the size the driver **granted**, never the size that was
    /// requested. ADR-0049: asking macOS CoreAudio for 8192 returns 4096 and
    /// says nothing, so a buffer sized from the request is an overrun with no
    /// warning. Whoever calls this reads the granted value back and passes
    /// that. Every allocation an implementation needs happens here, because
    /// `process` may not allocate.
    virtual void prepare(double sampleRate, std::int32_t maxFrames) = 0;

    /// Audio thread. Must not allocate, lock, log, touch the filesystem or
    /// throw — ADR-0010, and at 4096 frames a single allocation is a dropout
    /// the user will describe as "it glitched when I added a reverb".
    ///
    /// `noexcept` is load-bearing rather than decorative: an exception crossing
    /// a driver callback is undefined behaviour on every platform we target,
    /// and this makes it a `std::terminate` at the boundary that caused it
    /// instead of a corruption somewhere else.
    virtual void process(const AudioIo& io) noexcept = 0;

    /// Message thread, after the stream stops. Paired with `prepare`, and may
    /// be called without a preceding `prepare` when a device fails to open.
    virtual void release() = 0;

protected:
    BlockProcessor() = default;
};

/// Writes silence and nothing else.
///
/// Not a placeholder to delete later. It is what a device target is tested
/// against — "the callback ran, at the granted size, without allocating" is a
/// claim about the device, and proving it through a real graph would prove two
/// things at once and diagnose neither. It is also the correct processor to
/// install while a project loads or after a graph fails to build: outputting
/// silence is the only safe thing an audio callback can do when it has nothing
/// to say, and leaving the buffer untouched is how a driver's uninitialised
/// memory reaches somebody's monitors.
class SilenceProcessor final : public BlockProcessor {
public:
    void prepare(double sampleRate, std::int32_t maxFrames) override {
        sampleRate_ = sampleRate;
        maxFrames_ = maxFrames;
        ++prepareCount_;
    }

    void process(const AudioIo& io) noexcept override {
        for (std::int32_t c = 0; c < io.numOut; ++c) {
            float* dst = io.out ? io.out[c] : nullptr;
            if (dst == nullptr) continue;
            for (std::int32_t i = 0; i < io.frames; ++i) dst[i] = 0.0f;
        }
        ++callbacks_;
        frames_ += io.frames;
        if (io.frames > widest_) widest_ = io.frames;
    }

    void release() override { ++releaseCount_; }

    // Observable state, so a device test can assert on what actually happened
    // rather than on what it hoped would.
    [[nodiscard]] double sampleRate() const { return sampleRate_; }
    [[nodiscard]] std::int32_t maxFrames() const { return maxFrames_; }
    [[nodiscard]] std::int64_t callbacks() const { return callbacks_; }
    [[nodiscard]] std::int64_t frames() const { return frames_; }
    [[nodiscard]] std::int32_t widestBlock() const { return widest_; }
    [[nodiscard]] int prepareCount() const { return prepareCount_; }
    [[nodiscard]] int releaseCount() const { return releaseCount_; }

private:
    double sampleRate_ = 0.0;
    std::int32_t maxFrames_ = 0;
    std::int64_t callbacks_ = 0;
    std::int64_t frames_ = 0;
    std::int32_t widest_ = 0;
    int prepareCount_ = 0;
    int releaseCount_ = 0;
};

}  // namespace adi::engine
