// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/audio/io_audit.hpp"
#include "adi/engine/offline_render_scope.hpp"
#include "juce/remote_device.hpp"

#include "replace_operator_new.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <thread>

namespace {
int allocations = 0;
int checks = 0;
int failures = 0;
using namespace adi;
using namespace std::chrono_literals;
enum class ParameterCatalogue { Empty, Forward, Reverse, GainOnly, Duplicate };

void check(bool passed, const char* what) {
    ++checks;
    if (!passed) {
        ++failures;
        std::printf("FAIL: %s\n", what);
    }
}

struct Trace {
    const std::thread::id owner = std::this_thread::get_id();
    std::thread::id worker;
    std::atomic<bool> workerOnly{true};
    std::atomic<bool> frozen{false};
    std::atomic<bool> entered{false};
    std::atomic<int> prepared{0};
    std::atomic<bool> stateConnected{true};
    std::atomic<ParameterCatalogue> catalogue{ParameterCatalogue::Empty};
    std::atomic<bool> invalidParameterReply{false};
    bool stateSupported = false;
    std::vector<std::uint8_t> state{99, 3, 42};
    std::array<double, 2> parameterValues{};
    int exchanges = 0;
    int releases = 0;
    int failAt = -1;
    int latency = 0;
    std::array<std::vector<engine::Event>, 3> events;
    std::array<std::optional<engine::DeviceTransport>, 3> transport;
};

class HalfGain final : public device::RemoteProcessor {
public:
    explicit HalfGain(Trace& trace) : trace_(trace) {}

    bool prepare(double, std::int32_t, std::int32_t, bool, std::int32_t& latency,
                 std::string& error) override {
        trace_.worker = std::this_thread::get_id();
        confirmThread();
        ++trace_.prepared;
        trace_.parameterValues = {0.25, 0.75};
        latency = trace_.latency;
        if (trace_.failAt == 0) {
            error = "fixture server did not open";
            return false;
        }
        return true;
    }

    bool exchange(const device::RemoteBlock& input, std::span<float> output, std::string& error) override {
        confirmThread();
        trace_.entered.store(true, std::memory_order_release);
        trace_.frozen.wait(true, std::memory_order_acquire);
        const auto packet = static_cast<std::size_t>(trace_.exchanges++);
        if (trace_.exchanges == trace_.failAt) {
            error = "fixture server disconnected";
            return false;
        }
        if (packet < trace_.events.size()) {
            trace_.events[packet].assign(input.events.begin(), input.events.end());
            trace_.transport[packet] = input.transport;
        }
        std::transform(input.audio.begin(), input.audio.end(), output.begin(), [](float x) { return x * 0.5f; });
        return true;
    }

    [[nodiscard]] bool supportsState() const noexcept override { return trace_.stateSupported; }

    bool fetchState(std::vector<std::uint8_t>& bytes, StateDeadline, std::string& error) override {
        confirmThread();
        trace_.frozen.wait(true, std::memory_order_acquire);
        if (!trace_.stateConnected.load(std::memory_order_acquire)) {
            error = "fixture state socket disconnected";
            return false;
        }
        bytes = trace_.state;
        return true;
    }

    bool restoreState(std::span<const std::uint8_t> bytes, StateDeadline, std::string&) override {
        confirmThread();
        trace_.state.assign(bytes.begin(), bytes.end());
        trace_.parameterValues = {0.0, 0.25};
        return true;
    }

    bool readParameters(std::vector<device::RemoteParameter>& result, StateDeadline, std::string&) override {
        confirmThread();
        const auto catalogue = trace_.catalogue.load(std::memory_order_acquire);
        if (catalogue == ParameterCatalogue::Empty) return true;
        auto gain = parameter(0, "gain", "Gain");
        gain.descriptor.hasRealRange = true;
        gain.descriptor.minReal = -12.0;
        gain.descriptor.maxReal = 12.0;
        gain.current = device::ParamValue::withReal(gain.current.normalized,
            std::lerp(-12.0, 12.0, gain.current.normalized));
        auto tone = parameter(1, "tone", "Tone");
        if (catalogue == ParameterCatalogue::GainOnly) result = {std::move(gain)};
        else if (catalogue == ParameterCatalogue::Reverse) result = {std::move(tone), std::move(gain)};
        else if (catalogue == ParameterCatalogue::Duplicate) result = {gain, gain};
        else result = {std::move(gain), std::move(tone)};
        return true;
    }

