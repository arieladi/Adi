// SPDX-License-Identifier: AGPL-3.0-or-later
//
// A built-in device's parameter values in its saved state -- DF-E37 (c),
// ADR-0188 d3.
//
// A built-in's parameters are declared by its canonical patch (`[adi.param]`),
// and a declaration's range can change between versions (DEV-22 moved several
// to Live 12.4's). ADR-0188 d3 says a range change keeps the REAL value,
// clamped, and recomputes the knob's position. The state used to hold only
// positions (normalized), so a project reopened under a new range played other
// values: Saturator's Output at -12 dB came back at -7.2 dB.
//
// So the state ("version": 1, kept for readers that predate this) now carries,
// beside each parameter's position in "params", its value in its own unit in
// "real" and that unit in "units". On load the real value is preferred when its
// unit is still the declaration's: clamped to the range, rounded or snapped by
// the curve, and turned back into a position -- in the one `device.loadState`,
// so one undo restores both. A value whose unit changed (OneShot's Sustain from
// % to dB) falls back to the position. A state from before this field
// (version 1 with positions only) keeps its positions under the new ranges:
// those pre-release projects are accepted as lost, as ENGINE-BUILTINS.md says.
//
// `setParam` honours a value's real part only for the curves where the
// engine's real is the true one (lin, int, menu, toggle): the parameter
// mirror's `real` column is computed linearly from the position
// (`param_ops.cpp`'s fillRequest), which is wrong on a log or exponent curve,
// so there the position is taken (the engine is asked to make it curve-aware).

#pragma once

#include "adi/ops.hpp"
#include "juce/device_model.hpp"
#include "juce/pd_declarations.hpp"

#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace adi::device::builtin_values {

/// The value at a knob position, by the declaration's curve.
[[nodiscard]] double natural(const PdParamDecl& declaration, double normalized) noexcept;
/// The knob position of a value, clamped to the range.
[[nodiscard]] double normalized(const PdParamDecl& declaration, double value) noexcept;
/// A position made canonical: through the value and back, so Int and Menu
/// round and Toggle snaps.
[[nodiscard]] double canonical(const PdParamDecl& declaration, double normalized) noexcept;

/// The position `value` sets, or nothing when it is not a usable value.
/// Its real part is honoured where the engine's real is the true one.
[[nodiscard]] std::optional<double> positionFor(const PdParamDecl& declaration,
                                                const ParamValue& value) noexcept;

/// Writes "params" (positions), "real" (values) and "units" into `state`,
/// keeping its other fields and any ids this declaration does not have.
void write(Payload& state, const std::vector<PdParamDecl>& declarations, const float* positions);

/// Reads the positions to apply, as (index into `declarations`, position).
/// False when a known value is bad; then nothing is to change.
[[nodiscard]] bool read(const Payload& state, const std::vector<PdParamDecl>& declarations,
                        std::vector<std::pair<std::size_t, float>>& out);

}  // namespace adi::device::builtin_values
