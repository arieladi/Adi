// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The keyboard into the armed track's instrument (N60's window half; SRF-05). The MIDI port host's
// inputs fill one lock-free ring each, on their ports' own threads. This feed is the rings' one consumer:
// on the audio thread, in the device callback before the document processes the block, it drains every
// ring, reads each message with the engine's own parser (`engine::MpeParser`, the port's MPE switch
// choosing its zone), stamps each event in stream samples from its port time, and hands them in time
// order to `Session::enqueueMidiInput`, the session's one serialized MIDI producer.
//
// - Only a port whose Track switch is on reaches the tracks (Live 12 manual 30.2.1). A port opened for
//   Remote only is drained and its events go to nothing until a control surface consumes them.
// - Time. A port stamps an event with the high-resolution clock in ms. At each block the feed anchors
//   that clock to the block's first stream sample and plays what arrived during the last block's span
//   one block later at the same offset within the block, so the gaps between notes are kept. An event
//   older than that lands on the block's first sample; none is ever put before an earlier one.
// - The device restarting its sample clock (a new device, rate or buffer) invalidates the anchor and the
//   parsers' held notes (`restart`); the application then resets the session's input, as the session's
//   contract asks of a producer whose clock restarts.
// - One parser per port: a UMP-native device's groups (its jacks) share it and its channels; on ALSA and
//   CoreMIDI each jack is a port of its own.
// - Note ids are the parser's, made unique across ports with the port's slot in their high bits, and
//   nonzero, as the session requires; channels are given zero-based (the parser counts a note's from 1).
//
// No JUCE, no allocation and no lock on the audio thread. The source list the audio thread reads is
// published from the message thread into one of two fixed buffers; `publish` waits for the audio thread
// to leave the list it replaces, so the caller may free the rings that list named once it returns.
#pragma once
#include "adi/engine/events.hpp"
#include "adi/engine/mpe_input.hpp"
#include "adi/ui/midi_ports.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <thread>
#include <type_traits>
#include <utility>
namespace adi::ui {
class MidiInputFeed {
  public:
    using Ring = MidiEventRing<1024>;
    struct Source {
        Ring *ring = nullptr;
        std::uint32_t port = 0; // the host's slot: stable while the input stays open
        bool track = false, remote = false, mpe = false;
        std::uint64_t serial = 0; // which opening of the slot: a new one starts with a clean parser
        std::uint64_t portKey = 0;
    };
    static constexpr std::size_t kMaxSources = 64;
    // Events taken from the rings in one block; the rest wait in their rings for the next.
    static constexpr std::size_t kMaxPerBlock = 512;

    // The message thread supplies typed notes with unique computer-keyboard identities.
    // They share the port clock and the audio callback's single Session producer.
    bool pushComputerMidi(double timeMs, const engine::Event &event) noexcept {
        if (!std::isfinite(timeMs) || !engine::isNoteStream(event.type))
            return false;
        if (computerMidi_.push({timeMs, event}))
            return true;
        computerOverflow_.store(true, std::memory_order_release);
        return false;
    }

    // Message thread. The sources the audio thread drains from its next block on. Returns once the audio
    // thread no longer reads the list this replaces, so the rings only that list named may then be freed.
    // At most `kMaxSources`, and a slot below it; the rest are left out.
    void publish(std::span<const Source> sources) noexcept {
        const int next = 1 - active_.load(std::memory_order_acquire);
        auto &list = lists_[static_cast<std::size_t>(next)];
        list.count = 0;
        for (const auto &s : sources)
            if (s.ring != nullptr && s.serial != 0 && s.port < kMaxSources && list.count < kMaxSources)
                list.items[list.count++] = s;
        // Sequentially consistent with `pump`'s own store and load: either this sees the audio thread
        // holding the old list and waits, or the audio thread sees the new one before it reads any.
        active_.store(next, std::memory_order_seq_cst);
        // The audio thread holds a list only within one pump; wait it out.
        while (reading_.load(std::memory_order_seq_cst) == 1 - next)
            std::this_thread::yield();
    }
    // The device restarted its sample clock: reset the anchor and parsers, and discard queued computer notes.
    void restart() noexcept { restart_.store(true, std::memory_order_release); }
    // Events read but not enqueued because the session's queue was full or refused them.
    std::uint64_t refused() const noexcept { return refused_.load(std::memory_order_relaxed); }

