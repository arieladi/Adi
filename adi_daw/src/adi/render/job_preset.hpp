// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/render/queue.hpp"

namespace adi::render {
enum class PresetFields { BoundsAndOutput = 1, OptionsAndFormat = 2, All = 3 };
// Pure owner-thread value edit. No file access, enqueue, rendering or ID mapping.
// The target's project and runtime session bindings remain its own. Copy only
// the selected field groups, validate the complete candidate, then publish it
// by a nonthrowing move. Any exception leaves both arguments unchanged.
// See RENDER-JOB-PRESETS.md for clock, project and NI-layout refusal boundaries.
void applyJobPreset(Job& target, const Job& preset, PresetFields);
} // namespace adi::render
