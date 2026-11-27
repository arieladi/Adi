// SPDX-License-Identifier: GPL-3.0-or-later
#include "juce/remote_device.hpp"

#include "adi/engine/offline_render_scope.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <exception>
#include <limits>

namespace adi::device {
namespace {

static_assert(std::atomic<std::size_t>::is_always_lock_free);
static_assert(std::atomic<std::int32_t>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);

// Like ParamEditCapture, one producer and one consumer each own an index.
// Reservations let both sides use preallocated audio and event storage in
// place, including while a packet spans several graph segments.
template <class Packet>
class PacketRing {
public:
    Packet* reserve() noexcept {
        const auto write = write_.load(std::memory_order_relaxed);
        return next(write) != read_.load(std::memory_order_acquire) ? &packets_[write] : nullptr;
    }
    void publish() noexcept {
        write_.store(next(write_.load(std::memory_order_relaxed)), std::memory_order_release);
    }
    Packet* front() noexcept {
        const auto read = read_.load(std::memory_order_relaxed);
        return read != write_.load(std::memory_order_acquire) ? &packets_[read] : nullptr;
    }
    void pop() noexcept {
        read_.store(next(read_.load(std::memory_order_relaxed)), std::memory_order_release);
    }
    auto& storage() noexcept { return packets_; } // Owner only, before starting the worker.

private:
    static constexpr std::size_t kSlots = 4; // Three packets plus the SPSC sentinel.
    static constexpr std::size_t next(std::size_t index) noexcept { return (index + 1) % kSlots; }
    std::array<Packet, kSlots> packets_;
    alignas(64) std::atomic<std::size_t> write_{0};
    alignas(64) std::atomic<std::size_t> read_{0};
};

struct AudioPacket {
    std::vector<float> audio;
    std::uint64_t sequence = 0;
};

struct Request : AudioPacket {
    std::vector<engine::Event> events;
    std::size_t eventCount = 0;
    std::optional<engine::DeviceTransport> transport;
};

enum class LocalIssue : std::uint8_t { None, Shape, Events, Queue };
static_assert(std::atomic<LocalIssue>::is_always_lock_free);

void silence(const engine::NodeIo& io) noexcept {
    if (io.blockOffset < 0) return;
    for (std::int32_t channel = 0; channel < io.channels; ++channel) {
        if (io.out && io.out[channel]) std::fill_n(io.out[channel] + io.blockOffset, io.frames, 0.0f);
        if (io.out64 && io.out64[channel]) std::fill_n(io.out64[channel] + io.blockOffset, io.frames, 0.0);
    }
}

std::optional<engine::DeviceTransport> atOffset(const engine::NodeIo& io, std::int32_t offset,
                                                double sampleRate) noexcept {
    if (!io.deviceTransport) return {};
    auto result = *io.deviceTransport;
    if (result.playing) {
        result.sample = std::min(result.sample, std::numeric_limits<std::int64_t>::max() - offset) + offset;
        result.quarterNotes += static_cast<double>(offset) * result.bpm / (60.0 * sampleRate);
    }
    return result;
}

}  // namespace

struct RemoteDevice::Pipeline {
    Pipeline(double rate, std::int32_t channelCount, std::int32_t frameCount, bool forOffline,
             const std::string& issuePrefix)
        : sampleRate(rate), channels(channelCount), frames(frameCount), offline(forOffline),
          workerIssue(issuePrefix + "remote I/O failed"), shapeIssue(issuePrefix + "remote audio layout changed"),
          eventIssue(issuePrefix + "remote event packet is full"), queueIssue(issuePrefix + "remote packet queue is full"),
          prefix(issuePrefix) {
        const auto samples = static_cast<std::size_t>(channels) * static_cast<std::size_t>(frames);
        const auto events = static_cast<std::size_t>(engine::Graph::deriveEventCapacity(rate, frames, 16)) * 2;
        for (auto& packet : requests.storage()) {
            packet.audio.resize(samples);
            packet.events.resize(events);
        }
        for (auto& packet : replies.storage()) packet.audio.resize(samples);
        discardOutput.resize(samples);
    }

    void fail(std::string_view error) noexcept {
        if (!error.empty()) {
            try {
                workerIssue = prefix + std::string(error);
            } catch (...) {} // The preallocated diagnostic still names the plug-in.
        }
        workerFailed.store(true, std::memory_order_release);
    }

    [[nodiscard]] std::string_view issue() const noexcept {
        if (workerFailed.load(std::memory_order_acquire)) return workerIssue;
        switch (localIssue.load(std::memory_order_relaxed)) {
        case LocalIssue::Shape: return shapeIssue;
        case LocalIssue::Events: return eventIssue;
        case LocalIssue::Queue: return queueIssue;
        case LocalIssue::None: return {};
        }
        return {};
    }

    void start(RemoteProcessor& processor) {
        worker = std::jthread([this, &processor](std::stop_token stop) { run(processor, stop); });
        started.wait(false, std::memory_order_acquire);
    }

