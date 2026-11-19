// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/render/job_preset.hpp"
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace adi::render {
static_assert(std::is_nothrow_move_assignable_v<Job>);

void applyJobPreset(Job& target, const Job& preset, PresetFields fields) {
    if (fields != PresetFields::BoundsAndOutput && fields != PresetFields::OptionsAndFormat &&
        fields != PresetFields::All)
        throw std::invalid_argument("unknown export preset field group");
    const bool bounds = fields != PresetFields::OptionsAndFormat;
    const bool options = fields != PresetFields::BoundsAndOutput;
    // No project UUID or cross-project track/region relocation contract exists
    // in Job. Do not infer identity through canonicalization or file access.
    if (bounds && target.project.native() != preset.project.native())
        throw std::invalid_argument("preset bounds/source require the same project spelling; no ID relocation is available");
    if (fields != PresetFields::All && target.niStems.has_value() != preset.niStems.has_value())
        throw std::invalid_argument("changing WAV/NI layout requires All preset fields");
    // Explicit bounds are in session.sampleRate units. A partial application
    // must not silently reinterpret them. A region resolves its own bounds at
    // render start, and All transfers the bounds and their clock together.
    if (preset.session.sampleRate != target.session.sampleRate &&
        ((bounds && !options && !preset.regionId) || (options && !bounds && !target.regionId)))
        throw std::invalid_argument("partial preset would change the clock of explicit sample bounds");

    Job candidate = target;
    if (bounds) {
        candidate.directory = preset.directory;
        candidate.nameTemplate = preset.nameTemplate;
        candidate.beginSample = preset.beginSample;
        candidate.endSample = preset.endSample;
        candidate.regionId = preset.regionId;
        candidate.source = preset.source;
    }
    if (options) {
        candidate.session.channels = preset.session.channels;
        candidate.session.sampleRate = preset.session.sampleRate;
        candidate.session.maxFrames = preset.session.maxFrames;
        candidate.session.precision = preset.session.precision;
        candidate.session.delayCompensation = preset.session.delayCompensation;
        candidate.encoding = preset.encoding;
        candidate.normalization = preset.normalization;
    }
    if (fields == PresetFields::All) candidate.niStems = preset.niStems;
    else if (candidate.niStems) {
        if (bounds) candidate.niStems->stems = preset.niStems->stems;
        if (options) candidate.niStems->bitRate = preset.niStems->bitRate;
    }
    validateJob(candidate);
    if (candidate.session.externalTempoBpm)
        throw std::invalid_argument("live external tempo is not a render preset target");
    target = std::move(candidate);
}
} // namespace adi::render
