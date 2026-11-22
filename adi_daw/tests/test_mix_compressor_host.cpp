// SPDX-License-Identifier: AGPL-3.0-or-later
// Actual native factory and generated audio; no editor, device or reference host.
#include "adi/engine/param_automation.hpp"
#include "adi/mix/compressor/Core.hpp"
#include "adi/ui/project_document.hpp"
#include "adi/row_capture.hpp"
#include "juce/builtin_host.hpp"
#include "juce/mix_function_host.hpp"
#include "temp_directory.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <type_traits>
#if defined(_M_X64) || defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace {
thread_local bool audio = false;
std::atomic<unsigned> allocations{0}, frees{0};
} // namespace
void *operator new(std::size_t n) {
    if (audio)
        ++allocations;
    if (auto *p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void *p) noexcept {
    if (audio && p)
        ++frees;
    std::free(p);
}
void operator delete[](void *p) noexcept { ::operator delete(p); }
void operator delete(void *p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void *p, std::size_t) noexcept { ::operator delete(p); }
namespace {
using namespace adi;
namespace c = mix::compressor;
int checks = 0, failures = 0;
void check(bool value, const char *label) {
    ++checks;
    if (!value) {
        ++failures;
        std::printf("FAIL %s\n", label);
    }
}
std::unique_ptr<device::DeviceInstance> make(int channels = 2) {
    rows::PluginRef ref;
    ref.format = "internal";
    ref.uid = "org.adidaw.mix.compressor";
    ref.name = "Compressor";
    engine::DeviceRequest request;
    request.ref = &ref;
    request.channels = channels;
    std::string error;
    return device::withBuiltinDevices()(request, error);
}
c::Settings controls(const device::DeviceInstance &device) {
    c::Settings s;
    s.thresholdDb = static_cast<float>(device.getParam("0").real);
    s.ratio = static_cast<float>(device.getParam("1").real);
    s.kneeDb = static_cast<float>(device.getParam("2").real);
    s.rangeDb = static_cast<float>(device.getParam("3").real);
    s.attack = static_cast<float>(device.getParam("4").normalized);
    s.release = static_cast<float>(device.getParam("5").normalized);
    s.autoGain = device.getParam("7").normalized != 0;
    s.autoRelease = device.getParam("6").normalized != 0;
    return s;
}
template <class T> using Audio = std::array<std::array<T, 64>, 2>;
template <class T>
void process(device::DeviceInstance &device, Audio<T> &data, std::uint64_t block, bool split) {
    const T *in[]{data[0].data(), data[1].data()};
    T *out[]{data[0].data(), data[1].data()};
    engine::NodeIo io;
    io.channels = 2;
    io.renderBlock = block;
    io.frames = split ? 17 : 64;
    if constexpr (std::is_same_v<T, float>) {
        io.in = in;
        io.out = out;
    } else {
        io.in64 = in;
        io.out64 = out;
    }
    audio = true;
    device.process(io);
    if (split) {
        io.blockOffset = 17;
        io.frames = 47;
        device.process(io);
    }
    audio = false;
}
template <class T> void exactAudio(double rate) {
    auto whole = make(), split = make();
    check(whole && split, "normal builtin loader creates Clean compressor");
    if (!whole || !split)
        return;
    whole->setParam("7", device::ParamValue::fromNormalized(0));
    split->setParam("7", device::ParamValue::fromNormalized(0));
    whole->prepare(rate, 64);
    split->prepare(rate, 64);
    c::Core reference;
    check(reference.prepare({rate, 64, 2}, controls(*whole)) == c::Status::Ok,
          "direct Clean comparison prepares");
    bool exact = true, reduces = false;
    for (std::uint64_t block = 1; block <= 64; ++block) {
        if (block == 8 || block == 16 || block == 24 || block == 32 || block == 40 || block == 48) {
            const char *id = block == 8    ? "0"
                             : block == 16 ? "1"
                             : block == 24 ? "7"
                             : block == 32 ? "6"
                             : block == 40 ? "2"
                                           : "3";
            const double v = block == 24 || block == 32 ? 1. : .3;
            whole->setParam(id, device::ParamValue::fromNormalized(v));
            split->setParam(id, device::ParamValue::fromNormalized(v));
        }
        if (block == 20) {
            whole->prepare(rate, 64);
            split->prepare(rate, 64);
        }
        Audio<T> a{};
        for (std::size_t ch = 0; ch < 2; ++ch)
            for (std::size_t i = 0; i < 64; ++i)
                a[ch][i] = static_cast<T>(.6 * std::sin(.13 * static_cast<double>(block * 64 + i) +
                                                        static_cast<double>(ch)));
        if (block == 50)
            a[0][17] = std::numeric_limits<T>::quiet_NaN();
        if (block == 51)
            a[1][33] = std::numeric_limits<T>::infinity();
        auto b = a, expected = a;
        for (auto &channel : expected)
            for (auto &sample : channel)
                if (!std::isfinite(static_cast<float>(sample)))
                    sample = 0;
        const auto dry = expected;
        auto settings = controls(*whole);
        T *planes[]{expected[0].data(), expected[1].data()};
        audio = true;
        const auto context = reference.beginBlock(64);
        const auto result = reference.process(context, 0, 64, planes, nullptr, 0, settings);
        audio = false;
        process(*whole, a, block, false);
        process(*split, b, block, true);
        exact = (result == c::Status::Ok && a == expected && b == expected) && exact;
        if (block > 2 && block < 24)
            reduces = reduces || std::abs(a[0][63]) < std::abs(dry[0][63]) * .9;
    }
    check(exact, "native whole/split, live controls, same-format prepare and sanitized input equal "
                 "direct Clean samples");
    check(reduces, "loaded native compressor actually reduces programme level");
}
void quietEnvelope() {
    auto device = make();
    check(device != nullptr, "quiet-envelope fixture creates native Clean");
    if (!device) return;
    device->setParam("7", device::ParamValue::fromNormalized(0));
    device->prepare(48000, 64);
    Audio<float> data{};
#if defined(_M_X64) || defined(__SSE2__)
    const auto caller = _mm_getcsr();
    // The adapter must protect a caller that has not enabled FTZ or DAZ.
    const auto ordinary = caller & ~0x807fU;
    _mm_setcsr(ordinary);
#endif
    data[0].fill(.95f); data[1].fill(.95f);
    for (std::uint64_t block = 1; block <= 750; ++block) {
        data[0].fill(.95f); data[1].fill(.95f);
        process(*device, data, block, false);
    }
    bool finite = true;
    for (std::uint64_t block = 751; block <= 135750; ++block) {
        data[0].fill(.0001f); data[1].fill(.0001f);
        process(*device, data, block, false);
        finite = finite && std::isfinite(data[0][0]) && std::isfinite(data[1][0]);
    }
#if defined(_M_X64) || defined(__SSE2__)
    // Sticky denormal-operand flags also expose the old subnormal recurrence,
    // without a timing benchmark. Restoring all caller bits is part of the scope.
    const auto after = _mm_getcsr();
    _mm_setcsr(caller);
    check(after == ordinary, "long quiet processing restores caller FP flags and avoids denormal operands");
#endif
    check(finite && data[0][0] == .0001f && data[1][0] == .0001f,
          "180 seconds below threshold settles to exact finite unity output");
    data[0].fill(.95f); data[1].fill(.95f);
    process(*device, data, 135751, false);
    check(std::isfinite(data[0][63]) && data[0][63] < .95f,
          "compression resumes after the long quiet interval");
}
void cleanGraphSuspension() {
    constexpr double rate = 48000.;
    constexpr int frames = 64;
    auto device = make(), continuous = make(), cold = make();
    check(device && continuous && cold, "graph suspension fixture creates three native Clean devices");
    if (!device || !continuous || !cold) return;
    for (auto *instance : {device.get(), continuous.get(), cold.get()}) {
        instance->setParam("0", device::ParamValue::fromNormalized(0.));
        instance->setParam("2", device::ParamValue::fromNormalized(0.));
        instance->setParam("4", device::ParamValue::fromNormalized(0.));
        instance->setParam("5", device::ParamValue::fromNormalized(1.));
        instance->setParam("7", device::ParamValue::fromNormalized(0.));
    }
    continuous->prepare(rate, frames);
    cold->prepare(rate, frames);
    struct Programme final : engine::Node {
        float value = .95f;
        void process(const engine::NodeIo &io) noexcept override {
            for (auto ch = 0; ch < io.channels; ++ch)
                std::fill_n(io.out[ch] + io.blockOffset, io.frames, value);
        }
    } programme;
    device::DeviceNode node(*device);
    engine::Graph graph;
    const auto input = graph.addNode(programme), effect = graph.addNode(node);
    graph.connect(input, effect);
    graph.setOutput(effect);
    graph.prepare(rate, frames);
    const auto tail = node.tailSamples();
    check(tail == static_cast<std::int64_t>(rate * c::Core::quietTailSeconds) && tail < engine::kInfiniteTail,
          "the scheduler sees the finite maximum prepared Clean release");
    if (tail <= 0 || tail == engine::kInfiniteTail) return;

    Audio<float> produced{}, unsuspended{}, restarted{};
    float *outputs[]{produced[0].data(), produced[1].data()};
    engine::AudioIo io;
    io.frames = frames;
    io.numOut = 2;
    io.out = outputs;
    const auto render = [&] {
        audio = true;
        graph.process(io);
        audio = false;
    };
    std::uint64_t nextBlock = 1;
    const auto continuousRender = [&] {
        for (auto &lane : unsuspended) lane.fill(programme.value);
        process(*continuous, unsuspended, nextBlock++, false);
    };
    const auto alloc = allocations.load(), free = frees.load();
    for (int i = 0; i < 512; ++i) { render(); continuousRender(); }
    check(produced == unsuspended && produced[0].back() < programme.value * .25f,
          "the real graph drives a substantial Clean envelope before the quiet drain");
    const auto awake = graph.stats().nodesSuspended;
    programme.value = 0.f;
    for (int i = 0; i < 32; ++i) { render(); continuousRender(); }
    programme.value = .95f;
    render(); continuousRender();
    check(graph.stats().nodesSuspended == awake && produced == unsuspended,
          "brief silent gaps keep the monitored release continuous");

    programme.value = 0.f;
    const auto tailBlocks = (tail + frames - 1) / frames;
    for (std::int64_t i = 0; i < tailBlocks; ++i) { render(); continuousRender(); }
    check(graph.stats().nodesSuspended == awake, "the complete quiet release runs before suspension");
    const auto clears = graph.stats().suspendClears;
    for (int i = 0; i < 5; ++i) { render(); continuousRender(); }
    check(graph.stats().nodesSuspended == awake + 5 && graph.stats().suspendClears == clears + 1 &&
              produced == Audio<float>{}, "the resting Clean node stops DSP and clears the output once");
    programme.value = .95f;
    render(); continuousRender();
    for (auto &lane : restarted) lane.fill(programme.value);
    process(*cold, restarted, 1, false);
    double peakDifference = 0.;
    for (std::size_t channel = 0; channel < produced.size(); ++channel)
        for (std::size_t i = 0; i < produced[channel].size(); ++i)
            peakDifference = std::max(peakDifference, std::abs(static_cast<double>(produced[channel][i]) -
                                                             unsuspended[channel][i]));
    check(produced == restarted && peakDifference <= 1.e-7 && graph.stats().nodesSuspended == awake + 5,
          "Clean wakes on the first sample with fresh state within float tolerance of uninterrupted DSP");

    device->setOffline(true);
    check(node.tailSamples() == engine::kInfiniteTail,
          "offline Clean keeps processing after the same silent interval that suspends in realtime");
    programme.value = 0.f;
    const auto suspendedBeforeRender = graph.stats().nodesSuspended;
    for (std::int64_t i = 0; i < tailBlocks + 5; ++i) {
        render();
        continuousRender();
    }
    check(graph.stats().nodesSuspended == suspendedBeforeRender && produced == unsuspended,
          "offline Clean runs beyond the realtime tail and preserves digital silence");
    device->setOffline(false);
    check(node.tailSamples() == tail, "returning to realtime restores the prepared Clean tail");
    check(allocations.load() == alloc && frees.load() == free, "drain, suspend, and wake allocate or free no DSP storage");
}
void thresholdEditWhileSuspended() {
    auto parked = make(), cold = make();
    check(parked && cold, "parked-edit fixture creates two native Clean devices");
    if (!parked || !cold)
        return;

    const auto previousThreshold = device::ParamValue::fromNormalized(.8);
    const auto editedThreshold = device::ParamValue::fromNormalized(.45);
    const auto autoGainOn = device::ParamValue::fromNormalized(1.);
    check(parked->setParam("0", previousThreshold) && parked->setParam("7", autoGainOn) &&
              cold->setParam("0", editedThreshold) && cold->setParam("7", autoGainOn),
          "fixture stages distinct settled make-up targets");
    parked->prepare(48000, 64);
    cold->prepare(48000, 64);

    Audio<float> programme{};
    programme[0].fill(.001f);
    programme[1].fill(-.0015f);
    auto previous = programme;
    process(*parked, previous, 1, false);
    device::DeviceNode parkedNode(*parked);
    parkedNode.didSuspend();
    check(parked->setParam("0", editedThreshold), "owner edits the parked compressor");

    auto woke = programme, fresh = programme;
    process(*parked, woke, 2, false);
    process(*cold, fresh, 1, false);
    check(fresh[0][0] > previous[0][0], "edited Threshold raises first-sample make-up in a fresh instance");
    check(woke == fresh, "a Threshold edit while parked takes effect on the first sample after wake");
}
void stateAndDescriptors() {
    auto a = make(), b = make();
    check(a && b && a->paramCount() == 8 && a->latencySamples() == 0,
          "eight implemented controls and zero latency are declared");
    if (!a || !b)
        return;
    bool declared = true;
    for (int i = 0; i < a->paramCount(); ++i) {
        const auto *d = a->paramAt(i);
        declared = declared && d && d->automatable && d->groupId == "compressor" &&
                   d->groupName == "Clean";
    }
    check(declared && !a->paramAt(8), "Clean page contains only implemented automatable controls");
    check(std::abs(a->getParam("4").real - .25) < .001 &&
              std::abs(a->getParam("5").real - 200) < .001,
          "default display times use the canonical control domain");
    check(a->getParam("2").real == 18 && a->getParam("7").normalized == 1,
          "fresh Clean uses authored knee and automatic-gain defaults");
    const auto text = a->paramText("5", a->getParam("5").normalized);
    a->setParam("4", device::ParamValue::fromNormalized(.8));
    check(a->paramText("5", a->getParam("5").normalized) == text,
          "manual release display stays independent of its internal coupled coefficient");
    a->setParam("6", device::ParamValue::fromNormalized(1));
    check(a->paramText("5", a->getParam("5").normalized).find("Auto") != std::string::npos,
          "automatic release does not claim a fixed measured duration");
    const auto state = a->saveState("state");
    check(b->loadState("state", state) && b->saveState("state") == state,
          "all authored state recalls exactly");
    auto bad = Payload::from_cbor(state);
    bad["params"]["1"] = std::numeric_limits<double>::quiet_NaN();
    check(!b->loadState("state", Payload::to_cbor(bad)) && b->saveState("state") == state,
          "bad state refuses atomically");
    check(!b->setParam("0", device::ParamValue::fromNormalized(2)) &&
              !b->setParam("style", device::ParamValue::fromNormalized(0)),
          "out-of-range and unavailable style edits refuse");
}
template <class T> void dryOutsidePreparedStream() {
    auto device = make();
    check(device != nullptr, "unprepared pass-through fixture loads");
    if (!device) return;

    const Audio<T> input{[] {
        std::array<T, 64> channel{};
        for (std::size_t i = 0; i < channel.size(); ++i)
            channel[i] = static_cast<T>(i + 1) / static_cast<T>(64);
        return channel;
    }(), {}};
    Audio<T> output{};
    const T *in[]{input[0].data(), nullptr};
    T *out[]{output[0].data(), output[1].data()};
    engine::NodeIo io;
    io.channels = 2;
    io.blockOffset = 17;
    io.frames = 47;
    if constexpr (std::is_same_v<T, float>) {
        io.in = in;
        io.out = out;
    } else {
        io.in64 = in;
        io.out64 = out;
    }

    const auto expectDry = [&](const char *label) {
        for (auto &channel : output) channel.fill(static_cast<T>(-3));
        audio = true;
        device->process(io);
        audio = false;
        bool exact = true;
        for (std::size_t i = 0; i < output[0].size(); ++i) {
            const bool segment = i >= static_cast<std::size_t>(io.blockOffset);
            exact &= output[0][i] == (segment ? input[0][i] : static_cast<T>(-3));
            exact &= output[1][i] == (segment ? T{} : static_cast<T>(-3));
        }
        check(exact, label);
    };

    expectDry("first unprepared callback copies only its offset segment, including missing-input silence");
    device->prepare(48000, 32);
    device->release();
    expectDry("released adapter passes the valid segment without applying the previous stream maximum");
}

void automationAndRefusal() {
    auto a = make(), b = make();
    a->prepare(48000, 64);
    b->prepare(48000, 64);
    rows::Model model;
    const std::array<double, 8> target{.3, .5, .1, .6, .2, .4, 1, 0};
    for (int i = 0; i < 8; ++i) {
        rows::AutomationLane lane;
        lane.id = i + 1;
        lane.ownerKind = "device";
        lane.ownerId = 1;
        lane.paramRef = a->paramAt(i)->id;
        lane.valueDomain = "normalized";
        lane.enabled = true;
        lane.defaultValue = target[static_cast<std::size_t>(i)];
        model.automationLanes.push_back(lane);
        b->setParam(lane.paramRef, device::ParamValue::fromNormalized(lane.defaultValue));
    }
    engine::Transport transport;
    auto program = engine::compileAutomation(model, 48000);
    auto binding = engine::bindDeviceAutomation(program, {}, transport, 48000);
    check(binding->problems.empty() && binding->byDevice.contains(1),
          "all eight published IDs bind through real engine automation");
    if (!binding->byDevice.contains(1))
        return;
    std::array<engine::Event, 16> events{};
    engine::EventList list(events.data(), 16);
    audio = true;
    binding->byDevice.at(1)->emit(list, 64);
    audio = false;
    check(list.size() == 8, "automation chases every Clean control once");
    Audio<float> x{}, y{};
    for (auto &ch : x)
        ch.fill(.5f);
    y = x;
    const float *in[]{x[0].data(), x[1].data()};
    float *out[]{x[0].data(), x[1].data()};
    engine::NodeIo io;
    io.channels = 2;
    io.frames = 64;
    io.in = in;
    io.out = out;
    io.events = {list.begin(), list.size()};
    audio = true;
    a->process(io);
    audio = false;
    process(*b, y, 1, false);
    bool exact = x == y;
    for (int i = 0; i < 8; ++i)
        exact = exact && a->getParam(a->paramAt(i)->id).normalized ==
                             b->getParam(b->paramAt(i)->id).normalized;
    check(exact,
          "real automation events and owner edits produce identical Clean samples and targets");
    // Rejected buffers must never admit an unfinished logical core block.
    io.events = {};
    out[1] = out[0];
    audio = true;
    a->process(io);
    audio = false;
    x = {};
    for (auto &ch : x)
        ch.fill(.3f);
    y = x;
    out[1] = x[1].data();
    audio = true;
    a->process(io);
    audio = false;
    process(*b, y, 2, false);
    check(x == y, "overlapping planes refuse without poisoning the next valid callback");
    auto mono = make(1);
    mono->prepare(48000, 64);
    c::Core reference;
    check(reference.prepare({48000, 64, 1}, controls(*mono)) == c::Status::Ok,
          "mono Clean reference prepares");
    x = {};
    for (auto &ch : x)
        ch.fill(.4f);
    y = x;
    const float *mi[]{x[0].data()};
    float *mo[]{x[0].data()};
    float *expected[]{y[0].data()};
    io.in = mi;
    io.out = mo;
    io.channels = 1;
    const auto monoSettings = controls(*mono);
    audio = true;
    mono->process(io);
    const auto block = reference.beginBlock(64);
    const auto status = reference.process(block, 0, 64, expected, nullptr, 0, monoSettings);
    audio = false;
    check(status == c::Status::Ok && x == y, "mono native Clean preserves exact engine sound");
}
void documentPipeline() {
    test::TempDirectory temp("clean_device", "project");
    const auto path = temp.path() / "song.adi";
    std::string error;
    auto doc = ui::ProjectDocument::open(path, true, device::withBuiltinDevices(), error);
    check(doc && doc->addMixFunction(mix::Function::Compressor, 2),
          "actual document adds compressor through shared prepared insertion");
    if (!doc)
        return;
    doc->tick(0);
    const auto publication = doc->parameterFeed().current();
    const auto found = std::find_if(publication->begin(), publication->end(), [](const auto &d) {
        return d.uid == "org.adidaw.mix.compressor";
    });
    check(found != publication->end() && found->available,
          "inserted compressor is realized and published");
    if (found == publication->end())
        return;
    const auto id = found->id;
    doc->setParameterDemand(doc.get(), {id});
    doc->tick(1);
    const auto *instance = doc->session().instanceFor(id);
    const double before = instance->getParam("0").normalized;
    using K = engine::ParamEventKind;
    check(doc->parameterGesture(id, "0", K::Begin, before) &&
              doc->parameterGesture(id, "0", K::Value, .25) &&
              doc->parameterGesture(id, "0", K::End, .25),
          "common Begin/Value/End edits native compressor");
    const bool undone = doc->ops().undo().ok && doc->synchronise();
    doc->tick(2);
    check(undone &&
              std::abs(doc->session().instanceFor(id)->getParam("0").normalized - before) < 1e-7,
          "one Undo restores the parameter gesture");
    check(doc->ops().redo().ok && doc->synchronise(), "parameter redo applies");
    doc->tick(3);
    check(!doc->addMixFunction(mix::Function::Compressor, 2),
          "one compressor per channel is enforced by shared insertion");
    const auto priorRow = detail::captureRow(doc->store().db(), "devices", "id", id);
    const auto priorState = doc->session().instanceFor(id)->saveState("state");
    const bool removed = doc->deviceAction("device.remove", {{"id", id}});
    check(removed && !detail::captureRow(doc->store().db(), "devices", "id", id),
          "native removal deletes the authored device row, not just a live publication");
    const bool restored = doc->ops().undo().ok && doc->synchronise();
    doc->tick(4);
    check(restored && detail::captureRow(doc->store().db(), "devices", "id", id) == priorRow &&
              doc->session().instanceFor(id) &&
              doc->session().instanceFor(id)->saveState("state") == priorState,
          "one Undo restores exact function membership, chain position and every authored setting");
    doc.reset();
    doc = ui::ProjectDocument::open(path, false, device::withBuiltinDevices(), error);
    instance = doc ? doc->session().instanceFor(id) : nullptr;
    check(instance && std::abs(instance->getParam("0").normalized - .25) < 1e-7,
          "project reopen restores authored compressor settings");
}
} // namespace
int main() {
    stateAndDescriptors();
    quietEnvelope();
    cleanGraphSuspension();
    thresholdEditWhileSuspended();
    for (double rate : {44100., 48000., 96000., 176400., 192000., 384000., 768000.}) {
        exactAudio<float>(rate);
        exactAudio<double>(rate);
    }
    dryOutsidePreparedStream<float>();
    dryOutsidePreparedStream<double>();
    automationAndRefusal();
    documentPipeline();
    check(allocations.load() == 0 && frees.load() == 0,
          "native audio callbacks allocate and free nothing");
    std::printf("Native Clean: %d checks, %d failures; callback allocations %u/%u\n", checks,
                failures, allocations.load(), frees.load());
    return failures ? 1 : 0;
}
