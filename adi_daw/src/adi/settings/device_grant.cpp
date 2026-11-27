// SPDX-License-Identifier: GPL-3.0-or-later

#include "adi/settings/device_grant.hpp"

#include "adi/settings/registry.hpp"

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
