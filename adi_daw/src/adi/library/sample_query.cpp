// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/library/sample_query.hpp"

#include "adi/audio/io_audit.hpp"
#include "adi/dsp/masking.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace adi::library {
namespace {

// The manual describes the ranking order. These measured weights implement
// the distance for ADI's sample descriptors (TM-MD-07).
constexpr double kSimilaritySpectralWeight = 5.0;
constexpr double kSimilarityEnvelopeWeight = 1.0;
constexpr double kSimilarityLengthWeight = 0.7;
constexpr double kSimilarityHitDensityWeight = 0.7;
constexpr double kSimilarityLoudnessWeight = 0.4;
constexpr double kSimilarityTempoWeight = 0.4;
constexpr double kSimilarityKeyWeight = 0.4;
constexpr double kSimilarityRequiredWeight = kSimilaritySpectralWeight + kSimilarityEnvelopeWeight +
                                             kSimilarityLengthWeight + kSimilarityHitDensityWeight +
                                             kSimilarityLoudnessWeight;
constexpr double kSimilarityMaximumWeight = kSimilarityRequiredWeight + kSimilarityTempoWeight + kSimilarityKeyWeight;
constexpr double kFitsSpectrumWeight = 0.45;
constexpr double kFitsOnsetWeight = 0.25;
constexpr double kFitsKeyWeight = 0.30;

bool inRange(const std::optional<SampleQueryRange> &range, double value) {
    return !range || (std::isfinite(value) && value >= range->minimum && value <= range->maximum);
}

bool accepts(const SampleQuery &query, const SampleAnalysis *analysis) {
    if (analysis == nullptr)
        return !query.kind && !query.lengthSeconds && !query.key && !query.tempoBpm && !query.brightnessHz &&
               !query.hitsPerSecond;

    return (!query.kind || (analysis->classification.kind != audio::SampleKind::Unknown &&
                            analysis->classification.kind == *query.kind)) &&
           (!query.key || analysis->key == query.key) && inRange(query.lengthSeconds, analysis->lengthSeconds) &&
           (!query.tempoBpm || (analysis->tempoBpm && inRange(query.tempoBpm, *analysis->tempoBpm))) &&
           inRange(query.brightnessHz, analysis->descriptors.brightnessHz) &&
           inRange(query.hitsPerSecond, analysis->descriptors.hitDensityHz);
}

template <std::size_t Size>
double energyDistance(const std::array<float, Size> &a, const std::array<float, Size> &b) {
    double distance = 0.0;
    for (std::size_t i = 0; i < Size; ++i)
        distance += std::abs(static_cast<double>(a[i]) - static_cast<double>(b[i]));
    return std::min(distance * 0.5, 1.0);
}

double ratioDistance(double a, double b, double fullDistanceOctaves) {
    return std::min(std::abs(std::log2(a / b)) / fullDistanceOctaves, 1.0);
}

double keyCompatibility(const std::optional<SampleKey> &a, const std::optional<SampleKey> &b) {
    if (!a || !b)
        return 0.5;
    // Relative major and minor share a pitch collection. Compatibility falls
    // with the shortest distance between collections on the circle of fifths.
    const int majorA = (a->tonic + (a->minor ? 3 : 0)) % 12;
    const int majorB = (b->tonic + (b->minor ? 3 : 0)) % 12;
    const int clockwiseFifths = ((majorB - majorA + 12) * 7) % 12;
    return 1.0 - static_cast<double>(std::min(clockwiseFifths, 12 - clockwiseFifths)) / 6.0;
}

double similar(const SampleAnalysis &a, const SampleAnalysis &b, double cutoff) {
    const auto &first = a.similarityPrefix ? *a.similarityPrefix : a.descriptors;
    const auto &other = b.similarityPrefix ? *b.similarityPrefix : b.descriptors;
    const double spectralDistance = kSimilaritySpectralWeight * energyDistance(first.spectralEnergy, other.spectralEnergy);
    const double highestPossibleScore = 1.0 - spectralDistance / kSimilarityMaximumWeight;
    // The remaining distances only lower this bound. The ranked heap ignores
    // candidates below its weakest retained result, so they need no more work.
    if (highestPossibleScore < cutoff)
        return highestPossibleScore;

    const double densityA = 1.0 + first.hitDensityHz;
    const double densityB = 1.0 + other.hitDensityHz;
    // Compare integrated loudness only when both readings cover the same
    // material as their sound descriptors. Longer files use prefix RMS.
    const bool comparableLufs = a.integratedLufs && b.integratedLufs &&
                                a.lengthSeconds <= sampleSimilarityWindowSeconds &&
                                b.lengthSeconds <= sampleSimilarityWindowSeconds;
    const double levelA = comparableLufs ? *a.integratedLufs : first.rmsDbfs;
    const double levelB = comparableLufs ? *b.integratedLufs : other.rmsDbfs;
    const double loudnessDistance = std::abs(levelA - levelB) / 60.0;
    double distance = spectralDistance +
                      kSimilarityEnvelopeWeight * energyDistance(first.envelopeEnergy, other.envelopeEnergy) +
                      kSimilarityLengthWeight * ratioDistance(a.lengthSeconds, b.lengthSeconds, 4.0) +
                      kSimilarityHitDensityWeight * ratioDistance(densityA, densityB, 4.0) +
                      kSimilarityLoudnessWeight * std::min(loudnessDistance, 1.0);
    double weights = kSimilarityRequiredWeight;
    if (a.tempoBpm && b.tempoBpm && *a.tempoBpm > 0.0 && *b.tempoBpm > 0.0) {
        distance += kSimilarityTempoWeight * ratioDistance(*a.tempoBpm, *b.tempoBpm, 1.0);
        weights += kSimilarityTempoWeight;
    }
    if (a.key && b.key) {
        distance += kSimilarityKeyWeight * (1.0 - keyCompatibility(a.key, b.key));
        weights += kSimilarityKeyWeight;
    }
    return 1.0 - distance / weights;
}

using MaskingProfile = std::array<double, sampleMaskingBandCount>;

MaskingProfile maskingProfile(const SampleDescriptors &reference) {
    static const dsp::MaskingBands bands(sampleMaskingFftBins, sampleMaskingSampleRate);
    const double power = std::pow(10.0, static_cast<double>(reference.rmsDbfs) / 10.0);
    MaskingProfile energy{};
    MaskingProfile unitEnergy{};
    unitEnergy.fill(1.0);
    for (std::size_t i = 0; i < energy.size(); ++i)
        energy[i] = power * reference.maskingEnergy[i];

    // A unit-energy victim is 0 dB in every band. The measured excess then
    // gives the spread masker itself, so its spread need only be run once.
    const auto masking = dsp::measureMasking(energy, unitEnergy, bands);
    MaskingProfile profile{};
    for (std::size_t i = 0; i < std::min(profile.size(), masking.excessDb.size()); ++i)
        profile[i] = std::pow(10.0, static_cast<double>(masking.excessDb[i]) / 10.0);
    return profile;
}

double openSpectrum(const MaskingProfile &masker, const SampleDescriptors &candidate) {
    const double power = std::pow(10.0, static_cast<double>(candidate.rmsDbfs) / 10.0);
    double uncovered = 0.0;
    for (std::size_t i = 0; i < masker.size(); ++i) {
        const double share = candidate.maskingEnergy[i];
        if (power * share >= masker[i])
            uncovered += share;
    }
    return std::clamp(uncovered, 0.0, 1.0);
}

double openOnsets(const SampleDescriptors &reference, const SampleDescriptors &candidate) {
    const double peak = *std::ranges::max_element(reference.onsetDensity);
    if (peak <= 0.0)
        return 0.5;
    double overlap = 0.0;
    double onsets = 0.0;
    // Both profiles are phase-aligned to their own start and expressed as
    // fractions of one clip. Sparse reference bins are more open to new hits.
    for (std::size_t i = 0; i < reference.onsetDensity.size(); ++i) {
        const double share = candidate.onsetDensity[i];
        overlap += share * reference.onsetDensity[i] / peak;
        onsets += share;
    }
    return onsets > 0.0 ? 1.0 - overlap / onsets : 0.5;
}

double fits(const SampleAnalysis &reference, const SampleAnalysis &candidate, const MaskingProfile &profile) {
    return kFitsSpectrumWeight * openSpectrum(profile, candidate.descriptors) +
           kFitsOnsetWeight * openOnsets(reference.descriptors, candidate.descriptors) +
           kFitsKeyWeight * keyCompatibility(reference.key, candidate.key);
}

bool isRankable(const SampleAnalysis *sample, SampleQueryRank rank) {
    return sample != nullptr && std::isfinite(sample->lengthSeconds) && sample->lengthSeconds > 0.0 &&
           (rank != SampleQueryRank::Similar || sample->lengthSeconds <= sampleSimilarityMaximumSeconds);
}

bool betterMatch(const SampleQueryMatch &a, const SampleQueryMatch &b) {
    return a.score == b.score ? a.row < b.row : a.score > b.score;
}

const SampleAnalysis *analysisOf(const SampleAnalysis *analysis) { return analysis; }
const SampleAnalysis *analysisOf(const std::shared_ptr<const SampleAnalysis> &analysis) { return analysis.get(); }

template <class Entry>
void querySamplesImpl(std::span<const Entry> wordMatches, const SampleQuery &query,
                      std::vector<SampleQueryMatch> &results) {
    if (audio::onAudioThread)
        return;
    results.clear();
    if (query.limit == 0 || wordMatches.empty())
        return;

    const bool ranked = query.rank != SampleQueryRank::LibraryOrder;
    if (ranked && !isRankable(query.reference, query.rank))
        return;
    MaskingProfile profile{};
    if (query.rank == SampleQueryRank::Fits)
        profile = maskingProfile(query.reference->descriptors);

    const auto capacity = std::min(query.limit, wordMatches.size());
    const bool boundedRank = ranked && capacity < wordMatches.size();
    results.reserve(capacity);
    for (std::size_t i = 0; i < wordMatches.size(); ++i) {
        const auto *analysis = analysisOf(wordMatches[i]);
        if (!accepts(query, analysis))
            continue;
        if (!ranked) {
            results.push_back({i, 0.0});
            if (results.size() == capacity)
                break;
        } else if (analysis != query.reference && isRankable(analysis, query.rank)) {
            const double cutoff = boundedRank && results.size() == capacity ? results.front().score : -1.0;
            const double score = query.rank == SampleQueryRank::Similar ? similar(*query.reference, *analysis, cutoff)
                                                                       : fits(*query.reference, *analysis, profile);
            if (!std::isfinite(score))
                continue;
            const SampleQueryMatch match{i, score};
            if (!boundedRank) {
                results.push_back(match);
            } else if (results.size() < capacity) {
                results.push_back(match);
                std::push_heap(results.begin(), results.end(), betterMatch);
            } else if (betterMatch(match, results.front())) {
                std::pop_heap(results.begin(), results.end(), betterMatch);
                results.back() = match;
                std::push_heap(results.begin(), results.end(), betterMatch);
            }
        }
    }
    if (boundedRank)
        std::sort_heap(results.begin(), results.end(), betterMatch);
    else if (ranked)
        std::sort(results.begin(), results.end(), betterMatch);
}

} // namespace

void querySamples(std::span<const SampleAnalysis *const> wordMatches, const SampleQuery &query,
                  std::vector<SampleQueryMatch> &results) {
    querySamplesImpl(wordMatches, query, results);
}

void querySamples(std::span<const std::shared_ptr<const SampleAnalysis>> wordMatches, const SampleQuery &query,
                  std::vector<SampleQueryMatch> &results) {
    querySamplesImpl(wordMatches, query, results);
}

} // namespace adi::library
