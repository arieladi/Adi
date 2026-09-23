// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <optional>
#include <span>

namespace adi::engine {
// A caller-owned sample-domain tempo map, distinct from snapshot TempoEvent's
// tick-domain map. Tempo is constant from this point until the next one.
struct PlayTempoPoint { std::int64_t sample; double bpm; };
inline constexpr std::int64_t playQuantizePpq = 960;

// ADR-0133 d4: pure, allocation-free release calculation; no queue/graph wiring.
// The map begins at sample 0 and is strictly ordered, with finite positive BPM.
// Grid phase begins at sample 0 and continues across tempo changes (never resets).
// gridTicks is in 960-PPQ ticks: 240 = sixteenth, 160 = sixteenth-note triplet.
// Fractional grid samples round UP to the first realizable sample. A note on
// that sample, or <= forgivenessSamples after it, is released immediately.
// Otherwise the next grid line is integrated through every intervening tempo
// change. No block-relative position is accepted: callers pass absolute samples.
// Returns nullopt for invalid input or an unrepresentable release sample.
[[nodiscard]] std::optional<std::int64_t> playQuantizeRelease(
    std::int64_t eventSample, std::span<const PlayTempoPoint> tempo,
    std::uint32_t sampleRate, double gridTicks,
    std::int64_t forgivenessSamples = 0) noexcept;
} // namespace adi::engine
