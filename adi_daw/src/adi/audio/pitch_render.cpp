// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/pitch_render.hpp"

#include "adi/audio/io_audit.hpp"

#include <rubberband/RubberBandStretcher.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <limits>
#include <new>
#include <stdexcept>
#include <tuple>
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

template<std::size_t Channels>
using Planes = std::array<std::span<const float>, Channels>;

template<std::size_t Channels>
using Buffers = std::array<std::vector<float>, Channels>;

template<std::size_t Channels>
Buffers<Channels> buffers(std::size_t frames) {
    Buffers<Channels> result;
    for (auto& channel : result) channel.resize(frames);
    return result;
}

template<std::size_t Channels>
Planes<Channels> view(const Buffers<Channels>& source) {
    Planes<Channels> planes;
    for (std::size_t channel = 0; channel < Channels; ++channel) planes[channel] = source[channel];
    return planes;
}

template<typename SamplePlanes>
auto planePointers(SamplePlanes& planes, std::size_t frame) {
    std::array<decltype(planes[0].data()), std::tuple_size_v<SamplePlanes>> pointers{};
    for (std::size_t channel = 0; channel < planes.size(); ++channel) pointers[channel] = planes[channel].data() + frame;
    return pointers;
}

template<std::size_t Channels>
Buffers<Channels> stretch(const Planes<Channels>& source, std::size_t frames, std::uint32_t rate,
                          double pitchRatio, double formantRatio, std::stop_token stop) {
    using Stretcher = RubberBand::RubberBandStretcher;
    const auto inputFrames = source.front().size();
    const double ratio = static_cast<double>(frames) / static_cast<double>(inputFrames);
    checkRatio(ratio, maxStretchRatio);
    checkRatio(pitchRatio, maxPitchRatio);
    constexpr auto channelMode = Channels == 2 ? Stretcher::OptionChannelsTogether : Stretcher::OptionChannelsApart;
    constexpr auto options = Stretcher::OptionProcessOffline | Stretcher::OptionEngineFiner | channelMode |
        Stretcher::OptionThreadingNever | Stretcher::OptionFormantPreserved;
    Stretcher stretcher(rate, Channels, options, ratio, pitchRatio);
    if (stretcher.getEngineVersion() != 3) throw PitchRenderError::Process;
    stretcher.setDebugLevel(0);
    stretcher.setExpectedInputDuration(inputFrames);
    stretcher.setMaxProcessSize(processFrames);
    if (formantRatio != 1) stretcher.setFormantScale(formantRatio / pitchRatio);

    for (std::size_t frame = 0; frame < inputFrames;) {
        checkCancelled(stop);
        const std::size_t count = std::min(processFrames, inputFrames - frame);
        const auto input = planePointers(source, frame);
        frame += count;
        stretcher.study(input.data(), count, frame == inputFrames);
    }
    auto output = buffers<Channels>(frames);
    std::size_t written = 0;
    for (std::size_t frame = 0; frame < inputFrames;) {
        checkCancelled(stop);
        const std::size_t count = std::min(processFrames, inputFrames - frame);
        const auto input = planePointers(source, frame);
        frame += count;
        stretcher.process(input.data(), count, frame == inputFrames);
        for (int available = stretcher.available(); available > 0; available = stretcher.available()) {
            checkCancelled(stop);
            const auto requested = static_cast<std::size_t>(available);
            if (requested > frames - written) throw PitchRenderError::Process;
            const auto destination = planePointers(output, written);
            if (stretcher.retrieve(destination.data(), requested) != requested) throw PitchRenderError::Process;
            written += requested;
        }
    }
    if (written != frames || stretcher.available() != -1) throw PitchRenderError::Process;
    return output;
}

