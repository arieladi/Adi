// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Live's per-parameter Re-Enable Automation (Live 12 manual 25.4; AU-04, N26): a control whose automation
// was overridden by a manual change offers "Re-Enable Automation" in its context menu, which makes it
// follow its lane again. The session holds which lanes are overridden (ADR-0162: session state, not an op,
// not saved, so the item is no undo step); the snapshot's lanes carry it (`AutomationLaneNode::overridden`).
// This header finds the lane behind a control. No JUCE.
#pragma once
#include "adi/engine/session.hpp"
#include "adi/device_param_ref.hpp"
#include "adi/engine/snapshot.hpp"
#include <string>
#include <cstdint>
#include <string_view>
namespace adi::ui {
namespace automation_override_detail {
// Whether a lane's ref names the parameter `paramRef` does: a device's by the id it parses to (DF-W38: a
// file written "001" for parameter 1), a track's or a send's as written.
inline bool sameParam(std::string_view ownerKind, std::string_view laneRef, std::string_view paramRef) noexcept {
    if (ownerKind != "device")
        return laneRef == paramRef;
    // DF-E53's parser, the engine's own: "001" is parameter 1.
    std::uint32_t a = 0, b = 0;
    return parseDeviceParamRef(laneRef, a) && parseDeviceParamRef(paramRef, b) ? a == b : laneRef == paramRef;
}
} // namespace automation_override_detail
// The lane of `ownerKind` #`ownerId`'s `paramRef` that the engine plays, or null: a track's "volume",
// "pan", "panLeft" or "panRight" and a send's "gain_db" (owned by its routing row) in real units, a
// device's parameter in normalized units. As the engine's binders (`bindStripAutomation`,
// `bindDeviceAutomation`), a disabled lane or one in another domain is skipped, and of two that remain
// the first plays: that is the lane a manual change overrides.
inline const engine::AutomationLaneNode *laneOf(const engine::Snapshot &snapshot, std::string_view ownerKind,
                                                std::int64_t ownerId, std::string_view paramRef) noexcept {
    const std::string_view domain = ownerKind == "device" ? "normalized" : "real";
    const bool activator = ownerKind == "track" && paramRef == "mute";
    for (const auto &lane : snapshot.automationLanes)
        if (lane && lane->enabled && (activator || lane->valueDomain == domain) && lane->ownerKind == ownerKind &&
            lane->ownerId == ownerId && automation_override_detail::sameParam(ownerKind, lane->paramRef, paramRef))
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
