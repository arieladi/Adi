// SPDX-License-Identifier: GPL-3.0-or-later
#include "juce/remote_device.hpp"

#include "adi/audio/io_audit.hpp"
#include "adi/engine/offline_render_scope.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <exception>
#include <future>
#include <limits>
#include <map>
#include <mutex>
#include <set>

namespace adi::device {
namespace {

static_assert(std::atomic<std::size_t>::is_always_lock_free);
static_assert(std::atomic<std::int32_t>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);

constexpr auto kControlTimeout = std::chrono::milliseconds(500);

enum class ControlKind { FetchState, RestoreState, ReadParameters, SetParameter };

struct ControlRequest {
    explicit ControlRequest(ControlKind requested) : kind(requested) {}
    const ControlKind kind;
    const RemoteProcessor::StateDeadline deadline = std::chrono::steady_clock::now() + kControlTimeout;
    std::vector<std::uint8_t> bytes;
    std::vector<RemoteParameter> parameters;
    std::uint32_t selector = 0;
    double normalized = 0;
    double applied = std::numeric_limits<double>::quiet_NaN();
    std::string error;
    std::promise<bool> completion;
};

bool normalizedValue(double value) noexcept {
    return std::isfinite(value) && value >= 0.0 && value <= 1.0;
}

bool validParameterValue(const ParamValue& value) noexcept {
    return normalizedValue(value.normalized) && (!value.hasReal || std::isfinite(value.real));
}

// The SPSC cursors start 64 bytes apart. Explicit padding keeps the type at
// the atomic's natural alignment and avoids MSVC's C4324 in containing types.
struct RingCursor {
    static constexpr std::size_t kSpacing = 64;
    std::atomic<std::size_t> index{0};
    std::array<std::byte, kSpacing - sizeof(index)> padding{};
};
static_assert(sizeof(RingCursor) == RingCursor::kSpacing);
static_assert(alignof(RingCursor) == alignof(std::atomic<std::size_t>));

// Like ParamEditCapture, one producer and one consumer each own an index.
// Reservations let both sides use preallocated audio and event storage in
// place, including while a packet spans several graph segments.
template <class Packet>
class PacketRing {
public:
    Packet* reserve() noexcept {
        const auto write = write_.index.load(std::memory_order_relaxed);
        return next(write) != read_.index.load(std::memory_order_acquire) ? &packets_[write] : nullptr;
    }
    void publish() noexcept {
        write_.index.store(next(write_.index.load(std::memory_order_relaxed)), std::memory_order_release);
    }
    Packet* front() noexcept {
        const auto read = read_.index.load(std::memory_order_relaxed);
        return read != write_.index.load(std::memory_order_acquire) ? &packets_[read] : nullptr;
    }
    void pop() noexcept {
        read_.index.store(next(read_.index.load(std::memory_order_relaxed)), std::memory_order_release);
    }
    auto& storage() noexcept { return packets_; } // Owner only, before starting the worker.

private:
    static constexpr std::size_t kSlots = 4; // Three packets plus the SPSC sentinel.
    static constexpr std::size_t next(std::size_t index) noexcept { return (index + 1) % kSlots; }
    std::array<Packet, kSlots> packets_;
    RingCursor write_, read_;
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

    bool requestState(bool restoring, std::vector<std::uint8_t>& bytes, std::string& error) {
        const auto request = std::make_shared<ControlRequest>(restoring ? ControlKind::RestoreState : ControlKind::FetchState);
        if (restoring) request->bytes = bytes;
        if (!requestControl(request, error)) return false;
        if (!restoring) bytes = std::move(request->bytes);
        return true;
    }

    bool requestParameters(std::vector<RemoteParameter>& parameters, std::string& error) {
        const auto request = std::make_shared<ControlRequest>(ControlKind::ReadParameters);
        if (!requestControl(request, error)) return false;
        parameters = std::move(request->parameters);
        return true;
    }

    bool requestParameter(std::uint32_t selector, double normalized, double& applied, std::string& error) {
        const auto request = std::make_shared<ControlRequest>(ControlKind::SetParameter);
        request->selector = selector;
        request->normalized = normalized;
        if (!requestControl(request, error)) return false;
        applied = request->applied;
        return true;
    }

