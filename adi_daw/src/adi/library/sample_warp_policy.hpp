// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/audio/short_sample_warp.hpp"
#include <cstdint>
#include <filesystem>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <stop_token>
#include <string>

namespace adi::audio { struct AutoWarpOptions; }

namespace adi::library {
class Index;
class SampleAnalysisService;

// Capture the app setting on the owner thread, then use the value on the
// Warp worker. An explicitly supplied analysis service or Index is borrowed.
struct SampleWarpPolicy {
    std::string mode = "auto";
    SampleAnalysisService* analysis = nullptr;
    const Index* library = nullptr;
    int transientSensitivity = 50;
};

[[nodiscard]] SampleWarpPolicy sampleWarpPolicyFromSettings(const nlohmann::json& values,
    SampleAnalysisService* analysis = nullptr, const Index* library = nullptr);

[[nodiscard]] audio::ShortSampleWarpPlan resolveSampleWarpPlan(audio::ShortSampleWarpTrigger,
    const std::optional<audio::ShortSampleWarpInput>&, const std::optional<SampleWarpPolicy>&,
    const std::filesystem::path& source, const std::string& hash, std::uint64_t frames,
    std::uint32_t rate, const audio::AutoWarpOptions&, std::stop_token);
}
