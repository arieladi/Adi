// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/engine/events.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
namespace adi::engine {
// MpeParser note channels are 1..16; clip notes and Controller addresses are
// 0..15. Declare the note convention; never guess a channel or MPE member.
enum class NoteChannelBase { Zero, One };
struct RememberedMidiEvent {
    std::int64_t sample=0;
    std::uint64_t serial=0;
    Event event;
};
struct MidiMemorySnapshot {
    std::int64_t inputId=0,trackId=0,endSample=0;
    std::uint32_t sampleRate=0;
    std::uint64_t epoch=0;
    NoteChannelBase noteChannels=NoteChannelBase::Zero;
    bool prefixDropped=false; // oldest events exceeded capacity; not a complete phrase claim
    std::vector<RememberedMidiEvent> events;
};
// One eligible input/track, one producer serializing gates/push/advance, one
// concurrent owner taking snapshots. Queue UI gate changes to the producer;
// construct/destroy quiescent. Armed OR monitored input listens independently
// of transport play/record. On/off clears history, forward rests preserve it.
// Backwards clocks, explicit discontinuities, malformed musical input and
// pinned-slot drops begin a new epoch. Capacity is 2..1,048,576 events.
//
// Producer operations allocate, lock, log and perform file I/O nowhere. Neither
// side waits: a pinned slot drops/counts its event; a racing snapshot fails
// whole. Doubles, IDs, timestamps and coincident serial order are preserved.
// Feed live input only, never playback/controller-delivery events.
class MidiMemory {
public:
    MidiMemory(std::int64_t inputId,std::int64_t trackId,std::uint32_t sampleRate,
               NoteChannelBase,std::size_t capacity=65536);
    void setListening(bool armed,bool monitored) noexcept;
    [[nodiscard]] bool listening() const noexcept { return listening_.load(std::memory_order_relaxed); }
    // Absolute driver clock, not event.frame or consumer drain time. Original
    // nonnegative block-relative frame is retained unchanged.
    bool push(const Event&,std::int64_t sample) noexcept;
    // After all events through an exclusive block end, including empty blocks,
    // publish the clock so owner-side Capture can later close held notes.
    bool advance(std::int64_t endSample) noexcept;
    bool discontinuity(std::int64_t nextSample) noexcept;
    // Owner only; immutable bounded input history, not a phrase, MIDI file,
    // inferred tempo/loop or committed native clip. Failure leaves output empty.
    bool snapshot(MidiMemorySnapshot&,std::string& error) const;
    struct Stats { std::uint64_t copied=0,dropped=0,ignored=0,invalid=0; };
    [[nodiscard]] Stats stats() const noexcept;
private:
    enum : std::uint8_t { Vacant, Ready, Writing, Reading };
    struct Slot {
        mutable std::atomic<std::uint8_t> state{Vacant};
        std::uint64_t epoch=0;
        RememberedMidiEvent value;
    };
    void reset(std::int64_t) noexcept;
    void publish() noexcept;
    bool invalid() noexcept;
    const std::int64_t input_,track_;
    const std::uint32_t rate_;
    const NoteChannelBase channels_;
    const std::size_t capacity_;
    std::unique_ptr<Slot[]> slots_;
    std::atomic<bool> listening_{false};
    std::uint64_t epoch_=1,first_=0,next_=0;
    std::int64_t lastEvent_=-1,end_=0,floor_=0;
    std::atomic<std::uint64_t> revision_{0},publishedEpoch_{1},publishedFirst_{0},publishedNext_{0};
    std::atomic<std::int64_t> publishedEnd_{0};
    std::atomic<bool> publishedTruncated_{false};
    std::atomic<std::uint64_t> copied_{0},dropped_{0},ignored_{0},invalid_{0};
};
} // namespace adi::engine
