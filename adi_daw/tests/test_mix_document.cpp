// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/ui/project_document.hpp"
#include "adi/builtin_catalog.hpp"
#include "juce/builtin_host.hpp"
#include "juce/mix_function_host.hpp"
#include "knobs_device.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <optional>
#include <string_view>
#include <utility>

using namespace adi;
namespace {
int checks=0,failures=0;
void check(bool ok,const char* label){++checks;if(!ok){++failures;std::printf("FAIL %s\n",label);}}
void observeLimiterReduction() {
    test::TempDirectory temp("mix_document", "limiter-reduction");
    std::string error;
    auto doc = ui::ProjectDocument::open(temp.path() / "project.adi", true, device::withBuiltinDevices(), error);
    if (!doc || !doc->addMixFunction(mix::Function::Limiter, 2)) {
        check(false, "insert a real limiter for reduction observation");
        return;
    }
    const auto limiter = [&]() -> std::optional<ui::DevicePanelData> {
        doc->tick(0);
        const auto publication = doc->parameterFeed().current();
        const auto found = std::find_if(publication->begin(), publication->end(),
            [](const auto& row) { return row.uid == "org.adidaw.mix.limiter"; });
        if (found != publication->end()) return *found;
        return std::nullopt;
    };
    const auto old = limiter();
    check(old && old->available, "reduction source device is available");
    if (!old || !old->available) return;
    auto* instance = doc->session().instanceFor(old->id);
    const auto source = instance ? device::limiterReductionSource(*instance) : nullptr;
    check(source && !source->observed(), "new native limiter has no telemetry demand");
    if (!source) return;
    const auto render = [](device::DeviceInstance& target, std::uint64_t block, int offset, int frames,
                           std::uint64_t revision = 1, bool exporting = false) {
        std::array<float, 256> left{}, right{};
        left.fill(1.5f); right.fill(-1.5f);
        std::array<const float*, 2> input{left.data(), right.data()};
        std::array<float*, 2> output{left.data(), right.data()};
        engine::TransportInfo clock;
        clock.playing = true;
        clock.timelineSample = 48000;
        clock.transportRevision = revision;
        engine::NodeIo io;
        io.in = input.data(); io.out = output.data();
        io.channels = 2;
        io.blockOffset = offset;
        io.frames = frames;
        io.transport = &clock;
        io.renderBlock = block;
        io.configuredLatency = target.latencySamples();
        io.exporting = exporting;
        target.process(io);
    };
    for (std::uint64_t block = 1; block <= 8; ++block) render(*instance, block, 0, 256);
    check(!source->observed(), "ordinary audio keeps the telemetry gate inactive before any viewer");

    auto first = doc->observeLimiterReduction(old->id, old->runtime);
    const auto epoch = source->observationEpoch();
    auto second = doc->observeLimiterReduction(old->id, old->runtime);
    check(first && second && epoch != 0 && epoch == source->observationEpoch() &&
          !first.read() && !second.read(), "two observers share a fresh capture without inherited audio");
    render(*instance, 9, 0, 96);
    const auto early = first.read();
    render(*instance, 9, 96, 160);
    const auto a = first.read(), b = second.read();
    check(early && a && b && early->observedFrames == 96 &&
          a->renderBlock == 9 && a->observedFrames == 256 && a->firstTimelineSample == 48000 &&
          a->advancing && a->transportRevision == 1 && a->reduction.available &&
          a->reduction.peakLoss > 0 && a->reduction.peakLoss >= early->reduction.peakLoss &&
          a->reduction.peakLoss == b->reduction.peakLoss && b->observedFrames == a->observedFrames,
          "both readers get the same actual peak accumulated across split callback segments");
    render(*instance, 9, 160, 96, 2);
    const auto discontinuity = second.read();
    check(discontinuity && discontinuity->observedFrames == 96 &&
          discontinuity->transportRevision == 2 && discontinuity->firstTimelineSample == 48160,
          "a same-callback transport discontinuity starts a fresh segment group");
    first.reset();
    render(*instance, 10, 0, 256);
    check(source->observationEpoch() == epoch && second.read() && second.read()->renderBlock == 10,
          "one hidden view leaves the remaining subscription active");
    second.reset();
    render(*instance, 11, 0, 256);
    check(!source->observed(), "the last hidden view turns off capture immediately");

    auto held = doc->observeLimiterReduction(old->id, old->runtime);
    check(held && source->observationEpoch() != epoch && !held.read(),
          "reopening after the last hide starts empty");
    if (a) source->publishSegment(epoch, *a);
    render(*instance, 12, 0, 256, 1, true);
    check(!held.read(), "in-flight old epochs and export callbacks cannot repopulate the live mailbox");
    render(*instance, 13, 0, 256);
    check(held.read() && held.read()->renderBlock == 13, "live source resumes after export without a stale reading");
    check(doc->deviceAction("device.setEnabled", {{"id", old->id}, {"enabled", false}}) &&
          !held && !held.read() && !source->observed() &&
          !doc->observeLimiterReduction(old->id, old->runtime),
          "graph bypass revokes held demand and refuses a fresh reader");
    check(doc->deviceAction("device.setEnabled", {{"id", old->id}, {"enabled", true}}),
          "graph bypass can be removed through the normal device route");
    const auto enabled = limiter();
    if (!enabled) return;
    held = doc->observeLimiterReduction(enabled->id, enabled->runtime);
    check(held && !held.read(), "enabling the graph device opens a fresh telemetry epoch");

    check(doc->deviceAction("device.remove", {{"id", old->id}}), "remove the observed limiter");
    check(!held && !held.read() && !source->observed() &&
          !doc->observeLimiterReduction(old->id, enabled->runtime),
          "retirement immediately revokes the held lease and refuses new readers");

    // Undo can restore the same device ID and retained runtime. A previously
    // revoked observer must still stay outside its new capture interval.
    check(doc->ops().undo().ok && doc->synchronise(), "Undo restores the retired limiter");
    const auto restored = limiter();
    auto* restoredInstance = doc->session().instanceFor(old->id);
    check(restored && restored->id == old->id && restoredInstance == instance,
          "Undo restores the original ID and runtime");
    if (!restored || !restoredInstance) return;
    auto revived = doc->observeLimiterReduction(restored->id, restored->runtime);
    check(revived && !revived.read() && !held && !held.read(),
          "the restored runtime admits only a fresh lease");
    render(*restoredInstance, 14, 0, 256);
    check(revived.read() && revived.read()->renderBlock == 14 && !held.read(),
          "only the fresh lease sees audio from the restored runtime");
    held.reset();
    check(source->observed() && revived.read(), "closing the stale lease leaves the restored runtime's fresh reader active");
    check(doc->ops().redo().ok && doc->synchronise() && !revived && !revived.read() && !source->observed(),
          "Redo retires the restored runtime and revokes its fresh reader");

    check(doc->addMixFunction(mix::Function::Compressor, 3), "add a genuinely different native function");
    doc->tick(0);
    const auto publication = doc->parameterFeed().current();
    const auto foreign = std::find_if(publication->begin(), publication->end(),
        [](const auto& row) { return row.uid == "org.adidaw.mix.compressor"; });
    check(foreign != publication->end() && foreign->id > old->id &&
          !foreign->runtime.sameInstance(old->runtime) && !revived && !revived.read() && !source->observed(),
          "a newly authored function uses an ID beyond the retained limiter");
    if (foreign == publication->end()) return;
    check(!doc->observeLimiterReduction(foreign->id, old->runtime) &&
          !doc->observeLimiterReduction(foreign->id, foreign->runtime),
          "the new device rejects both the retired token and non-limiter telemetry");
    check(doc->addMixFunction(mix::Function::Limiter, 3), "another native limiter can be observed independently");
    const auto replacement = limiter();
    check(replacement && !replacement->runtime.sameInstance(old->runtime),
          "the second limiter has a distinct runtime identity");
    if (!replacement) return;
    auto fresh = doc->observeLimiterReduction(replacement->id, replacement->runtime);
    revived.reset();
    auto* next = doc->session().instanceFor(replacement->id);
    check(fresh && !fresh.read() && next && next != instance,
          "the replacement owns an independent fresh subscription");
    if (!next) return;
    render(*next, 15, 0, 256);
    check(fresh.read() && fresh.read()->renderBlock == 15,
          "releasing a held stale lease leaves the replacement reading active");

    const auto secondSource = device::limiterReductionSource(*next);
    next->release();
    check(!fresh && !fresh.read() && secondSource && !secondSource->observed(),
          "driver release invalidates readers of the same retained runtime");
    next->prepare(48000, 256);
    auto rearmed = doc->observeLimiterReduction(replacement->id, replacement->runtime);
    fresh.reset();
    render(*next, 16, 0, 256);
    check(rearmed && rearmed.read() && rearmed.read()->renderBlock == 16,
          "reprepare requires a fresh lease and stale release cannot alter it");
    doc.reset();
    check(!rearmed && !rearmed.read() && secondSource && !secondSource->observed(),
          "closing the document revokes even an externally held view lease");
}


void replaceHighestFunction() {
    test::TempDirectory temp("mix_document", "removed-native-id");
    std::string error;
    auto doc = ui::ProjectDocument::open(temp.path() / "project.adi", true, device::withBuiltinDevices(), error);
    if (!doc || !doc->addMixFunction(mix::Function::Eq, 2)) {
        check(false, "add original EQ through the native function edit");
        return;
    }
    const auto find = [&](std::string_view uid) -> std::optional<ui::DevicePanelData> {
        const auto publication = doc->parameterFeed().current();
        for (const auto& device : *publication)
            if (device.uid == uid) return device;
        return {};
    };
    const auto eq = find("org.adidaw.mix.eq");
    check(eq && eq->available, "highest authored EQ is available in the shared device feed");
    if (!eq) return;
    auto* oldInstance = doc->session().instanceFor(eq->id);
    check(oldInstance && doc->deviceAction("device.remove", {{"id", eq->id}}) &&
              doc->addMixFunction(mix::Function::Limiter, 3), "remove the highest device and add the limiter on another track");
    const auto limiter = find("org.adidaw.mix.limiter");
    check(limiter && limiter->available && limiter->track == 3 && limiter->id > eq->id,
          "new authored device ID advances past the retired EQ");
    if (!limiter) return;
    doc->setParameterDemand(doc.get(), {limiter->id});
    auto* newInstance = doc->session().instanceFor(limiter->id);
    const auto panel = find("org.adidaw.mix.limiter");
    if (!panel) {
        check(false, "limiter panel remains available after requesting control metadata");
        return;
    }
    const auto group = panel->groups().find("0");
    check(newInstance && newInstance != oldInstance && newInstance->identity().uid == panel->uid &&
              group != panel->groups().end() && group->second.groupId == "limiter" && !panel->parameters().empty() &&
              std::none_of(panel->parameters().begin(), panel->parameters().end(), [](const auto& control) { return control.missing; }),
          "Device View and STACK share the new limiter's actual controls");
    check(!doc->deviceTargetCurrent(eq->id, eq->runtime, eq->instance),
          "removed EQ target cannot edit the new limiter");
    const bool undoAdd = doc->ops().undo().ok && doc->synchronise();
    const bool undoRemove = doc->ops().undo().ok && doc->synchronise();
    doc->tick(1);
    check(undoAdd && undoRemove && doc->session().instanceFor(eq->id) == oldInstance && find(eq->uid).has_value(),
          "Undo restores the original EQ runtime");
    const bool redoRemove = doc->ops().redo().ok && doc->synchronise();
    const bool redoAdd = doc->ops().redo().ok && doc->synchronise();
    doc->tick(2);
    check(redoRemove && redoAdd && doc->session().instanceFor(limiter->id) == newInstance && find(limiter->uid).has_value(),
          "Redo restores the same independently authored limiter runtime");
}
void replaceRetainedDeviceFromPicker() {
    test::TempDirectory temp("mix_document", "external-after-remove");
    std::string error, loadedPath;
    int externalLoads = 0;
    const auto fake = test::knobsLoader("cutoff", "vst3");
    auto loader = device::withBuiltinDevices([&](const engine::DeviceRequest& request, std::string& why) {
        ++externalLoads;
        if (request.ref) loadedPath = request.ref->pathHint;
        return fake(request, why);
    });
    auto doc = ui::ProjectDocument::open(temp.path() / "project.adi", true, loader, error);
    check(doc && doc->addMixFunction(mix::Function::Limiter, 2), "the picker fixture has a live native limiter");
    if (!doc) return;
    const auto find = [&](std::string_view uid) -> std::optional<ui::DevicePanelData> {
        const auto publication = doc->parameterFeed().current();
        for (const auto& row : *publication)
            if (row.uid == uid) return row;
        return {};
    };
    const auto previous = find("org.adidaw.mix.limiter");
    check(previous && previous->available, "the old limiter is realised before removal");
    if (!previous || !previous->available) return;
    auto* formerInstance = doc->session().instanceFor(previous->id);
    check(formerInstance && doc->deviceAction("device.remove", {{"id", previous->id}}),
          "remove the highest limiter while its runtime is retained for Undo");

    rows::PluginRef reference;
    reference.format = "vst3";
    reference.uid = test::kKnobsUid;
    reference.name = "External fixture";
    reference.pathHint = "fixture/fake external.vst3";
    reference.subtype = "effect";
    check(doc->insertPlugin(reference, 3), "the picker transaction can insert a fake VST3 after removal");
    const auto inserted = find(reference.uid);
    check(inserted && inserted->id > previous->id && inserted->track == 3 && inserted->plugin && inserted->available &&
              externalLoads == 1 && loadedPath == reference.pathHint,
          "the picker allocates above retained IDs and calls the external loader with the documented path");
    if (!inserted) return;
    auto* replacement = doc->session().instanceFor(inserted->id);
    doc->setParameterDemand(doc.get(), {inserted->id});
    const auto readback = find(reference.uid);
    check(replacement && replacement != formerInstance && dynamic_cast<test::Knobs*>(replacement) &&
              doc->session().resolveDevice(inserted->runtime) == replacement && readback &&
              readback->runtime.sameInstance(inserted->runtime) &&
              std::any_of(readback->parameters().begin(), readback->parameters().end(),
                          [](const auto& record) { return record.id == "cutoff" && !record.missing; }) &&
              !doc->deviceTargetCurrent(previous->id, previous->runtime, previous->instance),
          "the inserted row reads back the fake VST3 controls while an old limiter target remains revoked");
    check(doc->ops().undo().ok && doc->synchronise(), "one Undo removes the picker insertion");
    doc->tick(1);
    check(!find(reference.uid) && !find(previous->uid), "undoing the insertion leaves the earlier limiter removal in effect");
    check(doc->ops().redo().ok && doc->synchronise(), "one Redo restores the picker insertion");
    doc->tick(2);
    const auto redone = find(reference.uid);
    check(redone && redone->id == inserted->id && redone->available &&
              doc->session().resolveDevice(redone->runtime) == replacement && externalLoads == 1,
          "Redo restores the same external row and retained runtime");

    constexpr std::string_view utility = "org.adidaw.builtin.utility";
    if (const auto* info = findBuiltin(utility); info && info->available) {
        check(doc->deviceAction("device.remove", {{"id", inserted->id}}) && doc->insertBuiltin(std::string(utility), 3, std::nullopt),
              "the built-in picker can insert a utility after removing the external device");
        const auto builtIn = find(utility);
        check(builtIn && builtIn->id > inserted->id && builtIn->available && !builtIn->plugin &&
                  doc->session().resolveDevice(builtIn->runtime) != replacement,
              "the built-in insertion uses the same runtime ID reservation");
    }
}

constexpr std::string_view ordinaryUid = "test.adidaw.device-identity";
constexpr std::string_view compressorUid = "org.adidaw.mix.compressor";
constexpr std::string_view limiterUid = "org.adidaw.mix.limiter";
constexpr std::string_view utilityUid = "org.adidaw.builtin.utility";
constexpr std::int64_t ordinarySource = 10, nextInChain = 11;

class OrdinaryDevice final : public device::DeviceInstance {
  public:
    explicit OrdinaryDevice(const rows::PluginRef &ref)
        : identity_{ref.format, ref.uid, ref.name, ref.vendor, ref.version} {
        gain_.id = "420";
        gain_.name = "Fixture Gain";
    }
    const device::DeviceIdentity &identity() const noexcept override { return identity_; }
    bool loaded() const noexcept override { return true; }
    void process(const engine::NodeIo &io) noexcept override { device::passThrough(io); }
    std::int32_t paramCount() const noexcept override { return 1; }
    const device::ParamDescriptor *paramAt(std::int32_t index) const noexcept override {
        return index == 0 ? &gain_ : nullptr;
    }
    device::ParamValue getParam(const std::string &id) const noexcept override {
        return id == gain_.id ? gainValue_ : device::ParamValue{};
    }
    bool setParam(const std::string &id, const device::ParamValue &value) override {
        if (id != gain_.id)
            return false;
        gainValue_ = value;
        return true;
    }