    bool setParameter(std::uint32_t selector, double proposed, double& applied,
                      StateDeadline, std::string&) override {
        confirmThread();
        if (trace_.invalidParameterReply.load(std::memory_order_acquire)) {
            applied = std::numeric_limits<double>::quiet_NaN();
            return true;
        }
        for (std::size_t index = 0; index < trace_.parameterValues.size(); ++index) {
            if (selector != selectorFor(index)) continue;
            applied = std::round(4.0 * proposed) / 4.0;
            trace_.parameterValues[index] = applied;
            return true;
        }
        return false;
    }

    void release() noexcept override { confirmThread(); ++trace_.releases; }

private:
    [[nodiscard]] std::uint32_t selectorFor(std::size_t index) const noexcept {
        const auto incarnation = static_cast<std::uint32_t>(trace_.prepared.load(std::memory_order_relaxed));
        return 100 * incarnation + static_cast<std::uint32_t>(index);
    }

    [[nodiscard]] device::RemoteParameter parameter(std::size_t index, std::string id, std::string display) const {
        device::RemoteParameter result;
        result.selector = selectorFor(index);
        result.descriptor.id = std::move(id);
        result.descriptor.name = std::move(display);
        result.current = device::ParamValue::fromNormalized(trace_.parameterValues[index]);
        return result;
    }

