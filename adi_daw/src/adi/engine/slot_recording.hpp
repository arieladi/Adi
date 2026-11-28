// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/engine/audio_recording.hpp"
#include "adi/engine/launch_data.hpp"
#include "adi/engine/midi_input_capture.hpp"
#include "adi/engine/midi_input_route.hpp"

#include <atomic>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace adi::engine::launch {

struct SlotRecordingTarget {
    SlotKey slot;
    // An audio target lists physical input channels in file order. Empty
    // selects typed MIDI with the track input fixed at prepare time.
    std::vector<std::uint32_t> audioChannels;
    MidiInputFilter midiInput{};
};

struct SlotRecordingWindow {
    // Launch and stop as resolved by the driver, on the monotonic stream
    // clock. The take itself starts at slot-local sample zero.
    std::int64_t begin;
    std::optional<std::int64_t> end;
};

struct SlotMidiInput {
    std::int64_t scheduledSample;
    std::int64_t inputSample;
    Event event; // frame relative to this callback, in scheduled order
    std::uint64_t port = 0; // stable physical endpoint, or zero for a virtual input
};

enum class SlotRecordingState : std::uint8_t {
    Idle, Armed, Recording, Complete, Interrupted, InputFailure, Cancelled
};

struct SlotRecordingResult {
    SlotRecordingState state;
    std::int64_t frames; // prefix accepted by every input, before writer finalization
    std::uint32_t sampleRate;
    std::vector<SlotRecordingTarget> targets;
    std::optional<AudioRecordingResult> audio;
    std::vector<MidiCapturePacket> midi;
    MidiCaptureState midiState;
    std::string sourceProject;
};

/// One quantized recording gesture at a time. Prepare the capture on the
/// owner, then pass the driver-resolved window with every active callback.
/// A scene gesture can start several distinct armed tracks together. An
/// interrupted take preserves its private audio and MIDI for owner recovery.
/// Destruction requires the driver quiescent.
class SlotInputRecorder {
public:
    // Owner only. Resolve input routing and empty/armed slot eligibility
    // before calling; the resulting take retains these destination keys.
    bool start(AudioRecordingSpec format, std::span<const SlotRecordingTarget>, std::string& error);
    /// Owner only. Abandon the active or queued capture at the next driver call.
    void cancel() noexcept;

    // Driver only. Call once per callback, including after the final window
    // for audio stop acknowledgement. A missing window waits while armed
    // and interrupts any running take. End is exclusive.
    void process(const AudioIo&, std::optional<SlotRecordingWindow>,
                 std::span<const SlotMidiInput> = {}, std::uint64_t inputEpoch = 0) noexcept;

    [[nodiscard]] SlotRecordingState state() const noexcept;

    // Owner only, regularly while armed/running too, to drain the MIDI ring.
    // Pass deviceStopped only after the driver has quiesced. A result is
    // returned once and its private WAVs are available for publication.
    [[nodiscard]] std::optional<SlotRecordingResult> poll(bool deviceStopped = false);

private:
    static_assert(std::atomic<SlotRecordingState>::is_always_lock_free);
    static_assert(std::atomic<bool>::is_always_lock_free);
    void finish(SlotRecordingState) noexcept;

    std::atomic<SlotRecordingState> state_{SlotRecordingState::Idle};
    std::atomic<bool> cancel_{false};
    AudioInputRecorder audio_;
    MidiInputCapture midi_;
    Transport clock_;
    std::int64_t beginStream_ = 0;
    bool hasAudio_ = false, hasMidi_ = false;
    std::uint32_t sampleRate_ = 0, maxFrames_ = 0;
    std::string sourceProject_;
    std::vector<SlotRecordingTarget> targets_;
    std::vector<MidiCapturePacket> midiPackets_;
};

} // namespace adi::engine::launch
