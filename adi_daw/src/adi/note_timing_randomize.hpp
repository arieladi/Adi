// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/note_diff.hpp"

namespace adi::notes {

/// Produce literal note moves with a deterministic offset in ±maximumOffset.
/// The bound is supplied in musical ticks by the tool policy. A note's draw
/// depends only on its ID and the seed, so selection enumeration and record
/// order cannot change it. The caller controls overlap and journal preparation.
[[nodiscard]] std::optional<Diff>
seededTimingDiff(const Snapshot &, std::span<const std::uint64_t> selected,
                 std::int64_t maximumOffset, std::uint64_t seed, std::string &error);

} // namespace adi::notes
