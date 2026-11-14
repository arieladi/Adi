// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/ui/project_document.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <stdexcept>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif
using namespace adi;
namespace {
int checks = 0, failures = 0;
void check(bool okay, const char *message) {
    ++checks;
    if (!okay) { ++failures; std::printf("FAIL %s\n", message); }
}
struct Probe {
    std::size_t counts = 0, descriptors = 0, values = 0, texts = 0, sets = 0;
    std::int32_t parameterCount = 730;
    bool available = true;
    std::array<double, 730> normalized{};
    Probe() { normalized.fill(.25); }
    void clearCounts() { counts = descriptors = values = texts = sets = 0; }
};
class CountedEq final : public device::DeviceInstance {
  public:
    explicit CountedEq(std::shared_ptr<Probe> probe) : probe_(std::move(probe)) {
        identity_ = {"internal", "org.adidaw.mix.eq", "Counted EQ", "ADI", "1"};
        for (std::size_t i = 0; i < descriptors_.size(); ++i) {
            auto &descriptor = descriptors_[i];
            descriptor.id = std::to_string(i); descriptor.name = "Control " + descriptor.id;
            descriptor.defaultValue = device::ParamValue::fromNormalized(.25);
            descriptor.automatable = i != 17;
            descriptor.groupId = descriptor.groupName = "eq";
            descriptor.bandIndex = i < 10 ? -1 : static_cast<std::int32_t>((i - 10) / 30);
            descriptor.sectionId = "basic"; descriptor.sectionName = "Basic";
            if (i == 3) { descriptor.shape = device::ParamShape::Menu; descriptor.stepCount = 3;
                descriptor.choiceLabels = {"Bell", "Shelf", "Cut"}; }
        }
    }
    const device::DeviceIdentity &identity() const noexcept override { return identity_; }
    bool loaded() const noexcept override { return probe_->available; }
    void process(const engine::NodeIo &io) noexcept override { device::passThrough(io); }
    std::int32_t latencySamples() const noexcept override { return 37; }
    std::int32_t paramCount() const noexcept override { ++probe_->counts; return probe_->parameterCount; }
    const device::ParamDescriptor *paramAt(std::int32_t index) const noexcept override {
        ++probe_->descriptors;
        return index >= 0 && index < probe_->parameterCount ? &descriptors_[static_cast<std::size_t>(index)] : nullptr;
    }
    device::ParamValue getParam(const std::string &id) const noexcept override {
        ++probe_->values;
        std::size_t index = 0;
        for (const auto digit : id) index = index * 10 + static_cast<std::size_t>(digit - '0');
        return index < 730 ? device::ParamValue::fromNormalized(probe_->normalized[index]) : device::ParamValue{};
    }
    bool setParam(const std::string &id, const device::ParamValue &value) override {
        ++probe_->sets;
        std::size_t index = 0;
        for (const auto digit : id) index = index * 10 + static_cast<std::size_t>(digit - '0');
        if (index >= 730 || index == 17) return false;
        probe_->normalized[index] = value.normalized;
        return true;
    }
    std::string paramText(const std::string &id, double value) const override {
        ++probe_->texts;
        return id + ":" + std::to_string(value);
    }
  private:
    device::DeviceIdentity identity_;
    std::shared_ptr<Probe> probe_;
    std::array<device::ParamDescriptor, 730> descriptors_;
};
}
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
#if defined(_MSC_VER) && defined(_DEBUG)
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    try {
        test::TempDirectory temporary("mix_parameter_feed", "demand-cache");
        std::map<std::int64_t, std::shared_ptr<Probe>> probes;
        engine::DeviceLoader loader = [&](const engine::DeviceRequest &request, std::string &error)
            -> std::unique_ptr<device::DeviceInstance> {
            if (!request.device || !request.ref || request.ref->uid != "org.adidaw.mix.eq") {
                error = "Not the counted parameter fixture"; return {};
            }
            auto probe = std::make_shared<Probe>(); probes[request.device->id] = probe;
            return std::make_unique<CountedEq>(std::move(probe));
        };
        std::string error;
        auto document = ui::ProjectDocument::open(temporary.path() / "song.adi", true, loader, error);
        check(document != nullptr, "counting document opens without an audio host");
        if (!document) return 2;
        auto &db = document->store().db();
        db.exec("INSERT INTO plugin_refs(id,format,uid,name) VALUES(3,'internal','org.adidaw.mix.eq','EQ')");
        for (int id = 10; id < 26; ++id) {
            const auto number = std::to_string(id);
            db.exec("INSERT INTO tracks(id,index_in_parent,kind,name) VALUES(" + number + "," + number + ",'audio','Channel')");
            db.exec("INSERT INTO device_chains(id,track_id) VALUES(" + number + "," + number + ")");
            db.exec("INSERT INTO devices(id,chain_id,plugin_ref_id,name) VALUES(" + number + "," + number + ",3,'EQ')");
        }
        document->view().refresh(document->store());
        check(document->synchronise() && probes.size() == 16, "sixteen independent EQ-shaped runtime fixtures are loaded");
        if (probes.size() != 16) return 1;
        const auto clearCounts = [&] { for (auto &[id, probe] : probes) { (void)id; probe->clearCounts(); } };
        const auto untouched = [&] {
            return std::all_of(probes.begin(), probes.end(), [](const auto &item) {
                const auto &probe = *item.second;
                return probe.counts == 0 && probe.descriptors == 0 && probe.values == 0 && probe.texts == 0 && probe.sets == 0;
            });
        };
        const auto panel = [&](std::int64_t id) -> const ui::DevicePanelData * {
            for (const auto &data : *document->parameterFeed().current()) if (data.id == id) return &data;
            return nullptr;
        };
        const auto requiredPanel = [&](std::size_t records, std::size_t declarations, const char *stage)
            -> const ui::DevicePanelData & {
            const auto *data = panel(10);
            if (!data || data->parameters().size() < records || data->declarations().size() < declarations) {
                std::printf("Feed diagnostic at %s: present=%d available=%d records=%zu declarations=%zu\n", stage,
                    data != nullptr, data && data->available, data ? data->parameters().size() : 0,
                    data ? data->declarations().size() : 0);
                throw std::runtime_error(stage);
            }
            return *data;
        };
        clearCounts();
        for (int tick = 0; tick < 12; ++tick) document->tick(tick);
        check(untouched(), "sixteen hidden EQs receive zero descriptor, value or text calls across real document ticks");
        check(panel(10) && requiredPanel(0, 0, "published identity").available && requiredPanel(0, 0, "published identity").latency == 37 && requiredPanel(0, 0, "published identity").name == "EQ" &&
            requiredPanel(0, 0, "published identity").parameters().empty() && requiredPanel(0, 0, "published identity").groups().empty(),
            "identity, actual latency and mixer metadata remain available without parameter materialization");
        int strip = 0, stack = 0;
        document->setParameterDemand(&strip, {10});
        const auto first = requiredPanel(730, 730, "first demand");
        check(first.metadata && first.cachedRecords && first.parameters().size() == 730 && first.declarations().size() == 730 &&
            first.declared.empty() && first.records.empty() && first.parameterGroups.empty(),
            "a demanded native EQ shares complete metadata and record storage instead of duplicate public tables");
        check(probes[10]->descriptors == 730 && probes[10]->texts == 730 && probes[11]->values == 0,
            "first demand resolves only its device's full descriptor and formatted-value set");
        check(!first.declarations().at(17).modifiable && first.parameters().at(3).choiceLabels.size() == 3 && first.resolved.entries.empty(),
            "read-only fields and named menus survive without an unused aggregate Configure list");
        clearCounts();
        for (int tick = 20; tick < 25; ++tick) document->tick(tick);
        check(requiredPanel(0, 0, "published identity").metadata == first.metadata && requiredPanel(0, 0, "published identity").cachedRecords == first.cachedRecords &&
            probes[10]->descriptors == 0 && probes[10]->texts == 0 && probes[10]->values == 5 * 730,
            "steady visible values reuse static metadata, immutable records and all unchanged text");
        probes[10]->normalized[2] = .6; document->tick(25);
        check(requiredPanel(0, 0, "published identity").metadata == first.metadata && requiredPanel(0, 0, "published identity").cachedRecords != first.cachedRecords &&
            probes[10]->texts == 1 && requiredPanel(0, 0, "published identity").parameters().at(2).playing == .6 && first.parameters().at(2).playing == .25,
            "one changed value formats once while retained publications remain immutable");
        document->setParameterDemand(&stack, {10, 11});
        document->setParameterDemand(&strip, {});
        clearCounts(); document->tick(26);
        check(probes[10]->values == 730 && probes[11]->values == 730 && probes[12]->values == 0,
            "releasing one consumer retains the overlapping second consumer's exact device union");
        document->setParameterDemand(&stack, {});
        clearCounts(); document->tick(27);
        check(untouched() && requiredPanel(0, 0, "published identity").parameters().empty() && !requiredPanel(0, 0, "published identity").metadata && !requiredPanel(0, 0, "published identity").cachedRecords,
            "last-consumer release removes all hidden parameter payloads and polling");
        probes[10]->normalized[2] = .8;
        document->setParameterDemand(&strip, {10});
        check(requiredPanel(0, 0, "published identity").parameters().at(2).playing == .8 && requiredPanel(0, 0, "published identity").metadata == first.metadata && probes[10]->descriptors == 0,
            "reveal reads current values immediately while reusing the exact runtime's descriptors");
        probes[10]->available = false; document->tick(28);
        const auto &unavailable = requiredPanel(18, 1, "availability loss");
        check(!unavailable.available && unavailable.parameters().at(17).missing && !unavailable.declarations().at(0).modifiable,
            "availability loss invalidates cached editability and marks the retained fields missing");
        probes[10]->available = true; document->tick(29);
        const auto &recovered = requiredPanel(18, 18, "availability recovery");
        check(recovered.available && !recovered.parameters().at(17).missing && recovered.declarations().at(0).modifiable &&
            !recovered.declarations().at(17).modifiable, "availability recovery restores writable and read-only distinctions");
        probes[10]->parameterCount = 729; document->tick(30);
        check(requiredPanel(0, 0, "published identity").parameters().size() == 729 && requiredPanel(0, 0, "published identity").declarations().size() == 729,
            "a native descriptor-count change rebuilds the cached parameter schema");
        probes[10]->parameterCount = 730; document->tick(31);
        check(requiredPanel(0, 0, "published identity").parameters().size() == 730, "restoring the native parameter schema repopulates its last field");
        db.exec("INSERT INTO plugin_params(device_id,param_id,normalized_value) VALUES(10,'2',0.3)");
        document->view().refresh(document->store());
        check(document->synchronise(), "a changed stored parameter revision synchronizes"); document->tick(32);
        check(requiredPanel(0, 0, "published identity").parameters().at(2).stored == .3, "stored-value index invalidates on the document model revision");
        const auto old = *panel(10);
        document->prepare(96000, 128); document->tick(33);
        (void)requiredPanel(730, 730, "runtime format generation");
        check(!document->session().resolveDevice(old.runtime) &&
            document->session().resolveDevice(document->session().deviceRuntimeToken(10)),
            "format change invalidates the old runtime independently of the publication cache");
        check(requiredPanel(0, 0, "published identity").instance != old.instance && requiredPanel(0, 0, "published identity").runtime.sameInstance(old.runtime) &&
            requiredPanel(0, 0, "published identity").metadata != old.metadata && !document->deviceTargetCurrent(10, old.runtime, old.instance),
            "cache and held targets invalidate for a new runtime generation with the same row, UID and SDK claim");
        const auto setsBeforeStale = probes[10]->sets;
        const auto valueBeforeStale = probes[10]->normalized[2];
        const auto operationsBeforeStale = db.execAndGet("SELECT count(*) FROM ops").getInt64();
        bool rejected = true;
        for (const auto kind : {engine::ParamEventKind::Begin, engine::ParamEventKind::Value, engine::ParamEventKind::End})
            rejected = !document->parameterGestureExpected(10, "2", kind, .9, old.runtime, old.instance) && rejected;
        check(rejected && probes[10]->sets == setsBeforeStale && probes[10]->normalized[2] == valueBeforeStale &&
            db.execAndGet("SELECT count(*) FROM ops").getInt64() == operationsBeforeStale,
            "a held target from the old runtime writes no value and creates no journal operation");
        db.exec("DELETE FROM devices WHERE id=10");
        document->view().refresh(document->store());
        check(document->synchronise(), "removing the demanded device synchronizes"); document->tick(34);
        check(!panel(10) && old.parameters().size() == 730, "removal drops current identity without mutating retained records");
        document->setParameterDemand(&strip, {});
        std::printf("parameter feed: %d checks, %d failures\n", checks, failures);
        return failures ? 1 : 0;
    } catch (const std::exception &exception) {
        std::printf("Parameter fixture exception: %s\n", exception.what());
        return 2;
    }
}
