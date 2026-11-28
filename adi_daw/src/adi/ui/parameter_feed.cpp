// SPDX-License-Identifier: GPL-3.0-or-later
#include "parameter_feed.hpp"
#include "adi/engine/param_ops.hpp"
#include <algorithm>
#include <cmath>
#include <tuple>
#include <unordered_set>
namespace adi::ui {
namespace {
panel::Declared declarationOf(const device::ParamDescriptor &descriptor, bool modifiable) {
    const auto shape = static_cast<panel::Shape>(descriptor.shape);
    return {descriptor.id, descriptor.name, modifiable, descriptor.automatable, shape,
            shape == panel::Shape::Switch ? 2 : descriptor.stepCount};
}
bool samePanel(const DevicePanelData &a, const DevicePanelData &b) {
    const auto card = [](const DevicePanelData &d) {
        // The instance stamp includes the preparation shape and token epoch.
        return std::tie(d.id, d.track, d.chain, d.instance, d.available, d.latency,
                        d.name, d.enabled, d.activatorAvailable, d.playingEnabled,
                        d.instrument, d.plugin, d.uid, d.route, d.sidechainChannels,
                        d.processingIssue, d.loadIssue,
                        d.pluginTouch, d.recentPluginTouches,
                        d.metadata, d.cachedRecords, d.defaults, d.parameterGroups,
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
                          return std::tie(x.id, x.name, x.modifiable, x.automatable, x.shape, x.stepCount) ==
                                 std::tie(y.id, y.name, y.modifiable, y.automatable, y.shape, y.stepCount);
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

std::shared_ptr<const ParameterPublication> ParameterFeed::describeForAutomation(engine::Session &session,
                                                                                 std::int64_t track) const {
    auto result = std::make_shared<ParameterPublication>();
    const auto &model = session.model();
    for (const auto &chain : model.deviceChains) {
        if (chain.trackId != track)
            continue;
        for (const auto &row : model.devices) {
            if (row.chainId != chain.id)
                continue;
            const auto *entry = session.entryFor(row.id);
            const auto *instance = session.instanceFor(row.id);
            if (!entry || entry->retired || entry->parked || entry->resolvedPluginRef != row.pluginRefId || !instance)
                continue;
            DevicePanelData data;
            data.track = track;
            data.id = row.id;
            data.chain = chain.id;
            data.name = row.name;
            data.enabled = row.enabled;
            data.activatorAvailable = session.deviceActivatorAvailable(row.id);
            data.format = entry->resolvedFormat;
            data.available = instance->loaded();
            // Reuse the published touch only while it still names this loaded instance.
            const auto identity = identities_.find(row.id);
            if (identity != identities_.end() && identity->second.touchSequence != 0 &&
                data.available && session.resolveDevice(identity->second.runtime) == instance) {
                const auto prior = std::find_if(current_->begin(), current_->end(), [&](const auto &device) {
                    return device.id == row.id && device.track == track && device.chain == chain.id &&
                           device.instance == identity->second.stamp;
                });
                if (prior != current_->end() && prior->pluginTouch) {
                    data.plugin = prior->plugin;
                    data.instance = prior->instance;
                    data.runtime = prior->runtime;
                    data.pluginTouch = prior->pluginTouch;
                }
            }
            const auto count = instance->paramCount();
            for (std::int32_t index = 0; index < count; ++index) {
                const auto *descriptor = instance->paramAt(index);
                if (descriptor && (descriptor->automatable || data.pluginTouch)) {
                    const bool modifiable = data.available && !descriptor->missing &&
                                            (descriptor->automatable || descriptor->configurable);
                    data.declared.push_back(declarationOf(*descriptor, modifiable));
                    data.defaults.emplace(descriptor->id, descriptor->defaultValue.normalized);
                }
            }
            if (data.pluginTouch)
                data.resolved = panel::resolve(model, row.id, data.declarations());
            result->push_back(std::move(data));
        }
    }
    return result;
}

void ParameterFeed::publish(engine::Session &session, std::uint64_t modelRevision) {
    auto next = std::make_shared<ParameterPublication>();
    const auto publication = ++publication_;
    const auto &model = session.model();
    // Room for every device first: growing would copy each record and its maps (MSVC's map does not
    // move without a possible throw), every tick (walk-A A31).
    next->reserve(model.devices.size());
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
                        data.uid = ref.uid; data.format = ref.format; data.plugin = ref.format != "internal";
                        break;
                    }
            for (const auto &route : model.deviceRoutes)
                if (route.deviceId == row.id) {
                    data.route = route.route;
                    break;
                }
            auto *instance = session.instanceFor(row.id);
            const auto *entry = session.entryFor(row.id);
            const bool resolved = entry && !entry->retired && !entry->parked &&
                                  entry->resolvedPluginRef == row.pluginRefId && entry->resolvedUid == data.uid;
            data.available = resolved && instance && instance->loaded();
            data.activatorAvailable = session.deviceActivatorAvailable(row.id);
            if (data.activatorAvailable)
                data.playingEnabled = session.deviceActivatorPlaying(row.id);
            data.latency = data.available ? instance->latencySamples() : 0;
            data.sidechainChannels = data.available ? instance->sidechainInputChannels() : 0;
            if (data.available)
                data.processingIssue = instance->processingIssue();
            if (data.plugin && resolved && entry->placeholder)
                data.loadIssue = entry->error;
            auto &identity = identities_[row.id];
            const bool runtimeSame = instance != nullptr && session.resolveDevice(identity.runtime) == instance;
            const bool unpublished = !entry && identity.runtime.claimIdentity().expired();
            if (identity.stamp == 0 || identity.pluginRef != row.pluginRefId || identity.uid != data.uid ||
                (!runtimeSame && !unpublished)) {
                identity.stamp = ++nextIdentity_;
                identity.runtime = session.deviceRuntimeToken(row.id);
                identity.pluginRef = row.pluginRefId; identity.uid = data.uid;
                identity.touchSequence = 0;
                identity.latestTouch.reset();
                identity.recentTouches.reset();
                native_.erase(row.id);
            }
            data.runtime = identity.runtime;
            data.instance = identity.stamp; identity.seen = publication;
            if (parameterOps_ && data.available && (data.format == "vst3" || data.format == "clap") &&
                session.resolveDevice(data.runtime) == instance) {
                const auto sequence = parameterOps_->lastTouchSequence(row.id);
                if (sequence != identity.touchSequence) {
                    std::vector<PluginParameterTouch> touches;
                    const auto recent = parameterOps_->recentTouches(row.id);
                    touches.reserve(recent.size());
                    for (const auto &touch : recent) {
                        const auto *parameter = instance->paramAt(touch.index);
                        if (parameter && !parameter->id.empty() && !parameter->missing &&
                            (parameter->automatable || parameter->configurable))
                            touches.push_back({parameter->id, touch.sequence});
                    }
                    identity.latestTouch = !touches.empty() && touches.back().sequence == sequence
                        ? std::optional{touches.back()} : std::nullopt;
                    identity.recentTouches = touches.empty() ? nullptr :
                        std::make_shared<const std::vector<PluginParameterTouch>>(std::move(touches));
                    identity.touchSequence = sequence;
                }
                data.pluginTouch = identity.latestTouch;
                data.recentPluginTouches = identity.recentTouches;
            }
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
                        metadata->declared.push_back(declarationOf(*descriptor,
                            (descriptor->automatable || descriptor->configurable) && !descriptor->missing && data.available));
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
            if (instance) {
                // The tables are shared while every descriptor reads as before (walk-A A31): a field
                // comparison, no allocation; a plug-in that renames or adds parameters rebuilds them.
                const auto tableFields = [](const device::ParamDescriptor &d) {
                    return std::tie(d.id, d.name, d.defaultValue.normalized, d.automatable, d.configurable, d.missing,
                                    d.shape, d.stepCount, d.groupId, d.groupName, d.groupEnableControl, d.bandIndex, d.sectionId,
                                    d.sectionName);
                };
                auto &tables = tables_[row.id];
                const auto count = instance->paramCount();
                bool same = tables.metadata && tables.instance == data.instance && tables.available == data.available &&
                            tables.parameterCount == count;
                std::size_t at = 0;
                for (std::int32_t i = 0; same && i < count; ++i)
                    if (const auto *descriptor = instance->paramAt(i))
                        same = at < tables.descriptors.size() &&
                               tableFields(*descriptor) == tableFields(tables.descriptors[at++]);
                same = same && at == tables.descriptors.size();
                if (!same) {
                    tables = {};
                    tables.instance = data.instance; tables.available = data.available; tables.parameterCount = count;
                    auto metadata = std::make_shared<ParameterMetadata>();
                    for (std::int32_t i = 0; i < count; ++i) {
                        const auto *descriptor = instance->paramAt(i);
                        if (!descriptor) continue;
                        tables.descriptors.push_back(*descriptor);
                        metadata->declared.push_back(declarationOf(*descriptor,
                            (descriptor->automatable || descriptor->configurable) && !descriptor->missing && data.available));
                        metadata->defaults[descriptor->id] = descriptor->defaultValue.normalized;
                        if (!descriptor->groupId.empty())
                            metadata->groups[descriptor->id] = {descriptor->groupId, descriptor->groupName,
                                descriptor->groupEnableControl, descriptor->bandIndex, descriptor->sectionId,
                                descriptor->sectionName};
                    }
                    tables.metadata = std::move(metadata);
                }
                data.metadata = tables.metadata;
            }
            data.resolved = panel::resolve(model, row.id, data.declarations());
            if (!data.plugin && !data.groups().empty() && !data.resolved.configured) {
                data.resolved.entries.clear();
                for (const auto &parameter : data.declarations()) data.resolved.entries.push_back({parameter.id, false});
                data.resolved.showsConfigureHint = false;
            }
            // Configure needs all metadata; only displayed controls need values/text.
            // Large controller maps must not turn a small panel into quadratic SDK work.
            const bool algorithmPanel = instance && instance->identity().uid.find("airwindows") != std::string::npos;
            if (instance) {
                const auto count = instance->paramCount();
                for (std::int32_t i = 0; i < count; ++i) {
                    const auto* descriptor = instance->paramAt(i);
                    if (!descriptor) continue;
                    if (!algorithmPanel && std::none_of(data.resolved.entries.begin(), data.resolved.entries.end(),
                        [&](const auto& entryToShow) { return entryToShow.id == descriptor->id; })) continue;
                    panel::Record record;
                    record.id = descriptor->id; record.name = descriptor->name;
                    readValues(*descriptor, engine::automationParamRef(instance->identity().format, descriptor->id), record);
                    record.text = instance->paramText(descriptor->id, record.playing);
                    record.shape = static_cast<panel::Shape>(descriptor->shape); record.stepCount = descriptor->stepCount;
                    if (descriptor->shape == device::ParamShape::Menu) record.choiceLabels = descriptor->choiceLabels;
                    data.records.push_back(std::move(record));
                }
            }
            if (instance && instance->identity().uid.find("airwindows") !=
                                std::string::npos) {
              const auto algorithm = instance->getParam("0");
              const auto active = panel::activeAlgorithmParams(
                  data.records, static_cast<std::int32_t>(
                                    algorithm.hasReal ? algorithm.real : 0));
              std::vector<panel::Entry> entries;
              for (auto index : active)
                entries.push_back(
                    {data.records[index].id, data.records[index].missing});
              data.resolved.entries = std::move(entries);
              data.resolved.showsConfigureHint = false;
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
    std::erase_if(tables_, [this](const auto &item) { return !identities_.contains(item.first); });
    // Consumers can keep their bindings and skip layout and painting between actual changes.
    if (!std::equal(next->begin(), next->end(), current_->begin(), current_->end(), samePanel))
        current_ = std::move(next);
}
} // namespace adi::ui
