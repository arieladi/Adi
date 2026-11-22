// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/audio/wav_file.hpp"
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>
namespace adi::render {
enum class NormalizeMode {Off,Peak,Loudness};
enum class NormalizationTarget { Integrated, Rms, MaximumMomentary, MaximumShortTerm };

[[nodiscard]] const char* normalizationTargetName(NormalizationTarget) noexcept;
struct Normalization {
    NormalizeMode mode=NormalizeMode::Off;
    double targetLufs=-14,truePeakCeilingDb=-1;
    std::vector<double> channelWeights{};
    NormalizationTarget target = NormalizationTarget::Integrated;
    double targetRmsDbfs = -14.0;
};
// Values describe the named stage. Silence and an unfitted loudness
// window have no numeric value. RMS gives each scalar sample equal weight.
struct ExportStatistics {
    std::optional<double> integratedLufs{}, samplePeakDbfs{}, truePeakDbtp{};
    std::optional<double> loudnessRangeLu{}, maximumMomentaryLufs{}, maximumShortTermLufs{};
    std::optional<double> rmsDbfs{};
};

class ExportAnalysis {
public:
    ExportAnalysis(std::uint32_t rate, int channels, std::span<const double> weights = {});
    ~ExportAnalysis();
    ExportAnalysis(const ExportAnalysis&) = delete;
    ExportAnalysis& operator=(const ExportAnalysis&) = delete;
    void append(std::span<const double> interleaved, std::uint32_t frames);
    [[nodiscard]] ExportStatistics finish();
private:
    [[nodiscard]] double samplePeak() const noexcept;
    friend class NormalizationCapture;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct NormalizationReport {
    std::optional<double> sourceLufs{},sourceSamplePeakDb{},sourceTruePeakDb{};
    std::optional<double> outputLufs{},outputTruePeakDb{};
    double appliedGainDb=0;
    bool ceilingLimited=false;
    unsigned encodingPasses=0;
    ExportStatistics captured{}, decoded{};
};
void validateNormalization(const Normalization&,std::uint32_t rate,int channels);
/// Offline worker, using a fresh job-owned staging file. Retains interleaved
/// double PCM so normalization never reruns a device or narrows a wide graph.
/// The caller owns/cleans the staging directory and publishes only on success.
class NormalizationCapture {
public:
    NormalizationCapture(const std::filesystem::path& privateSpool,std::uint32_t rate,int channels,const Normalization&);
    ~NormalizationCapture();
    NormalizationCapture(const NormalizationCapture&)=delete;
    NormalizationCapture& operator=(const NormalizationCapture&)=delete;
    void append(std::span<const double> interleaved,std::uint32_t frames);
    NormalizationReport finish(const std::filesystem::path& privateWav,audio::WavFormat,
        audio::WavDither,std::uint64_t seed,const std::function<void()>& service={});
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