    void confirmThread() const noexcept {
        const auto now = std::this_thread::get_id();
        if (now == trace_.owner || now != trace_.worker || audio::onAudioThread)
            trace_.workerOnly.store(false, std::memory_order_relaxed);
    }
    Trace& trace_;
};

const device::DeviceIdentity kIdentity{"vst3", "remote-gain", "Fixture Gain", "ADI", "1"};

struct Stereo {
    std::array<float, 24> left{}, right{}, outLeft{}, outRight{};
    engine::NodeIo io(std::size_t at, std::int32_t frames, std::uint64_t block,
                      std::int32_t blockOffset = 0) noexcept {
        const auto blockStart = at - static_cast<std::size_t>(blockOffset);
        inputs = {left.data() + blockStart, right.data() + blockStart};
        outputs = {outLeft.data() + blockStart, outRight.data() + blockStart};
        engine::NodeIo segment;
        segment.in = inputs.data();
        segment.out = outputs.data();
        segment.channels = 2;
        segment.frames = frames;
        segment.blockOffset = blockOffset;
        segment.renderBlock = block;
        return segment;
    }
private:
    std::array<const float*, 2> inputs{};
    std::array<float*, 2> outputs{};
};

void offlineAudio() {
    Trace trace;
    device::RemoteDevice remote(kIdentity, 2, std::make_unique<HalfGain>(trace));
    remote.setOffline(true);
    remote.prepare(48000.0, 8);
    remote.prepare(48000.0, 8);
    check(remote.loaded() && trace.prepared.load() == 1 && remote.latencySamples() == 8,
          "the device declares eight samples and reuses equivalent preparation");

    Stereo audio;
    audio.outLeft.fill(42.0f);
    audio.outRight.fill(42.0f);
    for (std::size_t i = 0; i < audio.left.size(); ++i) {
        audio.left[i] = static_cast<float>(i + 1);
        audio.right[i] = -audio.left[i] * 2.0f;
    }
    engine::Event note{1, engine::EventType::NoteOn, 3, 0, 91, 60, 0.75};
    engine::Event pressure{6, engine::EventType::NoteExpression, 3, 2, 91, 60, 0.123456789};
    engine::Event off{3, engine::EventType::NoteOff, 3, 0, 91, 60, 0.0};
    const std::array crossing{engine::Event{1, engine::EventType::Controller, 2, 0, 0, 64, 1.0},
                              engine::Event{4, engine::EventType::ParamValue, 0, 0, 0, 7, 0.25}};
    engine::DeviceTransport transport;
    transport.sample = 400;
    transport.playing = true;

    const engine::OfflineRenderScope scope;
    auto io = audio.io(0, 3, 1);
    io.events = {&note, 1};
    io.deviceTransport = &transport;
    remote.process(io);
    io = audio.io(3, 5, 1, 3);
    io.events = {&pressure, 1};
    remote.process(io);
    io = audio.io(8, 5, 2);
    io.events = {&off, 1};
    remote.process(io);
    io = audio.io(13, 6, 3);
    io.events = {crossing.data(), 2};
    remote.process(io);
    remote.process(audio.io(19, 5, 4));

    float difference = 0.0f;
    for (std::size_t i = 0; i < audio.left.size(); ++i) {
        const float left = i < 8 ? 0.0f : audio.left[i - 8] * 0.5f;
        const float right = i < 8 ? 0.0f : audio.right[i - 8] * 0.5f;
        difference = std::max(difference, std::abs(left - audio.outLeft[i]));
        difference = std::max(difference, std::abs(right - audio.outRight[i]));
    }
    check(difference == 0.0f && remote.droppedBlocks() == 0,
          "split and variable-sized calls preserve exact stereo gain and fixed delay");
    check(remote.processingIssue().empty(), "successful render has no remote error");
    remote.release();
    check(trace.workerOnly.load() && trace.releases == 1 && trace.exchanges == 3,
          "activation, all three exchanges and release run on one worker");
    check(trace.events[0].size() == 2 && trace.events[0][0].frame == 1 && trace.events[0][1].frame == 6 &&
          trace.events[0][1].noteId == pressure.noteId && trace.events[0][1].value == pressure.value &&
          trace.transport[0] && trace.transport[0]->sample == 400,
          "the engine seam retains event offsets, note expression precision and transport");
    check(trace.events[1].size() == 2 && trace.events[1][0].frame == 3 && trace.events[1][1].frame == 6 &&
          trace.events[2].size() == 1 && trace.events[2][0].frame == 1,
          "an event on the other side of a packet boundary is rebased exactly once");
    std::printf("Offline remote/local maximum PCM difference %.9f at 8 samples of wire latency\n",
                static_cast<double>(difference));
}

void realtimeDeadline() {
    Trace trace;
    trace.latency = 3;
    trace.frozen.store(true, std::memory_order_relaxed);
    device::RemoteDevice remote(kIdentity, 2, std::make_unique<HalfGain>(trace));
    // An offline-quality request on its own cannot make a driver call wait.
    remote.setOffline(true);
    remote.prepare(48000.0, 8);
    check(remote.latencySamples() == 11, "graph declaration sums plug-in latency and one quantum");

    Stereo samples;
    samples.left.fill(2.0f);
    samples.right.fill(2.0f);
    samples.outLeft.fill(1.0f);
    samples.outRight.fill(1.0f);
    auto first = samples.io(0, 8, 1);
    const engine::Event expression{2, engine::EventType::NoteExpression, 3, 2, 91, 60, 0.123456789};
    const engine::DeviceTransport transport;
    first.events = {&expression, 1};
    first.deviceTransport = &transport;
    // A watchdog releases the fixture even if processing mistakenly waits.
    std::jthread watchdog([&trace](std::stop_token stop) {
        const auto deadline = std::chrono::steady_clock::now() + 1s;
        while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
        trace.frozen.store(false, std::memory_order_release);
        trace.frozen.notify_one();
    });
    {
        const audio::CallbackScope callback;
        remote.process(first);
    }
    const auto waitUntil = std::chrono::steady_clock::now() + 1s;
    while (!trace.entered.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < waitUntil)
        std::this_thread::yield();
    check(trace.entered.load(std::memory_order_acquire), "worker is still handling the withheld reply");

    const auto second = samples.io(8, 8, 2);
    {
        const audio::CallbackScope callback;
        remote.process(second);
    }
    const bool returnedWhileWithheld = trace.frozen.load(std::memory_order_acquire);
    watchdog.request_stop();
    watchdog.join();

    check(returnedWhileWithheld && allocations == 0,
          "callback returns while the worker is blocked and performs zero C++ allocations");
    check(remote.droppedBlocks() == 1 && std::all_of(samples.outLeft.begin(), samples.outLeft.begin() + 16,
                                                   [](float x) { return x == 0.0f; }),
          "initial priming is silent and only the missed reply is counted");
    remote.release();
    check(trace.workerOnly.load(), "the callback does not execute the transport");
    std::printf("Realtime callback allocations %d, withheld deadlines %llu\n", allocations,
                static_cast<unsigned long long>(remote.droppedBlocks()));
}

void bypassFlushesPackets() {
    Trace trace;
    device::RemoteDevice remote(kIdentity, 2, std::make_unique<HalfGain>(trace));
    remote.setOffline(true);
    remote.prepare(48000.0, 8);
    const engine::OfflineRenderScope scope;
    Stereo samples;
    samples.left.fill(2.0f);
    samples.right.fill(4.0f);
    const auto first = samples.io(0, 8, 1);
    remote.process(first);
    check(remote.needsBypassTransition(remote.latencySamples()), "queued wet audio needs a bypass reset");
    remote.processBypassTransition(first);
    check(!remote.needsBypassTransition(remote.latencySamples()), "bypass reset is complete before the next block");

    samples.left.fill(3.0f);
    samples.right.fill(6.0f);
    remote.process(samples.io(8, 8, 2));
    remote.process(samples.io(16, 8, 3));
    check(samples.outLeft[8] == 0.0f && samples.outRight[8] == 0.0f &&
          samples.outLeft[16] == 1.5f && samples.outRight[16] == 3.0f && remote.droppedBlocks() == 0,
          "the first block after bypass primes silently, then plays the new input");
}

void offlineFailure() {
    {
        Trace trace;
        trace.failAt = 0;
        device::RemoteDevice remote(kIdentity, 2, std::make_unique<HalfGain>(trace));
        remote.prepare(48000.0, 8);
        check(!remote.loaded() && remote.processingIssue().find("Fixture Gain") != std::string_view::npos &&
              remote.processingIssue().find("did not open") != std::string_view::npos,
              "failed connection reports the plug-in and server response instead of appearing loaded");
    }
    Trace trace;
    trace.failAt = 2;
    device::RemoteDevice remote(kIdentity, 2, std::make_unique<HalfGain>(trace));
    remote.setOffline(true);
    remote.prepare(48000.0, 8);
    Stereo samples;
    const engine::OfflineRenderScope scope;
    remote.process(samples.io(0, 8, 1));
    check(remote.processingIssue().empty(), "first offline packet completed");
    remote.process(samples.io(8, 8, 2));
    const auto issue = remote.processingIssue();
    check(issue.find("Fixture Gain") != std::string_view::npos && issue.find("disconnected") != std::string_view::npos,
          "offline processing publishes the named fault before the render owner can accept the block");
}

void unsupportedState() {
    Trace trace;
    device::RemoteDevice remote(kIdentity, 2, std::make_unique<HalfGain>(trace));
    const std::vector<std::uint8_t> persisted{0, 6, 42, 255};
    check(!remote.loadState("chunk", persisted) && remote.stateRoles().empty(),
          "refused state offers no role that could overwrite the preceding capture");
    remote.setOffline(true);
    remote.prepare(48000.0, 8);
    const auto issue = remote.processingIssue();
    check(remote.loaded() && issue.find("Fixture Gain") != std::string_view::npos &&
          issue.find("cannot restore saved 'chunk' state") != std::string_view::npos,
          "a later successful connection does not clear the named state restore failure");
}

void connectedState() {
    Trace trace;
    trace.stateSupported = true;
    device::RemoteDevice remote(kIdentity, 2, std::make_unique<HalfGain>(trace));
    const std::vector<std::uint8_t> persisted{0, 6, 42, 255};
    const std::vector<std::uint8_t> edited{2, 22, 222};
    check(remote.stateRoles() == std::vector<std::string>{"chunk"} && remote.loadState("chunk", persisted),
          "a state-enabled device queues its persisted raw state before preparation");
    remote.prepare(48000.0, 8);
    check(remote.saveState("chunk") == persisted && remote.stateCaptureIssue().empty(),
          "the worker restored the exact previously persisted bytes");
    check(remote.loadState("chunk", edited) && remote.saveState("chunk") == edited,
          "updated raw state completes a round trip on the worker");
    trace.stateConnected.store(false, std::memory_order_release);
    check(remote.saveState("chunk").empty() && remote.stateCaptureIssue().find("Fixture Gain") != std::string_view::npos &&
          remote.stateCaptureIssue().find("stale") != std::string_view::npos && remote.processingIssue().empty(),
          "a disconnected state stream names the stale capture and publishes no replacement bytes");
    trace.stateConnected.store(true, std::memory_order_release);
    check(remote.saveState("chunk") == edited && remote.stateCaptureIssue().empty() && trace.workerOnly.load(),
          "a later successful worker reply clears the stale state diagnostic");

    trace.frozen.store(true, std::memory_order_release);
    std::jthread watchdog([&trace](std::stop_token stop) {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
        trace.frozen.store(false, std::memory_order_release);
        trace.frozen.notify_one();
    });
    const auto start = std::chrono::steady_clock::now();
    check(remote.saveState("chunk").empty() && trace.frozen.load(std::memory_order_acquire) &&
          remote.stateCaptureIssue().find("500 ms") != std::string_view::npos,
          "save continues with stale state before a withheld worker reply arrives");
    const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start);
    watchdog.request_stop();
    watchdog.join();
    std::printf("Withheld state capture returned in %.3f ms\n", elapsed.count());
}

