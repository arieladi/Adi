// SPDX-License-Identifier: GPL-3.0-or-later
//
// What an opened audio device leaves in the settings. The Audio page's own
// changes reach the settings this way, so the device's setup is written back
// each time the device opens or changes.
//
// The driver decides what it grants: WASAPI in shared mode runs at its own
// period (480 frames at 48 kHz), DirectSound at e.g. 2560. Live shows the
// granted size on the Audio tab and raises nothing; ADR-0145 d5 keeps the
// request and the grant side by side (ADR-0049). So a granted rate or buffer
// size the setting offers becomes the setting; one it does not offer leaves
// the request standing, and is not an error.

#pragma once

#include "adi/settings/store.hpp"

#include <string>
#include <vector>

namespace adi::settings {

struct DeviceGrant {
    std::string driverType;     // the setting's name for the type: driverSettingFor
    std::string inputDevice, outputDevice;
    int sampleRate = 0;
    int bufferSize = 0;
};

/// SC-379: `audio.driverType`'s value for one of JUCE's device type names. Windows Audio has three modes,
/// each its own type: "WASAPI" (shared), "WASAPI Exclusive", "WASAPI Low Latency"; every other type keeps
/// its own name. All three were saved as "WASAPI" and reopened shared.
[[nodiscard]] std::string driverSettingFor(const std::string& typeName);
/// Whether JUCE's type `typeName` is the one the setting names.
[[nodiscard]] bool driverSettingNames(const std::string& setting, const std::string& typeName);

/// SC-378: what the app does when the open device changes and the user did not choose it (an interface
/// unplugged, the system's default opened in its place, the interface plugged back). The user's choice is
/// kept: `Hold` while the chosen device is missing (say so; save nothing over it), `Return` when it is
/// there again but another is open (reopen it), `Keep` otherwise (save the grant, as before).
enum class DeviceChange { Keep, Hold, Return };
struct DeviceWatch {
    bool userChose = false;                      ///< the Audio page made this change
    std::string chosenOutput, chosenInput;       ///< the settings' devices; empty: the system's
    std::string openOutput, openInput;           ///< what is open now
    std::vector<std::string> outputs, inputs;    ///< the current driver's devices, present now
};
[[nodiscard]] DeviceChange deviceChange(const DeviceWatch&);

/// Writes the grant back. False, with `why`, only for a value the store
/// refuses that is not a granted rate or size. Does not save.
bool keepGrant(AppSettings&, const DeviceGrant&, const ChangeContext& who, std::string& why);

}  // namespace adi::settings
