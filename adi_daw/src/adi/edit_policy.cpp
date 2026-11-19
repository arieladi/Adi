// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/edit_plan.hpp"
#include "adi/settings/store.hpp"

namespace adi::edit_plan {
EditPolicy capturePolicy(const settings::AppSettings& settings) {
    EditPolicy result;result.deleteOverlaps=settings.get("editing.deleteOverlaps").get<bool>();
    result.automationFollowsClips=settings.get("editing.automationFollowsClips").get<bool>();
    result.mutedClipsAsDeleted=settings.get("editing.mutedClipsAsDeleted").get<bool>();return result;
}
} // namespace adi::edit_plan
