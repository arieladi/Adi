// SPDX-License-Identifier: GPL-3.0-or-later
//
// The JUCE half of driving a BlockProcessor. Deliberately thin.
//
// Everything that can be decided without JUCE lives in device_core.hpp and is
// tested on every ABI. What is left here is the part that genuinely needs a
// framework: translating JUCE's callback shape into `engine::AudioIo`, and
// asking a `juce::AudioIODevice` what it actually granted.
//
// THREADING, which is a precondition rather than an assumption:
//
//   `audioDeviceAboutToStart` and `audioDeviceStopped` are called by JUCE on
//   whichever thread started or stopped the device. `prepare` and `release`
//   allocate, so this class REQUIRES the device to be opened, reconfigured and
//   closed from the message thread — which is where `AudioDeviceManager` is
//   driven from in any normal application. Start a device from a worker and
//   you get an allocation on that worker; nothing here can detect it, so it is
//   written down instead.
//
//   `audioDeviceIOCallbackWithContext` is the audio thread and does nothing but
//   forward. ADR-0010.

#pragma once

#include "adi/engine/process.hpp"
#include "juce/device_core.hpp"
#include "juce/device_host.hpp"

#include <juce_audio_devices/juce_audio_devices.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>

namespace adi::device {

// A driver may return a nonpositive latency when an endpoint has no report.
inline std::int64_t reportedRoundTripLatency(juce::AudioIODevice& device) {
    const auto input = (std::max)(0, device.getInputLatencyInSamples());
    const auto output = (std::max)(0, device.getOutputLatencyInSamples());
    return static_cast<std::int64_t>(input) + output;
}

class DeviceBridge final : public juce::AudioIODeviceCallback {
public:
    /// The processor is not owned. It outlives the bridge, because the graph
    /// outlives any particular device — which is the whole of ADR-0042's
    /// decision 5: changing block size must not disturb the project.
    using RejectedBlock = void (*)(engine::BlockProcessor&) noexcept;
    explicit DeviceBridge(engine::BlockProcessor& proc, RejectedBlock rejected = nullptr)
        : proc_(&proc), rejected_(rejected) {}

    /// What the user asked for, so a mismatch can be reported. Set before the
    /// device is opened; it has no effect on what is requested of the driver,
    /// which is `AudioDeviceManager`'s job.
    void setRequestedBlockSize(std::int32_t frames) { requested_ = frames; }

    [[nodiscard]] const DeviceCore& core() const { return core_; }
    [[nodiscard]] std::optional<engine::OutputChannelMap> physicalOutputChannels() const noexcept {
        if (!hasOutputGrant_.load(std::memory_order_acquire))
            return std::nullopt;
        return engine::OutputChannelMap{outputMask_.load(std::memory_order_acquire)};
    }

    /// Non-empty when the device is running at a size other than the one
    /// requested. ADR-0049: reported, never silently corrected.
    [[nodiscard]] juce::String mismatchReport() const {
        if (!core_.isOpen() || core_.granted() == requested_ || requested_ <= 0)
            return {};
        return "requested " + juce::String(requested_) + " frames, running at "
             + juce::String(core_.granted());
    }

    // --- juce::AudioIODeviceCallback ---------------------------------------

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override {
        // A failed driver restart can still call back with a closed device.
        if (device == nullptr || !device->isOpen()) return;

        // The granted size, read back from the device. Never `requested_`:
        // asking CoreAudio for 8192 returns 4096 and says nothing (ADR-0049).
        const auto granted = static_cast<std::int32_t>(device->getCurrentBufferSizeSamples());
        const double rate = device->getCurrentSampleRate();
        const auto active = device->getActiveOutputChannels();
        std::uint64_t outputMask = 0;
        for (int channel = 0; channel < engine::OutputChannelMap::maximumChannels; ++channel)
            if (active[channel])
                outputMask |= std::uint64_t{1} << channel;

        // A restart on a live stream is a RECONFIGURATION, not a fresh open:
        // the processor keeps its state and the stream clock stays continuous,
        // or a transport following it jumps backwards mid-session.
        last_ = core_.isOpen() ? core_.changeBlockSize(rate, requested_, granted)
                               : core_.open(*proc_, rate, requested_, granted);
        if (last_.opened) {
            // Publish one complete, prepared mask. A running input-only device
            // has an empty mask, which is distinct from a stopped device.
            outputMask_.store(outputMask, std::memory_order_release);
            hasOutputGrant_.store(true, std::memory_order_release);
            currentDevice_ = {device->getTypeName(), device->getName()};
            if (stoppedDevice_ && *stoppedDevice_ == currentDevice_)
                stoppedDevice_.reset();
        }
    }

