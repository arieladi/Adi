// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/ui/project_document.hpp"
#include "adi/mix/clip/Values.hpp"
#include "adi/store_rows.hpp"
#include "juce/builtin_host.hpp"
#include "juce/mix_function_host.hpp"
#include "juce/mix_pipeline_host.hpp"
#include "temp_directory.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
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
bool edit(ui::ProjectDocument& doc,const ui::DevicePanelData& data,const std::string& id,double value){
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
    const bool initial = edit(*doc, shown, "0", seededGain) &&
        (full ? edit(*doc, shown, std::to_string(21 + mix::clip::threshold), .75) : edit(*doc, shown, stage, 0));
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
    check(instance->getParam(stage).real == 0 && edit(*doc, shown, stage, 1),
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
    check(shown.parameterGroups.at("19").groupId=="limiter"&&shown.parameterGroups.at("19").groupEnableControl&&
          shown.parameterGroups.at("20").groupId=="clip"&&shown.parameterGroups.at("20").groupEnableControl,
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
    check(edit(*doc,shown,std::to_string(21+mix::clip::threshold),.875),"Clip threshold uses shared gesture path");
    check(edit(*doc,shown,"20",1),"Clip can turn on while Limiter stays on");
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
    check(edit(*doc,shown,"0",.1),"Limiter input edit has a history title");
    const auto limiterTitle=History(doc->store()).nextUndo()->label;
    check(edit(*doc,shown,std::to_string(21+mix::clip::input),.6),"Clip input edit has a history title");
    const auto clipTitle=History(doc->store()).nextUndo()->label;
    check(!limiterTitle.empty() && limiterTitle!=clipTitle && clipTitle.find("Clip")!=std::string::npos,
        "Clip and Limiter input gains have distinct page-aware Undo labels");
    doc->release();
    bypassedClaim(true);
    bypassedClaim(false);
    bypassedPinnedClaim(true);
    bypassedPinnedClaim(false);
    std::printf("pipeline document: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
