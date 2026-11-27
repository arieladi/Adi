// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/io_audit.hpp"
#include "adi/audio/pitch_audition.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>

namespace {
using namespace adi::audio;
constexpr std::uint32_t rate = 48000;
constexpr std::size_t frames = 64000;
constexpr std::size_t noteStart = 4800, noteEnd = 19200;
constexpr std::size_t movedStart = 24000, movedEnd = 43200;
constexpr std::size_t secondStart = 48000, secondEnd = 57600;
int checks = 0, failures = 0;

void check(bool condition, const char* label) {
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL %s\n", label);
    }
}

void tone(std::span<float> pcm, double hz) {
    for (std::size_t frame = 0; frame < pcm.size(); ++frame)
        pcm[frame] = static_cast<float>(0.35 * std::sin(2 * std::numbers::pi * hz * static_cast<double>(frame) / rate));
}

double estimateHz(std::span<const float> pcm) {
    std::size_t crossings = 0;
    double first = 0, last = 0;
    for (std::size_t frame = 1; frame < pcm.size(); ++frame) {
        const auto a = pcm[frame - 1];
        const auto b = pcm[frame];
        if (a <= 0 && b > 0) {
            const double position = static_cast<double>(frame - 1) - a / (static_cast<double>(b) - a);
            if (crossings == 0) first = position;
            last = position;
            ++crossings;
        }
    }
    return crossings > 1 ? static_cast<double>(crossings - 1) * rate / (last - first) : 0;
}

double rms(std::span<const float> pcm) {
    double power = 0;
    for (const float sample : pcm) power += static_cast<double>(sample) * sample;
    return std::sqrt(power / static_cast<double>(pcm.size()));
}

PitchNote steady(std::size_t start, std::size_t end, double cents) {
    return {start, end, cents, 0, 0, {{start, cents}, {end - 1, cents}}};
}

