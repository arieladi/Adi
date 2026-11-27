// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/io_audit.hpp"
#include "adi/audio/pitch_render.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>

namespace {
using namespace adi::audio;
constexpr std::uint32_t rate = 48000;
constexpr double tau = 2 * std::numbers::pi;
int checks = 0;
int failures = 0;

void check(bool condition, const char* description) {
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL %s\n", description);
    }
}

double measureHz(std::span<const float> audio) {
    double first = 0;
    double last = 0;
    std::size_t crossings = 0;
    for (std::size_t frame = 1; frame < audio.size(); ++frame) {
        if (audio[frame - 1] <= 0 && audio[frame] > 0) {
            const double crossing = static_cast<double>(frame - 1) - audio[frame - 1] /
                (static_cast<double>(audio[frame]) - audio[frame - 1]);
            if (crossings == 0) first = crossing;
            last = crossing;
            ++crossings;
        }
    }
    return crossings > 1 ? static_cast<double>(crossings - 1) * rate / (last - first) : 0;
}

double cents(double from, double to) { return 1200 * std::log2(to / from); }

double rms(std::span<const float> audio) {
    double power = 0;
    for (const auto sample : audio) power += static_cast<double>(sample) * sample;
    return std::sqrt(power / static_cast<double>(audio.size()));
}

double harmonicLevel(std::span<const float> audio, double frequency) {
    double cosSum = 0;
    double sinSum = 0;
    for (std::size_t frame = 0; frame < audio.size(); ++frame) {
        const double angle = tau * frequency * static_cast<double>(frame) / rate;
        cosSum += audio[frame] * std::cos(angle);
        sinSum += audio[frame] * std::sin(angle);
    }
    return std::hypot(cosSum, sinSum);
}

PitchNote steadyNote(std::uint64_t start, std::uint64_t end, double pitch) {
    PitchNote note;
    note.startFrame = start;
    note.endFrame = end;
    note.pitchCents = pitch;
    note.curve = {{start, pitch}, {end - 1, pitch}};
    return note;
}

void tone(std::span<float> samples, double frequency) {
    for (std::size_t frame = 0; frame < samples.size(); ++frame)
        samples[frame] = static_cast<float>(0.35 * std::sin(tau * frequency * static_cast<double>(frame) / rate));
}

void moveSemitone() {
    constexpr std::uint32_t start = rate / 10;
    constexpr std::uint32_t end = rate * 4 / 10;
    std::vector<float> samples(rate);
    tone(std::span(samples).subspan(start, end - start), 220);
    tone(std::span(samples).subspan(rate / 2, rate / 3), 330);
    samples[15] = 0.17f;
    const PitchAnalysis analysis{rate, samples.size(), {steadyNote(start, end, 5700),
        steadyNote(rate / 2, rate / 2 + rate / 3, 6400)}};
    check(static_cast<bool>(renderPitchEdits(samples, analysis, {})), "empty edit renders");
    const auto neutral = renderPitchEdits(samples, analysis, {});
    check(neutral && *neutral.samples == samples, "empty edit is sample-identical");
    auto edit = resolvePitchNoteEdit(analysis, {}, 0);
    edit.pitchShiftCents = 100;
    const auto result = renderPitchEdits(samples, analysis, PitchEditState{{edit}});
    check(result && result.samples->size() == samples.size(), "semitone edit retains the clip frame count");
    if (!result) {
        std::printf("render error %d\n", static_cast<int>(result.error));
        return;
    }
    const std::span rendered(*result.samples);
    const auto body = rendered.subspan(start + rate / 20, rate / 5);
    const double frequency = measureHz(body);
    const double measured = cents(220, frequency);
    std::printf("single-note transpose: %.3f Hz, %.2f cents (requested 100)\n", frequency, measured);
    check(std::abs(measured - 100) < 10 && rms(body) > 0.08, "edited note moves one audible semitone");
    check(std::equal(samples.begin(), samples.begin() + start, rendered.begin()) &&
          std::equal(samples.begin() + end, samples.end(), rendered.begin() + end), "adjacent and unvoiced audio passes through unchanged");

    edit.pitchShiftCents = 0;
    edit.gainDb = 6;
    const auto gain = renderPitchEdits(samples, analysis, PitchEditState{{edit}});
    check(static_cast<bool>(gain), "level edit renders");
    if (gain) {
        const auto sourceBody = std::span(samples).subspan(start + rate / 20, rate / 5);
        const auto gainedBody = std::span(*gain.samples).subspan(start + rate / 20, rate / 5);
        const double db = 20 * std::log10(rms(gainedBody) / rms(sourceBody));
        std::printf("gain: %.3f dB (requested 6)\n", db);
        check(std::abs(db - 6) < 0.001, "gain changes level by the requested number of decibels");
    }
}

