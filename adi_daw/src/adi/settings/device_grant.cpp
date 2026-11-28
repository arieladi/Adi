// SPDX-License-Identifier: GPL-3.0-or-later

#include "adi/settings/device_grant.hpp"

#include "adi/settings/registry.hpp"

#include <tuple>

namespace adi::settings {

std::string driverSettingFor(const std::string& typeName) {
    if (typeName == "Windows Audio") return "WASAPI";
    if (typeName == "Windows Audio (Exclusive Mode)") return "WASAPI Exclusive";
    if (typeName == "Windows Audio (Low Latency Mode)") return "WASAPI Low Latency";
    return typeName;
}

bool driverSettingNames(const std::string& setting, const std::string& typeName) {
    return !setting.empty() && (setting == typeName || driverSettingFor(typeName) == setting);
}

DeviceChange deviceChange(const DeviceWatch& w) {
    if (w.userChose) return DeviceChange::Keep;
    const auto present = [](const std::vector<std::string>& list, const std::string& name) {
        for (const auto& n : list)
            if (n == name) return true;
        return false;
    };
    bool missing = false, back = false;
    for (const auto& [chosen, open, list] : {std::tie(w.chosenOutput, w.openOutput, w.outputs),
                                              std::tie(w.chosenInput, w.openInput, w.inputs)}) {
        if (chosen.empty() || chosen == open) continue;
        (present(list, chosen) ? back : missing) = true;
    }
    if (missing) return DeviceChange::Hold;
    return back ? DeviceChange::Return : DeviceChange::Keep;
}

bool keepGrant(AppSettings& s, const DeviceGrant& grant, const ChangeContext& who, std::string& why) {
    bool ok = true;
    auto set = [&](const char* key, const Value& value) {
        std::string refused;
        if (!s.set(key, value, who, refused) && ok) {
            ok = false;
            why = refused;
        }
    };
    // The driver's grant, kept only where the chooser offers it (ADR-0145 d5).
    auto granted = [&](const char* key, int value) {
        std::string unoffered;
        if (const auto* row = find(key); row != nullptr && validate(*row, value, unoffered))
            set(key, value);
    };
    set("audio.inputDevice", grant.inputDevice);
    set("audio.outputDevice", grant.outputDevice);
    granted("audio.sampleRate", grant.sampleRate);
    granted("audio.bufferSize", grant.bufferSize);
    set("audio.driverType", grant.driverType);
    return ok;
}

}  // namespace adi::settings
