// SPDX-License-Identifier: AGPL-3.0-or-later
//
// A proposal waiting for review was built on one head of the history tree.
// When the song has moved on, the AI mode says how far (ADR-0261 d7): the
// number of edits since, and whether Undo (or Redo) brings the project back to
// the point where the proposal is current again. A stale proposal is never
// rebased (ADR-0148 d4); this only tells the user where it stands.

#pragma once

#include "adi/control/project_access.hpp"
#include "adi/history.hpp"

#include <cstdint>
#include <optional>

namespace adi::control {

/// Reads `timeline` only. Steps are whole undo steps (transactions), as the
/// Edit menu counts them; the base must be a step boundary, which every head
/// a project read returns is.
[[nodiscard]] ProposalFreshness proposalFreshness(const History::Timeline& timeline,
                                                  std::optional<std::int64_t> baseHead);

}  // namespace adi::control
