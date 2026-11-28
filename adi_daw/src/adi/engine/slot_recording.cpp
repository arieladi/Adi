// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/slot_recording.hpp"

#include "adi/engine/audio_io_slice.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <utility>

namespace adi::engine::launch {

SlotRecordingState SlotInputRecorder::state() const noexcept {
    return state_.load(std::memory_order_acquire);
}

bool SlotInputRecorder::start(AudioRecordingSpec format, std::span<const SlotRecordingTarget> targets,
                              std::string& error) {
    if (state() != SlotRecordingState::Idle) {
        error = "Finish and drain the previous slot recording first";
        return false;
    }
    if (targets.empty() || targets.size() > 256 || !format.targets.empty() || format.loop ||
        format.placementOffsetSamples != 0 || format.sampleRate < 8000 || format.sampleRate > 768000 ||
        format.maxFrames == 0 || format.maxFrames > 16384) {
        error = "Invalid slot recording destinations or capture format";
        return false;
    }

    std::set<std::int64_t> tracks;
    std::vector<std::int64_t> midiTracks;
    for (const auto& target : targets) {
        if (target.slot.trackId <= 0 || target.slot.sceneId <= 0 || !tracks.insert(target.slot.trackId).second) {
            error = "Slot recording needs one valid destination per track";
            return false;
        }
        if (target.audioChannels.empty()) {
            if (target.midiInput.channel < 0 || target.midiInput.channel > 16) {
                error = "Invalid slot MIDI input channel";
                return false;
            }
            midiTracks.push_back(target.slot.trackId);
        } else
            format.targets.push_back({target.slot.trackId, target.audioChannels});
    }

    // Arrangement recovery stores track IDs and timeline offsets. A slot
    // also needs its scene identity and the tempo performed during capture.
    if (!format.targets.empty() && !format.recoveryProjectUuid.empty()) {
        error = "Session slot audio requires recovery metadata with scene ownership and performed tempo";
        return false;
    }

    std::vector<SlotRecordingTarget> frozen(targets.begin(), targets.end());
    const auto captureMidi = !midiTracks.empty();
    if (captureMidi && !midi_.start(std::move(midiTracks), format.sampleRate)) {
        error = "Unable to prepare the slot MIDI recorder";
        return false;
    }
    if (!format.targets.empty() && !audio_.start(format, error)) {
        if (captureMidi) midi_.deviceStopped();
        return false;
    }

    hasAudio_ = !format.targets.empty();
    hasMidi_ = captureMidi;
    sampleRate_ = format.sampleRate;
    maxFrames_ = format.maxFrames;
    sourceProject_ = std::move(format.sourceProject);
    targets_ = std::move(frozen);
    clock_ = Transport{};
    beginStream_ = 0;
    cancel_.store(false, std::memory_order_relaxed);
    error.clear();
    state_.store(SlotRecordingState::Armed, std::memory_order_release);
    return true;
}

void SlotInputRecorder::cancel() noexcept {
    cancel_.store(true, std::memory_order_release);
}

void SlotInputRecorder::finish(SlotRecordingState outcome) noexcept {
    clock_.play(false);
    if (hasAudio_) audio_.capture({}, clock_);
    if (hasMidi_ && (midi_.state() == MidiCaptureState::Waiting || midi_.state() == MidiCaptureState::Recording))
        midi_.finish(MidiCaptureState::Complete);
    state_.store(outcome, std::memory_order_release);
}

void SlotInputRecorder::process(const AudioIo& io, std::optional<SlotRecordingWindow> window,
                                std::span<const SlotMidiInput> events, std::uint64_t inputEpoch) noexcept {
    const auto current = state();
    if (current != SlotRecordingState::Armed && current != SlotRecordingState::Recording) {
        // The recorder's existing publisher expects the driver to acknowledge
        // the owner release even when there is no longer a recording window.
        audio_.capture({}, Transport{});
        return;
    }
    if (cancel_.load(std::memory_order_acquire)) {
        finish(SlotRecordingState::Cancelled);
        return;
    }
    if (!window) {
        if (current == SlotRecordingState::Recording) finish(SlotRecordingState::Interrupted);
        return;
    }
    if (io.frames < 0 || static_cast<std::uint32_t>(io.frames) > maxFrames_ ||
        io.streamTimeSamples < 0 || io.streamTimeSamples > std::numeric_limits<std::int64_t>::max() - io.frames ||
        window->begin < 0 || (window->end && *window->end < window->begin)) {
        finish(SlotRecordingState::Interrupted);
        return;
    }

    const auto blockBegin = io.streamTimeSamples;
    const auto blockEnd = blockBegin + io.frames;
    if ((current == SlotRecordingState::Armed && window->begin < blockBegin) ||
        (current == SlotRecordingState::Recording &&
         (window->begin != beginStream_ || blockBegin - beginStream_ != clock_.position())) ||
        (window->end && *window->end < blockBegin)) {
        finish(SlotRecordingState::Interrupted);
        return;
    }
    const auto from = std::max(blockBegin, window->begin);
    const auto to = window->end ? std::min(blockEnd, *window->end) : blockEnd;
    if (from < to) {
        const auto offset = static_cast<std::int32_t>(from - blockBegin);
        const auto frames = static_cast<std::int32_t>(to - from);
        const AudioIoSlice slice(io, offset, frames);
        if (!slice.valid()) {
            finish(SlotRecordingState::InputFailure);
            return;
        }
        if (current == SlotRecordingState::Armed) {
            beginStream_ = window->begin;
            clock_.play();
        }
        if (hasMidi_) {
            midi_.beginSlice(slice.io(), clock_, inputEpoch, RecordingPunch::Within, {});
            // The slot clock never wraps. Reserve its single span before
            // offering this slice to audio, then publish MIDI only if both
            // inputs could accept it.
            if (midi_.state() != MidiCaptureState::Recording || !midi_.nextSpan()) {
                finish(SlotRecordingState::InputFailure);
                return;
            }
            for (const auto& event : events) {
                if (event.event.frame < offset || event.event.frame >= offset + frames ||
                    !isNoteStream(event.event.type)) continue;
                auto local = event.event;
                local.frame -= offset;
                midi_.record(event.scheduledSample, event.inputSample, local, event.port);
            }
            if (midi_.state() != MidiCaptureState::Recording) {
                finish(SlotRecordingState::InputFailure);
                return;
            }
        }
        if (hasAudio_ && audio_.capture(slice.io(), clock_).empty()) {
            finish(SlotRecordingState::InputFailure);
            return;
        }
        if (hasMidi_) midi_.endSlice();
        clock_.advance(frames);
        if (current == SlotRecordingState::Armed)
            state_.store(SlotRecordingState::Recording, std::memory_order_release);
    }
    if (window->end && *window->end <= blockEnd) finish(SlotRecordingState::Complete);
}

std::optional<SlotRecordingResult> SlotInputRecorder::poll(bool deviceStopped) {
    auto current = state();
    if (current == SlotRecordingState::Idle) return {};
    if (deviceStopped && (current == SlotRecordingState::Armed || current == SlotRecordingState::Recording)) {
        if (hasMidi_) midi_.deviceStopped();
        current = cancel_.load(std::memory_order_acquire)
            ? SlotRecordingState::Cancelled : SlotRecordingState::Interrupted;
        state_.store(current, std::memory_order_release);
    }
    if (hasMidi_) {
        constexpr std::size_t batchSize = 128;
        for (;;) {
            const auto first = midiPackets_.size();
            // Acquire destination storage before consuming the ring so an
            // allocation failure on the owner cannot lose captured packets.
            midiPackets_.resize(first + batchSize);
            const auto count = midi_.drain(std::span(midiPackets_).subspan(first));
            midiPackets_.resize(first + count);
            if (count < batchSize) break;
        }
    }
    if (current == SlotRecordingState::Armed || current == SlotRecordingState::Recording) return {};

    std::optional<AudioRecordingResult> audio;
    if (hasAudio_) {
        audio_.stop(current == SlotRecordingState::Cancelled);
        audio = audio_.poll(deviceStopped);
        if (!audio) return {};
        if (audio->failure != RecordingFailure::None &&
            audio->failure != RecordingFailure::Cancelled &&
            !(audio->failure == RecordingFailure::NoFrames && clock_.position() == 0))
            current = SlotRecordingState::InputFailure;
    }
    SlotRecordingResult result{current, clock_.position(), sampleRate_, std::move(targets_), std::move(audio),
                               std::move(midiPackets_), hasMidi_ ? midi_.state() : MidiCaptureState::Idle,
                               std::move(sourceProject_)};
    midiPackets_.clear();
    state_.store(SlotRecordingState::Idle, std::memory_order_release);
    return result;
}

} // namespace adi::engine::launch