  private:
    device::DeviceIdentity identity_;
    device::ParamDescriptor gain_;
    device::ParamValue gainValue_;
};

std::unique_ptr<ui::ProjectDocument> identityFixture(const std::filesystem::path &path) {
    StoreError error{};
    auto seed = Store::create(path, error);
    if (!seed)
        return {};
    seed->db().exec("INSERT INTO tracks(id,kind,name) VALUES "
                    "(1,'master','Main'),(2,'audio','Source'),(3,'audio','Destination');"
                    "INSERT INTO device_chains(id,track_id,ord,name) VALUES"
                    "(10,2,0,'Source devices'),(20,3,0,'Destination devices');"
                    "INSERT INTO plugin_refs(id,format,uid,name) "
                    "VALUES(90,'vst3','test.adidaw.device-identity','Fixture gain');"
                    "INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name) "
                    "VALUES(10,10,0,90,'Source gain'),(11,10,1,90,'Next gain');"
                    "INSERT INTO plugin_params(device_id,param_id,name,normalized_value) "
                    "VALUES(10,'420','Fixture Gain',0.375);");
    seed.reset();
    std::string documentError;
    return ui::ProjectDocument::open(path, false, device::withBuiltinDevices(
        [](const engine::DeviceRequest &request, std::string &) -> std::unique_ptr<device::DeviceInstance> {
            if (request.ref && request.ref->uid == ordinaryUid)
                return std::make_unique<OrdinaryDevice>(*request.ref);
            return {};
        }), documentError);
}

std::optional<ui::DevicePanelData> matchingPanel(ui::ProjectDocument &doc, std::string_view uid,
                                                std::int64_t track, std::int64_t id = 0) {
    const auto publication = doc.parameterFeed().current();
    const auto found = std::find_if(publication->begin(), publication->end(), [&](const auto &d) {
        return d.uid == uid && d.track == track && (!id || d.id == id);
    });
    return found == publication->end() ? std::nullopt : std::optional{*found};
}

bool containsDevice(ui::ProjectDocument &doc, std::int64_t id) {
    SQLite::Statement row(doc.store().db(), "SELECT 1 FROM devices WHERE id = ?");
    row.bind(1, id);
    return row.executeStep();
}

enum class DeviceCreation { Paste, Duplicate, BrowserBuiltin, BrowserPlugin, LegacyId };

void createAfterHighestRemoval(DeviceCreation creation, const char *caseName) {
    if (creation == DeviceCreation::BrowserBuiltin) {
        const auto *utility = findBuiltin(utilityUid);
        if (!utility || !utility->available) {
            std::printf("SKIP Browser identity on a build without the Utility host\n");
            return;
        }
    }
    std::printf("Device identity fixture %s\n", caseName);
    test::TempDirectory temp("mix_document", caseName);
    auto doc = identityFixture(temp.path() / "project.adi");
    if (!doc || !doc->addMixFunction(mix::Function::Limiter, 2) ||
        !doc->addMixFunction(mix::Function::Compressor, 3)) {
        check(false, "fixture creates source Limiter and highest-ID Compressor");
        return;
    }
    doc->tick(0);
    const auto original = matchingPanel(*doc, compressorUid, 3);
    const auto limiter = matchingPanel(*doc, limiterUid, 2);
    if (!original || !limiter || !original->available || !limiter->available ||
        original->id != doc->store().db().execAndGet("SELECT MAX(id) FROM devices").getInt64()) {
        check(false, "the available native Compressor is the highest authored device");
        return;
    }
    auto *oldInstance = doc->session().instanceFor(original->id);
    if (!oldInstance) {
        check(false, "highest ID has a realised native Compressor");
        return;
    }
    using Kind = engine::ParamEventKind;
    constexpr double threshold = .375;
    const auto edit = [&](Kind kind, double value) {
        return doc->parameterGestureExpected(original->id, "0", kind, value,
                                             original->runtime, original->instance);
    };
    if (!(edit(Kind::Begin, oldInstance->getParam("0").normalized) &&
          edit(Kind::Value, threshold) && edit(Kind::End, threshold))) {
        check(false, "fixture authors nondefault Compressor state through one gesture");
        return;
    }
    doc->tick(1);
    const auto oldState = oldInstance->saveState("state");
    check(!oldState.empty() && oldInstance->getParam("0").normalized == threshold,
          "the nondefault Compressor state is captured before retirement");
    const auto beforeRemoval = OpJournal(doc->store()).headSeq();
    if (!doc->deviceAction("device.remove", {{"id", original->id}}) || containsDevice(*doc, original->id)) {
        check(false, "removing the highest device retires its runtime and frees its stored ID");
        return;
    }
    const auto afterRemoval = OpJournal(doc->store()).headSeq();
    const bool sameChain = creation == DeviceCreation::Duplicate;
    const bool ordinary = sameChain || creation == DeviceCreation::BrowserPlugin || creation == DeviceCreation::LegacyId;
    const auto sourceId = ordinary ? ordinarySource : limiter->id;
    const std::int64_t track = sameChain ? 2 : 3;
    const auto expectedUid = ordinary ? ordinaryUid : creation == DeviceCreation::BrowserBuiltin ? utilityUid : limiterUid;
    bool created = false;
    if (creation == DeviceCreation::BrowserBuiltin) {
        created = doc->insertBuiltin(std::string(utilityUid), track, {});
    } else if (creation == DeviceCreation::BrowserPlugin) {
        rows::PluginRef reference;
        reference.format = "vst3";
        reference.uid = ordinaryUid;
        reference.name = "Fixture gain";
        reference.subtype = "effect";
        created = doc->insertPlugin(reference, track);
    } else {
        DeviceCopy copy;
        if (!doc->copyDevice(sourceId, copy)) {
            check(false, "fixture captures the source device through Copy");
            return;
        }
        if (creation == DeviceCreation::LegacyId) {
            // Explicitly model an older operation reusing a historic numeric ID.
            // This bypasses only the UI allocator and still commits normally.
            auto prepared = prepareDevicePasteForTrack(doc->store(), copy, original->id, track);
            const auto commit = doc->ops().commitPrepared(
                std::move(prepared), [](Store &store, PreparedDevicePaste &paste, Actor actor) {
                    return commitDevicePaste(store, paste, actor);
                });
            created = commit.ok && doc->synchronise();
        } else {
            // Duplicate uses the same copy/paste commands in its source chain.
            const auto before = sameChain ? std::optional{nextInChain} : std::nullopt;
            created = doc->pasteDevice(copy, track, before);
        }
    }
    if (!created) {
        check(false, "the requested device insertion commits after removal");
        return;
    }
    doc->tick(2);
    // Resolve the written ID independently of how the device feed is ordered.
    const auto id = doc->store().db().execAndGet("SELECT MAX(id) FROM devices").getInt64();
    check(creation == DeviceCreation::LegacyId ? id == original->id : id > original->id,
          "ordinary allocation advances past the retained ID, or the deliberate replay reuses it");
    doc->setParameterDemand(doc.get(), {id});
    const auto panel = matchingPanel(*doc, expectedUid, track, id);
    auto *instance = doc->session().instanceFor(id);
    check(panel && panel->id == id && panel->available && !panel->parameters().empty() &&
              std::none_of(panel->parameters().begin(), panel->parameters().end(),
                           [](const auto &control) { return control.missing; }) &&
              instance && instance != oldInstance && instance->identity().uid == expectedUid &&
              instance->identity().format == (ordinary ? "vst3" : "internal"),
          "the new row has its own loaded instance and usable controls");
    if (sameChain && instance) {
        SQLite::Statement order(doc->store().db(),
                                "SELECT id FROM devices WHERE id IN (?, ?) ORDER BY ord");
        order.bind(1, id);
        order.bind(2, nextInChain);
        check(order.executeStep() && order.getColumn(0).getInt64() == id &&
                  order.executeStep() && order.getColumn(0).getInt64() == nextInChain &&
                  instance->getParam("420").normalized == .375,
              "Duplicate preserves source controls and places the copy before its next neighbour");
    }
    check(!doc->deviceTargetCurrent(original->id, original->runtime, original->instance),
          "the retired Compressor panel cannot write to the new device");

    const auto undoNew = doc->ops().undo();
    check(undoNew.ok && doc->synchronise() && OpJournal(doc->store()).headSeq() == afterRemoval &&
              !containsDevice(*doc, id),
          "one Undo removes the whole new device edit");
    const auto undoRemoval = doc->ops().undo();
    const bool restored = undoRemoval.ok && doc->synchronise();
    doc->tick(3);
    check(restored && OpJournal(doc->store()).headSeq() == beforeRemoval &&
              containsDevice(*doc, original->id) && doc->session().instanceFor(original->id) == oldInstance &&
              oldInstance->saveState("state") == oldState,
          "Undo Remove revives the original Compressor instance and exact state");
    const bool redone = doc->ops().redo().ok && doc->synchronise() &&
                        doc->ops().redo().ok && doc->synchronise();
    doc->tick(4);
    check(redone && instance && instance != oldInstance && doc->session().instanceFor(id) == instance &&
              instance->identity().uid == expectedUid,
          "Redo restores the separately authored device instance");
}

void replaceIdentityInOneJournalStep() {
    test::TempDirectory temp("mix_document", "legacy-atomic-device-replacement");
    auto doc = identityFixture(temp.path() / "project.adi");
    if (!doc || !doc->addMixFunction(mix::Function::Compressor, 3)) {
        check(false, "the legacy replacement fixture has a native Compressor");
        return;
    }
    doc->tick(0);
    const auto original = matchingPanel(*doc, compressorUid, 3);
    const auto* source = doc->session().entryFor(ordinarySource);
    auto* compressor = original ? doc->session().resolveDevice(original->runtime) : nullptr;
    if (!compressor || !source || !source->resolvedPluginRef) {
        check(false, "the original Compressor and the incompatible reference are realised");
        return;
    }
    const auto id = original->id;
    const auto state = compressor->saveState("state");
    const auto& devices = doc->session().model().devices;
    const auto row = std::find_if(devices.begin(), devices.end(), [id](const auto& device) { return device.id == id; });
    if (row == devices.end() || state.empty()) {
        check(false, "the fixture can capture the Compressor's authored position and state");
        return;
    }
    const auto before = OpJournal(doc->store()).headSeq();

    OpRequest remove;
    remove.opType = "device.remove";
    remove.payload = {{"id", id}};
    remove.label = "Legacy device replacement";
    remove.targetKind = "device";
    remove.targetId = id;
    OpRequest insert = remove;
    insert.opType = "device.insert";
    insert.payload = {{"id", id}, {"chain", row->chainId}, {"ord", row->ord}, {"ref", *source->resolvedPluginRef},
                      {"name", "Replaced gain"}};
    // Replay older authored operations as one edit. The session never sees a
    // generation with this numeric device ID missing.
    const auto replaced = doc->ops().submit({remove, insert});
    if (!replaced.ok || !doc->synchronise()) {
        check(false, "one journal group replaces the row using its existing numeric ID");
        return;
    }
    doc->tick(1);
    const auto replacement = matchingPanel(*doc, ordinaryUid, 3, id);
    auto* gain = doc->session().instanceFor(id);
    check(replacement && replacement->available && gain && gain != compressor &&
              doc->session().resolveDevice(replacement->runtime) == gain && gain->identity().uid == ordinaryUid &&
              !doc->deviceTargetCurrent(id, original->runtime, original->instance),
          "the changed row displaces the still-active Compressor with the correct runtime");

    const bool undone = doc->ops().undo().ok && doc->synchronise();
    doc->tick(2);
    check(undone && OpJournal(doc->store()).headSeq() == before && doc->session().instanceFor(id) == compressor &&
              compressor->saveState("state") == state,
          "one Undo restores the retained Compressor and its state at the same ID");
    const bool redone = doc->ops().redo().ok && doc->synchronise();
    doc->tick(3);
    const bool restored = redone && gain && gain != compressor && doc->session().instanceFor(id) == gain &&
                          gain->identity().uid == ordinaryUid;
    check(restored, "one Redo restores the retained replacement at the same ID");
    if (!restored)
        return;
    const auto active = matchingPanel(*doc, ordinaryUid, 3, id);
    if (!active || !gain->hasParamSink()) {
        check(false, "the current generation is published with its parameter capture attached");
        return;
    }

    OpRequest rename;
    rename.opType = "track.rename";
    rename.label = "Rename source track";
    rename.payload = {{"id", 2}, {"name", "Other source name"}};
    if (!doc->ops().submit(rename).ok) {
        check(false, "the unrelated edit publishes a new document generation");
        return;
    }
    {
        // Hold a real capture reader as the owner synchronises the already
        // committed edit. Detaching a valid binding now cannot reattach it.
        const auto reading = gain->beginStateSignal();
        check(!gain->captureCallbacksQuiescent(), "the current generation has an in-flight capture reader");
        check(doc->synchronise(), "the document realises the unrelated edit while capture is reading");
        check(gain->hasParamSink(),
              "an unrelated publication keeps the current capture attached despite an older retired entry at the same ID");
    }
    doc->tick(4);
    const auto beforeGesture = OpJournal(doc->store()).headSeq();
    const auto oldValue = gain->getParam("420").normalized;
    const auto edit = [&](engine::ParamEventKind kind, double value) {
        return doc->parameterGestureExpected(id, "420", kind, value, active->runtime, active->instance);
    };
    using Kind = engine::ParamEventKind;
    constexpr double newValue = .625;
    check(edit(Kind::Begin, oldValue) && edit(Kind::Value, newValue) && edit(Kind::End, newValue) &&
              gain->getParam("420").normalized == newValue && OpJournal(doc->store()).headSeq() > beforeGesture,
          "the current surface can author a complete gesture after the unrelated edit");
    check(doc->ops().undo().ok && doc->synchronise() && OpJournal(doc->store()).headSeq() == beforeGesture &&
              gain->getParam("420").normalized == oldValue,
          "one Undo restores the current generation's gesture without undoing the unrelated edit");
}
}
int main() {
    replaceHighestFunction();
    observeLimiterReduction();
    replaceRetainedDeviceFromPicker();
    createAfterHighestRemoval(DeviceCreation::Paste, "paste-after-removal");
    createAfterHighestRemoval(DeviceCreation::Duplicate, "duplicate-after-removal");
    createAfterHighestRemoval(DeviceCreation::BrowserBuiltin, "browser-builtin-after-removal");
    createAfterHighestRemoval(DeviceCreation::BrowserPlugin, "browser-plugin-after-removal");
    createAfterHighestRemoval(DeviceCreation::LegacyId, "legacy-device-identity");
    replaceIdentityInOneJournalStep();
    test::TempDirectory temp("mix_document","native-functions");std::string error;
    auto doc=ui::ProjectDocument::open(temp.path()/"project.adi",true,device::withBuiltinDevices(),error);
    check(doc!=nullptr,"native function document opens");if(!doc)return 2;
    auto count=[&] {return doc->store().db().execAndGet(
        "SELECT count(*) FROM devices d JOIN plugin_refs p ON p.id=d.plugin_ref_id WHERE p.uid LIKE 'org.adidaw.mix.%'").getInt();};
    check(count()==0,"fresh project has no reserved or silently enabled functions");
    check(!doc->addMixFunction(mix::Function::Deesser,2)&&count()==0,"unported core has an honest refusal");
    check(doc->addMixFunction(mix::Function::Limiter,2)&&count()==1,"opt-in real native limiter adds in one batch");
    doc->tick(0);
    const auto publication=doc->parameterFeed().current();
    auto found=std::find_if(publication->begin(),publication->end(),[](const auto& d){return d.uid=="org.adidaw.mix.limiter";});
    check(found!=publication->end()&&found->available&&!found->plugin,"native function published without external plugin disguise");
    if(found==publication->end())return 2;
    const auto shown=*found;auto* instance=doc->session().instanceFor(shown.id);
    check(instance&&instance->paramCount()==21&&shown.latency==instance->latencySamples(),"real controls and actual latency use common feed");
    check(!doc->addMixFunction(mix::Function::Limiter,2)&&count()==1,"duplicate + action refused");
    using K=engine::ParamEventKind;
    check(doc->parameterGestureExpected(shown.id,"0",K::Begin,0,shown.runtime,shown.instance)&&
          doc->parameterGestureExpected(shown.id,"0",K::Value,.2,shown.runtime,shown.instance)&&
          doc->parameterGestureExpected(shown.id,"0",K::End,.2,shown.runtime,shown.instance),"native parameter uses exact-instance common gesture");
    check(instance->getParam("0").real==6,"accepted value reaches reused core controls");
    const auto storedInput=[&]() -> std::optional<double> {
        SQLite::Statement row(doc->store().db(),"SELECT normalized_value FROM plugin_params WHERE device_id=? AND param_id='0'");
        row.bind(1,shown.id);return row.executeStep()?std::optional<double>{row.getColumn(0).getDouble()}:std::nullopt;
    };
    const auto undone=doc->ops().undo(); const bool undoSync=doc->synchronise();
    check(undone.ok&&undoSync&&!storedInput(),"undo immediately restores the original parameter-row presence");
    // Applied receipts deliberately queue until the document's normal drain. The graph/SQL
    // refresh is not a second setter authority for devices that were already loaded.
    doc->tick(1);
    const auto undoValue=doc->session().instanceFor(shown.id)->getParam("0").real;
    if(!undone.ok||!undoSync||undoValue!=0)std::printf("UNDO diagnostic: ok=%d sync=%d real=%.17g history=%s document=%s applied=%lld state_applied=%lld state_equal=%lld state_refused=%lld\n",
        undone.ok,undoSync,undoValue,undone.error.c_str(),doc->error().c_str(),
        static_cast<long long>(doc->parameterStats().applied),static_cast<long long>(doc->parameterStats().statesApplied),
        static_cast<long long>(doc->parameterStats().statesAppliedEqual),static_cast<long long>(doc->parameterStats().statesRefused));
    check(undone.ok&&undoSync&&undoValue==0,"one undo restores gesture start");
    const auto redone=doc->ops().redo();const bool redoSync=doc->synchronise();
    check(redone.ok&&redoSync&&storedInput()&&*storedInput()==.2,"redo immediately restores the committed normalized row");
    doc->tick(2);
    check(redone.ok&&redoSync&&doc->session().instanceFor(shown.id)->getParam("0").real==6,"redo restores full gesture");
    check(doc->parameterGestureExpected(shown.id,"73",K::Begin,0,shown.runtime,shown.instance)&&
          doc->parameterGestureExpected(shown.id,"73",K::Value,1,shown.runtime,shown.instance)&&
          doc->parameterGestureExpected(shown.id,"73",K::End,1,shown.runtime,shown.instance),
          "nonautomatable export policy uses the exact-instance common configuration gesture");
    check(instance->getParam("73").real==1,"configuration reaches native runtime exactly");
    const auto exportUndone=doc->ops().undo();const bool exportUndoSync=doc->synchronise();doc->tick(3);
    check(exportUndone.ok&&exportUndoSync&&doc->session().instanceFor(shown.id)->getParam("73").real==0,
          "one undo restores export configuration default through normal receipt drain");
    const auto exportRedone=doc->ops().redo();const bool exportRedoSync=doc->synchronise();doc->tick(4);
    check(exportRedone.ok&&exportRedoSync&&doc->session().instanceFor(shown.id)->getParam("73").real==1,
          "redo restores export configuration without an automation override");
    DeviceCopy copy;check(doc->copyDevice(shown.id,copy),"native function copy retains normal stored state");
    check(!doc->pasteDevice(copy,2,{})&&count()==1,"clipboard cannot evade one-function-per-channel rule");
    const bool pasted=doc->pasteDevice(copy,3,{});
    if(!pasted)std::printf("PASTE diagnostic: %s\n",doc->error().c_str());
    check(pasted&&count()==2,"copy to another channel creates independent function");
    check(doc->deviceAction("device.remove",{{"id",shown.id}})&&count()==1,"native off removes authored function");
    check(doc->ops().undo().ok&&doc->synchronise()&&count()==2,"undo removal restores function and state");
    check(!doc->parameterGestureExpected(shown.id,"0",K::Value,.9,shown.runtime,shown.instance),"old retained editor cannot address restored runtime");
    doc->tick(1);
    doc->views().save("analyzer",[&]{ui::WindowState s;s.mixViewTab=5;return s;}(),error);
    check(doc->views().load("analyzer").mixViewTab==5,"new analyzer tab persists without remapping old tabs");
    doc.reset();
    auto reopened=ui::ProjectDocument::open(temp.path()/"project.adi",false,device::withBuiltinDevices(),error);
    check(reopened!=nullptr,"configured native project reopens");
    if(reopened) {
        reopened->tick(0);auto* restored=reopened->session().instanceFor(shown.id);
        check(restored&&restored->getParam("73").real==1,"export configuration survives project close and reopen");
    }
    std::printf("mix document: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
