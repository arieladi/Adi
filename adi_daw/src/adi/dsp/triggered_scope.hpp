// SPDX-License-Identifier: GPL-3.0-only
// Acquisition mechanics derived from s(M)exoscope; see docs/TRIGGERED-SCOPE.md.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace adi::dsp {
// A continuous, single-selected-lane capture. No clock, audio writes, source
// subscription, channel routing, publication service or viewport dimensions.
class TriggeredScope final {
  public:
    enum class Trigger { Free, Rising, Falling, Internal };
    struct Settings {
        double sampleRate = 48000;
        std::size_t columns = 1024;
        Trigger trigger = Trigger::Free;
        float threshold = 0, gain = 1;
        double columnsPerSample = .1, internalCyclesPerSample = .0001;
        std::uint32_t holdoffSamples = 1;
        bool removeDc = false;
        bool operator==(const Settings &) const = default;
    };
    struct Normalized {
        float level = .5f, amplitude = .5f, time = .5f, holdoff = 0, speed = .5f;
    };
    struct Column {
        // Extrema in their final-extremum order, after display gain/clipping.
        float first = 0, second = 0;
        std::int64_t firstSequence = 0, lastSequence = 0;
        bool operator==(const Column &) const = default;
    };
    struct Capture {
        std::span<const Column> columns;
        std::uint64_t epoch = 0, serial = 0;
        // -1 denotes the initial, non-triggered acquisition.
        std::int64_t triggerSequence = -1;
    };
    static constexpr std::size_t kMaximumColumns = 8192;
    TriggeredScope() noexcept;
    ~TriggeredScope();
    TriggeredScope(const TriggeredScope &) = delete;
    TriggeredScope &operator=(const TriggeredScope &) = delete;
    // Optional original normalized equations; changes only acquisition controls
    // in output. Invalid input preserves output. Not an ADI factory preset.
    static bool mapNormalized(const Normalized &, Settings &output) noexcept;
    // Owner-stopped replacement; failure preserves the complete previous stream.
    bool prepare(const Settings &) noexcept;
    bool prepared() const noexcept;
    const Settings *settings() const noexcept;
    // Invalidate both captures and restart numerical state, without allocation.
    // Call at a gap/source epoch change before feeding a new contiguous sequence.
    void reset() noexcept;
    // Entire input preflight before mutation. Empty spans advance nothing.
    bool processChecked(std::span<const float>, std::int64_t firstSequence) noexcept;
    // Internal producer entry avoids the finite-input scan. Input must be finite;
    // prepared/span/sequence/contiguity guards remain. Single serialized owner.
    bool processTrustedFinite(std::span<const float>, std::int64_t firstSequence) noexcept;
    // Borrowed views expire on the next mutation/destruction. Invalid tails are
    // not exposed. Previous is captured BEFORE ingesting a retriggering sample.
    Capture current() const noexcept;
    Capture previous() const noexcept;
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace adi::dsp
