// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "juce/device_model.hpp"

#include <chrono>
#include <optional>
#include <span>

namespace adi::device {

/// A packet at the remote plug-in's input. PCM is planar, with `frames`
/// consecutive samples for each channel. Event offsets are packet-relative.
struct RemoteBlock {
    std::int32_t channels = 0;
    std::int32_t frames = 0;
    std::span<const float> audio;
    std::span<const engine::Event> events;
    std::optional<engine::DeviceTransport> transport;
};

/// A connected plug-in's worker-side interface. All calls use one I/O thread,
/// including activation and release. Implementations bound connection and
/// exchange time; false ends this connection. `output` has the same layout
/// as `input.audio` and must be filled on success. This interface is internal
/// to ADI and the backend translates events to its negotiated wire format.
class RemoteProcessor {
public:
    virtual ~RemoteProcessor() = default;
    virtual bool prepare(double sampleRate, std::int32_t channels, std::int32_t frames,
                         bool offline, std::int32_t& pluginLatency, std::string& error) = 0;
    virtual bool exchange(const RemoteBlock& input, std::span<float> output, std::string& error) = 0;
    /// Capability is immutable and may be queried by the owner. State calls
    /// run on the worker and honor the supplied absolute deadline.
    using StateDeadline = std::chrono::steady_clock::time_point;
    [[nodiscard]] virtual bool supportsState() const noexcept { return false; }
    virtual bool fetchState(std::vector<std::uint8_t>&, StateDeadline, std::string&) { return false; }
    virtual bool restoreState(std::span<const std::uint8_t>, StateDeadline, std::string&) { return false; }
    virtual void release() noexcept {}
};

/// ADR-0053. A regular DeviceInstance with one prepared processing quantum
/// of additional latency. The device collects Graph's event and transport
/// segments into fixed-size packets. A partial driver callback therefore does
/// not change the declared sample delay or move an event.
class RemoteDevice final : public DeviceInstance {
public:
    RemoteDevice(DeviceIdentity identity, std::int32_t channels, std::unique_ptr<RemoteProcessor> processor,
                 engine::EventFlow eventFlow = engine::EventFlow::Through);
    ~RemoteDevice() override;

    [[nodiscard]] const DeviceIdentity& identity() const noexcept override { return identity_; }
    [[nodiscard]] bool loaded() const noexcept override { return loaded_.load(std::memory_order_acquire); }
    [[nodiscard]] std::string_view processingIssue() const noexcept override;
    [[nodiscard]] engine::EventFlow eventFlow() const noexcept override { return eventFlow_; }
    [[nodiscard]] bool wantsDeviceTransport() const noexcept override { return true; }
    [[nodiscard]] std::int32_t latencySamples() const noexcept override { return latency_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t latencyEpoch() const noexcept override { return latencyEpoch_.load(std::memory_order_relaxed); }

    // An isolated render owner can wait for a worker reply. The driver uses
    // the realtime mode, which never waits, and neither mode performs I/O here.
    void setOffline(bool offline) noexcept override { requestedOffline_ = offline; }
    void prepare(double sampleRate, std::int32_t maxFrames) override;
    void release() override;
    void process(const engine::NodeIo& io) noexcept override;
    [[nodiscard]] bool needsBypassTransition(std::int32_t configuredLatency) noexcept override;
    void processBypassTransition(const engine::NodeIo&) noexcept override;

    [[nodiscard]] std::vector<std::string> stateRoles() const override;
    [[nodiscard]] std::vector<std::uint8_t> saveState(const std::string& role) const override;
    [[nodiscard]] std::string_view stateCaptureIssue() const noexcept override { return captureIssue_; }
    bool loadState(const std::string& role, const std::vector<std::uint8_t>& bytes) override;

    /// A late response does not get played during another packet's slot.
    /// One count per slot silenced, excluding intentional pipeline priming.
    [[nodiscard]] std::uint64_t droppedBlocks() const noexcept { return droppedBlocks_.load(std::memory_order_relaxed); }

private:
    struct Pipeline;
    DeviceIdentity identity_;
    std::int32_t channels_;
    engine::EventFlow eventFlow_;
    std::unique_ptr<RemoteProcessor> processor_;
    std::unique_ptr<Pipeline> pipeline_;
    ReactivationGate activation_;
    std::string issuePrefix_;
    std::string ownerIssue_;
    std::string stateIssue_;
    mutable std::string captureIssue_;
    mutable std::vector<std::uint8_t> confirmedState_;
    bool requestedOffline_ = false;
    std::atomic<bool> loaded_{false};
    std::atomic<std::int32_t> latency_{0};
    std::atomic<std::uint64_t> latencyEpoch_{0};
    std::atomic<std::uint64_t> droppedBlocks_{0};
};

}  // namespace adi::device
