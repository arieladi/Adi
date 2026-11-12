// SPDX-License-Identifier: GPL-3.0-or-later
#include "parameter_feed.hpp"
#include <algorithm>
namespace adi::ui {
void ParameterFeed::publish(engine::Session &session) {
    auto next = std::make_shared<ParameterPublication>();
    const auto publication = ++publication_;
    const auto &model = session.model();
    for (const auto &chain : model.deviceChains) {
        if (!chain.trackId)
            continue;
        for (const auto &row : model.devices) {
            if (row.chainId != chain.id)
                continue;
            DevicePanelData data;
            data.id = row.id;
            data.track = *chain.trackId;
            data.chain = chain.id;
            data.name = row.name;
            data.enabled = row.enabled;
            // A rack's kind has no values in the format yet, so only a plug-in can be an instrument.
            if (row.pluginRefId)
                for (const auto &ref : model.pluginRefs)
                    if (ref.id == *row.pluginRefId) {
                        data.instrument = ref.subtype == "instrument";
                        data.uid = ref.uid;
                        // A plug-in has a window of its own; ADI's built-ins ("internal") have none.
                        data.plugin = ref.format != "internal";
                        break;
                    }
            auto *instance = session.instanceFor(row.id);
            const auto *entry = session.entryFor(row.id);
            data.runtime = session.deviceRuntimeToken(row.id);
            data.available = instance && instance->loaded() && entry && !entry->retired && !entry->parked &&
                             entry->resolvedPluginRef == row.pluginRefId && entry->resolvedUid == data.uid;
            data.latency = data.available ? instance->latencySamples() : 0;
            auto &identity = identities_[row.id];
            const bool runtimeSame = identity.runtime.sameInstance(data.runtime) &&
                                     session.resolveDevice(identity.runtime) == instance;
            // Racks and unresolved rows have no runtime token; their authored reference is the
            // identity until a runtime exists. A disappearance below erases their stamp too.
            const bool neitherRuntime = identity.runtime.claimIdentity().expired() &&
                                        data.runtime.claimIdentity().expired();
            if (identity.stamp == 0 || identity.pluginRef != row.pluginRefId || identity.uid != data.uid ||
                (!runtimeSame && !(neitherRuntime && !entry))) {
                identity.stamp = ++nextIdentity_;
                identity.runtime = data.runtime;
                identity.pluginRef = row.pluginRefId;
                identity.uid = data.uid;
            }
            data.instance = identity.stamp;
            identity.seen = publication;
            if (instance)
                for (std::int32_t i = 0; i < instance->paramCount(); ++i) {
                    const auto *d = instance->paramAt(i);
                    if (!d)
                        continue;
                    data.declared.push_back({d->id, d->name,
                                             (d->automatable || d->configurable) && !d->missing && data.available,
                                             d->automatable});
                    data.defaults[d->id] = d->defaultValue.normalized;
                    if (!d->groupId.empty())
                        data.parameterGroups[d->id] = {d->groupId, d->groupName, d->groupEnableControl,
                                                      d->bandIndex, d->sectionId, d->sectionName};
                    panel::Record r;
                    r.id = d->id;
                    r.name = d->name;
                    r.missing = d->missing || !data.available;
                    r.playing = instance->getParam(d->id).normalized;
                    r.stored = r.playing;
                    for (const auto &saved : model.pluginParams)
                        if (saved.deviceId == row.id && saved.paramId == d->id) {
                            r.stored = saved.normalized;
                            break;
                        }
                    r.text = instance->paramText(d->id, r.playing);
                    r.shape = static_cast<panel::Shape>(d->shape);
                    r.stepCount = d->stepCount;
                    if (d->shape == device::ParamShape::Menu) r.choiceLabels = d->choiceLabels;
                    // The lane's ref is decimal where a CLAP id is hex (ADR-0165): the session's
                    // own spelling, so a CLAP parameter's lane is found as its override is.
                    const auto *lanes = session.deviceAutomation();
                    const auto ref = engine::automationParamRef(instance->identity().format, d->id);
                    if (d->automatable && lanes && ref) {
                        const auto lane = lanes->laneFor.find({row.id, *ref});
                        if (lane != lanes->laneFor.end()) {
                            r.automated = true;
                            r.overridden = session.overriddenLanes().contains(lane->second);
                        }
                    }
                    auto source = touched_.find({row.id, d->id});
                    r.lastTouchedBy =
                        source == touched_.end() ? panel::Source::None : source->second;
                    if (r.automated && !r.overridden)
                        r.lastTouchedBy = panel::Source::Automation;
                    data.records.push_back(std::move(r));
                }
            panel::finalize(data.records);
            data.resolved = panel::resolve(model, row.id, data.declared);
            if (!data.plugin && !data.parameterGroups.empty() && !data.resolved.configured) {
                // Explicit native pages partition their controls. The ordinary plug-in's
                // aggregate 64-control Configure threshold must not empty a two-stage page.
                data.resolved.entries.clear();
                for (const auto &parameter : data.declared)
                    data.resolved.entries.push_back({parameter.id, false});
                data.resolved.showsConfigureHint = false;
            }
            // Suite identity, not a name match against a user's renamed device.
            if (instance && instance->identity().uid.find("airwindows") != std::string::npos) {
                auto algorithm = instance->getParam("0");
                const auto active = panel::activeAlgorithmParams(
                    data.records,
                    static_cast<std::int32_t>(algorithm.hasReal ? algorithm.real : 0));
                std::vector<panel::Entry> entries;
                for (auto index : active)
                    entries.push_back({data.records[index].id, data.records[index].missing});
                data.resolved.entries = std::move(entries);
                data.resolved.showsConfigureHint = false;
            }
            for (const auto &e : data.resolved.entries) {
                if (std::none_of(data.records.begin(), data.records.end(),
                                 [&](const auto &r) { return r.id == e.id; })) {
                    panel::Record r;
                    r.id = e.id;
                    r.name = e.id;
                    r.missing = true;
                    data.records.push_back(std::move(r));
                }
            }
            next->push_back(std::move(data));
        }
    }
    std::erase_if(identities_, [publication](const auto &entry) { return entry.second.seen != publication; });
    current_ = std::move(next);
}
} // namespace adi::ui
