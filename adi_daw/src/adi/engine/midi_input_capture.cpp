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
bool MidiInputCapture::start(std::vector<std::int64_t> targets, double sampleRate) {
    const auto previous = state();
    if (previous == MidiCaptureState::Waiting || previous == MidiCaptureState::Recording ||
        read_.load(std::memory_order_relaxed) != written_.load(std::memory_order_acquire) ||
        epoch() == std::numeric_limits<std::uint64_t>::max())
        return false;
    tracks_ = std::move(targets);
    sampleRate_ = sampleRate;
    const auto next = epoch() + 1;
    state_.store(MidiCaptureState::Waiting, std::memory_order_relaxed);
    epoch_.store(next, std::memory_order_release);
    return true;
}
void MidiInputCapture::stop() noexcept {
    stopEpoch_.store(epoch(), std::memory_order_release);
}
std::size_t MidiInputCapture::drain(std::span<MidiCapturePacket> out) noexcept {
    const auto first = read_.load(std::memory_order_relaxed);
    const auto count = std::min(out.size(), written_.load(std::memory_order_acquire) - first);
    for (std::size_t i = 0; i < count; ++i)
        out[i] = ring_[(first + i) % capacity];
    read_.store(first + count, std::memory_order_release);
    return count;
}
void MidiInputCapture::finish(MidiCaptureState state) noexcept {
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
                                  std::uint64_t inputEpoch) noexcept {
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
    if (stopEpoch_.load(std::memory_order_acquire) == driverEpoch_ || (!transport.playing() && started_)) {
        finish(MidiCaptureState::Complete);
        return;
    }
    if (!transport.playing()) return; // wait through stopped callbacks and count-in
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
    cursor_ = written_.load(std::memory_order_relaxed);
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
    return append({MidiCapturePacket::Kind::Span, driverEpoch_, pass_, sliceStream_ + spanBegin_,
                   spanTimeline_, 0, static_cast<std::int32_t>(frames), {}});
}
void MidiInputCapture::record(std::int64_t inputSample, const Event& event) noexcept {
    if (!transport_ || inputSample < epochStart_) return;
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
                  spanTimeline_ + captured.frame, inputSample, 0, captured});
}
void MidiInputCapture::endSlice() noexcept {
    if (!transport_) return;
    while (spanEnd_ < sliceFrames_)
        if (!nextSpan()) return;
    expectedStream_ = sliceStream_ + sliceFrames_;
    transport_ = nullptr;
    written_.store(cursor_, std::memory_order_release);
}

}  // namespace adi::engine
