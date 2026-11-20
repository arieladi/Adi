// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/engine/host.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>

namespace adi::engine {

class MidiInputCapture;
class Transport;

/// Typed Track input. The producer and the audio driver share only the
/// fixed queue; per-device voice ownership stays on the audio driver.
class LiveMidiInput {
public:
    struct Voices {
        struct Voice {
            std::uint64_t sourceId = 0;
            Event off;
            bool releasing = false;
        };
        std::array<Voice, 128> active{};
        std::array<std::array<bool, 3>, 16> pedals{};
        std::int64_t track = 0;
        bool flush = false;
        bool retry = false;
    };
    struct Route {
        Node* node = nullptr;
        std::shared_ptr<Voices> voices;
        std::int64_t armedTrack = 0;
        bool processes = false;
        mutable NodeId slot = kInvalidNode;
    };

    bool push(std::int64_t sample, const Event&) noexcept;
    void reset() noexcept;
    void render(const GraphHost::RenderBlock&, const AudioIo&, std::span<const Route>,
                const Transport&, MidiInputCapture*) noexcept;

private:
    struct Timed {
        std::int64_t sample = 0;
        std::uint64_t epoch = 0;
        Event event;
    };
    static constexpr std::size_t capacity = 1024;
    std::array<Timed, capacity> ring_{};
    std::atomic<std::size_t> read_{0}, written_{0};
    std::atomic<std::uint64_t> epoch_{1};
    std::uint64_t renderedEpoch_ = 1;
    std::uint64_t generation_ = 0;
    std::uint64_t voiceSerial_ = 0;

    void deliver(const GraphHost::RenderBlock&, const Route&, const Event&) noexcept;
    static void release(const GraphHost::RenderBlock&, const Route&) noexcept;
};

}  // namespace adi::engine
