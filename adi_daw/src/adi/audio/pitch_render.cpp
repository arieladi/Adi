// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/pitch_render.hpp"

#include "adi/audio/io_audit.hpp"

#include <rubberband/RubberBandStretcher.h>

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace adi::audio {
namespace {

constexpr std::size_t processFrames = 4096;
constexpr std::uint64_t maxSourceFrames = (std::uint64_t{1} << 31) - 1;
constexpr std::uint32_t maxStretcherRate = 192000;
constexpr double maxStretchRatio = 16;
constexpr double maxPitchRatio = 16;
constexpr std::uint32_t fadeFramesPerSecond = 400; // 2.5 ms

void checkCancelled(std::stop_token stop) {
    if (stop.stop_requested()) throw PitchRenderError::Cancelled;
}

void checkRatio(double ratio, double maximum) {
    if (!std::isfinite(ratio) || ratio < 1 / maximum || ratio > maximum)
        throw PitchRenderError::Unsupported;
}

bool copyFinite(std::span<const float> source, std::span<float> destination, std::stop_token stop) {
    bool allFinite = true;
    for (std::size_t frame = 0; frame < source.size(); ++frame) {
        if (frame % processFrames == 0) checkCancelled(stop);
        const float sample = source[frame];
        const bool finite = std::isfinite(sample);
        destination[frame] = finite ? sample : 0.0f;
        allFinite = allFinite && finite;
    }
    return allFinite;
}

std::span<const float> noteSamples(std::span<const float> source, const PitchNote& note) {
    return source.subspan(static_cast<std::size_t>(note.startFrame), static_cast<std::size_t>(note.endFrame - note.startFrame));
}

std::vector<float> stretch(std::span<const float> source, std::size_t frames, std::uint32_t rate,
                           double pitchRatio, double formantRatio, std::stop_token stop) {
    using Stretcher = RubberBand::RubberBandStretcher;
    const double ratio = static_cast<double>(frames) / static_cast<double>(source.size());
    checkRatio(ratio, maxStretchRatio);
    checkRatio(pitchRatio, maxPitchRatio);
    constexpr auto options = Stretcher::OptionProcessOffline | Stretcher::OptionEngineFiner |
        Stretcher::OptionThreadingNever | Stretcher::OptionFormantPreserved;
    Stretcher stretcher(rate, 1, options, ratio, pitchRatio);
    if (stretcher.getEngineVersion() != 3) throw PitchRenderError::Process;
    stretcher.setDebugLevel(0);
    stretcher.setExpectedInputDuration(source.size());
    stretcher.setMaxProcessSize(processFrames);
    if (formantRatio != 1) stretcher.setFormantScale(formantRatio / pitchRatio);

    for (std::size_t frame = 0; frame < source.size();) {
        checkCancelled(stop);
        const std::size_t count = std::min(processFrames, source.size() - frame);
        const float* input = source.data() + frame;
        frame += count;
        stretcher.study(&input, count, frame == source.size());
    }
    std::vector<float> output(frames);
    std::size_t written = 0;
    for (std::size_t frame = 0; frame < source.size();) {
        checkCancelled(stop);
        const std::size_t count = std::min(processFrames, source.size() - frame);
        const float* input = source.data() + frame;
        frame += count;
        stretcher.process(&input, count, frame == source.size());
        for (int available = stretcher.available(); available > 0; available = stretcher.available()) {
            checkCancelled(stop);
            const auto requested = static_cast<std::size_t>(available);
            if (requested > frames - written) throw PitchRenderError::Process;
            float* destination = output.data() + written;
            if (stretcher.retrieve(&destination, requested) != requested) throw PitchRenderError::Process;
            written += requested;
        }
    }
    if (written != frames || stretcher.available() != -1) throw PitchRenderError::Process;
    return output;
}

std::vector<float> shiftCurve(std::span<const float> source, std::uint32_t rate, const PitchNote& note,
                              const PitchNoteEdit& edit, const PitchNoteEdit* preceding, std::stop_token stop) {
    using Stretcher = RubberBand::RubberBandStretcher;
    constexpr auto options = Stretcher::OptionProcessRealTime | Stretcher::OptionEngineFiner |
        Stretcher::OptionThreadingNever | Stretcher::OptionPitchHighConsistency |
        Stretcher::OptionFormantPreserved;
    const auto pitchAt = [&](std::size_t frame) {
        const auto sourceFrame = note.startFrame + std::min(frame, source.size() - 1);
        const double cents = pitchCorrectionCents(note, edit, static_cast<double>(sourceFrame), preceding);
        const double ratio = std::exp2(cents / 1200);
        checkRatio(ratio, maxPitchRatio);
        return ratio;
    };
    const double formant = std::exp2(edit.formantShiftCents / 1200);
    const double startingPitch = pitchAt(0);
    Stretcher stretcher(rate, 1, options, 1, startingPitch);
    if (stretcher.getEngineVersion() != 3) throw PitchRenderError::Process;
    stretcher.setDebugLevel(0);
    // High Consistency keeps the resampler after the phase vocoder on both
    // sides of unity. Time the pitch automation on the compensated output
    // clock. The streaming mode is only driven by this offline worker.
    const std::size_t controlFrames = rate / 200;
    stretcher.setMaxProcessSize(controlFrames);
    const std::size_t pad = stretcher.getPreferredStartPad();
    const std::size_t delay = stretcher.getStartDelay();
    std::vector<float> output(source.size());
    std::vector<float> silence(controlFrames);
    std::size_t written = 0;
    std::size_t discard = delay;
    const auto target = pad + source.size() + delay + processFrames;
    for (std::size_t frame = 0; frame < target && written < source.size();) {
        checkCancelled(stop);
        std::size_t count = std::min(controlFrames, target - frame);
        const float* input = silence.data();
        if (frame < pad) count = std::min(count, pad - frame);
        else if (frame - pad < source.size()) {
            count = std::min(count, source.size() - (frame - pad));
            input = source.data() + (frame - pad);
        }
        const double pitch = pitchAt(written);
        stretcher.setPitchScale(pitch);
        if (formant != 1) stretcher.setFormantScale(formant / pitch);
        frame += count;
        stretcher.process(&input, count, frame == target);
        for (int available = stretcher.available(); available > 0 && written < source.size(); available = stretcher.available()) {
            auto ready = static_cast<std::size_t>(available);
            if (discard > 0) {
                const auto skipped = std::min({discard, ready, controlFrames});
                float* destination = silence.data();
                if (stretcher.retrieve(&destination, skipped) != skipped) throw PitchRenderError::Process;
                std::fill_n(destination, skipped, 0.0f);
                discard -= skipped;
                ready -= skipped;
            }
            if (discard == 0 && ready > 0) {
                const auto requested = std::min(ready, source.size() - written);
                float* destination = output.data() + written;
                if (stretcher.retrieve(&destination, requested) != requested) throw PitchRenderError::Process;
                written += requested;
            }
        }
    }
    if (written != source.size()) throw PitchRenderError::Process;
    return output;
}

bool changesTime(const PitchNote& note, const PitchNoteEdit& edit) {
    return note.startFrame != edit.startFrame || note.endFrame != edit.endFrame;
}

double fadeWeight(std::size_t frame, std::size_t count, std::uint32_t rate) {
    const auto edge = std::min(frame, count - 1 - frame);
    const auto width = std::min<std::size_t>((count - 1) / 2, rate / fadeFramesPerSecond);
    return width == 0 || edge >= width ? 1 : static_cast<double>(edge) / static_cast<double>(width);
}

void overlay(std::span<float> destination, std::span<const float> note, std::uint32_t rate, double gain,
             std::stop_token stop) {
    constexpr double sampleLimit = std::numeric_limits<float>::max();
    for (std::size_t frame = 0; frame < destination.size(); ++frame) {
        if (frame % processFrames == 0) checkCancelled(stop);
        const double weight = fadeWeight(frame, destination.size(), rate);
        const double signal = note.empty() ? 0 : static_cast<double>(note[frame]);
        if (!std::isfinite(signal)) throw PitchRenderError::Process;
        const double mixed = destination[frame] * (1 - weight) + signal * gain * weight;
        destination[frame] = static_cast<float>(std::clamp(mixed, -sampleLimit, sampleLimit));
    }
}

void renderNote(std::span<const float> source, std::span<float> destination, std::uint32_t rate, const PitchNote& note,
                const PitchNoteEdit& edit, const PitchNoteEdit* preceding, std::stop_token stop) {
    const double gain = std::pow(10, edit.gainDb / 20);
    if (gain == 0) {
        overlay(destination, {}, rate, 0, stop);
        return;
    }

    const bool timeStretch = source.size() != destination.size();
    const bool changingOtherPitch = edit.drift != 1 || edit.tiltCents != 0 ||
        edit.tiltGeometry.startCents != 0 || edit.tiltGeometry.endCents != 0 ||
        (preceding != nullptr && edit.transitionFrames != 0 && edit.pitchShiftCents != 0 &&
         preceding->pitchShiftCents != edit.pitchShiftCents);
    const bool changingPitch = edit.straighten != 0 || changingOtherPitch;
    const bool needsDsp = changingPitch || timeStretch || edit.pitchShiftCents != 0 || edit.formantShiftCents != 0;
    if (needsDsp && rate > maxStretcherRate) throw PitchRenderError::Unsupported;
    if (!needsDsp && gain == 1 && !changesTime(note, edit)) return;

    std::vector<float> processed;
    if (changingPitch) {
        processed = shiftCurve(source, rate, note, edit, preceding, stop);
        if (timeStretch) processed = stretch(processed, destination.size(), rate, 1, 1, stop);
    } else if (needsDsp) {
        const double pitch = std::exp2(edit.pitchShiftCents / 1200);
        const double formant = std::exp2(edit.formantShiftCents / 1200);
        processed = stretch(source, destination.size(), rate, pitch, formant, stop);
    }
    std::span<const float> signal = processed.empty() ? source : std::span<const float>(processed);
    if (edit.straightenRange && edit.straighten != 0 && !changingOtherPitch && edit.pitchShiftCents == 0 &&
        edit.formantShiftCents == 0 && gain == 1 && !changesTime(note, edit)) {
        // Even a unity-ratio vocoder introduces processing outside the
        // requested span. Keep original samples where there is no edit.
        const auto first = static_cast<std::size_t>(edit.straightenRange->startFrame - note.startFrame);
        const auto count = static_cast<std::size_t>(edit.straightenRange->endFrame - edit.straightenRange->startFrame);
        destination = destination.subspan(first, count);
        signal = signal.subspan(first, count);
    }
    overlay(destination, signal, rate, gain, stop);
}

} // namespace

PitchRenderResult renderPitchEdits(std::span<const float> mono, const PitchAnalysis& analysis,
                                 const PitchEditState& edits, std::stop_token stop) {
    if (onAudioThread) return {PitchRenderError::Realtime, nullptr};
    if (stop.stop_requested()) return {PitchRenderError::Cancelled, nullptr};
    if (mono.size() != analysis.sourceFrames || mono.size() > maxSourceFrames)
        return {PitchRenderError::InvalidSource, nullptr};
    try {
        const auto validation = validatePitchEdits(analysis, edits);
        if (validation == PitchEditError::InvalidAnalysis) return {PitchRenderError::InvalidSource, nullptr};
        if (validation != PitchEditError::None) return {PitchRenderError::InvalidEdits, nullptr};
        auto output = std::make_unique<std::vector<float>>(mono.size());
        const bool finite = copyFinite(mono, *output, stop);

        for (const auto& edit : edits.notes) {
            const auto& note = analysis.notes[edit.noteIndex];
            if (changesTime(note, edit)) {
                auto span = std::span(*output).subspan(static_cast<std::size_t>(note.startFrame),
                    static_cast<std::size_t>(note.endFrame - note.startFrame));
                overlay(span, {}, analysis.sampleRate, 0, stop);
            }
        }

        // For incoming transitions look up the adjoining note in destination
        // order. Its pitch offset can come from an untouched note as well.
        std::vector<PitchNoteEdit> timeline;
        if (std::any_of(edits.notes.begin(), edits.notes.end(), [](const auto& edit) {
                return edit.transitionFrames != 0 && edit.pitchShiftCents != 0;
            })) {
            timeline.reserve(analysis.notes.size());
            for (std::size_t index = 0; index < analysis.notes.size(); ++index) {
                if (index % processFrames == 0) checkCancelled(stop);
                timeline.push_back(resolvePitchNoteEdit(analysis, edits, index));
            }
            std::sort(timeline.begin(), timeline.end(), [](const auto& a, const auto& b) { return a.endFrame < b.endFrame; });
        }

        for (const auto& edit : edits.notes) {
            checkCancelled(stop);
            const auto& note = analysis.notes[edit.noteIndex];
            const auto preceding = std::lower_bound(timeline.begin(), timeline.end(), edit.startFrame,
                [](const auto& candidate, std::uint64_t frame) { return candidate.endFrame < frame; });
            const auto* previous = preceding != timeline.end() && preceding->endFrame == edit.startFrame ? &*preceding : nullptr;
            const auto input = noteSamples(mono, note);
            std::vector<float> cleaned;
            std::span<const float> source = input;
            if (!finite) {
                cleaned.resize(input.size());
                copyFinite(input, cleaned, stop);
                source = cleaned;
            }
            auto destination = std::span(*output).subspan(static_cast<std::size_t>(edit.startFrame),
                static_cast<std::size_t>(edit.endFrame - edit.startFrame));
            renderNote(source, destination, analysis.sampleRate, note, edit, previous, stop);
        }
        checkCancelled(stop);
        return {PitchRenderError::None, std::move(output)};
    } catch (PitchRenderError error) {
        return {error, nullptr};
    } catch (const std::bad_alloc&) {
        return {PitchRenderError::Resource, nullptr};
    } catch (const std::length_error&) {
        return {PitchRenderError::Resource, nullptr};
    } catch (const std::exception&) {
        return {PitchRenderError::Process, nullptr};
    }
}

} // namespace adi::audio
