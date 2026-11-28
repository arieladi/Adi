// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/pitch_clip_scale.hpp"

#include "adi/audio/io_audit.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace adi::clip_edit {
namespace {

constexpr std::int64_t kPitchClassCount = 12;
constexpr double kCentsPerSemitone = 100;
constexpr std::int64_t kNaturalMinorMask = 0x05ad; // 0, 2, 3, 5, 7, 8, 10

struct ScoredScale {
    PitchClipScaleSuggestion suggestion;
    bool endsOnTonic;
    bool captured;
};

bool scoreScale(std::vector<ScoredScale>& scored, const audio::PitchQuantize& candidate,
                std::span<const double> pitches, double endingPitch, bool captured) {
    const auto degrees = candidate.scale.degrees();
    if (std::none_of(degrees.begin(), degrees.end(), [](const auto& degree) { return degree.inScale; })) return true;

    PitchClipScaleSuggestion suggestion{candidate, 0, 0};
    for (std::size_t index = 0; index < pitches.size(); ++index) {
        const auto pitch = pitches[index];
        const auto nearest = candidate.scale.nearest(pitch, candidate.tonicCents);
        const auto corrected = candidate.scale.nearest(pitch, candidate.tonicCents, true);
        if (!nearest || !corrected) return false;
        if (nearest->inScale) ++suggestion.matchedNotes;
        const auto distance = std::abs(corrected->cents - pitch);
        if (!std::isfinite(distance)) return false;
        suggestion.meanCorrectionCents = std::lerp(suggestion.meanCorrectionCents, distance,
                                                    1.0 / static_cast<double>(index + 1));
    }
    const auto ending = candidate.scale.nearest(endingPitch, candidate.tonicCents);
    scored.push_back({std::move(suggestion), ending && ending->degree == 0, captured});
    return true;
}

bool fitRanksBefore(const ScoredScale& left, const ScoredScale& right) noexcept {
    if (left.suggestion.matchedNotes != right.suggestion.matchedNotes)
        return left.suggestion.matchedNotes > right.suggestion.matchedNotes;
    if (left.suggestion.meanCorrectionCents != right.suggestion.meanCorrectionCents)
        return left.suggestion.meanCorrectionCents < right.suggestion.meanCorrectionCents;
    if (left.endsOnTonic != right.endsOnTonic) return left.endsOnTonic;
    return left.captured && !right.captured;
}

} // namespace

PitchClipScaleCapture capturePitchClipScale(const Store& store, std::int64_t clipId,
                                            std::optional<double> customTonicCents) {
    if (audio::onAudioThread) return {PitchClipScaleProblem::Realtime, {}, {}};
    auto captured = engine::readClipScaleContext(store, clipId);
    if (!captured.context) return {PitchClipScaleProblem::Read, {}, std::move(captured.error)};

    const auto& scale = *captured.context->scale();
    double tonic = static_cast<double>(scale.root) * kCentsPerSemitone;
    if (scale.tuningId) {
        if (!customTonicCents) return {PitchClipScaleProblem::TonicRequired, {}, "Custom tuning requires an absolute tonic in cents"};
        tonic = *customTonicCents;
        // A valid tuning includes unison. Check on that exact pitch so the
        // context itself enforces its supported absolute tonic coordinates.
        if (!captured.context->nearest(tonic, tonic))
            return {PitchClipScaleProblem::InvalidTonic, {}, "Custom tonic is outside supported absolute cents"};
    }
    return {PitchClipScaleProblem::None, audio::PitchQuantize{std::move(*captured.context), tonic}, {}};
}

PitchClipScaleSuggestions suggestPitchClipScales(const audio::PitchAnalysis& analysis, const audio::PitchEditState& edits,
                                                  const audio::PitchQuantize& captured,
                                                  std::span<const std::size_t> displayed,
                                                  std::span<const std::size_t> selected) {
    using audio::PitchEditError;
    const auto validated = audio::validatePitchEdits(analysis, edits);
    if (validated != PitchEditError::None) return {validated, 0, {}};
    const auto* clipScale = captured.scale.scale();
    if (!clipScale || !captured.scale.nearest(captured.tonicCents, captured.tonicCents))
        return {PitchEditError::InvalidScale, 0, {}};

    const auto unknown = [&](auto index) { return index >= analysis.notes.size(); };
    if (std::any_of(displayed.begin(), displayed.end(), unknown) || std::any_of(selected.begin(), selected.end(), unknown))
        return {PitchEditError::UnknownNote, 0, {}};
    std::vector<std::size_t> scope(displayed.begin(), displayed.end());
    std::sort(scope.begin(), scope.end());
    if (!selected.empty()) {
        if (std::any_of(selected.begin(), selected.end(), [&](auto index) {
                return !std::binary_search(scope.begin(), scope.end(), index);
            })) return {PitchEditError::InvalidValue, 0, {}};
        scope.assign(selected.begin(), selected.end());
        std::sort(scope.begin(), scope.end());
    }
    scope.erase(std::unique(scope.begin(), scope.end()), scope.end());

    std::vector<double> pitches;
    pitches.reserve(scope.size());
    double endingPitch = 0;
    std::uint64_t lastEnd = 0;
    for (const auto index : scope) {
        const auto edit = audio::resolvePitchNoteEdit(analysis, edits, index);
        if (!std::isfinite(edit.gainDb)) continue; // Negative infinite gain marks mute.
        const auto pitch = analysis.notes[index].pitchCents + edit.pitchShiftCents;
        pitches.push_back(pitch);
        if (edit.endFrame > lastEnd) {
            lastEnd = edit.endFrame;
            endingPitch = pitch;
        }
    }
    if (pitches.empty()) return {};

    std::vector<ScoredScale> scored;
    scored.reserve(2 * kPitchClassCount + 1);
    if (!scoreScale(scored, captured, pitches, endingPitch, true)) return {PitchEditError::InvalidScale, 0, {}};
    if (!clipScale->tuningId) {
        engine::ClipScale minor;
        minor.id = "minor";
        minor.mask = kNaturalMinorMask;
        const std::array presets{engine::ClipScale{}, std::move(minor)};
        for (auto definition : presets) {
            for (std::int64_t root = 0; root < kPitchClassCount; ++root) {
                const auto tonic = static_cast<double>(root) * kCentsPerSemitone;
                if (root == clipScale->root && definition.id == clipScale->id && definition.mask == clipScale->mask &&
                    tonic == captured.tonicCents) continue;

                definition.root = root;
                std::string error;
                auto context = engine::compileScaleContext(definition, {}, error);
                if (!context || !scoreScale(scored, {std::move(*context), tonic}, pitches, endingPitch, false))
                    return {PitchEditError::InvalidScale, 0, {}};
            }
        }
    }

    std::stable_sort(scored.begin(), scored.end(), fitRanksBefore);
    PitchClipScaleSuggestions result{PitchEditError::None, pitches.size(), {}};
    if (!scored.empty()) {
        auto& candidates = result.candidates.emplace();
        candidates.reserve(scored.size());
        for (auto& scale : scored) candidates.push_back(std::move(scale.suggestion));
    }
    return result;
}

} // namespace adi::clip_edit