void parameterControls() {
    using device::ParamValue;
    Trace trace;
    trace.stateSupported = true;
    trace.catalogue.store(ParameterCatalogue::Forward, std::memory_order_release);
    device::RemoteDevice remote(kIdentity, 2, std::make_unique<HalfGain>(trace));
    remote.prepare(48000.0, 8);
    const auto stable = [&] {
        return remote.paramCount() == 2 && remote.paramAt(0)->id == "gain" && remote.paramAt(1)->id == "tone";
    };
    check(stable() && remote.paramAt(-1) == nullptr && remote.paramAt(2) == nullptr &&
          remote.getParam("gain").normalized == 0.25 && remote.getParam("gain").real == -6.0,
          "owner snapshot exposes stable IDs, valid bounds and the initial server values");
    check(remote.setParam("gain", ParamValue::fromNormalized(0.62)) &&
          remote.getParam("gain").normalized == 0.5 && remote.getParam("gain").real == 0.0 &&
          remote.getParam("gain").hasReal,
          "a setter publishes the acknowledged normalized value and its declared real equivalent");
    check(!remote.setParam("absent", ParamValue::fromNormalized(0.25)) &&
          !remote.setParam("gain", ParamValue::fromNormalized(1.5)), "an unknown ID and an out-of-range value are refused");
    trace.invalidParameterReply.store(true, std::memory_order_release);
    check(!remote.setParam("gain", ParamValue::fromNormalized(0.25)) && remote.getParam("gain").normalized == 0.5,
          "a nonfinite acknowledgement does not overwrite the prior snapshot");
    trace.invalidParameterReply.store(false, std::memory_order_release);
    const std::string gainId = "gain";
    const auto priorAllocations = allocations;
    bool rejectedCallback = false;
    {
        const audio::CallbackScope callback;
        rejectedCallback = !remote.setParam(gainId, ParamValue::fromNormalized(0.75));
    }
    check(rejectedCallback && allocations == priorAllocations, "the audio callback rejects an owner setter without allocating");

    trace.catalogue.store(ParameterCatalogue::Reverse, std::memory_order_release);
    remote.prepare(48000.0, 16);
    check(stable() && remote.getParam("gain").normalized == 0.5,
          "new ordering and selectors preserve slots and restore the explicit owner edit");
    trace.catalogue.store(ParameterCatalogue::GainOnly, std::memory_order_release);
    remote.prepare(48000.0, 24);
    check(stable() && remote.paramAt(1)->missing && !remote.setParam("tone", ParamValue::fromNormalized(0.5)),
          "a removed ID keeps its old slot and refuses writes to the old selector");
    trace.catalogue.store(ParameterCatalogue::Forward, std::memory_order_release);
    check(remote.loadState("chunk", {3}) && stable() && !remote.paramAt(1)->missing &&
          remote.getParam("gain").normalized == 0.0 && remote.getParam("tone").normalized == 0.25,
          "state recall refreshes the server values and restores removed IDs to their old slots");
    remote.prepare(48000.0, 32);
    check(remote.getParam("gain").normalized == 0.0,
          "repreparing after explicit state recall keeps recalled values instead of older writes");

    trace.catalogue.store(ParameterCatalogue::Duplicate, std::memory_order_release);
    remote.prepare(48000.0, 40);
    check(stable() && remote.processingIssue().find("remote parameter catalogue is invalid") != std::string_view::npos &&
          !remote.setParam("gain", ParamValue::fromNormalized(0.75)) && trace.workerOnly.load(),
          "an invalid reply retains the descriptor snapshot and blocks writes by invalid selectors");
}

