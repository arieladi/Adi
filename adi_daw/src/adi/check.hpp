// SPDX-License-Identifier: GPL-3.0-or-later
//
// Integrity checking for a `.adi`. ADR-0029 flagged the need; this is it.
//
// SQLite enforces a great deal for us — foreign keys, CHECK constraints, and
// since ADR-0029 the column types too. What it structurally CANNOT enforce is
// what this file is for:
//
//   * POLYMORPHIC REFERENCES. `routing.(src_kind, src_id)` and six others like
//     it name a row in a table chosen at runtime. A FOREIGN KEY cannot express
//     that, which is the price of one routing table instead of six (ADR-0029).
//     Nothing detects a routing row pointing at a deleted track.
//   * WHAT IS INSIDE A BLOB. A notes blob is opaque to SQL. Whether its header
//     is well-formed, whether its rec_size is a size some writer released
//     (ADR-0023), and whether a note_expression row names a note that actually
//     exists in that clip, are all invisible to the database.
//   * STRUCTURAL INVARIANTS ACROSS ROWS. Whether the undo tree has a cycle,
//     whether the current branch's head names a real op.
//
// A reader that trusts these is a reader that will one day render silence and
// not say why.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace adi {

class Store;

enum class Severity { Error, Warning, Info };

const char* toString(Severity);

struct Finding {
    Severity severity = Severity::Error;
    std::string code;    // stable, greppable: "ref.dangling", "blob.malformed"
    std::string where;   // "routing#7.src"
    std::string detail;
};

struct CheckReport {
    std::vector<Finding> findings;
    int errors = 0;
    int warnings = 0;
    int checksRun = 0;
    [[nodiscard]] bool clean() const { return errors == 0; }
};

/// Every check. Read-only: it never repairs, because a repair that guesses is
/// how a corrupt project becomes a plausible-looking wrong one.
CheckReport checkProject(const Store&);

}  // namespace adi