void moveAndStretch() {
    constexpr std::uint32_t start = rate / 10;
    constexpr std::uint32_t end = rate * 4 / 10;
    constexpr std::uint32_t destination = rate / 2;
    constexpr std::uint32_t destinationEnd = rate * 95 / 100;
    std::vector<float> samples(rate);
    tone(std::span(samples).subspan(start, end - start), 220);
    const PitchAnalysis analysis{rate, samples.size(), {steadyNote(start, end, 5700)}};
    auto edit = resolvePitchNoteEdit(analysis, {}, 0);
    edit.startFrame = destination;
    edit.endFrame = destinationEnd;
    const auto result = renderPitchEdits(samples, analysis, PitchEditState{{edit}});
    check(result && result.samples->size() == samples.size(), "time move and stretch retains clip duration");
    if (!result) {
        std::printf("time render error %d\n", static_cast<int>(result.error));
        return;
    }
    const std::span rendered(*result.samples);
    const auto moved = rendered.subspan(destination + rate / 20, rate / 4);
    const double error = cents(220, measureHz(moved));
    std::printf("time stretch: %u to %u frames, pitch error %.2f cents\n", end - start, destinationEnd - destination, error);
    check(std::abs(error) < 10 && rms(moved) > 0.08, "stretch preserves pitch in the moved note");
    check(rms(rendered.subspan(start + rate / 20, end - start - rate / 10)) == 0, "old note location is vacated");
    check(rendered[destination - 1] == 0 && rms(rendered.subspan(destination + 1000, 1000)) > 0.08 &&
          rms(rendered.subspan(destinationEnd - 2000, 1000)) > 0.08 && rendered[destinationEnd] == 0,
          "moved sound is placed within exact destination bounds");
}

