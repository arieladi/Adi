// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/ui/project_document.hpp"
#include "adi/ui/shared_analysis.hpp"
#include "adi/mix/clip/Values.hpp"
#include "adi/store_rows.hpp"
#include "juce/builtin_host.hpp"
#include "juce/device_host.hpp"
#include "juce/mix_function_host.hpp"
#include "juce/mix_pipeline_host.hpp"
#include "temp_directory.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <chrono>
#include <optional>
#include <utility>
#include <vector>

using namespace adi;
namespace {
int checks=0,failures=0;
void check(bool ok,const char* label){++checks;if(!ok){++failures;std::printf("FAIL %s\n",label);}}
struct Tone final:engine::Node {
    std::int64_t cursor=0;
    static float at(std::int64_t sample) noexcept { return static_cast<float>(.9 * std::sin(static_cast<double>(sample) * .071)); }
    void process(const engine::NodeIo& io)noexcept override {
        for(int i=0;i<io.frames;++i){const auto value=at(cursor++);
            for(int c=0;c<io.channels;++c)io.out[c][io.blockOffset+i]=value;}
    }
};
struct LimiterGraph {
    static constexpr int capacity = 256;
    struct Source final : engine::Node {
        std::int64_t samples = 0;
        bool audible = false;
        float level = 1.f;
        void process(const engine::NodeIo &io) noexcept override {
            for (int i = 0; i < io.frames; ++i) {
                const auto sample = static_cast<double>(samples++);
                for (int c = 0; c < io.channels; ++c)
                    io.out[c][i + io.blockOffset] = audible
                        ? level * static_cast<float>(.78 * std::sin(sample * .173 + c) + .22 * std::cos(sample * .49))
                        : 0.f;
            }
        }
    } source;
    std::unique_ptr<device::DeviceInstance> instance;
    device::DeviceNode node;
    engine::Graph graph;
    std::array<std::array<float, capacity>, 2> output{};
    std::array<float *, 2> planes{output[0].data(), output[1].data()};

