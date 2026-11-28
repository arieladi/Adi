// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/midi_input_capture.hpp"
#include "adi/engine/transport.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace adi::engine {

MidiCaptureState MidiInputCapture::state() const noexcept {
    return state_.load(std::memory_order_acquire);
}
std::uint64_t MidiInputCapture::epoch() const noexcept {
    return epoch_.load(std::memory_order_acquire);
}
bool MidiInputCapture::start(std::vector<std::int64_t> targets, double sampleRate, bool overdub) {
    const auto previous = state();
    if (previous == MidiCaptureState::Waiting || previous == MidiCaptureState::Recording ||
        read_.load(std::memory_order_relaxed) != written_.load(std::memory_order_acquire) ||
        epoch() == std::numeric_limits<std::uint64_t>::max())
        return false;
    tracks_ = std::move(targets);
    overdub_ = overdub;
    sampleRate_ = sampleRate;
    const auto next = epoch() + 1;
    state_.store(MidiCaptureState::Waiting, std::memory_order_relaxed);
    epoch_.store(next, std::memory_order_release);
    return true;
}
void MidiInputCapture::stop() noexcept {
    stopEpoch_.store(epoch(), std::memory_order_release);
}
std::size_t MidiInputCapture::drain(std::span<MidiCapturePacket> out, RecordingPunchWindow* punchWindow) noexcept {
    const auto first = read_.load(std::memory_order_relaxed);
    const auto count = std::min(out.size(), written_.load(std::memory_order_acquire) - first);
    for (std::size_t i = 0; i < count; ++i)
        out[i] = ring_[(first + i) % capacity];
    // The packet acquire above also publishes these bounds. The owner cannot
    // start another epoch until it has drained this one.
    if (count && punchWindow)
        *punchWindow = punchWindow_;
    read_.store(first + count, std::memory_order_release);
    return count;
}
bool MidiInputCapture::overdubs(std::int64_t track) const noexcept {
    if (!transport_ || !overdub_) return false;
    // Debug vector iterators can take a lock on the audio thread.
    const auto* targets = tracks_.data();
    const auto count = tracks_.size();
    for (std::size_t index = 0; index < count; ++index)
        if (targets[index] == track) return true;
    return false;
}

void MidiInputCapture::finish(MidiCaptureState state) noexcept {
    // Every complete slice is published before the terminal state, held ones included. A slice
    // still open publishes nothing: a span it grew goes back to its length before the slice.
    if (transport_ && extended_)
        ring_[(sliceBegin_ - 1) % capacity].frames = heldFrames_;
    written_.store(transport_ ? sliceBegin_ : cursor_, std::memory_order_release);
    cursor_ = transport_ ? sliceBegin_ : cursor_;
    extended_ = false;
    active_ = false;
    transport_ = nullptr;
    state_.store(state, std::memory_order_release);
}
void MidiInputCapture::deviceStopped() noexcept {
    const auto previous = state();
    if (previous == MidiCaptureState::Waiting || previous == MidiCaptureState::Recording) {
        driverEpoch_ = epoch();
        finish(MidiCaptureState::DeviceStopped);
    }
}

void MidiInputCapture::beginSlice(const AudioIo& io, const Transport& transport,
                                  std::uint64_t inputEpoch, RecordingPunch punch,
                                  const RecordingPunchWindow& punchWindow) noexcept {
    const auto current = epoch();
    if (driverEpoch_ != current) {
        driverEpoch_ = current;
        active_ = true;
        started_ = false;
    }
    if (!active_) return;
    if (started_ && inputEpoch != inputEpoch_) {
        finish(MidiCaptureState::InputReset);
        return;
    }
    if (started_ && punchWindow != punchWindow_) {
        finish(MidiCaptureState::Discontinuity);
        return;
    }
    if (stopEpoch_.load(std::memory_order_acquire) == driverEpoch_ ||
        (started_ && (!transport.playing() || punch == RecordingPunch::After))) {
        finish(MidiCaptureState::Complete);
        return;
    }
    if (!transport.playing() || punch != RecordingPunch::Within) return;
    if (io.streamTimeSamples < 0 ||
        io.streamTimeSamples > std::numeric_limits<std::int64_t>::max() - io.frames) {
        finish(MidiCaptureState::Discontinuity);
        return;
    }
    if (!started_) {
        started_ = true;
        epochStart_ = expectedStream_ = io.streamTimeSamples;
        expectedTimeline_ = transport.sampleAt(0);
        revision_ = transport.revision();
        inputEpoch_ = inputEpoch;
        pass_ = 0;
        looping_ = transport.looping();
        loopBegin_ = transport.loopBegin();
        loopEnd_ = transport.loopEnd();
        punchWindow_ = punchWindow;
        state_.store(MidiCaptureState::Recording, std::memory_order_release);
    }
    if (io.streamTimeSamples != expectedStream_ || transport.revision() != revision_ ||
        transport.looping() != looping_ ||
        (looping_ && (transport.loopBegin() != loopBegin_ || transport.loopEnd() != loopEnd_))) {
        finish(MidiCaptureState::Discontinuity);
        return;
    }
    transport_ = &transport;
    sliceStream_ = io.streamTimeSamples;
    sliceFrames_ = io.frames;
    spanBegin_ = spanEnd_ = 0;
    lastEventFrame_ = -1;
    // The cursor carries on after slices held back from the owner (endSlice).
    sliceBegin_ = cursor_;
    extended_ = false;
    limitRead_ = read_.load(std::memory_order_acquire);
}