template<std::size_t Channels>
Buffers<Channels> shiftCurve(const Planes<Channels>& source, std::uint32_t rate, const PitchNote& note,
                             const PitchNoteEdit& edit, const PitchNoteEdit* preceding, std::stop_token stop) {
    using Stretcher = RubberBand::RubberBandStretcher;
    constexpr auto channelMode = Channels == 2 ? Stretcher::OptionChannelsTogether : Stretcher::OptionChannelsApart;
    constexpr auto options = Stretcher::OptionProcessRealTime | Stretcher::OptionEngineFiner | channelMode |
        Stretcher::OptionThreadingNever | Stretcher::OptionPitchHighConsistency |
        Stretcher::OptionFormantPreserved;
    const auto frames = source.front().size();
    const auto pitchAt = [&](std::size_t frame) {
        const auto sourceFrame = note.startFrame + std::min(frame, frames - 1);
        const double cents = pitchCorrectionCents(note, edit, static_cast<double>(sourceFrame), preceding);
        const double ratio = std::exp2(cents / 1200);
        checkRatio(ratio, maxPitchRatio);
        return ratio;
    };
    const double formant = std::exp2(edit.formantShiftCents / 1200);
    const double startingPitch = pitchAt(0);
    Stretcher stretcher(rate, Channels, options, 1, startingPitch);
    if (stretcher.getEngineVersion() != 3) throw PitchRenderError::Process;
    stretcher.setDebugLevel(0);
    // High Consistency keeps the resampler after the phase vocoder on both
    // sides of unity. Time the pitch automation on the compensated output
    // clock. The streaming mode is only driven by this offline worker.
    const std::size_t controlFrames = rate / 200;
    stretcher.setMaxProcessSize(controlFrames);
    const std::size_t pad = stretcher.getPreferredStartPad();
    const std::size_t delay = stretcher.getStartDelay();
    auto output = buffers<Channels>(frames);
    auto scratch = buffers<Channels>(controlFrames);
    const std::vector<float> silence(controlFrames);
    std::array<const float*, Channels> silencePlanes;
    silencePlanes.fill(silence.data());
    std::size_t written = 0;
    std::size_t discard = delay;
    const auto target = pad + frames + delay + processFrames;
    for (std::size_t frame = 0; frame < target && written < frames;) {
        checkCancelled(stop);
        std::size_t count = std::min(controlFrames, target - frame);
        auto input = silencePlanes;
        if (frame < pad) count = std::min(count, pad - frame);
        else if (frame - pad < frames) {
            count = std::min(count, frames - (frame - pad));
            input = planePointers(source, frame - pad);
        }
        const double pitch = pitchAt(written);
        stretcher.setPitchScale(pitch);
        if (formant != 1) stretcher.setFormantScale(formant / pitch);
        frame += count;
        stretcher.process(input.data(), count, frame == target);
        for (int available = stretcher.available(); available > 0 && written < frames; available = stretcher.available()) {
            auto ready = static_cast<std::size_t>(available);
            if (discard > 0) {
                const auto skipped = std::min({discard, ready, controlFrames});
                const auto destination = planePointers(scratch, 0);
                if (stretcher.retrieve(destination.data(), skipped) != skipped) throw PitchRenderError::Process;
                discard -= skipped;
                ready -= skipped;
            }
            if (discard == 0 && ready > 0) {
                const auto requested = std::min(ready, frames - written);
                const auto destination = planePointers(output, written);
                if (stretcher.retrieve(destination.data(), requested) != requested) throw PitchRenderError::Process;
                written += requested;
            }
        }
    }
    if (written != frames) throw PitchRenderError::Process;
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

template<std::size_t Channels>
void overlay(std::span<float> destination, const std::array<const float*, Channels>& note, std::size_t stride,
             std::uint32_t rate, double gain, std::stop_token stop) {
    constexpr double sampleLimit = std::numeric_limits<float>::max();
    const auto frames = destination.size() / Channels;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        if (frame % processFrames == 0) checkCancelled(stop);
        const double weight = fadeWeight(frame, frames, rate);
        for (std::size_t channel = 0; channel < Channels; ++channel) {
            const double signal = note[channel] ? note[channel][frame * stride] : 0;
            if (!std::isfinite(signal)) throw PitchRenderError::Process;
            auto& sample = destination[frame * Channels + channel];
            const double mixed = sample * (1 - weight) + signal * gain * weight;
            sample = static_cast<float>(std::clamp(mixed, -sampleLimit, sampleLimit));
        }
    }
}

template<std::size_t Channels>
Planes<Channels> preparePlanes(std::span<const float> source, Buffers<Channels>& planar, std::stop_token stop) {
    if constexpr (Channels == 1) {
        return {source};
    } else {
        const auto frames = source.size() / Channels;
        planar = buffers<Channels>(frames);
        for (std::size_t frame = 0; frame < frames; ++frame) {
            if (frame % processFrames == 0) checkCancelled(stop);
            for (std::size_t channel = 0; channel < Channels; ++channel)
                planar[channel][frame] = source[frame * Channels + channel];
        }
        return view(planar);
    }
}

