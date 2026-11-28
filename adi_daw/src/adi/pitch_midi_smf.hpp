// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/interchange/smf.hpp"
#include "adi/pitch_midi.hpp"

#include <cstdint>
#include <optional>
#include <stop_token>

namespace adi::clip_edit {

struct PitchMidiSmfOptions {
    std::uint16_t ticksPerQuarter;
    std::optional<std::uint32_t> microsecondsPerQuarter{};
    std::stop_token stop{};
};

// Detached format-0 SMF. Note times are clip-relative and rounded to the
// requested PPQ, with ties rounded up. A note must retain positive duration.
// Events that meet at the same output tick keep source order. An explicit
// tempo inserts a Set Tempo at zero. With no tempo event, SMF's default 120 BPM
// applies. No project tempo map or placement is exported.
[[nodiscard]] PitchMidiResult<interchange::smf::Bytes> encodePitchMidiChannelBendSmf(
    const PitchMidiProjection&, PitchMidiBendMode, int rangeSemitones, const PitchMidiSmfOptions&);

} // namespace adi::clip_edit