bool MidiInputCapture::append(MidiCapturePacket packet) noexcept {
    if (cursor_ - limitRead_ == capacity) {
        finish(MidiCaptureState::Overflow);
        return false;
    }
    ring_[cursor_++ % capacity] = packet;
    return true;
}
bool MidiInputCapture::nextSpan() noexcept {
    spanBegin_ = spanEnd_;
    spanTimeline_ = transport_->sampleAt(spanBegin_);
    if (spanTimeline_ != expectedTimeline_) {
        if (!looping_ || expectedTimeline_ != loopEnd_ || spanTimeline_ != loopBegin_) {
            finish(MidiCaptureState::Discontinuity);
            return false;
        }
        ++pass_;
    }
    const auto frames = transport_->contiguousFrames(spanBegin_, sliceFrames_ - spanBegin_);
    if (frames <= 0 || spanTimeline_ > std::numeric_limits<std::int64_t>::max() - frames) {
        finish(MidiCaptureState::Discontinuity);
        return false;
    }
    expectedTimeline_ = spanTimeline_ + frames;
    spanEnd_ += static_cast<std::int32_t>(frames);
    const auto stream = sliceStream_ + spanBegin_;
    const auto length = static_cast<std::int32_t>(frames);
    // Held slices are not yet visible to the owner, so their last span can grow in place
    // instead of this span taking a packet of its own.
    if (cursor_ != written_.load(std::memory_order_relaxed)) {
        auto &last = ring_[(cursor_ - 1) % capacity];
        if (last.kind == MidiCapturePacket::Kind::Span && last.epoch == driverEpoch_ && last.pass == pass_ &&
            last.streamSample + last.frames == stream && last.timelineSample + last.frames == spanTimeline_ &&
            last.frames <= std::numeric_limits<std::int32_t>::max() - length) {
            if (cursor_ == sliceBegin_ && !extended_) {
                heldFrames_ = last.frames;
                extended_ = true;
            }
            last.frames += length;
            return true;
        }
    }
    return append({MidiCapturePacket::Kind::Span, driverEpoch_, pass_, stream, spanTimeline_, 0, length, {}});
}
void MidiInputCapture::record(std::int64_t scheduledSample, std::int64_t inputSample, const Event& event,
                              std::uint64_t port) noexcept {
    if (!transport_ || scheduledSample < epochStart_) return;
    if (event.frame < lastEventFrame_ || event.frame >= sliceFrames_) {
        finish(MidiCaptureState::Discontinuity);
        return;
    }
    lastEventFrame_ = event.frame;
    while (event.frame >= spanEnd_)
        if (!nextSpan()) return;
    auto captured = event;
    captured.frame -= spanBegin_;
    (void)append({MidiCapturePacket::Kind::Event, driverEpoch_, pass_, sliceStream_ + event.frame,
                  spanTimeline_ + captured.frame, inputSample, 0, captured, port});
}
void MidiInputCapture::endSlice() noexcept {
    if (!transport_) return;
    while (spanEnd_ < sliceFrames_)
        if (!nextSpan()) return;
    expectedStream_ = sliceStream_ + sliceFrames_;
    transport_ = nullptr;
    extended_ = false;
    // An owner far behind (a stalled window) would see the ring fill with a span per slice and
    // the take end. Hold complete slices back while it is: the next slices then grow the last
    // span. They are published as soon as the owner catches up, or when the take finishes.
    const auto published = written_.load(std::memory_order_relaxed);
    if (published - limitRead_ < holdThreshold)
        written_.store(cursor_, std::memory_order_release);
}

}  // namespace adi::engine
