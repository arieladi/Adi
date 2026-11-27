// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/pitch_clip_scale.hpp"

#include "adi/audio/io_audit.hpp"

#include <utility>

namespace adi::clip_edit {

PitchClipScaleCapture capturePitchClipScale(const Store& store, std::int64_t clipId,
                                            std::optional<double> customTonicCents) {
    if (audio::onAudioThread) return {PitchClipScaleProblem::Realtime, {}, {}};
    auto captured = engine::readClipScaleContext(store, clipId);
    if (!captured.context) return {PitchClipScaleProblem::Read, {}, std::move(captured.error)};

    const auto& scale = *captured.context->scale();
    double tonic = static_cast<double>(scale.root) * 100;
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

} // namespace adi::clip_edit
