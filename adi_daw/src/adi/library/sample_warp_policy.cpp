// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/library/sample_warp_policy.hpp"
#include "adi/audio/decode.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/audio/warp_analysis_settings.hpp"
#include "adi/library/index.hpp"
#include "adi/library/sample_analysis.hpp"
#include "adi/media/analysis_cache.hpp"
#include <stdexcept>
#include <utility>

namespace adi::library {
SampleWarpPolicy sampleWarpPolicyFromSettings(const nlohmann::json& values,
    SampleAnalysisService* analysis, const Index* library) {
    SampleWarpPolicy result;
    const auto found = values.find("record.shortSampleMode");
    if (found != values.end() && (*found == "unwarpedOneShot" || *found == "warpedOneShot" ||
        *found == "warpedLoop" || *found == "auto"))
        result.mode = found->get<std::string>();
    result.analysis = analysis;
    result.library = library;
    result.transientSensitivity = audio::warpTransientSensitivity(values);
    return result;
}

audio::ShortSampleWarpInput readSampleWarpInput(const SampleWarpPolicy& policy,
    const std::filesystem::path& source, const std::string& hash, std::stop_token stop) {
    if (audio::onAudioThread) return {};
    if (stop.stop_requested()) throw std::runtime_error("short sample analysis cancelled");
    if (policy.mode == "unwarpedOneShot") return {policy.mode, {}, hash};

    auto* service = policy.analysis;
    if (!service) {
        static SampleAnalysisService appAnalysis(media::defaultAnalysisCache(), audio::defaultDecodeCache());
        service = &appAnalysis;
    }
    auto result = service->analyze(source, hash, stop);
    if (!result) {
        if (result.problem == SampleAnalysisProblem::Cancelled)
            throw std::runtime_error("short sample analysis cancelled");
        if (result.problem == SampleAnalysisProblem::SourceChanged)
            throw std::runtime_error("short sample source changed during analysis");
        throw std::runtime_error("cannot measure the short sample");
    }
    auto measured = std::move(*result.analysis);
    if (policy.library) measured = applySampleOverrides(std::move(measured), policy.library->analysisOverrides(hash));
    if (measured.tempoBpm) measured.classification.bpm = measured.tempoBpm;
    return {policy.mode, measured.classification, std::move(result.hash)};
}
}
