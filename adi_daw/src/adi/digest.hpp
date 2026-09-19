// SPDX-License-Identifier: GPL-3.0-or-later
//
// A canonical digest of a project's STATE, for use as an oracle.
//
// ADR-0021 §7.4 requires a replay test: apply an op log twice, under
// deliberately different UI state, and assert the results are identical. That
// needs something to compare, and "identical" has to mean identical in the way
// that matters.
//
// This renders the project tier of a `.adi` into one canonical string, ordered
// by content rather than by storage, so two databases built by different routes
// compare equal exactly when they hold the same project.
//
// WHAT IT DELIBERATELY EXCLUDES, and why each exclusion is the point rather
// than a convenience:
//
//   ops, op_branches   The log is not the project. Replaying a log produces new
//                      seq numbers and new timestamps; requiring those to match
//                      would test the clock, not the ops.
//   session_state,     UI state. ADR-0021 exists because ops must NOT read
//   ui_view,           ambient state — so the replay test SETS THESE
//   window_state       DIFFERENTLY in the two projects on purpose. Excluding
//                      them is what makes a match meaningful: it proves the ops
//                      did not consume them.
//   adi_meta,          Creation timestamps, host, pid. Volatile by nature.
//   session_lock
//
// Everything else — every table that holds music — is compared in full,
// including columns nothing reads yet. A digest that only covered what some
// renderer happens to emit would pass while the two projects differed.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace adi {

class Store;

struct Digest {
    /// The canonical rendering. Compared directly in tests, because a string
    /// diff says *what* differs and a hash mismatch does not.
    std::string text;
    /// A short stable fingerprint of `text`, for logs and CLI output.
    std::string fingerprint;
    /// Tables actually present and digested.
    int tableCount = 0;
    int rowCount = 0;
};

/// Canonical digest of the project tier. Deterministic: the same project
/// produces the same string regardless of insertion order, rowid assignment or
/// query plan.
Digest digestProject(const Store&);

/// The tables a digest covers, in canonical order. Exposed so a test can assert
/// the exclusion list is what it thinks it is — an oracle that silently stopped
/// covering a table would pass everything.
std::vector<std::string> digestedTables(const Store&);

}  // namespace adi
