// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/engine/events.hpp"
#include "adi/engine/process.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace adi::engine {

class Session;
class Transport;
class LiveMidiInput;

enum class MidiCaptureState : std::uint8_t {
    Idle, Waiting, Recording, Complete, InputReset, Discontinuity, Overflow, DeviceStopped
};

struct MidiCapturePacket {
    enum class Kind : std::uint8_t { Span, Event };
    Kind kind = Kind::Span;
    std::uint64_t epoch = 0;
    std::uint64_t pass = 0;          ///< zero-based, including passes without events
    std::int64_t streamSample = 0;   ///< accepted driver sample
    std::int64_t timelineSample = 0;
    std::int64_t inputSample = 0;    ///< the producer's sample, for an Event (may arrive late)
    std::int32_t frames = 0;         ///< Span's half-open length
    Event event;                    ///< typed input, frame relative to preceding Span
};

/// One recording at a time. The audio driver publishes packets, in order,
/// after accepting a complete slice. The owner drains them and retains the
/// take material. Each epoch freezes the ordered set of destination tracks.
class MidiInputCapture {
public:
    [[nodiscard]] MidiCaptureState state() const noexcept;
    [[nodiscard]] std::uint64_t epoch() const noexcept;
    /// Owner-side metadata for this epoch, valid until the next accepted start.
    [[nodiscard]] std::span<const std::int64_t> tracks() const noexcept { return tracks_; }
    [[nodiscard]] double sampleRate() const noexcept { return sampleRate_; }
    void stop() noexcept;  ///< owner request, applied at the next slice boundary
    std::size_t drain(std::span<MidiCapturePacket>) noexcept; ///< one owner reader

private:
    friend class Session;
    friend class LiveMidiInput;
    bool start(std::vector<std::int64_t> targets, double sampleRate);
    void deviceStopped() noexcept; ///< owner, with the driver already quiescent
    void beginSlice(const AudioIo&, const Transport&, std::uint64_t inputEpoch) noexcept;
    void record(std::int64_t inputSample, const Event&) noexcept;
    void endSlice() noexcept;

    void finish(MidiCaptureState) noexcept;
    bool append(MidiCapturePacket) noexcept;
    bool nextSpan() noexcept;

    static constexpr std::size_t capacity = 4096;
    std::array<MidiCapturePacket, capacity> ring_{};
    std::atomic<std::size_t> read_{0}, written_{0};
    std::atomic<std::uint64_t> epoch_{0}, stopEpoch_{0};
    std::atomic<MidiCaptureState> state_{MidiCaptureState::Idle};
    std::vector<std::int64_t> tracks_; // owner-only; the tap observes one common Track stream
    double sampleRate_ = 0;

    std::uint64_t driverEpoch_ = 0;
    bool active_ = false, started_ = false;
    std::uint64_t inputEpoch_ = 0, revision_ = 0, pass_ = 0;
    std::int64_t epochStart_ = 0, expectedStream_ = 0, expectedTimeline_ = 0;
    bool looping_ = false;
    std::int64_t loopBegin_ = 0, loopEnd_ = 0;

    // Driver scratch for the open slice. Queue publication advances only
    // after all spans and events fit, so overflow never exposes half a slice.
    const Transport* transport_ = nullptr;
    std::int64_t sliceStream_ = 0, spanTimeline_ = 0;
    std::int32_t sliceFrames_ = 0, spanBegin_ = 0, spanEnd_ = 0, lastEventFrame_ = -1;
    std::size_t cursor_ = 0, limitRead_ = 0;
};

}  // namespace adi::engine
