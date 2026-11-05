// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/ops.hpp"
#include "adi/engine/scale_context.hpp"
namespace adi::tunings {
// Pure owner-side payload construction. IDs and every authored value are
// explicit; no file, selection, playhead, preset or identity reservation.
Payload definitionPayload(const engine::TuningDefinition&);
Payload scalePayload(const engine::ClipScale&);
std::span<const OpDescriptor> ops();
} // namespace adi::tunings
