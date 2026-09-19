// SPDX-License-Identifier: GPL-3.0-or-later
//
// The store adapter: a `.adi` becomes a `textproj::Tree`, and the pure layer
// turns that into bytes.
//
// `textproj.hpp` says in its own header comment that this is a separate
// concern and that the determinism-critical parts must be testable without a
// database. The seam stays exactly where it was put:
//
//     Store  --readModel-->  rows::Model  --buildTree-->  Tree  --project-->  text
//             (SQLite)        (a value)       (pure)              (pure)
//
// Only `readModel` touches SQLite, so `buildTree` is a total function of a
// plain value and every shaping decision below is testable by constructing a
// Model by hand. That is not tidiness: the hierarchy, cycle and default-
// omission rules are where this file can be wrong, and none of them need a
// file on disk to exercise.
//
// WHAT IS PROJECTED, AND WHAT IS NOT. Not everything in `schema.sql` is
// rendered yet, and silence about that would be the worst option -- a
// projection missing a table reads as a diff that deleted it. `coverage()`
// names every table with a status and a reason, and a test asserts that the
// manifest and the live schema list exactly the same tables, so a table added
// to the schema cannot quietly go unprojected (TEXT-PROJECTION 10).

#pragma once

#include "adi/store_rows.hpp"
#include "adi/textproj.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace adi {
class Store;
}

namespace adi::textproj {

// ---------------------------------------------------------------------------
// Positions (TEXT-PROJECTION 8)
// ---------------------------------------------------------------------------

/// `bar|beat|tick`, 1-based bar and beat, tick 0-based within the beat.
///
/// Takes the signature map by value rather than living in `textproj.hpp`,
/// because the pure layer has no type for a signature map and inventing one
/// there to hold four integers would put a schema shape in the file that is
/// deliberately free of them. If that trade is wrong the fix is a pure
/// `Meter` type, not a query in here.
///
/// An empty map, or one whose first event is after tick 0, is 4/4 from the
/// start -- the same assumption the engine makes about a missing initial tempo
/// (`snapshot.cpp`), for the same reason: a projector cannot render a warning.
///
/// A signature change part-way through a bar starts a new bar at the change,
/// which is what every DAW does and what a musician reading the file expects.
[[nodiscard]] std::string renderPosition(std::int64_t ticks,
                                         const std::vector<rows::TimeSignature>& sigs);

// ---------------------------------------------------------------------------
// Coverage (TEXT-PROJECTION 10)
// ---------------------------------------------------------------------------

enum class Coverage {
    Projected,   ///< every column is rendered, or is on this table's own exclusion note
    Excluded,    ///< deliberately absent; `reason` says why
};

struct TableCoverage {
    std::string_view table;
    Coverage status;
    std::string_view reason;
};

/// Every table in `schema.sql`, with what this adapter does about it.
[[nodiscard]] std::span<const TableCoverage> coverage();

// ---------------------------------------------------------------------------
// The adapter
// ---------------------------------------------------------------------------

/// Shape a Model into the tree the pure projector consumes. Pure and total:
/// no I/O, and no input makes it fail to return -- including a `parent_id`
/// cycle, which the schema does not forbid and which a naive recursive build
/// turns into a stack overflow on a corrupt file.
[[nodiscard]] Tree buildTree(const rows::Model&);

/// `project(buildTree(readModel(store)))`, which is the whole pipeline and the
/// only call most users want.
[[nodiscard]] Projection projectStore(const Store&);

}  // namespace adi::textproj