    // Audio thread, once per device callback, before the document processes the block that starts at
    // stream sample `blockStart` and lasts `frames` at `sampleRate`; `nowMs` is the ports' clock now.
    // `enqueue(streamSample, event, inputSample, portKey)` returns whether the session took it. Returns how many it took.
    template <class Enqueue>
    std::size_t pump(std::int64_t blockStart, std::int32_t frames, double sampleRate, double nowMs, Enqueue &&enqueue) noexcept {
        return pump(blockStart, frames, sampleRate, nowMs, std::forward<Enqueue>(enqueue), []() noexcept {});
    }
    // If the keyboard queue overflowed, its missing event may be a release. Reset the Session's
    // live input before enqueueing any new events so old voices end on this callback.
    template <class Enqueue, class ResetComputerMidi>
    std::size_t pump(std::int64_t blockStart, std::int32_t frames, double sampleRate, double nowMs,
                     Enqueue &&enqueue, ResetComputerMidi &&resetComputerMidi) noexcept {
        const bool restarted = restart_.exchange(false, std::memory_order_acq_rel);
        const bool overflowed = computerOverflow_.exchange(false, std::memory_order_acq_rel);
        if (restarted || overflowed)
            computerMidi_.discardPending();
        if (overflowed && !restarted)
            resetComputerMidi();
        if (restarted) {
            for (auto &p : *parsers_)
                p.reset();
            seen_.fill(0);
            last_ = -1;
        }
        // Announce the list, then confirm it is still the one in force; until a load after the store
        // finds it unchanged, a publish may have moved on and its caller be freeing the rings it named.
        // Once confirmed, any publish moving away from it sees it announced and waits for the end.
        int now = active_.load(std::memory_order_seq_cst);
        for (;;) {
            reading_.store(now, std::memory_order_seq_cst);
            const int again = active_.load(std::memory_order_seq_cst);
            if (again == now)
                break;
            now = again;
        }
        const auto &list = lists_[static_cast<std::size_t>(now)];
        std::size_t taken = 0;
        pending_ = 0;
        // Give computer notes their own bounded share and keep all remaining
        // capacity available to the physical ports, including when idle.
        std::array<ComputerMidi, kMaxComputerPerBlock> computer;
        for (auto &message : computer) {
            if (!computerMidi_.pop(message))
                break;
            MidiPortEvent stamp;
            stamp.timeMs = message.timeMs;
            buffer_[pending_++] = {stamp, &message.event};
        }
        // The block's budget is shared in turn: the port served first rotates, and each takes at most an
        // equal share of what is left, so one busy port cannot starve the others.
        const std::size_t n = list.count;
        for (std::size_t j = 0; j < n; ++j) {
            const auto &s = list.items[(start_ + j) % n];
            auto &parser = (*parsers_)[s.port];
            // A slot that now holds another input starts with a clean parser; a change of its MPE
            // switch changes the zone only, so the keys held keep their releases.
            if (seen_[s.port] != s.serial) {
                parser.reset();
                seen_[s.port] = s.serial;
                mpe_[s.port] = !s.mpe;
            }
            if (mpe_[s.port] != s.mpe) {
                parser.setZone(s.mpe ? engine::MpeZone::lower() : engine::MpeZone{});
                mpe_[s.port] = s.mpe;
            }
            const std::size_t share = (kMaxPerBlock - pending_) / (n - j);
            MidiPortEvent e;
            for (std::size_t popped = 0; popped < share && s.ring->pop(e); ++popped) {
                if (!s.track)
                    continue; // Remote only: no track hears it
                e.port = s.port;
                buffer_[pending_++] = {e, nullptr, s.portKey};
            }
        }
        reading_.store(-1, std::memory_order_release);
        start_ = n > 0 ? (start_ + 1) % n : 0;
        // Time order across the ports; a port's own order where the times are equal. In place and stable:
        // `std::stable_sort` may allocate, which the audio thread must not, and a block holds a few events.
        for (std::size_t i = 1; i < pending_; ++i) {
            const auto e = buffer_[i];
            std::size_t j = i;
            for (; j > 0 && e.wire.timeMs < buffer_[j - 1].wire.timeMs; --j)
                buffer_[j] = buffer_[j - 1];
            buffer_[j] = e;
        }
        const std::int64_t first = blockStart, lastSample = blockStart + std::max<std::int32_t>(frames, 1) - 1;
        for (std::size_t i = 0; i < pending_; ++i) {
            const auto &e = buffer_[i].wire;
            const auto *computerEvent = buffer_[i].computer;
            auto inputSample = first;
            auto sample = first;
            if (sampleRate > 0 && std::isfinite(e.timeMs) && std::isfinite(nowMs)) {
                // The ports stamp with JUCE's millisecond counter, which some backends read in its 32-bit
                // form that wraps every 2^32 ms while the high-resolution one does not: the age is taken
                // modulo the wrap.
                inputSample = blockStart + static_cast<std::int64_t>(
                    std::llround(std::remainder(e.timeMs - nowMs, 4294967296.0) * sampleRate / 1000.0));
                sample = inputSample + frames;
            }
            sample = std::clamp(sample, std::max(first, last_), lastSample);
            inputSample = std::clamp(inputSample, std::int64_t{0}, sample);
            Out out{};
            if (computerEvent) {
                out.events[0] = *computerEvent;
                out.count = 1;
            } else {
                (*parsers_)[e.port].feed(e.bytes.data(), e.size, &Out::sink, &out);
            }
            for (std::size_t k = 0; k < out.count; ++k) {
                auto ev = out.events[k];
                ev.frame = 0;
                if (!computerEvent) {
                    // The parser numbers a note's channel 1 to 16 and a controller's 0 to 15.
                    // Computer events already have their Session channel and unique ID.
                    if (ev.type != engine::EventType::Controller)
                        ev.channel = static_cast<std::uint8_t>(ev.channel > 0 ? ev.channel - 1 : 0);
                    // A release after a restart has no note identity and plays nothing.
                    if ((ev.type == engine::EventType::NoteOn || ev.type == engine::EventType::NoteOff) && ev.noteId == 0)
                        continue;
                    if (ev.noteId != 0)
                        ev.noteId = (static_cast<std::uint64_t>(e.port + 1) << 40) | (ev.noteId & ((std::uint64_t{1} << 40) - 1));
                }
                const bool accepted = [&] {
                    if constexpr (std::is_invocable_v<Enqueue &, std::int64_t, const engine::Event &, std::int64_t, std::uint64_t>)
                        return enqueue(sample, ev, inputSample, buffer_[i].portKey);
                    else
                        return enqueue(sample, ev, inputSample);
                }();
                if (accepted) {
                    ++taken;
                    last_ = sample;
                } else {
                    refused_.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        return taken;
    }

  private:
    static constexpr std::size_t kMaxComputerPerBlock = 32;
    struct ComputerMidi {
        double timeMs = 0;
        engine::Event event;
    };
    struct PendingInput {
        MidiPortEvent wire;
        const engine::Event *computer = nullptr;
        std::uint64_t portKey = 0;
    };
    struct List {
        std::array<Source, kMaxSources> items{};
        std::size_t count = 0;
    };
    // What one message parses into: a note with its initial expression is a handful of events.
    struct Out {
        std::array<engine::Event, 16> events{};
        std::size_t count = 0;
        static bool sink(void *ctx, const engine::Event &e) {
            auto &o = *static_cast<Out *>(ctx);
            if (o.count == o.events.size())
                return false;
            o.events[o.count++] = e;
            return true;
        }
    };
    std::array<List, 2> lists_{};
    std::atomic<int> active_{0};
    std::atomic<int> reading_{-1};
    std::atomic<bool> restart_{false};
    MidiEventRing<1024, ComputerMidi> computerMidi_;
    std::atomic<bool> computerOverflow_{false};
    std::atomic<std::uint64_t> refused_{0};
    // Audio thread only.
    // About 74 kB a parser: on the heap, so a feed fits on any stack. Made with the feed, never on the
    // audio thread.
    std::unique_ptr<std::array<engine::MpeParser, kMaxSources>> parsers_ =
        std::make_unique<std::array<engine::MpeParser, kMaxSources>>();
    std::size_t start_ = 0;
    std::array<std::uint64_t, kMaxSources> seen_{};
    std::array<bool, kMaxSources> mpe_{};
    std::array<PendingInput, kMaxPerBlock> buffer_{};
    std::size_t pending_ = 0;
    std::int64_t last_ = -1;
};
} // namespace adi::ui
