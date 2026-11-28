// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/engine/midi_hardware.hpp"
#include <array>
#include <atomic>

namespace adi::ui {

// Single audio producer, one output worker. A completed audio callback is
// published in one step so the worker can order events from multiple tracks.
class MidiOutputFeed {
public:
    struct Packet {
        std::uint64_t port = 0;
        std::int64_t track = 0;
        double dueMs = 0;
        std::uint32_t word = 0;
        std::uint32_t epoch = 0;
    };

    class Block {
    public:
        Block(MidiOutputFeed& feed, std::int64_t first, double nowMs, double rate) noexcept
            : feed_(feed), first_(first), nowMs_(nowMs), msPerSample_(rate > 0.0 ? 1000.0 / rate : 0.0) {}
        ~Block() { feed_.written_.store(feed_.pending_, std::memory_order_release); }
        Block(const Block&) = delete;
        Block& operator=(const Block&) = delete;

        engine::MidiHardwareSink sink{this,
            [](void* context, std::uint64_t port, std::int64_t track,
               std::int64_t sample, std::uint32_t word) noexcept {
                auto& block = *static_cast<Block*>(context);
                const auto time = block.nowMs_ + static_cast<double>(sample - block.first_) * block.msPerSample_;
                block.feed_.push(port, track, time, word);
            },
            [](void* context) noexcept { static_cast<Block*>(context)->feed_.reset(); }};

    private:
        MidiOutputFeed& feed_;
        std::int64_t first_;
        double nowMs_;
        double msPerSample_;
    };

    // A driver restart, lost queue data, or change of output routing releases
    // the notes the worker sent and invalidates stale packets.
    void reset() noexcept { revision_.fetch_add(1, std::memory_order_release); }
    std::uint32_t revision() const noexcept { return revision_.load(std::memory_order_acquire); }

    bool pop(Packet& packet) noexcept {
        const auto position = read_.load(std::memory_order_relaxed);
        if (position == written_.load(std::memory_order_acquire)) return false;
        packet = packets_[position % packets_.size()];
        read_.store(position + 1, std::memory_order_release);
        return true;
    }

private:
    void push(std::uint64_t port, std::int64_t track, double dueMs, std::uint32_t word) noexcept {
        const auto head = read_.load(std::memory_order_acquire);
        if (pending_ - head >= packets_.size()) {
            reset();
            return;
        }
        packets_[pending_ % packets_.size()] = {port, track, dueMs, word, revision()};
        ++pending_;
    }

    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
    std::array<Packet, 4096> packets_{};
    std::atomic<std::uint32_t> read_{0}, written_{0}, revision_{1};
    std::uint32_t pending_ = 0; // audio producer only
};
} // namespace adi::ui