template<std::size_t Channels>
void renderNote(std::span<const float> source, std::span<float> destination, std::uint32_t rate, const PitchNote& note,
                const PitchNoteEdit& edit, const PitchNoteEdit* preceding, std::stop_token stop) {
    const double gain = std::pow(10, edit.gainDb / 20);
    if (gain == 0) {
        overlay<Channels>(destination, {}, Channels, rate, 0, stop);
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

    std::array<const float*, Channels> signal;
    auto stride = Channels;
    Buffers<Channels> processed;
    if (needsDsp) {
        Buffers<Channels> planar;
        const auto input = preparePlanes(source, planar, stop);
        const auto frames = destination.size() / Channels;
        if (changingPitch) {
            processed = shiftCurve(input, rate, note, edit, preceding, stop);
            if (timeStretch) processed = stretch(view(processed), frames, rate, 1, 1, stop);
        } else {
            const double pitch = std::exp2(edit.pitchShiftCents / 1200);
            const double formant = std::exp2(edit.formantShiftCents / 1200);
            processed = stretch(input, frames, rate, pitch, formant, stop);
        }
        const auto output = view(processed);
        signal = planePointers(output, 0);
        stride = 1;
    } else {
        for (std::size_t channel = 0; channel < Channels; ++channel) signal[channel] = source.data() + channel;
    }
    if (edit.straightenRange && edit.straighten != 0 && !changingOtherPitch && edit.pitchShiftCents == 0 &&
        edit.formantShiftCents == 0 && gain == 1 && !changesTime(note, edit)) {
        // Even a unity-ratio vocoder introduces processing outside the
        // requested span. Keep original samples where there is no edit.
        const auto first = static_cast<std::size_t>(edit.straightenRange->startFrame - note.startFrame);
        const auto count = static_cast<std::size_t>(edit.straightenRange->endFrame - edit.straightenRange->startFrame);
        destination = destination.subspan(first * Channels, count * Channels);
        for (auto& channel : signal) channel += first * stride;
    }
    overlay(destination, signal, stride, rate, gain, stop);
}

template<std::size_t Channels>
PitchRenderResult render(std::span<const float> sourcePcm, const PitchAnalysis& analysis, const PitchEditState& edits,
                         std::stop_token stop) {
    if (onAudioThread) return {PitchRenderError::Realtime, nullptr};
    if (stop.stop_requested()) return {PitchRenderError::Cancelled, nullptr};
    const auto inputFrames = sourcePcm.size() / Channels;
    if (sourcePcm.size() % Channels != 0 || inputFrames != analysis.sourceFrames || inputFrames > maxSourceFrames)
        return {PitchRenderError::InvalidSource, nullptr};
    try {
        const auto validation = validatePitchEdits(analysis, edits);
        if (validation == PitchEditError::InvalidAnalysis) return {PitchRenderError::InvalidSource, nullptr};
        if (validation != PitchEditError::None) return {PitchRenderError::InvalidEdits, nullptr};
        auto output = std::make_unique<std::vector<float>>(sourcePcm.size());
        const bool finite = copyFinite(sourcePcm, *output, stop);

        for (const auto& edit : edits.notes) {
            const auto& note = analysis.notes[edit.noteIndex];
            if (changesTime(note, edit)) {
                auto span = std::span(*output).subspan(static_cast<std::size_t>(note.startFrame) * Channels,
                    static_cast<std::size_t>(note.endFrame - note.startFrame) * Channels);
                overlay<Channels>(span, {}, Channels, analysis.sampleRate, 0, stop);
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
            const auto input = sourcePcm.subspan(static_cast<std::size_t>(note.startFrame) * Channels,
                static_cast<std::size_t>(note.endFrame - note.startFrame) * Channels);
            std::vector<float> cleaned;
            std::span<const float> source = input;
            if (!finite) {
                cleaned.resize(input.size());
                copyFinite(input, cleaned, stop);
                source = cleaned;
            }
            auto destination = std::span(*output).subspan(static_cast<std::size_t>(edit.startFrame) * Channels,
                static_cast<std::size_t>(edit.endFrame - edit.startFrame) * Channels);
            renderNote<Channels>(source, destination, analysis.sampleRate, note, edit, previous, stop);
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

} // namespace

PitchRenderResult renderPitchEdits(std::span<const float> mono, const PitchAnalysis& analysis,
                                  const PitchEditState& edits, std::stop_token stop) {
    return render<1>(mono, analysis, edits, stop);
}

PitchRenderResult renderPitchEdits(std::span<const float> interleaved, std::uint16_t channels, const PitchAnalysis& analysis,
                                  const PitchEditState& edits, std::stop_token stop) {
    if (onAudioThread) return {PitchRenderError::Realtime, nullptr};
    if (stop.stop_requested()) return {PitchRenderError::Cancelled, nullptr};
    if (channels == 1) return render<1>(interleaved, analysis, edits, stop);
    if (channels == 2) return render<2>(interleaved, analysis, edits, stop);
    return {PitchRenderError::InvalidSource, nullptr};
}

} // namespace adi::audio
