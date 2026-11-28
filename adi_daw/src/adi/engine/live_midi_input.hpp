// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/engine/host.hpp"
#include "adi/engine/midi_input_route.hpp"
#include "adi/engine/midi_memory.hpp"
#include "adi/engine/midi_monitoring.hpp"
#include "adi/engine/play_quantize.hpp"

#include <array>
#include <cstddef>
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace adi::engine {

class MidiInputCapture;
class Transport;
enum class RecordingPunch : std::uint8_t;
struct RecordingPunchWindow;

/// Typed Track input. The producer and the audio driver share only the
/// fixed queue; per-device voice ownership stays on the audio driver.
class LiveMidiInput {
public:
    struct PlayClock {
        std::span<const PlayTempoPoint> tempo;
        std::uint32_t sampleRate = 0;
        const WarpArrangementClock* projectClock = nullptr;
    };
    struct Voices {
        enum class Bend : std::uint8_t { None, Controller, Expression };
        struct Voice {
            std::uint64_t sourceId = 0;
            Event off;
            bool releasing = false, awaitingOn = false;
            std::int64_t playDelay = 0;
        };
        struct Pending {
            std::int64_t due;
            Event event; // the reserved runtime note ID
        };
        std::array<Voice, 128> active{};
        // Direct Track input does not pass through a graph edge's latency queue.
        // These delayed note events belong to the same driver as the live voices.
        std::array<Pending, 512> pending{};
        std::size_t pendingCount = 0;
        std::uint64_t transportRevision = 0;
        std::int64_t playGrid = 0, playForgiveness = 0;
        std::array<std::array<bool, 3>, 16> pedals{};
        std::array<Bend, 16> bends{};
        std::uint16_t pitchCenteredAtStart = 0;
        std::int64_t track = 0;
        MidiInputFilter filter;
        bool flush = false;
        bool retry = false;
        bool blocked = false;
    };
    struct Route {
        Node* node = nullptr;
        std::shared_ptr<Voices> voices;
        std::int64_t monitorTrack = 0;
        bool processes = false;
        MidiInputFilter filter;
        std::vector<MidiMonitorRange> blockedRanges; // merged, in ascending timeline order
        std::int64_t playGrid = 0, playForgiveness = 0; // in 960-PPQ ticks and samples
        mutable NodeId slot = kInvalidNode;
        std::shared_ptr<std::uint16_t> authoredPitchChase;
    };
    struct History {
        History(std::int64_t identity, std::int64_t track, std::uint32_t rate, MidiInputFilter route)
            : memory(identity, track, rate, NoteChannelBase::Zero, 16384), inputId(identity), sampleRate(rate), filter(route) {}
        MidiMemory memory;
        const std::int64_t inputId;
        const std::uint32_t sampleRate;
        const MidiInputFilter filter;
        std::uint64_t inputEpoch = 0; // audio driver only
        std::uint64_t consumed = 0; // owner only, first uncaptured serial
    };
    struct PreviewVoice {
        Event off;
        std::int64_t track = 0;
        std::uint64_t epoch = 0, earliestRelease = 0, releaseAt = 0;
    };
    struct PreviewRoute {
        const Node* node = nullptr;
        PreviewVoice* voice = nullptr;
        std::int64_t track = 0;
        mutable NodeId slot = kInvalidNode;
    };

    bool push(std::int64_t sample, const Event&, std::int64_t inputSample, std::uint64_t port) noexcept;
    void reset() noexcept;
    void render(const GraphHost::RenderBlock&, const AudioIo&, std::span<const Route>,
                std::span<const std::shared_ptr<History>>, const Transport&, const PlayClock&,
                MidiInputCapture*, RecordingPunch, const RecordingPunchWindow&) noexcept;
    [[nodiscard]] std::uint64_t historyRevision() const noexcept { return historyRevision_.load(); }
    // A separate owner-to-driver queue keeps audition out of Track input and recording.
    bool preview(const PreviewRoute&, int key, int velocity, bool held) noexcept;
    void stopPreview() noexcept;
    void clearPreviewDevice() noexcept; // owner, driver quiescent
    void renderPreview(const GraphHost::RenderBlock&, std::int32_t frames,
                       std::span<const PreviewRoute>, double sampleRate) noexcept;

private:
    struct Timed {
        std::int64_t sample = 0;
        std::int64_t inputSample = 0;
        std::uint64_t epoch = 0;
        Event event;
        std::uint64_t port = 0;
    };
    static constexpr std::size_t capacity = 1024;
    std::array<Timed, capacity> ring_{};
    std::atomic<std::size_t> read_{0}, written_{0};
    std::atomic<std::uint64_t> epoch_{1};
    std::atomic<std::uint64_t> historyRevision_{0};
    std::uint64_t renderedEpoch_ = 1;
    std::uint64_t generation_ = 0;
    std::uint64_t voiceSerial_ = 0;

    struct PreviewCommand {
        const Node* node = nullptr; // null is a release request
        std::int64_t track = 0;
        std::uint64_t epoch = 0;
        int key = 0, velocity = 0;
        bool held = false;
    };
    bool pushPreview(PreviewCommand) noexcept;
    static constexpr std::size_t previewCapacity = 128;
    std::array<PreviewCommand, previewCapacity> previewRing_{};
    std::atomic<std::size_t> previewRead_{0}, previewWritten_{0};
    std::atomic<std::uint64_t> previewEpoch_{1};
    std::uint64_t previewGeneration_ = 0, previewClock_ = 0;
    PreviewVoice* activePreview_ = nullptr;

    void deliver(const GraphHost::RenderBlock&, const Route&, const Event&, std::int64_t streamStart,
                 const Transport&, const PlayClock&) noexcept;
    static void drainPending(const GraphHost::RenderBlock&, const Route&, std::int64_t streamStart,
                             std::int32_t beforeFrame) noexcept;
    static void sendVoice(const GraphHost::RenderBlock&, const Route&, Voices::Voice&, const Event&) noexcept;
    static void release(const GraphHost::RenderBlock&, const Route&, std::int32_t frame) noexcept;
};

}  // namespace adi::engine
