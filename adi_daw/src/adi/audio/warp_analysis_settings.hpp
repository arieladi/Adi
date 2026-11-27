// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/audio/io_audit.hpp"
#include "adi/settings/registry.hpp"

namespace adi::audio {

inline constexpr char transientSensitivityKey[] = "editing.transientSensitivity";
inline constexpr int defaultWarpTransientSensitivity = 50;

// Resolve the captured application settings once when making the analysis job.
// The value is also suitable for the cached analysis configuration key.
[[nodiscard]] inline int warpTransientSensitivity(const settings::Value& values) noexcept {
    if (onAudioThread || !values.is_object()) return defaultWarpTransientSensitivity;
    const auto value = values.find(transientSensitivityKey);
    if (value == values.end() || !value->is_number_integer()) return defaultWarpTransientSensitivity;
    if (value->is_number_unsigned()) {
        const auto count = value->get<std::uint64_t>();
        return count <= 100 ? static_cast<int>(count) : defaultWarpTransientSensitivity;
    }
    const auto count = value->get<std::int64_t>();
    return count >= 0 && count <= 100 ? static_cast<int>(count) : defaultWarpTransientSensitivity;
}

} // namespace adi::audio
