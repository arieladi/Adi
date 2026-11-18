// SPDX-License-Identifier: GPL-3.0-or-later
#include "parameter_feed.hpp"
#include <algorithm>
#include <cmath>
#include <tuple>
#include <unordered_set>
namespace adi::ui {
namespace {
bool samePanel(const DevicePanelData &a, const DevicePanelData &b) {
    const auto card = [](const DevicePanelData &d) {
        // The instance stamp includes the preparation shape and token epoch.
        return std::tie(d.id, d.track, d.chain, d.instance, d.available, d.latency,
                        d.sidechainChannels, d.name, d.enabled, d.instrument, d.plugin, d.uid,
                        d.metadata, d.cachedRecords, d.defaults, d.parameterGroups, d.processingIssue,
                        d.resolved.configured, d.resolved.showsConfigureHint, d.resolved.modifiableCount);
    };
    return card(a) == card(b) &&
           std::equal(a.resolved.entries.begin(), a.resolved.entries.end(),
                      b.resolved.entries.begin(), b.resolved.entries.end(),
                      [](const panel::Entry &x, const panel::Entry &y) {
                          return std::tie(x.id, x.missing) == std::tie(y.id, y.missing);
                      }) &&
           std::equal(a.declared.begin(), a.declared.end(), b.declared.begin(), b.declared.end(),
                      [](const panel::Declared &x, const panel::Declared &y) {
                          return std::tie(x.id, x.name, x.modifiable, x.automatable) ==
                                 std::tie(y.id, y.name, y.modifiable, y.automatable);
                      }) &&
           std::equal(a.records.begin(), a.records.end(), b.records.begin(), b.records.end(),
                      [](const panel::Record &x, const panel::Record &y) {
                          const auto record = [](const panel::Record &p) {
                              return std::tie(p.id, p.name, p.stored, p.playing, p.driven, p.text,
                                              p.shape, p.stepCount, p.automated, p.overridden, p.missing,
                                              p.lastTouchedBy, p.choiceLabels);
                          };
                          return record(x) == record(y);
                      });
}
}

bool ParameterFeed::setDemand(const void *consumer, std::vector<std::int64_t> devices) {
    std::sort(devices.begin(), devices.end());
    devices.erase(std::unique(devices.begin(), devices.end()), devices.end());
    if (devices.empty()) {
        if (!demands_.erase(consumer)) return false;
    } else {
        const auto found = demands_.find(consumer);
        if (found != demands_.end() && found->second == devices) return false;
        demands_[consumer] = std::move(devices);
    }
    std::set<std::int64_t> next;
    for (const auto &[owner, ids] : demands_) {
        (void)owner;
        next.insert(ids.begin(), ids.end());
    }
    if (next == demanded_) return false;
    demanded_ = std::move(next);
    return true;
}
void ParameterFeed::publish(engine::Session &session, std::uint64_t modelRevision) {
    auto next = std::make_shared<ParameterPublication>();
    const auto publication = ++publication_;
    const auto &model = session.model();
    if (!demanded_.empty() && savedRevision_ != modelRevision) {
        saved_.clear();
        for (const auto &saved : model.pluginParams) saved_[{saved.deviceId, saved.paramId}] = saved.normalized;
        savedRevision_ = modelRevision;
    }
    for (const auto &chain : model.deviceChains) {
        if (!chain.trackId) continue;
        for (const auto &row : model.devices) {
            if (row.chainId != chain.id) continue;
            DevicePanelData data;
            data.id = row.id; data.track = *chain.trackId; data.chain = chain.id;
            data.name = row.name; data.enabled = row.enabled;
            if (row.pluginRefId)
                for (const auto &ref : model.pluginRefs)
                    if (ref.id == *row.pluginRefId) {
                        data.instrument = ref.subtype == "instrument";
                        data.uid = ref.uid; data.plugin = ref.format != "internal";
                        break;
                    }
            auto *instance = session.instanceFor(row.id);
            const auto *entry = session.entryFor(row.id);
            data.available = instance && instance->loaded() && entry && !entry->retired && !entry->parked &&
                             entry->resolvedPluginRef == row.pluginRefId && entry->resolvedUid == data.uid;
            data.latency = data.available ? instance->latencySamples() : 0;
            auto &identity = identities_[row.id];
            const bool runtimeSame = instance != nullptr && session.resolveDevice(identity.runtime) == instance;
            const bool unpublished = !entry && identity.runtime.claimIdentity().expired();
            if (identity.stamp == 0 || identity.pluginRef != row.pluginRefId || identity.uid != data.uid ||
                (!runtimeSame && !unpublished)) {
                identity.stamp = ++nextIdentity_;
                identity.runtime = session.deviceRuntimeToken(row.id);
                identity.pluginRef = row.pluginRefId; identity.uid = data.uid;
                native_.erase(row.id);
            }
            data.runtime = identity.runtime;
            data.instance = identity.stamp; identity.seen = publication;
            // Mixer and selection identity stays available without touching parameters.
            if (!demanded_.contains(row.id)) { next->push_back(std::move(data)); continue; }
            const auto readValues = [&](const device::ParamDescriptor &descriptor,
                                        const std::optional<std::string> &ref, panel::Record &record) {
                record.missing = descriptor.missing || !data.available;
                record.playing = instance->getParam(descriptor.id).normalized;
                const auto saved = saved_.find({row.id, descriptor.id});
                record.stored = saved == saved_.end() ? record.playing : saved->second;
                if (record.missing) record.playing = record.stored;
                record.driven = !record.missing && std::abs(record.playing - record.stored) > 1e-9;
                record.automated = record.overridden = false;
                const auto *lanes = session.deviceAutomation();
                if (descriptor.automatable && lanes && ref) {
                    const auto lane = lanes->laneFor.find({row.id, *ref});
                    if (lane != lanes->laneFor.end()) {
                        record.automated = true;
                        record.overridden = session.overriddenLanes().contains(lane->second);
                    }
                }
                const auto source = touched_.find({row.id, descriptor.id});
                record.lastTouchedBy = source == touched_.end() ? panel::Source::None : source->second;
                if (record.automated && !record.overridden) record.lastTouchedBy = panel::Source::Automation;
            };
            const bool nativeEq = instance && !data.plugin && data.uid == "org.adidaw.mix.eq";
            if (nativeEq) {
                auto &cache = native_[row.id];
                const auto count = instance->paramCount();
                if (!cache.metadata || cache.instance != data.instance || cache.available != data.available ||
                    cache.parameterCount != count) {
                    cache = {};
                    cache.instance = data.instance; cache.available = data.available; cache.parameterCount = count;
                    auto metadata = std::make_shared<ParameterMetadata>();
                    for (std::int32_t i = 0; i < count; ++i) {
                        const auto *descriptor = instance->paramAt(i);
                        if (!descriptor) continue;
                        cache.descriptors.push_back(*descriptor);
                        cache.automationRefs.push_back(engine::automationParamRef(instance->identity().format, descriptor->id));
                        metadata->declared.push_back({descriptor->id, descriptor->name,
                            (descriptor->automatable || descriptor->configurable) && !descriptor->missing && data.available,
                            descriptor->automatable});
                        metadata->defaults[descriptor->id] = descriptor->defaultValue.normalized;
                        if (!descriptor->groupId.empty())
                            metadata->groups[descriptor->id] = {descriptor->groupId, descriptor->groupName,
                                descriptor->groupEnableControl, descriptor->bandIndex, descriptor->sectionId, descriptor->sectionName};
                    }
                    cache.metadata = std::move(metadata);
                }
                std::shared_ptr<std::vector<panel::Record>> changed;
                for (std::size_t i = 0; i < cache.descriptors.size(); ++i) {
                    const auto &descriptor = cache.descriptors[i];
                    panel::Record values;
                    readValues(descriptor, cache.automationRefs[i], values);
                    const auto *prior = cache.records ? &(*cache.records)[i] : nullptr;
                    if (prior && prior->playing == values.playing && prior->stored == values.stored &&
                        prior->missing == values.missing && prior->automated == values.automated &&
                        prior->overridden == values.overridden && prior->lastTouchedBy == values.lastTouchedBy) continue;
                    if (!changed)
                        changed = cache.records ? std::make_shared<std::vector<panel::Record>>(*cache.records)
                                                : std::make_shared<std::vector<panel::Record>>(cache.descriptors.size());
                    auto &record = (*changed)[i];
                    if (!prior) {
                        record.id = descriptor.id; record.name = descriptor.name;
                        record.shape = static_cast<panel::Shape>(descriptor.shape); record.stepCount = descriptor.stepCount;
                        if (descriptor.shape == device::ParamShape::Menu) record.choiceLabels = descriptor.choiceLabels;
                    }
                    if (!prior || prior->playing != values.playing) record.text = instance->paramText(descriptor.id, values.playing);
                    record.playing = values.playing; record.stored = values.stored; record.driven = values.driven;
                    record.missing = values.missing; record.automated = values.automated;
                    record.overridden = values.overridden; record.lastTouchedBy = values.lastTouchedBy;
                }
                if (changed) cache.records = std::move(changed);
                data.metadata = cache.metadata; data.cachedRecords = cache.records;
                // EQ selects controls directly by band/section. Its unused aggregate
                // Configure list must not create 730 entries or quadratic lookups.
                next->push_back(std::move(data));
                continue;
            }
            if (instance)
                for (std::int32_t i = 0; i < instance->paramCount(); ++i) {
                    const auto *descriptor = instance->paramAt(i);
                    if (!descriptor) continue;
                    data.declared.push_back({descriptor->id, descriptor->name,
                        (descriptor->automatable || descriptor->configurable) && !descriptor->missing && data.available,
                        descriptor->automatable});
                    data.defaults[descriptor->id] = descriptor->defaultValue.normalized;
                    if (!descriptor->groupId.empty())
                        data.parameterGroups[descriptor->id] = {descriptor->groupId, descriptor->groupName,
                            descriptor->groupEnableControl, descriptor->bandIndex, descriptor->sectionId, descriptor->sectionName};
                    panel::Record record;
                    record.id = descriptor->id; record.name = descriptor->name;
                    readValues(*descriptor, engine::automationParamRef(instance->identity().format, descriptor->id), record);
                    record.text = instance->paramText(descriptor->id, record.playing);
                    record.shape = static_cast<panel::Shape>(descriptor->shape); record.stepCount = descriptor->stepCount;
                    if (descriptor->shape == device::ParamShape::Menu) record.choiceLabels = descriptor->choiceLabels;
                    data.records.push_back(std::move(record));
                }
            data.resolved = panel::resolve(model, row.id, data.declared);
            if (!data.plugin && !data.parameterGroups.empty() && !data.resolved.configured) {
                data.resolved.entries.clear();
                for (const auto &parameter : data.declared) data.resolved.entries.push_back({parameter.id, false});
                data.resolved.showsConfigureHint = false;
            }
            if (instance && instance->identity().uid.find("airwindows") != std::string::npos) {
                const auto algorithm = instance->getParam("0");
                const auto active = panel::activeAlgorithmParams(data.records,
                    static_cast<std::int32_t>(algorithm.hasReal ? algorithm.real : 0));
                std::vector<panel::Entry> entries;
                for (auto index : active) entries.push_back({data.records[index].id, data.records[index].missing});
                data.resolved.entries = std::move(entries); data.resolved.showsConfigureHint = false;
            }
            std::unordered_set<std::string_view> recorded;
            recorded.reserve(data.records.size());
            for (const auto &record : data.records) recorded.insert(record.id);
            // Finish lookups before growing record storage invalidates string views.
            std::vector<std::string> missing;
            for (const auto &entryToShow : data.resolved.entries)
                if (!recorded.contains(entryToShow.id)) missing.push_back(entryToShow.id);
            for (const auto &id : missing) {
                panel::Record record; record.id = record.name = id; record.missing = true;
                data.records.push_back(std::move(record));
            }
            next->push_back(std::move(data));
        }
    }
    std::erase_if(identities_, [publication](const auto &item) { return item.second.seen != publication; });
    std::erase_if(native_, [this](const auto &item) { return !identities_.contains(item.first); });
    // Consumers can keep their bindings and skip layout and painting between actual changes.
    if (!std::equal(next->begin(), next->end(), current_->begin(), current_->end(), samePanel))
        current_ = std::move(next);
}
} // namespace adi::ui