    explicit LimiterGraph(std::unique_ptr<device::DeviceInstance> limiter) : instance(std::move(limiter)), node(*instance) {}
    bool prepare(double rate, bool continuous) {
        node.setAlwaysProcess(continuous);
        const auto generator = graph.addNode(source);
        const auto effect = graph.addNode(node);
        if (!graph.connect(generator, effect)) return false;
        graph.setOutput(effect);
        graph.prepare(rate, capacity);
        return graph.ok();
    }
    void render(int frames) noexcept {
        engine::AudioIo io;
        io.out = planes.data();
        io.numOut = static_cast<int>(planes.size());
        io.frames = frames;
        graph.process(io);
    }
};

void limiterGraphSilence(bool full) {
    rows::PluginRef reference;
    reference.format = "internal";
    reference.uid = "org.adidaw.mix.limiter";
    reference.name = "Limiter";
    engine::DeviceRequest request;
    request.ref = &reference;
    request.channels = 2;
    auto loader = device::withBuiltinDevices();
    if (full) loader = device::withFullMixFunctions(std::move(loader));
    std::string error;

    for (const double rate : {44100., 48000.})
        for (int style = 0; style < 8; ++style)
            for (const bool oversampling : {false, true})
                for (const bool truePeak : {false, true}) {
                    auto first = loader(request, error), second = loader(request, error);
                    check(first && second, "sleep fixture loads both native Limiter instances");
                    if (!first || !second) return;
                    LimiterGraph sleeping(std::move(first)), continuous(std::move(second));
                    bool applied = true;
                    for (auto *device : {sleeping.instance.get(), continuous.instance.get()}) {
                        applied = device->setParam("0", device::ParamValue::fromNormalized(.4)) && applied;
                        applied = device->setParam("1", device::ParamValue::fromNormalized(static_cast<double>(style) / 7.)) && applied;
                        applied = device->setParam("7", device::ParamValue::fromNormalized(oversampling ? .2 : 0.)) && applied;
                        applied = device->setParam("8", device::ParamValue::fromNormalized(truePeak ? 1. : 0.)) && applied;
                    }
                    check(applied && sleeping.prepare(rate, false) && continuous.prepare(rate, true),
                          "both graph paths prepare the same limiter settings");
                    if (!applied || !sleeping.graph.ok() || !continuous.graph.ok()) return;
                    bool exact = true;
                    const auto tick = [&](int frames) {
                        sleeping.render(frames);
                        continuous.render(frames);
                        const auto bytes = static_cast<std::size_t>(frames) * sizeof(float);
                        for (std::size_t c = 0; c < sleeping.output.size(); ++c)
                            exact = std::memcmp(sleeping.output[c].data(), continuous.output[c].data(), bytes) == 0 && exact;
                    };
                    for (int block = 0; block < 64; ++block) tick(LimiterGraph::capacity);
                    const auto before = sleeping.graph.stats().nodesSuspended;
                    for (const int frames : {1, 17, 63, 129, 251, 117, 23}) tick(frames);
                    const auto asleep = sleeping.graph.stats().nodesSuspended;
                    const bool supported = style != 2;
                    if (asleep != before + (supported ? 7u : 0u))
                        std::printf("Limiter sleep census %s %.0f Hz style %d OS %d TP %d skipped %llu\n",
                                    full ? "serial" : "direct", rate, style, oversampling, truePeak,
                                    static_cast<unsigned long long>(asleep - before));
                    check(asleep == before + (supported ? 7u : 0u),
                          "only certified prepared limiter paths sleep across the irregular segments");
                    check(continuous.graph.stats().nodesSuspended == 0, "the continuous test path runs every segment");
                    sleeping.node.setBypassed(true);
                    continuous.node.setBypassed(true);
                    tick(111);
                    sleeping.node.setBypassed(false);
                    continuous.node.setBypassed(false);
                    sleeping.source.audible = continuous.source.audible = true;
                    for (int block = 0; block < 24; ++block) tick(block % 2 == 0 ? 117 : 139);
                    sleeping.source.audible = continuous.source.audible = false;
                    for (int block = 0; block < 75; ++block) tick(LimiterGraph::capacity);
                    if (!exact)
                        std::printf("Limiter wake PCM %s %.0f Hz style %d OS %d TP %d\n",
                                    full ? "serial" : "direct", rate, style, oversampling, truePeak);
                    check(exact && sleeping.instance->latencySamples() == continuous.instance->latencySamples(),
                          "after odd-length sleep the rendered programme and its delay drain are byte-identical");
                }
}
void limiterGraphSilentEdits(bool full) {
    rows::PluginRef reference;
    reference.format = "internal";
    reference.uid = "org.adidaw.mix.limiter";
    engine::DeviceRequest request;
    request.ref = &reference;
    request.channels = 2;
    auto loader = device::withBuiltinDevices();
    if (full) loader = device::withFullMixFunctions(std::move(loader));
    std::string error;
    auto instance = loader(request, error);
    if (!instance) { check(false, "silent edit fixture loads the Limiter"); return; }
    LimiterGraph fixture(std::move(instance));
    if (!fixture.prepare(48000., false)) { check(false, "silent edit graph prepares"); return; }
    const auto render = [&] { fixture.render(129); };
    for (int i = 0; i < 64; ++i) render();
    const auto first = fixture.graph.stats().nodesSuspended;
    check(first != 0, "the default limiter sleeps before a parameter edit");
    fixture.instance->setOffline(true);
    render();
    check(fixture.graph.stats().nodesSuspended == first, "offline intent keeps a previously sleeping limiter awake");
    fixture.instance->setOffline(false);
    render();
    render();
    const auto resumed = fixture.graph.stats().nodesSuspended;
    check(resumed > first, "the unchanged limiter returns to suspension in realtime");

    check(fixture.instance->setParam("0", device::ParamValue::fromNormalized(.4)), "a sleeping limiter accepts an input gain edit");
    render();
    check(fixture.graph.stats().nodesSuspended == resumed, "the owner gain edit wakes the limiter while input is still zero");
    for (int i = 0; i < 64; ++i) render();
    const auto quiet = fixture.graph.stats().nodesSuspended;
    check(quiet > resumed, "after the gain edit settles the silent limiter can sleep again");
    check(fixture.instance->setParam("15", device::ParamValue::fromNormalized(.2)), "the sleeping limiter accepts dither");
    bool noise = false;
    for (int i = 0; i < 8; ++i) {
        render();
        noise = noise || std::any_of(fixture.output[0].begin(), fixture.output[0].begin() + 129,
                                   [](float sample) { return sample != 0.f; });
    }
    check(noise && fixture.graph.stats().nodesSuspended == quiet,
          "dither wakes the limiter and continues generating audio from silent input");
}
void limiterGraphWarmSleep() {
    rows::PluginRef reference;
    reference.format = "internal";
    reference.uid = "org.adidaw.mix.limiter";
    reference.name = "Limiter";
    engine::DeviceRequest request;
    request.ref = &reference;
    request.channels = 2;
    auto loader = device::withFullMixFunctions(device::withBuiltinDevices());
    std::string error;
    auto first = loader(request, error), second = loader(request, error);
    if (!first || !second) { check(false, "warm silence fixture loads both Limiters"); return; }
    LimiterGraph sleeping(std::move(first)), continuous(std::move(second));
    if (!sleeping.prepare(48000., false) || !continuous.prepare(48000., true)) {
        check(false, "warm silence fixture prepares both graphs");
        return;
    }
    bool exact = true, heard = false;
    const auto render = [&](int frames) {
        sleeping.render(frames);
        continuous.render(frames);
        const auto size = static_cast<std::size_t>(frames) * sizeof(float);
        for (std::size_t c = 0; c < sleeping.output.size(); ++c)
            exact = std::memcmp(sleeping.output[c].data(), continuous.output[c].data(), size) == 0 && exact;
        heard = heard || std::any_of(sleeping.output[0].begin(), sleeping.output[0].begin() + frames,
                                    [](float sample) { return sample != 0.f; });
    };
    sleeping.source.level = continuous.source.level = .25f;
    sleeping.source.audible = continuous.source.audible = true;
    for (int i = 0; i < 16; ++i) render(256);
    check(heard, "the prepared Transparent Limiter passes real programme before its late sleep");
    sleeping.source.audible = continuous.source.audible = false;
    for (int i = 0; i < 2250; ++i) render(256);
    const auto before = sleeping.graph.stats().nodesSuspended;
    for (const int frames : {17, 69, 65, 144, 256}) render(frames);
    check(sleeping.graph.stats().nodesSuspended == before + 5,
          "Transparent enters late sleep after its actual signal histories drain");
    sleeping.source.level = continuous.source.level = 1.4f;
    sleeping.source.audible = continuous.source.audible = true;
    for (int i = 0; i < 30; ++i) render(256);
    check(exact, "late sleep keeps the next programme byte-identical even when it drives gain reduction");
}
bool editParameter(ui::ProjectDocument& doc,const ui::DevicePanelData& data,const std::string& id,double value){
    const auto* instance=doc.session().instanceFor(data.id);if(!instance)return false;
    const auto before=instance->getParam(id).normalized;using K=engine::ParamEventKind;
    const bool accepted=doc.parameterGestureExpected(data.id,id,K::Begin,before,data.runtime,data.instance)&&
        doc.parameterGestureExpected(data.id,id,K::Value,value,data.runtime,data.instance)&&
        doc.parameterGestureExpected(data.id,id,K::End,value,data.runtime,data.instance);
    if(!accepted)std::printf("EDIT diagnostic: id=%s reason=%s\n",id.c_str(),doc.error().c_str());
    return accepted;
}
// Real ProjectDocument, DeviceHost, DeviceNode and graph. Observe compensated
// audio on each side of the retained device, without an audio driver or editor.
void bypassedClaim(bool full) {
    test::TempDirectory temp("mix_pipeline_document", full ? "bypass-full" : "bypass-lite");
    Tone tone;
    std::string error;
    auto loader = device::withBuiltinDevices();
    if (full) loader = device::withFullMixFunctions(std::move(loader));
    auto doc = ui::ProjectDocument::open(temp.path() / "song.adi", true, std::move(loader), error);
    if (!doc || !doc->addMixFunction(mix::Function::Limiter, 2)) {
        check(false, "bypass claim fixture adds the actual project Limiter");
        return;
    }
    constexpr int frames = 256;
    auto &session = doc->session();
    session.setSourcesFor([&](std::int64_t track) {
        return track == 2 ? std::vector<engine::Node*>{&tone} : std::vector<engine::Node*>{};
    });
    doc->prepare(48000, frames);
    session.devices().coalescer().setQuietPeriodMs(0);
    session.transport().play();
    std::int64_t now = 1;
    std::array<float, frames> left{}, right{};
    std::array<float*, 2> planes{left.data(), right.data()};
    engine::AudioIo io;
    io.out = planes.data();
    io.numOut = 2;
    io.frames = frames;
    const auto render = [&] {
        io.streamTimeSamples = now * frames;
        doc->process(io);
    };
    const auto cycle = [&](int count) {
        for (int i = 0; i < count; ++i) {
            render();
            doc->tick(++now);
        }
    };
    cycle(8);
    const auto publication = doc->parameterFeed().current();
    const auto row = std::find_if(publication->begin(), publication->end(), [](const auto &p) {
        return p.uid == "org.adidaw.mix.limiter";
    });
    if (row == publication->end()) {
        check(false, "bypass claim fixture exposes the real runtime");
        return;
    }
    const auto shown = *row;
    auto *instance = session.instanceFor(shown.id);
    const auto *entry = session.entryFor(shown.id);
    if (!instance || !entry) {
        check(false, "bypass claim fixture resolves the actual DeviceNode");
        return;
    }
    const auto hostIndex = entry->hostIndex;
    const auto isBypassed = [&] { return session.devices().nodeAt(hostIndex).bypassed(); };
    const auto trackInput = session.requestScope({{2, engine::ScopePoint::DeviceInput, shown.id}, 1.});
    const auto trackOutput = session.requestScope({{2, engine::ScopePoint::DeviceOutput, shown.id}, 1.});
    check(trackInput && trackOutput && !isBypassed(), "device input/output taps observe the active graph");
    // The two providers have different owner claims. Full enables Clip
    // against immutable serial compensation; Lite enables its actual Limiter.
    const std::string stage = full ? "20" : "19";
    constexpr double seededGain = .3;
    const bool initial = editParameter(*doc, shown, "0", seededGain) &&
        (full ? editParameter(*doc, shown, std::to_string(21 + mix::clip::threshold), .75) : editParameter(*doc, shown, stage, 0));
    check(initial, "prepare an audible effect and an initially disabled stage");
    if (!initial) return;
    cycle(16);
    // A first raw journal set with no existing mirror row has a null inverse.
    // Exercise a value-to-value Undo here, independently of that store behavior.
    const auto stored = rows::readModel(doc->store());
    const bool seeded = std::any_of(stored.pluginParams.begin(), stored.pluginParams.end(), [&](const auto &param) {
        return param.deviceId == shown.id && param.paramId == "0" && std::abs(param.normalized - seededGain) < .005;
    });
    check(seeded, "both native providers start the claim test with a durable Gain value");
    if (!seeded) return;
    const auto previousLatency = instance->latencySamples();
    check(instance->getParam(stage).real == 0 && editParameter(*doc, shown, stage, 1),
          "a real authored edit requests the latency-changing stage enable");
    // Document delivery may rebuild or prepare while publishing the new
    // latency. The separate one-prepare graph fixture isolates the live claim.
    render();
    doc->tick(++now);
    const auto offered = instance->latencySamples();
    check(offered > previousLatency, "the document advertises the latency of the authored stage before bypass");
    if (offered <= previousLatency) return;
    check(doc->deviceAction("device.setEnabled", {{"id", shown.id}, {"enabled", false}}) && isBypassed() &&
          session.instanceFor(shown.id) == instance,
          "graph activator bypass retains the native instance");
    cycle(16);

    const auto audioError = [&]() -> std::optional<double> {
        const auto input = trackInput.tap(), output = trackOutput.tap();
        if (!input || !output) return {};
        std::vector<float> inLeft(static_cast<std::size_t>(offered) + frames), inRight(inLeft.size());
        std::array<float, frames> outLeft{}, outRight{};
        std::int64_t firstIn = 0, firstOut = 0;
        if (!input->read(inLeft, inRight, firstIn) || !output->read(outLeft, outRight, firstOut) ||
            firstOut != firstIn) return {};
        // At the advertised compensated latency, the latest output starts
        // exactly with the input measured one device latency earlier.
        double maxError = 0.;
        for (std::size_t i = 0; i < outLeft.size(); ++i) {
            if (!std::isfinite(outLeft[i]) || !std::isfinite(outRight[i])) return {};
            maxError = std::max({maxError, std::abs(static_cast<double>(inLeft[i]) - outLeft[i]),
                                 std::abs(static_cast<double>(inRight[i]) - outRight[i])});
        }
        return maxError;
    };
    const auto dry = audioError();
    check(session.graph().currentGraph() && session.graph().currentGraph()->latencySamples() == offered,
          "the pinned graph retains the advertised delay while bypassed");
    check(dry && *dry < 2.e-5, "bypassed node actually outputs coherent compensated dry");

    // Exercise the analyser's production attachment alongside the direct probes.
    {
        using Analysis = ui::SharedAnalysis;
        using Point = engine::ScopePoint;
        using Tap = engine::ScopeTap;
        const auto &broker = doc->analysis();
        const auto idle = broker->stats();
        Analysis::Request scopeIn{2, Point::DeviceInput, frames, Analysis::Channel::Left};
        scopeIn.device = shown.id;
        scopeIn.outputs = Analysis::Scope;
        auto scopeOut = scopeIn;
        scopeOut.point = Point::DeviceOutput;
        auto inputView = broker->acquire(scopeIn);
        auto outputView = broker->acquire(scopeOut);
        auto spectrum = broker->acquireDeviceSpectrum({2, shown.id, frames, Analysis::DeviceSpectrumRequest::Channel::Left});
        for (int i = 0; i < 6; ++i) {
            cycle(1);
            broker->pump();
        }
        const auto input = inputView.read(), output = outputView.read();
        const auto stats = broker->stats();
        check(input && output && spectrum.read() == output && stats.taps == idle.taps + 2 &&
                  stats.domains == idle.domains + 2 && stats.spectralPlans == idle.spectralPlans + 1,
              "the Document's real device Scope and Spectrum share one output picture and spectral plan");

        const auto inputTap = trackInput.tap(), outputTap = trackOutput.tap();
        const bool valid = input && output && inputTap && outputTap &&
            output->scopeLeft.size() == static_cast<std::size_t>(frames) &&
            output->scopeRight.size() == static_cast<std::size_t>(frames);
        check(valid && input->request.source() == scopeIn.source() && output->request.source() == scopeOut.source() &&
                  input->generation == inputTap->generation() && output->generation == outputTap->generation() &&
                  input->advancing && output->advancing &&
                  input->transportRevision == session.transport().revision() &&
                  output->transportRevision == input->transportRevision &&
                  input->firstStamp - output->firstStamp == offered,
              "the real device's declared latency reaches both broker pictures as heard time");
        if (valid) {
            auto inCursor = inputTap->cursorAtEnd(), outCursor = outputTap->cursorAtEnd();
            inCursor.next = input->firstSequence - offered;
            outCursor.next = output->firstSequence;
            std::array<float, frames> inLeft{}, inRight{}, outLeft{}, outRight{};
            const auto heard = inputTap->read(inCursor, inLeft, inRight);
            const auto observed = outputTap->read(outCursor, outLeft, outRight);
            check(observed.status == Tap::ReadStatus::Ready && observed.frames == frames &&
                      observed.firstStamp == output->firstStamp &&
                      std::ranges::equal(outLeft, output->scopeLeft) && std::ranges::equal(outRight, output->scopeRight),
                  "the broker's output waveform retains the physical tap's samples and stamp");
            bool sameAtHeardTime = heard.status == Tap::ReadStatus::Ready && heard.frames == frames &&
                heard.firstStamp == output->firstStamp;
            bool heardTone = false;
            for (std::size_t i = 0; i < outLeft.size(); ++i) {
                heardTone = heardTone || std::abs(outLeft[i]) > .1f;
                sameAtHeardTime = sameAtHeardTime &&
                    std::abs(inLeft[i] - outLeft[i]) < 2.e-5f && std::abs(inRight[i] - outRight[i]) < 2.e-5f;
            }
            check(heardTone && sameAtHeardTime, "the bypass waveform and delayed input agree at the same heard sample");
        }
        spectrum.reset(); outputView.reset(); inputView.reset();
        check(broker->stats().taps == idle.taps && broker->stats().consumers == idle.consumers &&
                  trackInput.tap() == inputTap && trackOutput.tap() == outputTap,
              "closing the analyser releases its demand while the direct observers keep their physical taps");
    }

    const auto authored = [&](const std::string &param, double normalized, const char *label) {
        OpRequest op;
        op.opType = "device.setParam";
        op.payload = {{"dev", shown.id}, {"param", param}, {"norm", normalized}};
        op.label = label;
        return doc->ops().submit(std::move(op)).ok;
    };
    const auto ready = [&] {
        const auto &source = session.appliedNotifications();
        return session.presentationReady(source, source.sequence());
    };
    const double previousGain = instance->getParam("0").normalized;
    check(authored(stage, 0, "Change structure while graph-bypassed") &&
          authored("0", .45, "Gain behind structural edit"),
          "the store accepts the structural edit and its following gain edit");
    cycle(12);
    check(isBypassed() && ready() && instance->getParam(stage).real == 0 &&
          std::abs(instance->getParam("0").normalized - .45) < .005,
          "DF-M15 committed delivery must drain while the actual graph remains bypassed");
    check(doc->ops().undo().ok, "undo the gain edit while bypassed");
    cycle(4);
    check(isBypassed() && ready() && std::abs(instance->getParam("0").normalized - previousGain) < .005,
          "the first Undo restores gain without waiting for wet processing");
    check(doc->ops().undo().ok, "undo the structural edit while bypassed");
    cycle(4);
    check(isBypassed() && ready() && instance->getParam(stage).real == 1 &&
          session.instanceFor(shown.id) == instance,
          "the second Undo restores only the retained runtime's authored stage");
    check(doc->ops().undo().ok, "Undo graph bypass");
    cycle(24);
    const auto wet = audioError();
    check(!isBypassed() && ready() && instance->getParam(stage).real == 1 &&
          instance->latencySamples() == offered && session.graph().currentGraph()->latencySamples() == offered &&
          wet && *wet > 1.e-3,
          "Undo re-enables genuine wet output with the same compensated device latency");
    doc->release();
}

// Use the real native DeviceNode and its one prepared Graph. Retapping only
// replaces compensation, so an unrelated document rebuild cannot prepare
// away the outstanding native claim.
void bypassedPinnedClaim(bool full) {
    rows::PluginRef reference;
    reference.format = "internal";
    reference.uid = "org.adidaw.mix.limiter";
    reference.name = "L";
    engine::DeviceRequest request;
    request.ref = &reference;
    std::string error;
    auto instance = full ? device::makeFullMixFunction(request, error) : device::makeMixFunction(request, error);
    check(instance != nullptr, "pinned claim fixture assembles a real native Limiter");
    if (!instance) return;
    const std::string stage = full ? "20" : "19";
    const auto setStage = [&](double value) {
        return instance->setParam(stage, device::ParamValue::fromNormalized(value));
    };
    if (!full && !setStage(0)) {
        check(false, "Lite Limiter starts this test with its stage disabled");
        return;
    }

    constexpr int frames = 256;
    Tone tone;
    device::DeviceNode node(*instance);
    engine::Graph graph;
    const auto source = graph.addNode(tone);
    const auto limiter = graph.addNode(node);
    check(graph.connect(source, limiter), "the test tone drives the native graph node");
    graph.setOutput(limiter);
    graph.setLatencyHeadroom(8192);
    graph.prepare(48000., frames);
    check(graph.ok(), "the native-device graph prepares once");
    if (!graph.ok()) return;
    // Observe only after preparation, which revokes earlier leases.
    const auto reduction = mix::limiter::ReductionMailbox::observe(device::limiterReductionSource(*instance));

    std::array<float, frames> left{}, right{};
    std::array<float *, 2> planes{left.data(), right.data()};
    engine::AudioIo io;
    io.out = planes.data();
    io.numOut = 2;
    io.frames = frames;
    const auto render = [&] { ++io.renderBlock; graph.process(io); };
    render();
    const auto oldLatency = graph.latencySamples();
    const bool requested = instance->getParam(stage).real == 0 && setStage(1);
    check(requested, "the native stage starts an owner-authorized latency change");
    if (!requested) return;
    render();
    instance->pumpMainThread();
    const auto offered = instance->latencySamples();
    const bool claimed = offered > oldLatency && !setStage(0) && instance->getParam(stage).real == 1;
    check(claimed, "the old active graph leaves a verifiably claimed new latency");
    if (!claimed) return;
    const auto lastWet = reduction.read();
    check(lastWet && lastWet->renderBlock == io.renderBlock,
          "the active graph publishes reduction under the current observation epoch");

    node.setBypassed(true);
    render();
    const bool stillClaimed = graph.latencySamples() == oldLatency && !setStage(0);
    check(stillClaimed, "a bypassed callback pinned to old compensation does not release the new claim");
    if (!stillClaimed) return;
    const bool scheduled = graph.retapLatency();
    check(scheduled && graph.latencySamples() == oldLatency,
          "owner schedules the new graph compensation without preparing the native node again");
    if (!scheduled) return;
    for (int i = 0; i < 12; ++i) render();
    check(graph.latencySamples() == offered && node.bypassed(),
          "the bypassed graph physically admits the offered compensation");
    const auto held = reduction.read();
    check(lastWet && held && held->renderBlock == lastWet->renderBlock,
          "private bypass transition processing leaves the last audible reduction publication intact");
    const auto first = tone.cursor - frames - offered;
    double maxError = 0.;
    bool finite = true;
    for (std::size_t i = 0; i < left.size(); ++i) {
        const float expected = Tone::at(first + static_cast<std::int64_t>(i));
        finite = finite && std::isfinite(left[i]) && std::isfinite(right[i]);
        maxError = std::max({maxError, std::abs(static_cast<double>(left[i] - expected)),
                             std::abs(static_cast<double>(right[i] - expected))});
    }
    check(first >= 0 && finite && maxError < 2.e-5,
          "the native node still outputs the compensated dry tone under the new plan");
    check(setStage(0) && node.bypassed() && instance->getParam(stage).real == 0,
          "the real bypassed native node releases the structural claim under its new pinned compensation");
    graph.release();
}

void offlineInitialLatency(bool full) {
    rows::PluginRef reference;
    reference.format = "internal";
    reference.uid = "org.adidaw.mix.limiter";
    reference.name = "Limiter";
    engine::DeviceRequest request;
    request.ref = &reference;
    request.exporting = true;
    std::string error;
    auto instance = full ? device::makeFullMixFunction(request, error) : device::makeMixFunction(request, error);
    check(instance != nullptr, "offline latency fixture creates the registered device");
    if (!instance)
        return;
    if (full)
        check(instance->setParam("19", device::ParamValue::fromNormalized(0)) &&
              instance->setParam("20", device::ParamValue::fromNormalized(1)),
              "offline full provider starts with only the Clip stage at 8x");
    auto* native = instance.get();
    const auto initialEpoch = native->latencyEpoch();
    device::DeviceHost host;
    auto& node = host.add(std::move(instance), reference.name);
    Tone tone;
    engine::Graph graph;
    const auto output = graph.addNode(node);
    check(graph.connect(graph.addNode(tone), output), "offline tone drives the native device node");
    graph.setOutput(output);
    constexpr int frames = 256;
    graph.prepare(48000, frames);
    const auto initialDelay = native->latencySamples();
    check(graph.ok() && initialDelay > 0 && graph.latencySamples() == initialDelay &&
              (!full || initialDelay == 65),
          "first export graph includes the prepared delay before its first callback");
    host.attachGraph(graph);
    auto& coalescer = host.coalescer();
    coalescer.setQuietPeriodMs(0);
    host.tick(0);
    check(coalescer.stats().reports == 0 && native->latencyEpoch() == initialEpoch,
          "cold offline prepare does not report a change to the already prepared graph");
    const auto startedReports = coalescer.stats().reports;
    const auto preparedEpoch = native->latencyEpoch();

    std::array<float, frames> left{}, right{};
    std::array<float*, 2> planes{left.data(), right.data()};
    engine::AudioIo io;
    io.out = planes.data();
    io.numOut = 2;
    io.frames = frames;
    io.exporting = true;
    io.renderBlock = 1;
    graph.process(io);
    check(native->setParam(full ? "20" : "19", device::ParamValue::fromNormalized(0)),
          "owner can request a real change to the prepared delay");
    ++io.renderBlock;
    graph.process(io);
    host.tick(1);
    check(native->latencySamples() == 0 && native->latencyEpoch() == preparedEpoch + 1 &&
              coalescer.stats().reports == startedReports + 1,
          "a post-prepare delay change still notifies the graph and export guard once");
    graph.release();
}

}
int main(){
    test::TempDirectory temp("mix_pipeline_document","serial-pages");std::string error;
    auto doc=ui::ProjectDocument::open(temp.path()/"song.adi",true,
        device::withFullMixFunctions(device::withBuiltinDevices()),error);
    check(doc!=nullptr,"full native function document opens");if(!doc)return 2;
    check(doc->addMixFunction(mix::Function::Limiter,2),"add one L function through common prepared batch");
    doc->tick(0);const auto publication=doc->parameterFeed().current();
    const auto found=std::find_if(publication->begin(),publication->end(),[](const auto& p){return p.uid=="org.adidaw.mix.limiter";});
    check(found!=publication->end()&&found->available,"real full processing provider resolves");if(found==publication->end())return 2;
    auto shown=*found;auto* instance=doc->session().instanceFor(shown.id);
    doc->setParameterDemand(doc.get(), {shown.id});
    const auto requested=doc->parameterFeed().current();
    shown=*std::find_if(requested->begin(),requested->end(),[&](const auto& p){return p.id==shown.id;});
    check(instance&&instance->paramCount()==67,"full processing controls exist before first processing");if(!instance)return 2;
    check(instance->getParam("19").real==1&&instance->getParam("20").real==0,"Limiter starts on and Clip off");
    check(shown.groups().at("19").groupId=="limiter"&&shown.groups().at("19").groupEnableControl&&
          shown.groups().at("20").groupId=="clip"&&shown.groups().at("20").groupEnableControl,
          "real independent stage enables drive group metadata");
    Tone tone;doc->session().setSourcesFor([&](std::int64_t track){return track==2?std::vector<engine::Node*>{&tone}:std::vector<engine::Node*>{};});
    doc->prepare(48000,256);doc->tick(1);doc->session().transport().play();
    // Preparation/source replacement changes the presentation token. A real retained
    // editor receives the fresh publication before it can start another gesture.
    const auto prepared=doc->parameterFeed().current();
    const auto fresh=std::find_if(prepared->begin(),prepared->end(),[&](const auto& p){return p.id==shown.id;});
    check(fresh!=prepared->end()&&fresh->available,"prepared function publishes a fresh editable identity");
    if(fresh==prepared->end())return 2;
    shown=*fresh;instance=doc->session().instanceFor(shown.id);
    const auto limiterLatency=instance->latencySamples();
    check(editParameter(*doc,shown,std::to_string(21+mix::clip::threshold),.875),"Clip threshold uses shared gesture path");
    check(editParameter(*doc,shown,"20",1),"Clip can turn on while Limiter stays on");
    std::array<float,256> left{},right{};std::array<float*,2> pointers{left.data(),right.data()};
    engine::AudioIo io;io.out=pointers.data();io.numOut=2;io.frames=256;
    bool finite=true;
    for(int block=0;block<100;++block){io.streamTimeSamples=block*256;doc->process(io);doc->tick(block+1);
        finite=finite&&std::all_of(left.begin(),left.end(),[](float v){return std::isfinite(v);});}
    check(finite&&instance->getParam("19").real==1&&instance->getParam("20").real==1,"both enabled stages run through the real session");
    check(instance->latencySamples()>limiterLatency,"host observes summed Clip and Limiter latency");
    const auto bothState=instance->saveState("state");
    check(doc->ops().undo().ok&&doc->synchronise(),"one undo reverses Clip enable");doc->tick(101);
    check(instance->getParam("19").real==1&&instance->getParam("20").real==0,"undo changes only the chosen page enable");
    for(int block=0;block<20;++block){doc->process(io);doc->tick(102+block);}
    check(instance->latencySamples()==limiterLatency,"Clip off returns to exact Limiter-only latency");
    rows::PluginRef ref;ref.format="internal";ref.uid="org.adidaw.mix.limiter";ref.name="L";
    engine::DeviceRequest request;request.ref=&ref;request.exporting=true;
    auto offline=device::makeFullMixFunction(request,error);
    check(offline&&offline->loadState("state",bothState),"fresh export instance restores full stages and continuation");
    if(offline){
        check(offline->setParam("73",device::ParamValue::fromNormalized(1)),"explicit export quality flag sets");
        const auto online=offline->getParam("7").normalized;offline->prepare(48000,256);
        if(offline->latencySamples()<=limiterLatency)std::printf("EXPORT diagnostic: latency=%d baseline=%d error=%s\n",offline->latencySamples(),limiterLatency,error.c_str());
        check(offline->latencySamples()>limiterLatency&&offline->getParam("7").normalized==online,
              "export quality is prepared before audio and preserves online control");offline->release();
    }
    check(editParameter(*doc,shown,"0",.1),"Limiter input edit has a history title");
    const auto limiterTitle=History(doc->store()).nextUndo()->label;
    check(editParameter(*doc,shown,std::to_string(21+mix::clip::input),.6),"Clip input edit has a history title");
    const auto clipTitle=History(doc->store()).nextUndo()->label;
    check(!limiterTitle.empty() && limiterTitle!=clipTitle && clipTitle.find("Clip")!=std::string::npos,
        "Clip and Limiter input gains have distinct page-aware Undo labels");
    doc->release();
    bypassedClaim(true);
    bypassedClaim(false);
    bypassedPinnedClaim(true);
    bypassedPinnedClaim(false);
    offlineInitialLatency(true);
    offlineInitialLatency(false);
    limiterGraphSilence(true);
    limiterGraphSilence(false);
    limiterGraphSilentEdits(true);
    limiterGraphSilentEdits(false);
    limiterGraphWarmSleep();
    std::printf("pipeline document: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
