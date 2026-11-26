// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <algorithm>
#include <string_view>

namespace adi::ui {
// Settings presented here include future features. A key needs a working consumer
// before the window offers it. This does not change the registry or stored values.
inline bool settingAvailable(std::string_view key) noexcept {
    static constexpr std::string_view readers[]{
        "agent.maxOpsPerRequest",
        "audio.bufferSize",
        "audio.channelConfig",
        "audio.delayCompensation",
        "audio.driverType",
        "audio.inputDevice",
        "audio.keepMonitoringLatency",
        "audio.outputDevice",
        "audio.sampleRate",
        "audio.testTone",
        "audio.testToneFrequencyHz",
        "audio.testToneVolumeDb",
        "cache.maxSizeMb",
        "cache.minFreeSpaceMb",
        "editing.arrowKeysMoveClips",
        "editing.automationFollowsClips",
        "editing.deleteOverlaps",
        "editing.importOptions",
        "editing.linkLoopToSelection",
        "editing.mutedClipsAsDeleted",
        "editing.splitAllWithNoSelection",
        "files.copyImportedMedia",
        "files.projectsFolder",
        "library.backgroundIndexing",
        "library.contentFolders",
        "library.userLibrary",
        "lookfeel.density",
        "lookfeel.followMode",
        "lookfeel.showInfoView",
        "lookfeel.showTooltips",
        "lookfeel.tabMovesFocus",
        "lookfeel.theme",
        "lookfeel.tooltipDelayMs",
        "lookfeel.wrapTabNavigation",
        "lookfeel.zoomOnSelection",
        "midi.port.mpe",
        "midi.port.name",
        "midi.port.track",
        "midi.ports",
        "plugins.autoHideWindows",
        "plugins.autoOpenWindows",
        "plugins.clapFolders",
        "plugins.editorsAlwaysOnTop",
        "plugins.multipleWindows",
        "plugins.useVst3SystemFolders",
        "plugins.vst3Folders",
        "privacy.diagnosticLog",
        "project.name",
        "project.sampleRate",
        "record.defaultWarpMode",
        "record.exclusiveArm",
        "record.exclusiveSolo",
        "record.importAutoWarp",
        "record.importEdgeFades",
        "record.quantize",
        "settings.logChanges",
        "shortcuts.overrides",
        "shortcuts.preset",
        "window.places",
        "window.zoom",
    };
    return std::ranges::binary_search(readers, key);
}
} // namespace adi::ui