    void run(RemoteProcessor& processor, const std::stop_token& stop) noexcept {
        std::uint64_t inFlight = 0;
        try {
            std::string error;
            if (!processor.prepare(sampleRate, channels, frames, offline, pluginLatency, error)) {
                fail(error);
            } else if (pluginLatency < 0 || pluginLatency > std::numeric_limits<std::int32_t>::max() - frames) {
                fail("server reported invalid plug-in latency");
            }
            started.store(true, std::memory_order_release);
            started.notify_one();

            while (!workerFailed.load(std::memory_order_relaxed) && !stop.stop_requested()) {
                const auto* request = requests.front();
                if (request == nullptr) {
                    // The realtime producer performs only an atomic publish.
                    // Poll here, keeping syscalls and scheduling off the callback.
                    std::this_thread::sleep_for(std::chrono::microseconds(50));
                    continue;
                }
                inFlight = request->sequence;
                auto* reply = replies.reserve();
                auto& output = reply ? reply->audio : discardOutput;
                error.clear();
                const RemoteBlock block{channels, frames, request->audio,
                    std::span<const engine::Event>(request->events.data(), request->eventCount), request->transport};
                if (!processor.exchange(block, output, error)) {
                    fail(error);
                } else if (reply != nullptr) {
                    reply->sequence = inFlight;
                    replies.publish();
                }
                requests.pop();
                complete(inFlight);
                inFlight = 0;
            }
        } catch (const std::exception& error) {
            fail(error.what());
        } catch (...) {
            fail({});
        }
        if (inFlight != 0) complete(inFlight);
        processor.release();
        if (!started.load(std::memory_order_relaxed)) {
            started.store(true, std::memory_order_release);
            started.notify_one();
        }
    }

    void complete(std::uint64_t id) noexcept {
        completed.store(id, std::memory_order_release);
        if (offline) completed.notify_one();
    }

    void resetAudio() noexcept {
        if (playing) replies.pop();
        playing = nullptr;
        collecting = nullptr;
        offset = 0;
        priming = true;
    }

    void begin(const engine::NodeIo& io, std::int32_t sourceFrame, std::uint64_t& misses) noexcept {
        const auto wanted = sequence - 1;
        while (auto* ready = replies.front()) {
            if (ready->sequence >= wanted && !priming) {
                if (ready->sequence == wanted) playing = ready;
                break;
            }
            replies.pop();
        }
        if (!playing && !priming) ++misses;
        collecting = workerFailed.load(std::memory_order_acquire) ? nullptr : requests.reserve();
        if (collecting) {
            collecting->eventCount = 0;
            collecting->transport = atOffset(io, sourceFrame, sampleRate);
        } else if (offline && !workerFailed.load(std::memory_order_relaxed)) {
            localIssue.store(LocalIssue::Queue, std::memory_order_relaxed);
        }
    }

    void finish() noexcept {
        if (playing) replies.pop();
        if (collecting) {
            collecting->sequence = sequence;
            requests.publish();
            if (offline && engine::inOfflineRender) {
                auto done = completed.load(std::memory_order_acquire);
                while (done < sequence && !workerFailed.load(std::memory_order_acquire)) {
                    completed.wait(done, std::memory_order_acquire);
                    done = completed.load(std::memory_order_acquire);
                }
            }
        }
        ++sequence;
        playing = nullptr;
        collecting = nullptr;
        offset = 0;
        priming = false;
    }

    std::uint64_t render(const engine::NodeIo& io) noexcept {
        if (io.channels != channels || io.wide() || io.blockOffset < 0 || io.blockOffset > frames - io.frames) {
            localIssue.store(LocalIssue::Shape, std::memory_order_relaxed);
            silence(io);
            return 0;
        }
        std::uint64_t misses = 0;
        std::int32_t eventIndex = 0;
        for (std::int32_t start = 0; start < io.frames;) {
            if (offset == 0) begin(io, start, misses);
            const auto count = std::min(io.frames - start, frames - offset);
            const auto graphFrame = io.blockOffset + start;
            for (std::int32_t channel = 0; channel < channels; ++channel) {
                const auto sample = static_cast<std::size_t>(frames) * static_cast<std::size_t>(channel) +
                                    static_cast<std::size_t>(offset);
                if (collecting) {
                    auto* destination = collecting->audio.data() + sample;
                    if (io.in && io.in[channel]) std::copy_n(io.in[channel] + graphFrame, count, destination);
                    else std::fill_n(destination, count, 0.0f);
                }
                if (io.out && io.out[channel]) {
                    auto* destination = io.out[channel] + graphFrame;
                    if (playing) std::copy_n(playing->audio.data() + sample, count, destination);
                    else std::fill_n(destination, count, 0.0f);
                }
            }
            while (io.events.first && eventIndex < io.events.count) {
                auto event = io.events.first[eventIndex];
                const auto at = static_cast<std::int64_t>(event.frame) - io.blockOffset;
                if (at >= start + count) break;
                ++eventIndex;
                if (collecting == nullptr || at < start) continue;
                if (collecting->eventCount == collecting->events.size()) {
                    localIssue.store(LocalIssue::Events, std::memory_order_relaxed);
                    collecting = nullptr; // No partial stream can lose a note-off silently.
                    continue;
                }
                event.frame = offset + (static_cast<std::int32_t>(at) - start);
                collecting->events[collecting->eventCount++] = event;
            }
            offset += count;
            start += count;
            if (offset == frames) finish();
        }
        return misses;
    }

