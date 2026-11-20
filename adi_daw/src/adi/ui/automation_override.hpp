// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Live's per-parameter Re-Enable Automation (Live 12 manual 25.4; AU-04, N26): a control whose automation
// was overridden by a manual change offers "Re-Enable Automation" in its context menu, which makes it
// follow its lane again. The session holds which lanes are overridden (ADR-0162: session state, not an op,
// not saved, so the item is no undo step); the snapshot's lanes carry it (`AutomationLaneNode::overridden`).
// This header finds the lane behind a control. No JUCE.
#pragma once
#include "adi/engine/session.hpp"
#include "adi/engine/snapshot.hpp"
#include <string>
#include <cstdint>
#include <string_view>
namespace adi::ui {
// The lane of `ownerKind` #`ownerId`'s `paramRef`, or null: a track's "volume", "pan", "panLeft" or
// "panRight"; a send's "gain_db", owned by its routing row; a device's parameter id.
inline const engine::AutomationLaneNode *laneOf(const engine::Snapshot &snapshot, std::string_view ownerKind,
                                                std::int64_t ownerId, std::string_view paramRef) noexcept {
    for (const auto &lane : snapshot.automationLanes)
        if (lane && lane->ownerKind == ownerKind && lane->ownerId == ownerId && lane->paramRef == paramRef)
            return lane.get();
    return nullptr;
}
// The lane of device #`device`'s parameter `param`, the device a plug-in of `format`: its lane names the
// parameter as the engine does (a CLAP's numeric id in decimal, ADR-0165).
inline const engine::AutomationLaneNode *deviceLaneOf(const engine::Snapshot &snapshot, std::int64_t device,
                                                      const std::string &format, const std::string &param) {
    const auto ref = engine::automationParamRef(format, param);
    return ref ? laneOf(snapshot, "device", device, *ref) : nullptr;
}
} // namespace adi::ui