void editCurve() {
    constexpr std::uint32_t length = rate * 8 / 10;
    std::vector<float> samples(length);
    auto note = steadyNote(0, length, 5700);
    note.curve = {{0, 5580}, {length - 1, 5820}};
    note.driftCents = 240;
    double phase = 0;
    for (std::uint32_t frame = 0; frame < length; ++frame) {
        const double pitch = -120 + 240.0 * frame / (length - 1);
        const double frequency = 220 * std::exp2(pitch / 1200);
        samples[frame] = static_cast<float>(0.35 * std::sin(phase));
        phase += tau * frequency / rate;
    }
    const PitchAnalysis analysis{rate, samples.size(), {note}};
    auto edit = resolvePitchNoteEdit(analysis, {}, 0);
    edit.straighten = 1;
    const auto result = renderPitchEdits(samples, analysis, PitchEditState{{edit}});
    check(static_cast<bool>(result), "curve straightening renders");
    if (!result) {
        std::printf("curve render error %d\n", static_cast<int>(result.error));
        return;
    }
    const std::span rendered(*result.samples);
    const double early = cents(220, measureHz(rendered.subspan(rate / 5, rate / 10)));
    const double late = cents(220, measureHz(rendered.subspan(rate / 2, rate / 10)));
    std::printf("straightened pitch error: %.2f cents early, %.2f cents late\n", early, late);
    check(std::abs(early) < 10 && std::abs(late) < 10, "straighten removes a measured 240-cent sweep");

    edit.tiltCents = 200;
    const auto tilted = renderPitchEdits(samples, analysis, PitchEditState{{edit}});
    check(static_cast<bool>(tilted), "curve tilt renders");
    if (tilted) {
        const std::span pcm(*tilted.samples);
        const double a = measureHz(pcm.subspan(rate / 5, rate / 10));
        const double b = measureHz(pcm.subspan(rate / 2, rate / 10));
        const double slope = cents(a, b);
        std::printf("pitch tilt over 300 ms: %.2f cents (requested 75)\n", slope);
        check(std::abs(slope - 75) < 10, "tilt introduces the authored pitch slope");
    }

    edit.tiltCents = 0;
    constexpr auto rangeStart = rate / 8;
    constexpr auto rangeEnd = rate * 4 / 10;
    edit.straightenRange = PitchStraightenRange{rangeStart, rangeEnd};
    const auto ranged = renderPitchEdits(samples, analysis, PitchEditState{{edit}});
    check(static_cast<bool>(ranged), "ranged straightening renders");
    if (ranged) {
        const std::span pcm(*ranged.samples);
        const double center = cents(220, measureHz(pcm.subspan(rate / 5, rate / 10)));
        std::printf("range-only straighten: %.2f cents error in selected area\n", center);
        check(std::abs(center) < 10 && std::equal(samples.begin(), samples.begin() + rangeStart, pcm.begin()) &&
              std::equal(samples.begin() + rangeEnd, samples.end(), pcm.begin() + rangeEnd),
              "ranged straighten changes pitch within the range and preserves all other samples");
    }

    edit.straightenRange.reset();
    edit.tiltGeometry = {0.5, 100, 0};
    const auto sided = renderPitchEdits(samples, analysis, PitchEditState{{edit}});
    check(static_cast<bool>(sided), "one-sided tilt renders");
    if (sided) {
        const std::span pcm(*sided.samples);
        const double left = cents(220, measureHz(pcm.subspan(rate / 5, rate / 10)));
        const double right = cents(220, measureHz(pcm.subspan(rate / 2, rate / 10)));
        std::printf("one-sided tilt: %.2f cents on left, %.2f cents on right\n", left, right);
        check(std::abs(left - 37.5) < 10 && std::abs(right) < 10,
              "side tilt applies the selected anchor slope independently of the opposite side");
    }
}

void transitionAndLimits() {
    std::vector<float> samples(rate * 6 / 10);
    tone(samples, 220);
    const std::uint32_t boundary = rate / 5;
    PitchAnalysis analysis{rate, samples.size(), {steadyNote(0, boundary, 5700),
        steadyNote(boundary, samples.size(), 5700)}};
    auto edit = resolvePitchNoteEdit(analysis, {}, 1);
    edit.pitchShiftCents = 200;
    edit.transitionFrames = rate * 16 / 100;
    const auto result = renderPitchEdits(samples, analysis, PitchEditState{{edit}});
    check(static_cast<bool>(result), "transition renders between adjacent notes");
    if (result) {
        const std::span rendered(*result.samples);
        const double start = cents(220, measureHz(rendered.subspan(boundary + rate / 50, rate / 25)));
        const double target = cents(220, measureHz(rendered.subspan(boundary + rate / 5, rate / 10)));
        std::printf("incoming transition: %.2f cents near onset, %.2f cents after transition\n", start, target);
        check(start > -10 && start < 120 && std::abs(target - 200) < 10, "transposed note eases in from the preceding offset");
        check(std::equal(samples.begin(), samples.begin() + boundary, rendered.begin()), "transition leaves the preceding note unchanged");
    }

    edit.transitionFrames = 0;
    edit.formantShiftCents = 1;
    edit.tiltCents = 50;
    for (const auto pitch : {0.0, -2400.0, 2400.0}) {
        edit.pitchShiftCents = pitch;
        const auto automated = renderPitchEdits(samples, analysis, PitchEditState{{edit}});
        check(automated && automated.samples->size() == samples.size(), "short tilt combines with zero and maximum transpose");
    }
    analysis.sampleRate = 384000;
    const auto untouched = renderPitchEdits(samples, analysis, {});
    const auto highRateDsp = renderPitchEdits(samples, analysis, PitchEditState{{edit}});
    check(untouched && *untouched.samples == samples && highRateDsp.error == PitchRenderError::Unsupported,
          "high-rate passthrough succeeds and unsupported DSP refuses explicitly");
}

