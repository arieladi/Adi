// SPDX-License-Identifier: GPL-3.0-or-later
//
// See device_core.hpp for why this file has no JUCE in it.

#include "juce/device_core.hpp"

namespace adi::device {

OpenResult DeviceCore::open(engine::BlockProcessor& proc, double sampleRate,
                            std::int32_t requested, std::int32_t granted) {
    OpenResult r;
    r.requested = requested;
    r.granted = granted;
    r.sampleRate = sampleRate;

    if (granted <= 0) {
        r.error = "driver granted a block size of zero or less";
        return r;
    }
    if (sampleRate <= 0.0) {
        r.error = "driver granted a sample rate of zero or less";
        return r;
    }

    // Anything already running is released first. Preparing a second time over
    // a live stream would leak whatever the first prepare allocated.
    if (open_) close();

    proc_ = &proc;
    sampleRate_ = sampleRate;
    granted_ = granted;
    requested_ = requested;
    streamTime_ = 0;
    callbacks_ = 0;
    oversize_ = 0;
    reconfigs_ = 0;

    // GRANTED, not requested. The whole of ADR-0049 is this argument.
    proc_->prepare(sampleRate_, granted_);
    open_ = true;
    r.opened = true;
    return r;
}

OpenResult DeviceCore::changeBlockSize(double sampleRate, std::int32_t requested,
                                       std::int32_t granted) {
    OpenResult r;
    r.requested = requested;
    r.granted = granted;
    r.sampleRate = sampleRate;

    if (!open_ || proc_ == nullptr) {
        r.error = "no open stream to reconfigure";
        return r;
    }
    if (granted <= 0) {
        r.error = "driver granted a block size of zero or less";
        return r;
    }
    if (sampleRate <= 0.0) {
        r.error = "driver granted a sample rate of zero or less";
        return r;
    }

    // The processor is re-prepared, NOT replaced. Everything it represents --
    // the project, the graph, the undo history -- survives, which is the whole
    // requirement: someone moving from 4096 for playback to 256 to overdub must
    // not lose their session to do it.
    //
    // streamTime_ is deliberately NOT reset. The driver's own frame counter
    // starts again at zero after a restart, and a transport following that
    // would jump backwards mid-session.
    proc_->prepare(sampleRate, granted);

    sampleRate_ = sampleRate;
    granted_ = granted;
    requested_ = requested;
    ++reconfigs_;

    r.opened = true;
    return r;
}

void DeviceCore::close() {
    // Guarded rather than unconditional: `release` may be called without a
    // preceding `prepare` (process.hpp), and a device that failed to open takes
    // exactly that path -- but calling it twice for one prepare would be a
    // different bug, and the counters in SilenceProcessor would show it.
    if (proc_ != nullptr) proc_->release();
    proc_ = nullptr;
    open_ = false;
    granted_ = 0;
    requested_ = 0;
    sampleRate_ = 0.0;
}

bool DeviceCore::setProcessor(engine::BlockProcessor& next) {
    if (!open_) return false;
    proc_ = &next;
    return true;
}

void DeviceCore::process(float* const* out, std::int32_t numOut,
                         const float* const* in, std::int32_t numIn,
                         std::int32_t frames) noexcept {
    // Silence first, unconditionally, before any early return below. A callback
    // that returns without writing hands the driver whatever was in that memory
    // -- which on a first callback is uninitialised and on a later one is the
    // previous block, repeated. Both reach the monitors.
    auto silence = [&]() noexcept {
        for (std::int32_t c = 0; c < numOut; ++c) {
            float* dst = (out != nullptr) ? out[c] : nullptr;
            if (dst == nullptr) continue;
            for (std::int32_t i = 0; i < frames; ++i) dst[i] = 0.0f;
        }
    };

    if (frames <= 0) return;
    if (!open_ || proc_ == nullptr) { silence(); return; }

    // The overrun ADR-0049 exists to prevent. The processor allocated for
    // `granted_`; a driver handing over more than that has changed its block
    // size without telling us, and processing it would write past every buffer
    // the processor owns. Refuse loudly in the counter, silently in the audio.
    if (frames > granted_) {
        ++oversize_;
        silence();
        return;
    }

    engine::AudioIo io;
    io.out = out;
    io.in = in;
    io.numOut = numOut;
    io.numIn = numIn;
    io.frames = frames;
    io.streamTimeSamples = streamTime_;

    proc_->process(io);

    streamTime_ += frames;
    ++callbacks_;
}

}  // namespace adi::device
