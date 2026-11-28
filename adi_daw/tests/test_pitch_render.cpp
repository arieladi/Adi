// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/io_audit.hpp"
#include "adi/audio/pitch_render.hpp"

#include <algorithm>
#include <array>
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

struct StereoLevel { double mid, side; };

StereoLevel stereoLevel(std::span<const float> interleaved, double frequency) {
    const auto frames = interleaved.size() / 2;
    double midCos = 0, midSin = 0, sideCos = 0, sideSin = 0;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const double left = interleaved[2 * frame];
        const double right = interleaved[2 * frame + 1];
        const double angle = tau * frequency * static_cast<double>(frame) / rate;
        const double cosine = std::cos(angle);
        const double sine = std::sin(angle);
        midCos += (left + right) * cosine;
        midSin += (left + right) * sine;
        sideCos += (left - right) * cosine;
        sideSin += (left - right) * sine;
    }
    return {std::hypot(midCos, midSin) / static_cast<double>(frames),
            std::hypot(sideCos, sideSin) / static_cast<double>(frames)};
}

void stereoPitchPrecision() {
    constexpr auto frames = rate / 2;
    std::vector<float> pcm(2 * frames);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const double phase = tau * 220 * static_cast<double>(frame) / rate;
        const double center = 0.2 * std::sin(phase);
        const double side = 0.12 * std::cos(phase);
        pcm[2 * frame] = static_cast<float>(center + side);
        pcm[2 * frame + 1] = static_cast<float>(center - side);
    }
    const PitchAnalysis analysis{rate, frames, {steadyNote(0, frames, 5700)}};
    auto edit = resolvePitchNoteEdit(analysis, {}, 0);
    edit.pitchShiftCents = 100;
    const auto result = renderPitchEdits(pcm, 2, analysis, PitchEditState{{edit}});
    check(result && result.samples->size() == pcm.size(), "steady quadrature stereo renders a shared pitch shift");
    if (!result || result.samples->size() != pcm.size()) return;
    std::vector<float> mid(rate / 5);
    for (std::size_t frame = 0; frame < mid.size(); ++frame) {
        const auto first = 2 * (rate / 8 + frame);
        mid[frame] = ((*result.samples)[first] + (*result.samples)[first + 1]) * 0.5f;
    }
    const double shifted = cents(220, measureHz(mid));
    std::printf("steady stereo center transpose: %.2f cents (requested 100)\n", shifted);
    check(std::abs(shifted - 100) < 5 && rms(mid) > 0.06, "steady stereo center stays within five cents of the target");
}

void coherentStereo() {
    constexpr auto noteEnd = rate * 7 / 10;
    constexpr double centerHz = 221, sideHz = 673;
    std::vector<float> pcm(2 * rate);
    for (std::size_t frame = 0; frame < noteEnd; ++frame) {
        const double time = static_cast<double>(frame) / rate;
        const double center = 0.2 * std::sin(tau * centerHz * time);
        // Strong side-only transients drive different phase decisions if
        // the two channels are analyzed by independent mono processors.
        const double envelope = 0.12 + 0.35 * std::pow(std::max(0.0, std::cos(tau * 7 * time)), 8);
        const double side = envelope * std::sin(tau * sideHz * time);
        pcm[2 * frame] = static_cast<float>(center + side);
        pcm[2 * frame + 1] = static_cast<float>(center - side);
    }
    pcm[pcm.size() - 2] = 0.17f;
    pcm.back() = -0.11f;
    const PitchAnalysis analysis{rate, rate, {steadyNote(0, noteEnd, 5700)}};
    const PitchEditState neutral;
    const auto untouched = renderPitchEdits(pcm, 2, analysis, neutral);
    check(untouched && *untouched.samples == pcm, "neutral stereo retains every interleaved sample");

    struct Case { const char* name; double shift, tilt; std::uint64_t end; };
    const std::array cases = {Case{"static transpose", 100, 0, noteEnd},
                              Case{"transpose and stretch", 300, 0, rate},
                              Case{"varying tilt", 300, 200, noteEnd}};
    auto edit = resolvePitchNoteEdit(analysis, neutral, 0);
    for (const auto& test : cases) {
        edit.pitchShiftCents = test.shift;
        edit.tiltCents = test.tilt;
        edit.endFrame = test.end;
        const auto result = renderPitchEdits(pcm, 2, analysis, PitchEditState{{edit}});
        check(result && result.samples->size() == pcm.size(), "stereo DSP retains interleaved frame count");
        if (!result || result.samples->size() != pcm.size()) continue;
        const std::span rendered(*result.samples);
        const auto body = rendered.subspan(rate / 4, rate);
        const double ratio = std::exp2(test.shift / 1200);
        const auto center = stereoLevel(body, centerHz * ratio);
        const auto side = stereoLevel(body, sideHz * ratio);
        const double centerLeak = 20 * std::log10(center.side / center.mid);
        const double sideLeak = 20 * std::log10(side.mid / side.side);
        std::printf("stereo %s: center-to-side %.2f dB, side-to-center %.2f dB\n", test.name, centerLeak, sideLeak);
        check(center.mid > 0.02 && side.side > 0.008 && centerLeak < -36 && sideLeak < -36,
              "center and side stay audible and separated during stereo correction");
        const auto end = static_cast<std::size_t>(test.end) * 2;
        check(std::equal(pcm.begin() + static_cast<std::ptrdiff_t>(end), pcm.end(), rendered.subspan(end).begin()),
              "stereo correction preserves samples past its destination");
        if (test.end > noteEnd) {
            check(rms(rendered.subspan(2 * (noteEnd + rate / 20), rate / 5)) > 0.05,
                  "stereo stretch renders sound past the original note boundary");
        }
        if (test.tilt != 0) {
            const auto midFrequency = [&](std::size_t start) {
                std::vector<float> mid(rate / 10);
                for (std::size_t frame = 0; frame < mid.size(); ++frame)
                    mid[frame] = (rendered[2 * (start + frame)] + rendered[2 * (start + frame) + 1]) * 0.5f;
                return measureHz(mid);
            };
            const double tilt = cents(midFrequency(rate / 5), midFrequency(rate / 2));
            const double expected = test.tilt * static_cast<double>(rate * 3 / 10) / noteEnd;
            std::printf("stereo mid slope: %.2f cents over 300 ms (requested %.2f)\n", tilt, expected);
            // Phase locking around large side transients can perturb the
            // center's local frequency. Require a measurable intended sweep.
            check(std::abs(tilt - expected) < 25, "stereo center follows the varying pitch correction");
        }
    }
    const auto oddSamples = renderPitchEdits(std::span(pcm).first(pcm.size() - 1), 2, analysis, neutral);
    const auto unsupportedChannels = renderPitchEdits(pcm, 3, analysis, neutral);
    check(oddSamples.error == PitchRenderError::InvalidSource && unsupportedChannels.error == PitchRenderError::InvalidSource,
          "partial stereo frames and unsupported channel counts refuse");
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
    stereoPitchPrecision();
    coherentStereo();
    formantAndErrors();
    std::printf("%d pitch-render checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