void formantAndErrors() {
    std::vector<float> samples(rate / 2);
    constexpr double frequency = 200;
    for (std::size_t frame = 0; frame < samples.size(); ++frame) {
        const double t = static_cast<double>(frame) / rate;
        double sum = 0;
        for (int harmonic = 1; harmonic <= 14; ++harmonic) {
            const double distance = (harmonic * frequency - 1000) / 220;
            const double amplitude = 0.035 + 0.12 * std::exp(-distance * distance / 2);
            sum += amplitude * std::sin(tau * harmonic * frequency * t);
        }
        samples[frame] = static_cast<float>(sum);
    }
    const PitchAnalysis analysis{rate, samples.size(), {steadyNote(0, samples.size(), 5535)}};
    auto edit = resolvePitchNoteEdit(analysis, {}, 0);
    edit.formantShiftCents = 600;
    const PitchEditState state{{edit}};
    const auto result = renderPitchEdits(samples, analysis, state);
    check(static_cast<bool>(result), "formant-only edit renders");
    if (result) {
        const auto source = std::span(samples).subspan(rate / 10, rate / 4);
        const auto output = std::span(*result.samples).subspan(rate / 10, rate / 4);
        const auto envelopeDb = [&](std::span<const float> audio) {
            return 20 * std::log10(harmonicLevel(audio, 1400) / harmonicLevel(audio, 1000));
        };
        const double formantChange = envelopeDb(output) - envelopeDb(source);
        std::printf("formant +600 cents: 1400/1000 Hz balance changes %.2f dB\n", formantChange);
        check(formantChange > 2, "formant moves the harmonic envelope toward its target frequency");
        check(harmonicLevel(output, 200) > 1.5 * std::max(harmonicLevel(output, 190), harmonicLevel(output, 210)),
              "formant preserves the fundamental frequency");
    }

    std::stop_source stop;
    stop.request_stop();
    const auto cancelled = renderPitchEdits(samples, analysis, state, stop.get_token());
    check(cancelled.error == PitchRenderError::Cancelled && !cancelled.samples, "cancellation publishes no output");
    PitchRenderResult realtime;
    {
        const CallbackScope callback;
        realtime = renderPitchEdits(samples, analysis, state);
    }
    check(realtime.error == PitchRenderError::Realtime && !realtime.samples, "render refuses the callback");
    const auto wrongLength = renderPitchEdits(std::span(samples).first(samples.size() - 1), analysis, state);
    check(wrongLength.error == PitchRenderError::InvalidSource && !wrongLength.samples, "mismatched PCM length refuses");
    edit.gainDb = std::numeric_limits<double>::quiet_NaN();
    const auto invalid = renderPitchEdits(samples, analysis, PitchEditState{{edit}});
    check(invalid.error == PitchRenderError::InvalidEdits && !invalid.samples, "invalid edit publishes no output");
}
} // namespace

int main() {
    moveSemitone();
    moveAndStretch();
    editCurve();
    transitionAndLimits();
    formantAndErrors();
    std::printf("%d pitch-render checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
