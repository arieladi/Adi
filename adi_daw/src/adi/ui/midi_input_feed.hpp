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
#include <span>
#include <thread>
namespace adi::ui {
class MidiInputFeed {
  public:
    using Ring = MidiEventRing<1024>;
    struct Source {
        Ring *ring = nullptr;
        std::uint32_t port = 0; // the host's slot: stable while the input stays open
        bool track = false, remote = false, mpe = false;
        std::uint64_t serial = 0; // which opening of the slot: a new one starts with a clean parser
    };
    static constexpr std::size_t kMaxSources = 64;
    // Events taken from the rings in one block; the rest wait in their rings for the next.
    static constexpr std::size_t kMaxPerBlock = 512;

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
    // The device restarted its sample clock: the anchor and the parsers' notes go at the next pump.
    void restart() noexcept { restart_.store(true, std::memory_order_release); }
    // Events read but not enqueued because the session's queue was full or refused them.
    std::uint64_t refused() const noexcept { return refused_.load(std::memory_order_relaxed); }

    // Audio thread, once per device callback, before the document processes the block that starts at
    // stream sample `blockStart` and lasts `frames` at `sampleRate`; `nowMs` is the ports' clock now.
    // `enqueue(streamSample, event)` returns whether the session took it. Returns how many it took.
    template <class Enqueue>
    std::size_t pump(std::int64_t blockStart, std::int32_t frames, double sampleRate, double nowMs, Enqueue &&enqueue) noexcept {
        if (restart_.exchange(false, std::memory_order_acq_rel)) {
            for (auto &p : parsers_)
                p.reset();
            seen_.fill(0);
            last_ = -1;
        }
        const int at = active_.load(std::memory_order_seq_cst);
        reading_.store(at, std::memory_order_seq_cst);
        // A publish between the load and the store left `at` stale, and its caller may already be
        // freeing that list's rings: read the list in force now, never the one `at` names.
        const int now = active_.load(std::memory_order_seq_cst);
        if (now != at)
            reading_.store(now, std::memory_order_seq_cst);
        const auto &list = lists_[static_cast<std::size_t>(now)];
        std::size_t taken = 0;
        pending_ = 0;
        for (std::size_t i = 0; i < list.count; ++i) {
            const auto &s = list.items[i];
            // A slot that now holds another input starts with a clean parser.
            if (seen_[s.port] != s.serial || mpe_[s.port] != s.mpe) {
                parsers_[s.port].reset();
                parsers_[s.port].setZone(s.mpe ? engine::MpeZone::lower() : engine::MpeZone{});
                seen_[s.port] = s.serial;
                mpe_[s.port] = s.mpe;
            }
            MidiPortEvent e;
            while (pending_ < kMaxPerBlock && s.ring->pop(e)) {
                if (!s.track)
                    continue; // Remote only: no track hears it
                e.port = s.port;
                buffer_[pending_++] = e;
            }
        }
        reading_.store(-1, std::memory_order_release);
        // Time order across the ports; a port's own order where the times are equal.
        std::stable_sort(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(pending_),
                         [](const MidiPortEvent &a, const MidiPortEvent &b) { return a.timeMs < b.timeMs; });
        const std::int64_t first = blockStart, lastSample = blockStart + std::max<std::int32_t>(frames, 1) - 1;
        for (std::size_t i = 0; i < pending_; ++i) {
            const auto &e = buffer_[i];
            auto sample = first;
            if (sampleRate > 0 && std::isfinite(e.timeMs) && std::isfinite(nowMs))
                sample = blockStart + frames + static_cast<std::int64_t>(std::llround((e.timeMs - nowMs) * sampleRate / 1000.0));
            sample = std::clamp(sample, std::max(first, last_), lastSample);
            Out out{};
            parsers_[e.port].feed(e.bytes.data(), e.size, &Out::sink, &out);
            for (std::size_t k = 0; k < out.count; ++k) {
                auto ev = out.events[k];
                ev.frame = 0;
                // The parser numbers a note's channel 1 to 16 and a controller's 0 to 15
                // (`midi_memory.hpp`); the session takes every channel from 0.
                if (ev.type != engine::EventType::Controller)
                    ev.channel = static_cast<std::uint8_t>(ev.channel > 0 ? ev.channel - 1 : 0);
                // A release whose note the parser no longer holds (one from before a restart) has no
                // id, and the session would refuse it: it plays nothing either way.
                if ((ev.type == engine::EventType::NoteOn || ev.type == engine::EventType::NoteOff) && ev.noteId == 0)
                    continue;
                if (ev.noteId != 0)
                    ev.noteId = (static_cast<std::uint64_t>(e.port + 1) << 40) | (ev.noteId & ((std::uint64_t{1} << 40) - 1));
                if (enqueue(sample, ev)) {
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
    std::atomic<bool> restart_{true};
    std::atomic<std::uint64_t> refused_{0};
    // Audio thread only.
    std::array<engine::MpeParser, kMaxSources> parsers_{};
    std::array<std::uint64_t, kMaxSources> seen_{};
    std::array<bool, kMaxSources> mpe_{};
    std::array<MidiPortEvent, kMaxPerBlock> buffer_{};
    std::size_t pending_ = 0;
    std::int64_t last_ = -1;
};
} // namespace adi::ui
