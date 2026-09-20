// SPDX-License-Identifier: GPL-3.0-or-later
//
// The event half of a port (ADR-0045), and the type that carries MPE+.
//
// Every port in the graph is a pair — audio buffers AND an event list, never
// one or the other. A typed port is what would force a special case at every
// junction and make "hybrid track" a feature rather than the absence of a
// restriction.
//
// THE VALUE IS A DOUBLE AND THIS IS LOAD-BEARING. ADR-0054: 7-bit and 14-bit
// are wire encodings, and no MIDI byte survives the input parser. A single
// `std::uint8_t` here would quietly undo the whole MPE+ mandate while every
// document still claimed compliance, and nothing would fail — the Continuum's
// extra bits would simply stop arriving. VST3's `INoteExpressionController`
// and CLAP's `CLAP_EVENT_NOTE_EXPRESSION` both take a double, so the narrow
// point in the chain would only ever have been our own code.

#pragma once

#include <cstdint>

namespace adi::engine {

enum class EventType : std::uint8_t {
    NoteOn = 0,
    NoteOff = 1,
    /// Per-note pitch / timbre / pressure. `dim` is `adi::ExpressionDim`.
    NoteExpression = 2,
    /// A parameter's stored value changed: the user turned a knob.
    ParamValue = 3,
    /// A modulation offset applied ON TOP of the stored value, which is not
    /// changed (ADR-0046, ADR-0052). CLAP carries this natively; for VST3 the
    /// host resolves it into a value and keeps the user's setting shadowed.
    ParamMod = 4,
};

struct Event {
    /// Offset from the start of the BLOCK, not of the segment. Segment-relative
    /// offsets would have to be rewritten every time the scheduler split
    /// differently, and a value that changes with how it was scheduled is not
    /// a property of the music.
    std::int32_t frame = 0;

    EventType type = EventType::NoteOn;

    /// The channel a note arrived on. Transport, not identity: ADR-0054 says a
    /// reader MUST NOT reconstruct MPE member allocation from it.
    std::uint8_t channel = 0;

    /// `adi::ExpressionDim` for NoteExpression; unused otherwise.
    std::uint16_t dim = 0;

    /// Anchors expression to its note, matching `ANOT.note_id`. Zero means
    /// unassigned, which is legal for a note that carries no expression.
    std::uint64_t noteId = 0;

    std::uint32_t paramId = 0;

    /// Floating point, always. See the header comment.
    double value = 0.0;
};

/// A fixed-capacity list over storage someone else owns.
///
/// Fixed because `push` runs on the audio thread and ADR-0010 forbids
/// allocating there. Capacity comes from `prepare`, where allocating is
/// allowed, and an overflow is COUNTED rather than silently dropped: at 500 Hz
/// with heavy polyphony the capacity is a real design number, and "some events
/// vanished sometimes" is exactly the bug nobody can reproduce.
class EventList {
public:
    EventList() = default;
    EventList(Event* storage, std::int32_t capacity) noexcept
        : data_(storage), capacity_(capacity) {}

    [[nodiscard]] std::int32_t size() const noexcept { return size_; }
    [[nodiscard]] std::int32_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] std::int64_t dropped() const noexcept { return dropped_; }

    [[nodiscard]] const Event* begin() const noexcept { return data_; }
    [[nodiscard]] const Event* end() const noexcept { return data_ + size_; }

    /// Total: a bad index yields a default Event rather than undefined
    /// behaviour, the same rule `StreamReader::at` follows for a blob.
    [[nodiscard]] Event at(std::int32_t i) const noexcept {
        if (data_ == nullptr || i < 0 || i >= size_) return Event{};
        return data_[i];
    }

    bool push(const Event& e) noexcept {
        if (data_ == nullptr || size_ >= capacity_) { ++dropped_; return false; }
        data_[size_++] = e;
        return true;
    }

    void clear() noexcept { size_ = 0; }
    void resetDropped() noexcept { dropped_ = 0; }

    /// Append every event of `other`, unchanged. What a summing node does with
    /// the event half of its inputs (ADR-0044): audio mixes, events
    /// concatenate.
    void append(const EventList& other) noexcept {
        for (std::int32_t i = 0; i < other.size_; ++i) push(other.data_[i]);
    }

    /// Stable sort by frame, so a segment boundary sees events in time order
    /// and two events at the same frame keep the order they were produced in.
    /// Insertion sort: the lists are short and nearly sorted already, and it
    /// allocates nothing, which `std::stable_sort` does not promise.
    void sortByFrame() noexcept {
        for (std::int32_t i = 1; i < size_; ++i) {
            const Event key = data_[i];
            std::int32_t j = i - 1;
            while (j >= 0 && data_[j].frame > key.frame) {
                data_[j + 1] = data_[j];
                --j;
            }
            data_[j + 1] = key;
        }
    }

private:
    Event* data_ = nullptr;
    std::int32_t capacity_ = 0;
    std::int32_t size_ = 0;
    std::int64_t dropped_ = 0;
};

/// A read-only window onto part of an EventList — the events of one segment.
struct EventSpan {
    const Event* first = nullptr;
    std::int32_t count = 0;

    [[nodiscard]] const Event* begin() const noexcept { return first; }
    [[nodiscard]] const Event* end() const noexcept { return first + count; }
    [[nodiscard]] bool empty() const noexcept { return count == 0; }
};

}  // namespace adi::engine