    bool requestControl(const std::shared_ptr<ControlRequest>& request, std::string& error) {
        if (workerFailed.load(std::memory_order_acquire)) {
            error = "the remote processing connection is closed";
            return false;
        }
        auto completion = request->completion.get_future();
        {
            const std::lock_guard lock(controlOwner);
            // A prior owner wait may have expired before the worker could
            // pick it up. Replace it with the only current request.
            pendingControl = request;
            controlWaiting.store(true, std::memory_order_release);
        }
        if (completion.wait_until(request->deadline) != std::future_status::ready) {
            error = "the server did not answer the control request within 500 ms";
            return false;
        }
        if (!completion.get()) {
            error = std::move(request->error);
            return false;
        }
        return true;
    }

    void processControl(RemoteProcessor& processor) {
        if (!controlWaiting.load(std::memory_order_acquire)) return;
        std::shared_ptr<ControlRequest> request;
        {
            const std::lock_guard lock(controlOwner);
            request = std::move(pendingControl);
            controlWaiting.store(false, std::memory_order_release);
        }
        bool succeeded = false;
        try {
            if (std::chrono::steady_clock::now() >= request->deadline) {
                request->error = "the control request expired before I/O started";
            } else {
                switch (request->kind) {
                case ControlKind::FetchState:
                    succeeded = processor.fetchState(request->bytes, request->deadline, request->error);
                    break;
                case ControlKind::RestoreState:
                    succeeded = processor.restoreState(request->bytes, request->deadline, request->error);
                    break;
                case ControlKind::ReadParameters:
                    succeeded = processor.readParameters(request->parameters, request->deadline, request->error);
                    break;
                case ControlKind::SetParameter:
                    succeeded = processor.setParameter(request->selector, request->normalized, request->applied,
                                                       request->deadline, request->error);
                    break;
                }
            }
        } catch (const std::exception& error) {
            request->error = error.what();
        }
        request->completion.set_value(succeeded);
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
                processControl(processor);
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
    std::atomic<bool> controlWaiting{false};
    std::mutex controlOwner;
    std::shared_ptr<ControlRequest> pendingControl;
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
    if (!issue.empty()) return issue;
    if (!stateIssue_.empty()) return stateIssue_;
    return parameterIssue_;
}

std::int32_t RemoteDevice::paramCount() const noexcept {
    return static_cast<std::int32_t>(parameters_.size());
}

const ParamDescriptor* RemoteDevice::paramAt(std::int32_t index) const noexcept {
    if (index < 0 || index >= paramCount()) return nullptr;
    return &parameters_[static_cast<std::size_t>(index)].parameter.descriptor;
}

ParamValue RemoteDevice::getParam(const std::string& id) const noexcept {
    const auto found = std::find_if(parameters_.begin(), parameters_.end(), [&id](const ParameterSlot& slot) {
        return slot.parameter.descriptor.id == id;
    });
    return found == parameters_.end() ? ParamValue{} : found->parameter.current;
}

bool RemoteDevice::setParam(const std::string& id, const ParamValue& value) {
    if (audio::onAudioThread || !pipeline_ || !loaded() || !parametersCurrent_ || !normalizedValue(value.normalized))
        return false;
    const auto found = std::find_if(parameters_.begin(), parameters_.end(), [&id](const ParameterSlot& slot) {
        return slot.parameter.descriptor.id == id;
    });
    if (found == parameters_.end() || found->parameter.descriptor.missing) return false;
    std::string error;
    return applyParameter(*pipeline_, *found, value.normalized, error);
}

bool RemoteDevice::applyParameter(Pipeline& pipeline, ParameterSlot& slot, double value, std::string& error) {
    double applied = 0;
    if (!pipeline.requestParameter(slot.parameter.selector, value, applied, error)) return false;
    if (!normalizedValue(applied)) {
        error = "the server reported an invalid parameter value";
        return false;
    }
    const auto& descriptor = slot.parameter.descriptor;
    slot.parameter.current = descriptor.hasRealRange
        ? ParamValue::withReal(applied, std::lerp(descriptor.minReal, descriptor.maxReal, applied))
        : ParamValue::fromNormalized(applied);
    slot.explicitValue = applied;
    return true;
}

bool RemoteDevice::refreshParameters(Pipeline& pipeline) {
    parametersCurrent_ = false;
    std::vector<RemoteParameter> fresh;
    std::string error;
    if (!pipeline.requestParameters(fresh, error)) {
        parameterIssue_ = issuePrefix_ + "the remote parameter catalogue is unavailable";
        if (!error.empty()) parameterIssue_ += " (" + error + ")";
        return false;
    }
    std::map<std::string, std::size_t, std::less<>> byId;
    std::set<std::uint32_t> selectors;
    for (std::size_t i = 0; i < fresh.size(); ++i) {
        const auto& parameter = fresh[i];
        const auto& descriptor = parameter.descriptor;
        if (descriptor.id.empty() || !validParameterValue(parameter.current) ||
            !validParameterValue(descriptor.defaultValue) ||
            (descriptor.hasRealRange && (!std::isfinite(descriptor.minReal) ||
             !std::isfinite(descriptor.maxReal) || descriptor.minReal > descriptor.maxReal)) ||
            !byId.emplace(descriptor.id, i).second || !selectors.insert(parameter.selector).second) {
            parameterIssue_ = issuePrefix_ + "the remote parameter catalogue is invalid";
            return false;
        }
    }

    auto updated = parameters_;
    std::vector<bool> retained(fresh.size(), false);
    for (auto& slot : updated) {
        const auto id = byId.find(slot.parameter.descriptor.id);
        if (id == byId.end()) {
            slot.parameter.descriptor.missing = true;
            continue;
        }
        retained[id->second] = true;
        slot.parameter = std::move(fresh[id->second]);
    }
    for (std::size_t i = 0; i < fresh.size(); ++i) {
        if (retained[i]) continue;
        if (updated.size() == static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
            parameterIssue_ = issuePrefix_ + "the remote parameter catalogue is too large";
            return false;
        }
        updated.push_back({std::move(fresh[i]), {}});
    }
    parameters_ = std::move(updated);
    parameterIssue_.clear();
    parametersCurrent_ = true;
    return true;
}

std::vector<std::string> RemoteDevice::stateRoles() const {
    return processor_ && processor_->supportsState() ? std::vector<std::string>{"chunk"} : std::vector<std::string>{};
}

std::vector<std::uint8_t> RemoteDevice::saveState(const std::string& role) const {
    if (audio::onAudioThread || role != "chunk" || !processor_ || !processor_->supportsState()) return {};
    std::vector<std::uint8_t> bytes;
    std::string error;
    if (!pipeline_ || !loaded() || !pipeline_->requestState(false, bytes, error) || bytes.empty()) {
        captureIssue_ = issuePrefix_ + "saved state is stale";
        if (!error.empty()) captureIssue_ += " (" + error + ")";
        return {};
    }
    captureIssue_.clear();
    confirmedState_ = bytes;
    return bytes;
}

bool RemoteDevice::loadState(const std::string& role, const std::vector<std::uint8_t>& bytes) {
    if (audio::onAudioThread || bytes.empty()) return false;
    if (role != "chunk" || !processor_ || !processor_->supportsState()) {
        stateIssue_ = issuePrefix_ + "the remote connection cannot restore saved '" + role + "' state";
        return false;
    }
    auto staged = bytes;
    std::string error;
    if (pipeline_ && loaded() && !pipeline_->requestState(true, staged, error)) {
        stateIssue_ = issuePrefix_ + "the remote connection could not restore saved state";
        if (!error.empty()) stateIssue_ += " (" + error + ")";
        return false;
    }
    confirmedState_ = std::move(staged);
    stateIssue_.clear();
    for (auto& slot : parameters_) slot.explicitValue.reset();
    if (pipeline_ && loaded()) (void)refreshParameters(*pipeline_);
    return true;
}

void RemoteDevice::prepare(double sampleRate, std::int32_t maxFrames) {
    if (pipeline_ && loaded() && pipeline_->sampleRate == sampleRate && pipeline_->frames == maxFrames &&
        pipeline_->offline == requestedOffline_ && parameterIssue_.empty() &&
        !pipeline_->workerFailed.load(std::memory_order_acquire)) return;

    const ReactivationGate::Hold hold(activation_);
    pipeline_.reset();
    loaded_.store(false, std::memory_order_release);
    parametersCurrent_ = false;
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
            if (!confirmedState_.empty()) {
                std::string error;
                if (!next->requestState(true, confirmedState_, error)) {
                    stateIssue_ = issuePrefix_ + "the remote connection could not reapply saved state";
                    if (!error.empty()) stateIssue_ += " (" + error + ")";
                } else {
                    stateIssue_.clear();
                }
            }
            if (refreshParameters(*next)) {
                for (auto& slot : parameters_) {
                    if (slot.parameter.descriptor.missing || !slot.explicitValue) continue;
                    std::string error;
                    if (!applyParameter(*next, slot, *slot.explicitValue, error)) {
                        parameterIssue_ = issuePrefix_ + "the remote connection could not reapply parameter '" +
                                          slot.parameter.descriptor.id + "'";
                        if (!error.empty()) parameterIssue_ += " (" + error + ")";
                        break;
                    }
                }
            }
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
    parametersCurrent_ = false;
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