    const double sampleRate;
    const std::int32_t channels;
    const std::int32_t frames;
    const bool offline;
    std::int32_t pluginLatency = 0; // Published by started.
    PacketRing<Request> requests;
    PacketRing<AudioPacket> replies;
    std::vector<float> discardOutput;
    std::atomic<bool> started{false};
    std::atomic<bool> workerFailed{false};
    std::atomic<std::uint64_t> completed{0};
    std::atomic<LocalIssue> localIssue{LocalIssue::None};
    std::string workerIssue;
    const std::string shapeIssue, eventIssue, queueIssue, prefix;
    std::uint64_t sequence = 1;
    std::int32_t offset = 0;
    bool priming = true;
    Request* collecting = nullptr;
    AudioPacket* playing = nullptr;
    std::jthread worker; // Joined before storage and the processor are destroyed.
};

RemoteDevice::RemoteDevice(DeviceIdentity identity, std::int32_t channels, std::unique_ptr<RemoteProcessor> processor,
                           engine::EventFlow eventFlow)
    : identity_(std::move(identity)), channels_(channels), eventFlow_(eventFlow), processor_(std::move(processor)),
      issuePrefix_(identity_.describe() + ": "), ownerIssue_(issuePrefix_ + "remote plug-in is not prepared") {}

RemoteDevice::~RemoteDevice() = default;

std::string_view RemoteDevice::processingIssue() const noexcept {
    const auto issue = pipeline_ ? pipeline_->issue() : std::string_view(ownerIssue_);
    return issue.empty() ? std::string_view(stateIssue_) : issue;
}

bool RemoteDevice::loadState(const std::string& role, const std::vector<std::uint8_t>& bytes) {
    if (!bytes.empty() && stateIssue_.empty())
        stateIssue_ = issuePrefix_ + "the remote connection cannot restore saved '" + role + "' state";
    return false;
}

void RemoteDevice::prepare(double sampleRate, std::int32_t maxFrames) {
    if (pipeline_ && loaded() && pipeline_->sampleRate == sampleRate && pipeline_->frames == maxFrames &&
        pipeline_->offline == requestedOffline_) return;

    const ReactivationGate::Hold hold(activation_);
    pipeline_.reset();
    loaded_.store(false, std::memory_order_release);
    const auto previousLatency = latency_.exchange(0, std::memory_order_relaxed);
    if (processor_ == nullptr || !std::isfinite(sampleRate) || sampleRate <= 0.0 || channels_ <= 0 || maxFrames <= 0 ||
        static_cast<std::size_t>(maxFrames) > std::vector<float>().max_size() / static_cast<std::size_t>(channels_)) {
        ownerIssue_ = issuePrefix_ + "invalid remote plug-in preparation";
        return;
    }
    try {
        auto next = std::make_unique<Pipeline>(sampleRate, channels_, maxFrames, requestedOffline_, issuePrefix_);
        next->start(*processor_);
        if (!next->workerFailed.load(std::memory_order_acquire)) {
            const auto latency = maxFrames + next->pluginLatency;
            latency_.store(latency, std::memory_order_relaxed);
            if (latency != previousLatency) latencyEpoch_.fetch_add(1, std::memory_order_relaxed);
            loaded_.store(true, std::memory_order_release);
        }
        pipeline_ = std::move(next);
    } catch (const std::exception& error) {
        ownerIssue_ = issuePrefix_ + error.what();
    }
}

void RemoteDevice::release() {
    const ReactivationGate::Hold hold(activation_);
    pipeline_.reset();
    loaded_.store(false, std::memory_order_release);
    ownerIssue_ = issuePrefix_ + "remote plug-in is not prepared";
}

void RemoteDevice::process(const engine::NodeIo& io) noexcept {
    if (io.frames <= 0) return;
    const ReactivationGate::Pass pass(activation_);
    if (!pass || !pipeline_ || !loaded()) {
        silence(io);
        return;
    }
    const auto misses = pipeline_->render(io);
    if (misses != 0) droppedBlocks_.fetch_add(misses, std::memory_order_relaxed);
}

bool RemoteDevice::needsBypassTransition(std::int32_t) noexcept {
    const ReactivationGate::Pass pass(activation_);
    return pass && pipeline_ && (!pipeline_->priming || pipeline_->offset != 0);
}

void RemoteDevice::processBypassTransition(const engine::NodeIo&) noexcept {
    const ReactivationGate::Pass pass(activation_);
    if (pass && pipeline_) pipeline_->resetAudio();
}

}  // namespace adi::device
