// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/engine/events.hpp"
#include "adi/engine/launcher.hpp"
#include "adi/blob.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace adi::engine {

struct NoteExpressionCurve;

/// Owner-prepared ANOT intervals in a slot clip's native musical coordinates.
/// A zero or duplicate stored note ID can play, but cannot authorize retaining
/// a voice when a new clip program is published.
class LaunchMidiClip {
public:
    [[nodiscard]] static std::optional<LaunchMidiClip> prepare(
        std::int64_t clipId, std::span<const rows::Note> notes,
        std::span<const NoteExpressionCurve* const> expression = {},
        std::span<const controllers::Lane* const> controllers = {},
        std::vector<std::string>* problems = nullptr);
    [[nodiscard]] std::int64_t id() const noexcept { return id_; }

private:
    struct Point { std::int64_t tick; double value; float tension; std::uint8_t curve; };
    struct Range { std::size_t first = 0, count = 0; };
    struct Note {
        std::int64_t start, end, longestEnd;
        std::uint64_t id;
        Event on, off;
        double tuning;
        std::array<Range,kStandardExpressionDimensionCount> expression{};
        std::uint64_t expressionIdentity = 0;
        [[nodiscard]] bool sameAuthored(const Note&) const noexcept;
    };
    struct Boundary {
        std::int64_t tick;
        std::size_t note;
        bool start;
    };
    struct ControlLane { std::uint16_t address; std::vector<Point> points; };

    [[nodiscard]] const std::size_t* stableIndex(std::uint64_t id) const noexcept;
    std::int64_t id_ = 0;
    std::vector<Note> notes_;
    std::vector<Boundary> boundaries_;
    std::vector<std::pair<std::uint64_t, std::size_t>> byId_;
    std::vector<Point> expression_;
    std::vector<ControlLane> controls_;
    std::vector<std::int64_t> controlKnots_;
    std::uint64_t controllerIdentity_ = 0;
    friend class LaunchMidiTrack;
};

/// Reuse the host track's runtime note allocator. Its IDs must be distinct
/// from the IDs of other voices the same instrument can hold.
struct LaunchMidiVoiceIds {
    void* context = nullptr;
    std::uint64_t (*take)(void*) noexcept = nullptr;
};

struct LaunchMidiRevision {
    std::uint64_t program = 0, route = 0;
};

/// The track source owns CC delivery/retries, including pedal releases and
/// pending graph events. Session uses the same address/acknowledgement state.
struct LaunchMidiControllers {
    void* context = nullptr;
    bool (*send)(void*, EventList&, std::int32_t offset, std::uint16_t address, double value) noexcept = nullptr;
    bool (*reset)(void*, EventList&, std::int32_t offset) noexcept = nullptr;
};

/// One driver-owned track state, shared across prepared graph generations.
/// Events are written to the graph's EventList, with block-relative frames.
/// The graph forwards delivery/rejection back to the driver. No ownership is
/// forgotten until its note-off is confirmed.
class LaunchMidiTrack {
public:
    explicit LaunchMidiTrack(std::int64_t track) noexcept : track_(track) {}
    LaunchMidiTrack(const LaunchMidiTrack&) = delete;
    LaunchMidiTrack& operator=(const LaunchMidiTrack&) = delete;

    /// Sections are chronological. baseOffset is the start of a contiguous
    /// transport slice inside the current callback. Keep the route identity
    /// stable for unchanged consumers and update the program revision on a
    /// new note image. Call release when the transport stops without a section.
    [[nodiscard]] bool render(const LaunchBlock&, const LaunchSection&, std::int32_t baseOffset,
                              const LaunchMidiClip*, LaunchMidiRevision,
                              LaunchMidiVoiceIds, EventList&, LaunchMidiControllers = {}) noexcept;
    [[nodiscard]] bool release(EventList&, std::int32_t offset, LaunchMidiControllers = {}) noexcept;
    [[nodiscard]] std::int64_t activeClip() const noexcept { return clip_; }
    [[nodiscard]] bool canRetainControllers(const LaunchMidiClip*, LaunchMidiRevision) const noexcept;
    // Share a published source's PDC handover with the Arrangement source.
    // Retained releases remain owned until its downstream acknowledgement.
    void adoptProgram(std::uint64_t program) noexcept;
    [[nodiscard]] std::int64_t eventClip(const Event&) const noexcept;
    [[nodiscard]] bool canRetainPendingEvent(const Event&, const LaunchMidiClip*, LaunchMidiRevision) const noexcept;
    void pendingEventRetained(const Event&, std::uint64_t program) noexcept;
    [[nodiscard]] bool retainedEventIsValid(const Event&, std::uint64_t program) const noexcept;
    [[nodiscard]] bool holdingNotes() const noexcept;
    void noteOnDelivered(const Event&) noexcept;
    void noteOnRejected(const Event&) noexcept;
    void noteOffDelivered(const Event&) noexcept;
    void noteOffRejected(const Event&) noexcept;
    [[nodiscard]] std::uint64_t droppedNotes() const noexcept { return dropped_; }

private:
    static constexpr std::size_t maximumOwned = 128; // ADR-0155
    struct Voice {
        LaunchMidiClip::Note note{};
        std::uint64_t runtimeId = 0;
        std::size_t index = 0;
        std::int64_t clip = 0;
        LaunchMidiRevision revision{};
        std::uint64_t retainedOn = 0, retainedOff = 0;
        bool pending = false, releasing = false, sounding = false, retry = false;
        bool expressionFresh = false;
        std::array<double,kStandardExpressionDimensionCount> expressionValue{};
        std::array<bool,kStandardExpressionDimensionCount> expressionSent{};
    };

    [[nodiscard]] bool stop(Voice&, EventList&, std::int32_t offset) noexcept;
    void start(const LaunchMidiClip&, std::size_t index, float velocity, EventList&,
               std::int32_t offset, LaunchMidiVoiceIds) noexcept;
    void emitOn(const Voice&, float velocity, EventList&, std::int32_t offset) noexcept;
    [[nodiscard]] Voice* voice(std::uint64_t runtimeId) noexcept;
    [[nodiscard]] const Voice* voice(std::uint64_t runtimeId) const noexcept;

    std::array<Voice, maximumOwned> active_{};
    std::int64_t track_, clip_ = 0, nextTick_ = 0, nextFrame_ = 0;
    std::uint64_t run_ = 0, dropped_ = 0, adoptedProgram_ = 0;
    std::uint64_t controllerIdentity_ = 0;
    LaunchMidiRevision revision_{};
};

} // namespace adi::engine
