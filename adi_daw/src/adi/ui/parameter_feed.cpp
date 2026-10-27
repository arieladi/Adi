// SPDX-License-Identifier: GPL-3.0-or-later
#include "parameter_feed.hpp"
#include <algorithm>
namespace adi::ui {
void ParameterFeed::publish(engine::Session &session) {
    auto next = std::make_shared<ParameterPublication>();
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
            data.name = row.name;
            data.enabled = row.enabled;
            auto *instance = session.instanceFor(row.id);
            if (instance)
                for (std::int32_t i = 0; i < instance->paramCount(); ++i) {
                    const auto *d = instance->paramAt(i);
                    if (!d)
                        continue;
                    data.declared.push_back({d->id, d->name, d->automatable && !d->missing});
                    panel::Record r;
                    r.id = d->id;
                    r.name = d->name;
                    r.missing = d->missing || !instance->loaded();
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
                    if (const auto *lanes = session.deviceAutomation()) {
                        const auto lane = lanes->laneFor.find({row.id, d->id});
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
    current_ = std::move(next);
}
} // namespace adi::ui
