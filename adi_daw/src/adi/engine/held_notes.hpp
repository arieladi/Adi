// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The notes a node has been handed and not yet released (ADR-0158).
//
// ADR-0043 suspends a node whose input is silent, that has no events this
// block, and whose declared tail has run out. An instrument holding a chord
// meets all three: nothing reaches it between the note-on and the note-off,
// it has no audio input, and a plugin that reports no tail -- the fixture, and
// many real ones -- runs out on the next block. The chord stops, and comes
// back for a moment when the note-off wakes the node to release it. Surge XT
// reports two seconds, so a held chord stopped two seconds in. This table is
// what tells the graph the instrument is still playing.

#include "adi/engine/events.hpp"

#include <array>
#include <atomic>
#include <cstdint>

namespace adi::engine {

class HeldNotes {
public:
    /// ADR-0155's polyphony per track, so a MIDI clip cannot outgrow it.
    static constexpr std::int32_t kCapacity = 128;

    /// One block's events, in list order (the list is stably sorted by frame,
    /// and a note's on always precedes its off in it).
    ///
    /// Note-ons count only when `consumes`: a node that passes notes on is not
    /// the one sounding them. Note-offs always count, so a device bypassed
    /// between a note's start and its end still lets it go.
    void take(const EventList& events, bool consumes) noexcept {
        bool keysChanged = false;
        for (const Event& e : events) {
            if (e.type == EventType::NoteOn && consumes) {
                on(e);
                keysChanged = true;
            } else if (e.type == EventType::NoteOff) {
                off(e);
                keysChanged = true;
            }
        }
        if (keysChanged) publishKeys();
    }

    [[nodiscard]] bool any() const noexcept { return count_ > 0 || untracked_ > 0; }
    [[nodiscard]] std::int32_t count() const noexcept { return count_ + untracked_; }

    // Owner/UI read of the driver's published key state. Neither the note IDs nor
    // the audio thread's mutable key table are exposed across threads.
    [[nodiscard]] std::array<std::uint64_t, 2> keyMask() const noexcept {
        for (int tries = 0; tries < 3; ++tries) {
            const auto before = keyRevision_.load();
            if ((before & 1u) != 0) continue;
            const std::array result{keyMask_[0].load(), keyMask_[1].load()};
            if (keyRevision_.load() == before) return result;
        }
        return {};
    }

    // A quiescent driver releases all instruments when the audio device closes.
    void clear() noexcept {
        count_ = untracked_ = 0;
        publishKeys();
    }

private:
    void publishKeys() noexcept {
        std::array<std::uint64_t, 2> mask{};
        for (std::int32_t i = 0; i < count_; ++i) {
            const auto key = keys_[static_cast<std::size_t>(i)];
            if (key < 128) mask[static_cast<std::size_t>(key / 64)] |= std::uint64_t{1} << (key % 64);
        }
        // Odd during publication. The two words always describe one callback.
        keyRevision_.fetch_add(1);
        keyMask_[0].store(mask[0]);
        keyMask_[1].store(mask[1]);
        keyRevision_.fetch_add(1);
    }
    // IDS, NOT A COUNTER. A source resends an off whose delivery it could not
    // confirm (ADR-0155 d2), and a counter would let the duplicate release a
    // different note that is still held -- the one failure here that is heard.
    // Id 0 is legal for a note with no expression (events.hpp) and is matched
    // by key among the other unassigned notes, as MpeRouter matches it.
    void on(const Event& e) noexcept {
        if (e.noteId != 0)
            for (std::int32_t i = 0; i < count_; ++i)
                if (ids_[static_cast<std::size_t>(i)] == e.noteId) return;   // already held
        if (count_ == kCapacity) {
            // Counted, not tracked: it keeps the node awake until an off that
            // matches nothing retires it. Awake is the harmless failure.
            ++untracked_;
            return;
        }
        ids_[static_cast<std::size_t>(count_)] = e.noteId;
        keys_[static_cast<std::size_t>(count_)] = e.dim;
        ++count_;
    }

    void off(const Event& e) noexcept {
        for (std::int32_t i = 0; i < count_; ++i) {
            const auto k = static_cast<std::size_t>(i);
            if (ids_[k] != e.noteId || (e.noteId == 0 && keys_[k] != e.dim)) continue;
            --count_;
            ids_[k] = ids_[static_cast<std::size_t>(count_)];   // order does not matter
            keys_[k] = keys_[static_cast<std::size_t>(count_)];
            return;
        }
        if (untracked_ > 0) --untracked_;
    }

    std::array<std::uint64_t, kCapacity> ids_{};
    std::array<std::uint16_t, kCapacity> keys_{};
    std::array<std::atomic<std::uint64_t>, 2> keyMask_{};
    std::atomic<std::uint32_t> keyRevision_{0};
    std::int32_t count_ = 0;
    std::int32_t untracked_ = 0;
};

}  // namespace adi::engine
