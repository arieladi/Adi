// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/audio/short_sample_warp.hpp"
#include <filesystem>
#include <nlohmann/json_fwd.hpp>
#include <stop_token>
#include <string>

namespace adi::library {
class Index;
class SampleAnalysisService;

// Capture the app setting on the owner thread, then use the value on the
// Warp worker. An explicitly supplied analysis service or Index is borrowed.
struct SampleWarpPolicy {
    std::string mode = "auto";
    SampleAnalysisService* analysis = nullptr;
    const Index* library = nullptr;
};

[[nodiscard]] SampleWarpPolicy sampleWarpPolicyFromSettings(const nlohmann::json& values,
    SampleAnalysisService* analysis = nullptr, const Index* library = nullptr);

[[nodiscard]] audio::ShortSampleWarpInput readSampleWarpInput(const SampleWarpPolicy&,
    const std::filesystem::path& source, const std::string& hash, std::stop_token);
}
