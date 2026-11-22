// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/note_diff.hpp"
namespace adi::engine { class ScaleContext; }
namespace adi::notes {
struct QuantizeSettings {
    std::int64_t gridTicks = 0, originTicks = 0;
    // Fixed-point percent: 10000 means 100.00%. Swing delays odd divisions by
    // up to half a division. Integers make the tick arithmetic ABI independent.
    int strength = 10000, swing = 0;
    bool starts = true, ends = false;
};
OpRequest quantizeRequest(std::int64_t clip, std::span<const std::uint64_t> ids, const QuantizeSettings &);
OpRequest transposeRequest(std::int64_t clip, std::span<const std::uint64_t> ids, int semitones);
/// Pitch-only edit of the explicit selection. An empty selection is a no-op.
/// Implicit 12-TET uses the context's root unless a tonic is supplied.
/// A custom tuning requires the caller's absolute tonic in cents.
[[nodiscard]] PreparedDiff prepareFitToScale(const Snapshot &, std::span<const std::uint64_t> ids,
                                              const engine::ScaleContext &,
                                              std::optional<double> tonicCents = std::nullopt);
/// Read the clip's local or inherited scale and note selection in one owner
/// snapshot. Commit through commitDiff; the journal keeps the literal pitches.
[[nodiscard]] PreparedDiff prepareClipFitToScale(Store &, std::int64_t clip,
                                                  std::span<const std::uint64_t> ids,
                                                  std::optional<double> tonicCents = std::nullopt);
/// Reflect selected absolute pitches about the supplied chromatic axis in
/// cents. Preserve note IDs, positions, durations and expression.
[[nodiscard]] PreparedDiff prepareInvertPitches(const Snapshot &, std::span<const std::uint64_t> ids,
                                                 double axisCents);
std::span<const OpDescriptor> transformOps();
} // namespace adi::notes
