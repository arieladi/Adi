// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/clip_commands.hpp"
#include "adi/media/clip_pitch.hpp"
#include "adi/pitch_edit_store.hpp"

#include <optional>
#include <stop_token>
#include <string>

namespace adi::clip_edit {

enum class PitchReanalysisPolicy { PreserveCompatible, ResetAuthored };

struct PitchReanalysisResult {
    media::ClipPitchResult rebuilt;
    // Present in preserve mode when an authored record exists. A failed
    // resolution names the dormant record while fresh analysis stays usable.
    std::optional<ClipPitchEditsResolve> kept;
    // Present only for an explicit reset of an existing record. Use
    // commitPrepared on the same live project generation for one Undo step.
    std::optional<PreparedEdit> clear;
    std::optional<std::string> error; // preparation or authored-record read

    [[nodiscard]] explicit operator bool() const noexcept { return rebuilt && !error; }
};

// Owner-worker capture. Reanalyze the verified source independently of the
// existing optional analysis cache, then inspect authored state in the same
// project snapshot. Preparation writes no project rows or ADIA sections.
// Source decoding may populate the ordinary PCM decode cache.
[[nodiscard]] PitchReanalysisResult preparePitchReanalysis(const Store&, std::int64_t clipId,
                                                            PitchReanalysisPolicy, std::stop_token = {},
                                                            audio::DecodeCache* = nullptr);

} // namespace adi::clip_edit
