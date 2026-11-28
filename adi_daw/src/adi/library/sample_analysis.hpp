// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/audio/sample_classification.hpp"
#include "adi/audio/warp_timing_cache.hpp"
#include "adi/library/sample_descriptors.hpp"
#include "adi/library/sample_tonality.hpp"
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace adi::audio { class DecodeCache; }
namespace adi::media { class AnalysisCache; }
namespace adi::library {
inline constexpr std::uint32_t sampleSimilarityWindowSeconds = 2;
inline constexpr std::uint32_t sampleSimilarityMaximumSeconds = 60;

struct SampleAnalysis {
    double lengthSeconds = 0;
    audio::SampleClassification classification{};
    std::optional<double> tempoBpm;
    std::optional<SampleKey> key;
    SampleDescriptors descriptors;
    std::optional<SampleDescriptors> similarityPrefix;
    audio::SampleKindSource tempoSource = audio::SampleKindSource::Detection;
    audio::SampleKindSource keySource = audio::SampleKindSource::Detection;
    float keyStrength = 0;
    std::optional<float> integratedLufs;
    std::vector<std::string> tags;
    std::string contentHash; // Preserved for read-only Places before ADIL has a hash.
};

struct SampleAnalysisOverrides {
    audio::SampleKindHint classification;
    std::optional<SampleKey> key;
    std::vector<std::string> tags;

    [[nodiscard]] bool empty() const noexcept {
        return classification.kind == audio::SampleKind::Unknown && !classification.bpm && !key && tags.empty();
    }
};

// Rebuildable observations carried by library exchange. The content hash
// identifies the source independently of path. Authored fields live in ADIL.
struct SampleAnalysisMetadata {
    std::string contentHash;
    std::optional<double> tempoBpm;
    std::optional<SampleKey> key;
    audio::SampleKindSource tempoSource = audio::SampleKindSource::Detection;
    audio::SampleKindSource keySource = audio::SampleKindSource::Detection;
    float keyStrength = 0;
    friend bool operator==(const SampleAnalysisMetadata&, const SampleAnalysisMetadata&) = default;
};

[[nodiscard]] SampleAnalysisMetadata portableSampleMetadata(const SampleAnalysis&);
void applySampleMetadata(SampleAnalysis&, const SampleAnalysisMetadata&);

[[nodiscard]] SampleAnalysis applySampleOverrides(SampleAnalysis, const SampleAnalysisOverrides&);

struct SampleAnalysisResolution {
    SampleAnalysisOverrides authored;
    std::optional<SampleAnalysisMetadata> exchanged;

    [[nodiscard]] bool empty() const noexcept { return authored.empty() && !exchanged; }

    [[nodiscard]] SampleAnalysis apply(SampleAnalysis local) const {
        if (exchanged) applySampleMetadata(local, *exchanged);
        if (!authored.empty()) return applySampleOverrides(std::move(local), authored);
        return local;
    }
};

struct SampleAnalysisResolutionChanges {
    std::uint64_t revision = 0;
    bool fullSnapshot = false;
    std::vector<std::pair<std::string, SampleAnalysisResolution>> entries;
};

enum class SampleAnalysisProblem { None, Realtime, Cancelled, Missing, SourceChanged, Read, CacheWrite };
struct SampleAnalysisResult {
    SampleAnalysisProblem problem = SampleAnalysisProblem::None;
    std::optional<SampleAnalysis> analysis;
    std::string hash;
    bool fromCache = false;
    [[nodiscard]] explicit operator bool() const noexcept {
        return problem == SampleAnalysisProblem::None && analysis.has_value();
    }
};

// Owner-worker service. A known hash identifies an indexed publication and
// permits a cache-only read. New measurements are committed only after the
// source hash has been verified. The caller owns both existing caches.
class SampleAnalysisService {
public:
    SampleAnalysisService(media::AnalysisCache& analysis, audio::DecodeCache& decode)
        : analysis_(analysis), decode_(decode) {}
    [[nodiscard]] SampleAnalysisResult read(const std::string& hash, std::stop_token = {}) const;
    [[nodiscard]] SampleAnalysisResult analyze(const std::filesystem::path& source,
        const std::string& expectedHash = {}, std::stop_token = {},
        std::optional<audio::WarpTimingSettings> timing = {});

private:
    media::AnalysisCache& analysis_;
    audio::DecodeCache& decode_;
};

// Used by launchers after they have resolved the sample's measurements and
// current authored overrides. The caller chooses when to enable audition looping.
[[nodiscard]] bool samplePreviewLoops(const SampleAnalysis&) noexcept;
}