    void audioDeviceIOCallbackWithContext(const float* const* in, int numIn,
                                          float* const* out, int numOut,
                                          int numSamples,
                                          const juce::AudioIODeviceCallbackContext&) override {
        const engine::OutputChannelMap outputs{outputMask_.load(std::memory_order_acquire)};
        if (core_.process(out, numOut, in, numIn, numSamples, outputs) == CallbackResult::Oversized && rejected_)
            rejected_(*proc_);
    }

    void audioDeviceStopped() override {
        hasOutputGrant_.store(false, std::memory_order_release);
        if (core_.isOpen() && !stoppedDevice_)
            stoppedDevice_ = currentDevice_;
        core_.close();
    }

    void audioDeviceError(const juce::String& message) override {
        // Some drivers report errors from a worker. The sample callback
        // never takes this lock.
        const std::lock_guard lock(errorMutex_);
        pendingError_ = message;
        lastError_ = message;
    }

    // The last error a driver reported, kept for a tool's receipt (adi_play). The window takes its
    // problems once, through takeDeviceProblem.
    [[nodiscard]] juce::String lastError() const {
        const std::lock_guard lock(errorMutex_);
        return lastError_;
    }

    // The owner's explicit Settings change may replace a device or turn audio off.
    void acknowledgeDeviceSetup() { stoppedDevice_.reset(); }

    [[nodiscard]] juce::String takeDeviceProblem() {
        const auto stopped = std::exchange(stoppedDevice_, std::nullopt);
        std::optional<juce::String> error;
        {
            const std::lock_guard lock(errorMutex_);
            error = std::exchange(pendingError_, std::nullopt);
        }
        if (stopped) {
            auto problem = stopped->name.isEmpty() ? juce::String("Audio device stopped.")
                                                   : "Audio device stopped: " + stopped->name + ".";
            if (error && error->isNotEmpty())
                problem << " " << *error;
            return problem;
        }
        if (!error)
            return {};
        return error->isEmpty() ? juce::String("Audio device reported an error.") : "Audio device error: " + *error;
    }

    [[nodiscard]] const OpenResult& lastOpen() const { return last_; }

private:
    struct DeviceName {
        juce::String type, name;
        bool operator==(const DeviceName&) const = default;
    };
    engine::BlockProcessor* proc_ = nullptr;
    RejectedBlock rejected_ = nullptr;
    DeviceCore core_;
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    std::atomic<std::uint64_t> outputMask_{0};
    std::atomic<bool> hasOutputGrant_{false};
    std::int32_t requested_ = 0;
    OpenResult last_;
    DeviceName currentDevice_;
    std::optional<DeviceName> stoppedDevice_; // owner thread, like open/close
    mutable std::mutex errorMutex_;
    std::optional<juce::String> pendingError_;
    juce::String lastError_;
};

/// The timer that ticks `DeviceHost`, and the only JUCE-shaped part of it.
///
/// ADR-0082 left "what calls poll()" open and `DeviceHost` answered the
/// ownership half. This is the other half, and it is deliberately three
/// lines: everything worth testing lives in `DeviceHost::tick`, which takes a
/// plain millisecond clock and runs on every ABI. A `juce::Timer` here would
/// otherwise have dragged the whole latency path into the one CI job that has
/// JUCE.
///
/// THE RATE IS MEASURED, not chosen. FabFilter Pro-Q 3's reports arrive with
/// a largest gap of 26 ms and a burst spanning 75 ms, against a 50 ms quiet
/// period. A tick slower than the quiet period would let a burst close
/// between ticks and be seen as several; 20 ms gives at least two ticks
/// inside the shortest useful window without being a busy loop.
///
/// Message thread, because that is where `juce::Timer` fires and where
/// reconfiguring the graph is allowed (ADR-0066: never on the thread a plugin
/// reported from).
class DeviceHostTimer final : private juce::Timer {
public:
    explicit DeviceHostTimer(DeviceHost& host) : host_(&host) {}
    ~DeviceHostTimer() override { stopTimer(); }

    void start(int intervalMs = 20) { startTimer(intervalMs > 0 ? intervalMs : 20); }
    void stop() { stopTimer(); }

    /// Ticks observed, and retaps caused. Exposed so a test or a probe can
    /// assert the timer is actually running rather than trust that it is.
    [[nodiscard]] std::int64_t ticks() const noexcept { return ticks_; }
    [[nodiscard]] std::int64_t retaps() const noexcept { return retaps_; }

private:
    void timerCallback() override {
        ++ticks_;
        // juce::Time's counter is monotonic and in milliseconds, which is all
        // the coalescer wants -- it only ever subtracts two of them.
        if (host_->tick(static_cast<std::int64_t>(juce::Time::getMillisecondCounter())))
            ++retaps_;
    }

    DeviceHost* host_ = nullptr;
    std::int64_t ticks_ = 0;
    std::int64_t retaps_ = 0;
};

}  // namespace adi::device
