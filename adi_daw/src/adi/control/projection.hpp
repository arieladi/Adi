// SPDX-License-Identifier: AGPL-3.0-or-later
//
// What a control caller reads (AGT-02; AI-AGENT §4; docs/CONTROL-MCP.md §6.1, §7).
//
// Derived, read-only, never a second source of truth. Unlike the canonical
// text projection -- which by its rule R2 carries no row ids, so that two
// replays project byte-identically -- this view carries ADI's durable integer
// ids (ADR-0232 d6), because an op addresses rows by id and an agent that
// could not see them could not say what to change. It also carries `nextIds`:
// ops never allocate ids (ADR-0021), so a caller creating rows takes them
// from here, and the changeset's stale check guarantees they are still free
// when it applies.
//
// Positions are given twice: ticks, which ops take, and `bar|beat|tick`,
// which is what a model reasons about. Durations are reduced fractions of a
// whole note where they reduce. Note names put middle C (60) at C3, Live's
// convention and ADI's default.

#pragma once

#include "adi/control/jsonrpc.hpp"
#include "adi/store_rows.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace adi::control {

struct ReadScope {
    std::vector<std::int64_t> tracks;   ///< empty: every track
    std::vector<std::int64_t> clips;    ///< level 2: empty means the clips in scope
    std::optional<std::int64_t> fromBar;   ///< 1-based, inclusive
    std::optional<std::int64_t> toBar;     ///< 1-based, exclusive
};

inline constexpr std::size_t kMaxNotesPerRead = 4000;
inline constexpr std::size_t kMaxParamsPerRead = 2000;

/// Levels 0 to 2 (AI-AGENT §4). Level 3 is AGT-07's analysis and is refused
/// by the caller before it reaches here.
[[nodiscard]] Json projectLevel(const rows::Model&, int level, const ReadScope&,
                                std::optional<std::int64_t> head, std::string_view project);

[[nodiscard]] Json manifest(const rows::Model&, std::optional<std::int64_t> head,
                            std::string_view project, std::string_view label,
                            std::string_view fileSchema);

[[nodiscard]] Json graph(const rows::Model&, std::string_view project);

/// The next free id of every table a changeset may add rows to: MAX(id) + 1.
[[nodiscard]] Json nextIds(const rows::Model&);

/// "C3" for 60; "F#-1" for 18.
[[nodiscard]] std::string noteName(std::int64_t key);

/// The first tick of a 1-based bar under the project's meters.
[[nodiscard]] std::int64_t ticksAtBar(std::int64_t bar, const rows::Model&);

}  // namespace adi::control
