// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/audio/io_audit.hpp"
#include "adi/library/sample_query.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <initializer_list>

namespace {
using namespace adi;
using namespace adi::library;

int failures = 0;
void check(bool ok, const char *message) {
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", message);
    }
}

bool rows(const std::vector<SampleQueryMatch> &matches, std::initializer_list<std::size_t> expected) {
    return std::ranges::equal(matches, expected, {}, &SampleQueryMatch::row);
}

SampleAnalysis sample(audio::SampleKind kind, double length, std::optional<double> bpm,
                      std::optional<SampleKey> key, float brightness, float hits) {
    SampleAnalysis result{};
    result.classification.kind = kind;
    result.lengthSeconds = length;
    result.tempoBpm = bpm;
    result.key = key;
    result.descriptors.brightnessHz = brightness;
    result.descriptors.hitDensityHz = hits;
    result.descriptors.rmsDbfs = -18.f;
    return result;
}

void filters() {
    const auto kick = sample(audio::SampleKind::OneShot, 0.25, {}, SampleKey{0, false}, 80.f, 4.f);
    const auto hat = sample(audio::SampleKind::OneShot, 0.125, {}, {}, 8'000.f, 8.f);
    const auto loop = sample(audio::SampleKind::Loop, 2.0, 120.0, SampleKey{9, true}, 400.f, 2.f);
    const auto field = sample(audio::SampleKind::Unknown, 10.0, {}, {}, 1'500.f, 0.f);
    const std::array<const SampleAnalysis *, 5> names{&kick, &hat, &loop, &field, nullptr};
    std::vector<SampleQueryMatch> result;

    querySamples(names, {}, result);
    check(rows(result, {0, 1, 2, 3, 4}), "ordinary filename matches remain visible while analysis runs");

    SampleQuery query;
    query.kind = audio::SampleKind::OneShot;
    querySamples(names, query, result);
    check(rows(result, {0, 1}), "type uses stored loop and one-shot classification");
    query = {};
    query.lengthSeconds = SampleQueryRange{0.125, 0.25};
    querySamples(names, query, result);
    check(rows(result, {0, 1}), "length endpoints are inclusive and evaluated in seconds");
    query = {};
    query.key = SampleKey{9, true};
    querySamples(names, query, result);
    check(rows(result, {2}), "key requires the selected tonic and mode");
    query = {};
    query.tempoBpm = SampleQueryRange{120.0, 120.0};
    querySamples(names, query, result);
    check(rows(result, {2}), "tempo filter excludes unknown tempo and includes an exact match");
    query = {};
    query.brightnessHz = SampleQueryRange{1'000.0, 9'000.0};
    querySamples(names, query, result);
    check(rows(result, {1, 3}), "brightness filter uses hertz");
    query = {};
    query.hitsPerSecond = SampleQueryRange{2.0, 4.0};
    querySamples(names, query, result);
    check(rows(result, {0, 2}), "hit-density filter uses hits per second");
    query = {};
    query.kind = audio::SampleKind::Loop;
    query.lengthSeconds = SampleQueryRange{1.0, 3.0};
    query.key = SampleKey{9, true};
    query.tempoBpm = SampleQueryRange{120.0, 120.0};
    query.brightnessHz = SampleQueryRange{0.0, 800.0};
    query.hitsPerSecond = SampleQueryRange{2.0, 4.0};
    querySamples(names, query, result);
    check(rows(result, {2}), "all six active filters intersect");
    query.lengthSeconds = SampleQueryRange{3.0, 1.0};
    querySamples(names, query, result);
    check(result.empty(), "a reversed range matches no samples");

    query = {};
    query.limit = 2;
    querySamples(names, query, result);
    check(rows(result, {0, 1}), "unranked limit preserves word-candidate order");
    query.limit = 0;
    querySamples(names, query, result);
    check(result.empty(), "zero limit clears a reusable result");

    const std::array<std::shared_ptr<const SampleAnalysis>, 3> catalog{
        std::make_shared<const SampleAnalysis>(loop), nullptr, std::make_shared<const SampleAnalysis>(hat)};
    query = {};
    query.kind = audio::SampleKind::OneShot;
    querySamples(catalog, query, result);
    check(rows(result, {2}), "the same filters consume an immutable catalog publication directly");
}

void similarity() {
    auto kick = sample(audio::SampleKind::OneShot, 0.25, {}, SampleKey{0, false}, 80.f, 4.f);
    kick.descriptors.spectralEnergy[0] = 1.f;
    kick.descriptors.envelopeEnergy[0] = 1.f;
    auto related = kick;
    related.descriptors.spectralEnergy[0] = 0.94f;
    related.descriptors.spectralEnergy[1] = 0.06f;
    const auto tied = related;
    auto hat = kick;
    hat.descriptors.spectralEnergy[0] = 0.f;
    hat.descriptors.spectralEnergy[7] = 1.f;
    auto atLimit = kick;
    atLimit.lengthSeconds = 60.0;
    auto overLimit = kick;
    overLimit.lengthSeconds = 60.001;
    const std::array<const SampleAnalysis *, 7> names{&hat, &kick, &related, &tied, nullptr, &atLimit, &overLimit};
    std::vector<SampleQueryMatch> result;
    SampleQuery query;
    query.rank = SampleQueryRank::Similar;
    query.reference = &kick;
    query.limit = 2;
    querySamples(names, query, result);
    check(rows(result, {2, 3}), "the two closest kicks beat the hat and a tie keeps word-candidate order");
    query.limit = names.size();
    querySamples(names, query, result);
    check(rows(result, {2, 3, 5, 0}), "the reference, queued files and files above Live's 60-second limit are omitted");
    query.reference = &overLimit;
    querySamples(names, query, result);
    check(result.empty(), "a reference longer than 60 seconds is ineligible");
    query.reference = &kick;
    query.lengthSeconds = SampleQueryRange{1.0, 60.0};
    querySamples(names, query, result);
    check(rows(result, {5}), "ranked queries respect measured filters and accept the exact 60-second boundary");

    auto sharedStart = hat;
    sharedStart.lengthSeconds = 3.0;
    sharedStart.similarityPrefix = kick.descriptors;
    auto sharedEnd = kick;
    sharedEnd.lengthSeconds = 3.0;
    sharedEnd.similarityPrefix = hat.descriptors;
    const std::array<const SampleAnalysis *, 2> parts{&sharedEnd, &sharedStart};
    querySamples(parts, query, result);
    check(rows(result, {1, 0}), "the two-second similarity prefix controls timbre even when later audio differs");
}

void complement() {
    auto bass = sample(audio::SampleKind::Loop, 2.0, 120.0, SampleKey{0, false}, 80.f, 2.f);
    bass.descriptors.maskingEnergy[4] = 1.f;
    bass.descriptors.onsetDensity[0] = 1.f;
    auto secondBass = bass;
    secondBass.descriptors.rmsDbfs = -24.f;
    auto hat = secondBass;
    hat.descriptors.maskingEnergy[4] = 0.f;
    hat.descriptors.maskingEnergy[32] = 1.f;
    auto gap = hat;
    gap.descriptors.onsetDensity[0] = 0.f;
    gap.descriptors.onsetDensity[4] = 1.f;
    auto tritone = gap;
    tritone.key = SampleKey{6, false};
    const std::array<const SampleAnalysis *, 4> names{&secondBass, &tritone, &hat, &gap};
    std::vector<SampleQueryMatch> result;
    SampleQuery query;
    query.rank = SampleQueryRank::Fits;
    query.reference = &bass; // The loop can be a clip outside these filename results.
    querySamples(names, query, result);
    check(rows(result, {3, 2, 1, 0}), "Fits rewards the spectral opening, onset gap and compatible key");
    if (result.size() == names.size()) {
        const double onsetBenefit = result[0].score - result[1].score;
        const double tritoneCost = result[0].score - result[2].score;
        check(onsetBenefit > 0.0 && tritoneCost > 0.0, "ranking exposes scores for each ordered result");
        std::printf("Fits scores: gap %.3f, hat %.3f, tritone %.3f, second bass %.3f\n", result[0].score,
                    result[1].score, result[2].score, result[3].score);
    }
    querySamples(names, query, result);
    check(rows(result, {3, 2, 1, 0}), "reusing a result produces the same ordering");
    audio::onAudioThread = true;
    querySamples(names, {}, result);
    audio::onAudioThread = false;
    check(rows(result, {3, 2, 1, 0}), "calling from the audio callback leaves result storage untouched");
}

void comparableLoudness() {
    auto reference = sample(audio::SampleKind::OneShot, 1.0, {}, {}, 80.f, 1.f);
    reference.integratedLufs = -20.f;
    reference.descriptors.rmsDbfs = -10.f;
    auto sameRms = reference;
    sameRms.integratedLufs = -40.f;
    auto sameLoudness = reference;
    sameLoudness.descriptors.rmsDbfs = -25.f;
    const std::array<const SampleAnalysis *, 2> names{&sameRms, &sameLoudness};
    SampleQuery query;
    query.rank = SampleQueryRank::Similar;
    query.reference = &reference;
    std::vector<SampleQueryMatch> result;
    querySamples(names, query, result);
    check(rows(result, {1, 0}), "comparable integrated loudness takes precedence for short samples");

    reference.lengthSeconds = sameRms.lengthSeconds = sameLoudness.lengthSeconds = 3.0;
    reference.similarityPrefix = reference.descriptors;
    sameRms.similarityPrefix = sameRms.descriptors;
    sameLoudness.similarityPrefix = sameLoudness.descriptors;
    querySamples(names, query, result);
    check(rows(result, {0, 1}), "a longer sample uses prefix RMS in place of whole-file loudness");
}
} // namespace

int main() {
    filters();
    similarity();
    complement();
    comparableLoudness();
    return failures != 0 ? 1 : 0;
}