void graphIsolation() {
    struct Tone final : engine::Node {
        explicit Tone(float value) : value_(value) {}
        void process(const engine::NodeIo& io) noexcept override {
            for (std::int32_t channel = 0; channel < io.channels; ++channel)
                std::fill_n(io.out[channel] + io.blockOffset, io.frames, value_);
        }
        float value_;
    } remoteInput(2.0f), unaffected(0.25f);

    Trace trace;
    trace.failAt = 2;
    device::RemoteDevice remote(kIdentity, 2, std::make_unique<HalfGain>(trace));
    remote.setOffline(true);
    device::DeviceNode node(remote);
    engine::SumNode master;
    engine::Graph graph;
    const auto a = graph.addNode(remoteInput);
    const auto b = graph.addNode(node);
    const auto c = graph.addNode(unaffected);
    const auto main = graph.addNode(master);
    check(graph.connect(a, b) && graph.connect(b, main) && graph.connect(c, main), "graph has remote and dry branches");
    graph.setOutput(main);
    graph.prepare(48000.0, 8);
    check(graph.ok() && graph.outputLatencyOf(main) == 8, "ordinary DeviceNode latency aligns the two branches");

    std::array<float, 8> left{}, right{};
    std::array<float*, 2> outputs{left.data(), right.data()};
    engine::AudioIo io;
    io.out = outputs.data();
    io.numOut = 2;
    io.frames = 8;
    const auto isSteady = [&](float level) {
        return left == right && std::all_of(left.begin(), left.end(), [=](float x) { return x == level; });
    };
    const engine::OfflineRenderScope scope;
    graph.process(io);
    check(isSteady(0.0f), "both branches take the same initial compensation delay");
    graph.process(io);
    check(isSteady(1.25f) && !remote.processingIssue().empty(), "the prior wet block and dry branch stay aligned through disconnect");
    graph.process(io);
    check(isSteady(0.25f) && remote.droppedBlocks() == 1, "a missed remote block leaves the dry branch untouched");
}
}  // namespace

void adi::test::countReplacedNew() noexcept {
    if (audio::onAudioThread) ++allocations;
}

int main() {
    offlineAudio();
    realtimeDeadline();
    bypassFlushesPackets();
    offlineFailure();
    unsupportedState();
    connectedState();
    parameterControls();
    graphIsolation();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
