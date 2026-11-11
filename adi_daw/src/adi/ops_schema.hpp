// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The op registry describes itself (CTL-02; ADR-0016, ADR-0039 d2, ADR-0168 d5.1).
//
// The wire schema and the agent's `ops_describe` are GENERATED from the
// registry's Fields and never written twice. The generated schema and
// validateSubmission are two readings of one declaration: a payload passes the
// schema exactly when the boundary accepts it (AC-24, tests/test_ops_schema.cpp).
//
// JSON Schema 2020-12, closed objects:
//   * `additionalProperties: false`, as validateSubmission refuses unknown keys;
//   * an optional field's type is `[T, "null"]`, as an explicit null clears it;
//   * Int is `integer`, from INT64_MIN to UINT64_MAX unless narrower: what
//     nlohmann holds as a signed or an unsigned integer (note ids are uint64);
//   * Remote (the control boundary) adds MAGDA's request limits: text at most
//     16 KiB characters, arrays at most 4096 elements, unless declared.

#pragma once

#include "adi/ops.hpp"

namespace adi {

/// The closed input schema of one op's payload.
[[nodiscard]] Payload opInputSchema(const OpDescriptor&, Bounds = Bounds::Remote);

/// The boundary's normalisation before validateSubmission. JSON does not tell
/// `1` from `1.0`, and JSON Schema calls both an integer, so an Int field
/// holding a float with no fractional part (from -2^63 up to, not including,
/// 2^64) becomes that integer. Everything else is left for validation to refuse.
/// Recurses into declared `items`.
void normalizeSubmission(const OpDescriptor&, Payload&);

/// One op as `ops_describe` and `adi_tool ops --schema` show it:
/// `{name, summary, scope, engineImpact, undoable, coalescable, inverse,
///   inputSchema}`.
[[nodiscard]] Payload describeOp(const OpDescriptor&, Bounds = Bounds::Remote);

}  // namespace adi
