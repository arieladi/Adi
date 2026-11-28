// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/pitch_audition.hpp"

#include "adi/audio/io_audit.hpp"

#include <algorithm>
#include <cmath>
#include <new>
#include <stdexcept>
#include <utility>

namespace adi::audio {
namespace {

PitchAuditionResult failure(PitchRenderError error) noexcept { return {{error, nullptr}, {}, {}}; }

PitchNoteEdit applyBypasses(const PitchNote& note, const PitchNoteEdit& authored,
                            const PitchAuditionSpec& spec) {
    const auto start = spec.bypassSegmentTiming ? note.startFrame : authored.startFrame;
    const auto end = spec.bypassSegmentTiming ? note.endFrame : authored.endFrame;
    if (spec.bypassPitchFormantGain) return {authored.noteIndex, start, end};
    auto result = authored;
    result.startFrame = start;
    result.endFrame = end;
    return result;
}

PitchAuditionResult finish(PitchRenderResult rendered, PitchAuditionRange range, bool loop) {
    const auto loopBounds = loop ? std::optional(PitchAuditionRange{0, range.endFrame - range.startFrame}) : std::nullopt;
    return {std::move(rendered), range, loopBounds};
}

} // namespace

PitchAuditionResult renderPitchAudition(std::span<const float> mono, const PitchAnalysis& analysis,
                                       const PitchEditState& authored, const PitchAuditionSpec& spec,
                                       std::stop_token stop) {
    if (onAudioThread) return failure(PitchRenderError::Realtime);
    if (stop.stop_requested()) return failure(PitchRenderError::Cancelled);
    auto range = spec.clipRange;
    if (mono.size() != analysis.sourceFrames || range.startFrame >= range.endFrame || range.endFrame > mono.size())
        return failure(PitchRenderError::InvalidSource);

    try {
        const auto validation = validatePitchEdits(analysis, authored);
        if (validation == PitchEditError::InvalidAnalysis) return failure(PitchRenderError::InvalidSource);
        if (validation != PitchEditError::None) return failure(PitchRenderError::InvalidEdits);

        if (!spec.selectedNotes.empty()) {
            PitchAuditionRange selected{range.endFrame, range.startFrame};
            for (const auto index : spec.selectedNotes) {
                if (stop.stop_requested()) return failure(PitchRenderError::Cancelled);
                if (index >= analysis.notes.size()) return failure(PitchRenderError::InvalidEdits);
                const auto edit = resolvePitchNoteEdit(analysis, authored, index);
                const auto audible = applyBypasses(analysis.notes[index], edit, spec);
                const auto start = std::max(range.startFrame, audible.startFrame);
                const auto end = std::min(range.endFrame, audible.endFrame);
                if (start < end) {
                    selected.startFrame = std::min(selected.startFrame, start);
                    selected.endFrame = std::max(selected.endFrame, end);
                }
            }
            if (selected.startFrame >= selected.endFrame) return failure(PitchRenderError::InvalidSource);
            range = selected;
        }

        PitchEditState filtered;
        const bool compare = spec.bypassPitchFormantGain || spec.bypassSegmentTiming;
        const bool sourceOnly = spec.bypassPitchFormantGain && spec.bypassSegmentTiming;
        if (compare && !sourceOnly) {
            filtered.notes.reserve(authored.notes.size());
            for (const auto& edit : authored.notes) {
                if (stop.stop_requested()) return failure(PitchRenderError::Cancelled);
                const auto& note = analysis.notes[edit.noteIndex];
                auto audible = applyBypasses(note, edit, spec);
                if (audible != PitchNoteEdit{edit.noteIndex, note.startFrame, note.endFrame})
                    filtered.notes.push_back(std::move(audible));
            }
        }

        const auto& edits = compare ? filtered : authored;
        PitchRenderResult rendered;
        std::span<const float> pcm = mono;
        if (!edits.notes.empty()) {
            rendered = renderPitchEdits(mono, analysis, edits, stop);
            if (!rendered) return failure(rendered.error);
            if (range.startFrame == 0 && range.endFrame == mono.size())
                return finish(std::move(rendered), range, spec.loop);
            pcm = *rendered.samples;
        }
        const auto window = pcm.subspan(static_cast<std::size_t>(range.startFrame),
                                       static_cast<std::size_t>(range.endFrame - range.startFrame));
        auto cropped = std::make_unique<std::vector<float>>(window.size());
        for (std::size_t frame = 0; frame < window.size(); ++frame) {
            if (frame % 4096 == 0 && stop.stop_requested()) return failure(PitchRenderError::Cancelled);
            const float value = window[frame];
            (*cropped)[frame] = std::isfinite(value) ? value : 0.0f;
        }
        if (stop.stop_requested()) return failure(PitchRenderError::Cancelled);
        return finish({PitchRenderError::None, std::move(cropped)}, range, spec.loop);
    } catch (const std::bad_alloc&) {
        return failure(PitchRenderError::Resource);
    } catch (const std::length_error&) {
        return failure(PitchRenderError::Resource);
    }
}

} // namespace adi::audio