void run() {
    std::vector<float> original(frames);
    tone(std::span(original).subspan(noteStart, noteEnd - noteStart), 220);
    tone(std::span(original).subspan(secondStart, secondEnd - secondStart), 330);
    original[45500] = 0.11f;
    const PitchAnalysis analysis{rate, original.size(), {steady(noteStart, noteEnd, 5700), steady(secondStart, secondEnd, 6400)}};
    auto edited = resolvePitchNoteEdit(analysis, {}, 0);
    edited.startFrame = movedStart;
    edited.endFrame = movedEnd;
    edited.pitchShiftCents = 100;
    edited.formantShiftCents = 300;
    edited.gainDb = -6;
    const PitchEditState authored{{edited}};
    const auto saved = authored.notes;
    const std::array<std::size_t, 1> firstNote{0};
    PitchAuditionSpec spec{{0, frames}, firstNote};

    const auto editedPcm = renderPitchEdits(original, analysis, authored);
    const auto fullEdits = renderPitchAudition(original, analysis, authored, spec);
    check(fullEdits && fullEdits.clipRange == PitchAuditionRange{movedStart, movedEnd} && !fullEdits.loop,
          "selection uses authored destination bounds with loop disabled by default");
    if (fullEdits && editedPcm)
        check(std::ranges::equal(*fullEdits.rendered.samples, std::span(*editedPcm.samples).subspan(movedStart, movedEnd - movedStart)),
              "normal audition exactly crops ordinary rendered PCM");

    spec.bypassPitchFormantGain = true;
    spec.loop = true;
    const auto timingOnly = renderPitchAudition(original, analysis, authored, spec);
    auto timeEdit = resolvePitchNoteEdit(analysis, {}, 0);
    timeEdit.startFrame = movedStart;
    timeEdit.endFrame = movedEnd;
    const auto timingPcm = renderPitchEdits(original, analysis, {{timeEdit}});
    check(timingOnly && timingOnly.clipRange == PitchAuditionRange{movedStart, movedEnd} &&
          timingOnly.loop == PitchAuditionRange{0, movedEnd - movedStart},
          "pitch/formant/gain bypass retains edited timing and exact buffer-local loop bounds");
    if (timingOnly && timingPcm) {
        const auto samples = std::span(*timingOnly.rendered.samples);
        check(std::ranges::equal(samples, std::span(*timingPcm.samples).subspan(movedStart, movedEnd - movedStart)),
              "group bypass removes all spectral, pitch and level edits without changing the timing DSP");
        const auto hz = estimateHz(samples.subspan(2400, 12000));
        std::printf("pitch/formant/gain bypass: %.4f Hz at frames %zu–%zu\n", hz, movedStart, movedEnd);
        check(std::abs(hz - 220) < 1, "restored original pitch remains audible at the edited destination");
    }

    spec.bypassPitchFormantGain = false;
    spec.bypassSegmentTiming = true;
    const auto previousTiming = renderPitchAudition(original, analysis, authored, spec);
    auto pitchEdit = edited;
    pitchEdit.startFrame = noteStart;
    pitchEdit.endFrame = noteEnd;
    const auto newPitchPcm = renderPitchEdits(original, analysis, {{pitchEdit}});
    check(previousTiming && previousTiming.clipRange == PitchAuditionRange{noteStart, noteEnd} &&
          previousTiming.loop == PitchAuditionRange{0, noteEnd - noteStart},
          "timing-only bypass restores original destination and loop extent");
    if (previousTiming && newPitchPcm) {
        const auto samples = std::span(*previousTiming.rendered.samples);
        check(std::ranges::equal(samples, std::span(*newPitchPcm.samples).subspan(noteStart, noteEnd - noteStart)),
              "timing bypass retains pitch, formant and level edits exactly");
        const auto hz = estimateHz(samples.subspan(2400, 9600));
        std::printf("segment-timing bypass: %.4f Hz at original frames %zu–%zu\n", hz, noteStart, noteEnd);
        check(std::abs(hz - 233.08188) < 1 && rms(samples) > 0.04, "edited note remains audible one semitone up");
    }

    spec.bypassPitchFormantGain = true;
    const auto originalNote = renderPitchAudition(original, analysis, authored, spec);
    check(originalNote && originalNote.loop == PitchAuditionRange{0, noteEnd - noteStart} &&
          std::ranges::equal(*originalNote.rendered.samples, std::span(original).subspan(noteStart, noteEnd - noteStart)),
          "combined bypass returns bit-identical finite source samples");
    check(authored.notes == saved, "temporary comparisons preserve every authored edit field");

    const std::array<std::size_t, 3> unorderedNotes{1, 0, 1};
    spec.selectedNotes = unorderedNotes;
    spec.clipRange = {noteStart + 50, secondEnd - 50};
    const auto selection = renderPitchAudition(original, analysis, authored, spec);
    check(selection && selection.clipRange == spec.clipRange &&
          selection.loop == PitchAuditionRange{0, secondEnd - noteStart - 100} &&
          std::ranges::equal(*selection.rendered.samples,
              std::span(original).subspan(noteStart + 50, secondEnd - noteStart - 100)),
          "multi-note loop follows selected bounding span, retaining the gap and excluding trimmed edges");
    spec.selectedNotes = {};
    spec.clipRange = {45500, 45501};
    const auto frame = renderPitchAudition(original, analysis, authored, spec);
    check(frame && frame.loop == PitchAuditionRange{0, 1} && *frame.rendered.samples == std::vector<float>{original[45500]},
          "single-sample audition with no note selection uses the explicit clip range");

    spec.clipRange.endFrame = frames + 1;
    const auto outside = renderPitchAudition(original, analysis, authored, spec);
    check(outside.rendered.error == PitchRenderError::InvalidSource && !outside.loop && !outside.rendered.samples,
          "out-of-source comparison range refuses without a loop or samples");
    spec.clipRange = {0, frames};
    const std::array<std::size_t, 1> invalidNote{analysis.notes.size()};
    spec.selectedNotes = invalidNote;
    check(renderPitchAudition(original, analysis, authored, spec).rendered.error == PitchRenderError::InvalidEdits,
          "unknown selected note refuses even with both bypasses enabled");
    spec.selectedNotes = firstNote;
    spec.clipRange = {movedStart, movedEnd};
    check(renderPitchAudition(original, analysis, authored, spec).rendered.error == PitchRenderError::InvalidSource,
          "empty intersection after timing bypass refuses");

    spec.clipRange = {0, frames};
    auto invalidState = authored;
    invalidState.notes.front().gainDb = std::numeric_limits<double>::quiet_NaN();
    check(renderPitchAudition(original, analysis, invalidState, spec).rendered.error == PitchRenderError::InvalidEdits,
          "bypass does not accept invalid authored state");
    std::stop_source stop;
    stop.request_stop();
    const auto stopped = renderPitchAudition(original, analysis, authored, spec, stop.get_token());
    check(stopped.rendered.error == PitchRenderError::Cancelled && !stopped.loop && !stopped.rendered.samples,
          "cancellation publishes no partially prepared loop");
    PitchAuditionResult callback;
    {
        const CallbackScope scope;
        callback = renderPitchAudition(original, analysis, authored, spec);
    }
    check(callback.rendered.error == PitchRenderError::Realtime && !callback.loop && !callback.rendered.samples,
          "audio callback refuses audition preparation");
}
} // namespace

int main() {
    run();
    std::printf("%d pitch audition checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
