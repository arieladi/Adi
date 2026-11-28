// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/io_audit.hpp"
#include "adi/audio/pitch_analysis.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <numbers>

namespace {
std::atomic<unsigned> callbackAllocations{0};
}

#if defined(__GNUC__) && !defined(__clang__)
#define ADI_TEST_NOINLINE __attribute__((noinline))
#else
#define ADI_TEST_NOINLINE
#endif
ADI_TEST_NOINLINE void* operator new(std::size_t size) {
    if (adi::audio::onAudioThread) ++callbackAllocations;
    if (void* data = std::malloc(size == 0 ? 1 : size)) return data;
    throw std::bad_alloc();
}
ADI_TEST_NOINLINE void* operator new[](std::size_t size) { return operator new(size); }
ADI_TEST_NOINLINE void operator delete(void* data) noexcept { std::free(data); }
ADI_TEST_NOINLINE void operator delete[](void* data) noexcept { std::free(data); }
ADI_TEST_NOINLINE void operator delete(void* data, std::size_t) noexcept { std::free(data); }
ADI_TEST_NOINLINE void operator delete[](void* data, std::size_t) noexcept { std::free(data); }
#undef ADI_TEST_NOINLINE

namespace {
using namespace adi::audio;

int failures = 0;

void check(bool passed, const char* description) {
    if (!passed) {
        ++failures;
        std::printf("FAIL %s\n", description);
    }
}

// An analytic voice-like fixture with known note regions and pitch. Weak
// fundamentals, vowel-shaped upper harmonics, drift and vibrato exercise more
// of the detector than a pure sine. The fixture does not model a real singer.
void vowel(std::span<float> signal, std::uint32_t rate, double midi, double drift = 0, double vibrato = 0) {
    double phase = 0;
    const double attack = 0.006 * rate;
    const double release = 0.009 * rate;
    constexpr std::array harmonics{0.10, 0.43, 0.33, 0.12, 0.06, 0.18, 0.03};
    for (std::size_t frame = 0; frame < signal.size(); ++frame) {
        const double time = static_cast<double>(frame) / rate;
        const double fraction = static_cast<double>(frame) / static_cast<double>(signal.size() - 1);
        const double deviation = drift * (fraction - 0.5) + vibrato * std::sin(2 * std::numbers::pi * time * 5.7);
        const double hz = 440.0 * std::exp2((midi - 69.0 + deviation / 100.0) / 12.0);
        const double gain = std::min({1.0, static_cast<double>(frame) / attack,
                                      static_cast<double>(signal.size() - frame - 1) / release});
        double mixed = 0;
        for (std::size_t harmonic = 0; harmonic < harmonics.size(); ++harmonic) {
            const auto multiple = static_cast<double>(harmonic + 1);
            if (multiple * hz >= rate / 2.0) break;
            mixed += harmonics[harmonic] * std::sin(phase * multiple + multiple * 0.43);
        }
        signal[frame] = static_cast<float>(gain * mixed / 2.0);
        phase = std::remainder(phase + 2 * std::numbers::pi * hz / rate, 2 * std::numbers::pi);
    }
}

void scale() {
    constexpr std::array midi{60, 62, 64, 65, 67, 69, 71, 72};
    for (const auto rate : {8000u, 44100u, 48000u, 96000u}) {
        const auto duration = rate / 5;
        std::array<std::uint64_t, midi.size()> starts{};
        std::vector<float> mono(rate / 25);
        for (std::size_t note = 0; note < midi.size(); ++note) {
            starts[note] = mono.size();
            mono.resize(mono.size() + duration);
            vowel(std::span(mono).subspan(static_cast<std::size_t>(starts[note]), duration), rate, midi[note], 12, 18);
            if (note == 2 || note == 5) mono.resize(mono.size() + rate / 25);
        }
        mono.resize(mono.size() + rate / 30);
        const auto before = std::chrono::steady_clock::now();
        const auto result = analyzePitch(mono, rate);
        const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - before).count();
        check(result && result.analysis->notes.size() == midi.size(), "voice-like scale has eight notes, including semitone and legato changes");
        if (!result || result.analysis->notes.size() != midi.size()) {
            if (result) {
                for (const auto& note : result.analysis->notes)
                    std::printf("  found %llu–%llu at %.1f cents\n", static_cast<unsigned long long>(note.startFrame),
                                static_cast<unsigned long long>(note.endFrame), note.pitchCents);
            }
            continue;
        }
        const auto& analysis = *result.analysis;
        check(validPitchAnalysis(analysis) && analysis.sampleRate == rate && analysis.sourceFrames == mono.size(),
              "analysis returns ordered source-frame note ranges and curves");
        double maxCents = 0;
        double maxMilliseconds = 0;
        for (std::size_t index = 0; index < midi.size(); ++index) {
            const auto& note = analysis.notes[index];
            maxCents = std::max(maxCents, std::abs(note.pitchCents - 100 * midi[index]));
            const double onsetError = std::abs(static_cast<double>(note.startFrame) - static_cast<double>(starts[index]));
            const double endError = std::abs(static_cast<double>(note.endFrame) - static_cast<double>(starts[index] + duration));
            maxMilliseconds = std::max(maxMilliseconds, 1000.0 * std::max(onsetError, endError) / rate);
            check(note.curve.size() > 20, "voice-like note keeps the pitch curve");
        }
        std::printf("Voice-like scale, %u Hz: max %.2f cents, max boundary %.2f ms, analyzed %.0f ms of source in %.2f ms\n",
                    rate, maxCents, maxMilliseconds, 1000.0 * static_cast<double>(mono.size()) / rate, elapsed);
        check(maxCents < 15, "scale nominal pitches are within 15 cents");
        check(maxMilliseconds < 22, "scale boundaries are within 22 ms");
    }
}

void contoursAndRests() {
    constexpr std::uint32_t rate = 48000;
    std::vector<float> mono(rate * 2);
    vowel(std::span(mono).first(rate * 9 / 10), rate, 57, 64, 23);
    vowel(std::span(mono).subspan(rate), rate, 57, -64, 23);
    const auto result = analyzePitch(mono, rate);
    check(result && result.analysis->notes.size() == 2, "rests separate same-pitch notes without dividing vibrato or drift");
    if (result && result.analysis->notes.size() == 2) {
        const auto& notes = result.analysis->notes;
        std::printf("Drift %.2f/%.2f cents, residual RMS %.2f/%.2f cents\n", notes[0].driftCents, notes[1].driftCents,
                    notes[0].vibratoCents, notes[1].vibratoCents);
        check(notes[0].driftCents > 49 && notes[0].driftCents < 79 && notes[1].driftCents < -49 && notes[1].driftCents > -79,
              "drift is measured in signed cents across a segment");
        check(notes[0].vibratoCents > 10 && notes[0].vibratoCents < 20 && notes[1].vibratoCents > 10 && notes[1].vibratoCents < 20,
              "vibrato measurement follows residual RMS after drift removal");
    }

    for (const double midi : {35.0, 84.0, 88.0}) {
        std::vector<float> single(rate / 3);
        vowel(single, rate, midi);
        const auto tracked = analyzePitch(single, rate);
        if (tracked && tracked.analysis->notes.size() == 1)
            std::printf("Range check MIDI %.0f: %.2f cents\n", midi, tracked.analysis->notes.front().pitchCents - 100 * midi);
        check(tracked && tracked.analysis->notes.size() == 1 &&
              std::abs(tracked.analysis->notes.front().pitchCents - 100.0 * midi) < 12,
              "bass and soprano bounds retain fundamental pitch even with stronger harmonics");
    }
}

void smoothLegato() {
    constexpr std::uint32_t rate = 48000;
    constexpr std::array pitches{6000.0, 6200.0, 6000.0};
    std::vector<float> mono(rate * 3 / 2);
    double phase = 0;
    for (std::size_t frame = 0; frame < mono.size(); ++frame) {
        const double time = static_cast<double>(frame) / rate;
        double pitch = pitches.front();
        for (std::size_t index = 1; index < pitches.size(); ++index) {
            const double center = static_cast<double>(index) / 2;
            const double ramp = std::clamp((time - center + 0.045) / 0.09, 0.0, 1.0);
            pitch += (pitches[index] - pitches[index - 1]) * ramp * ramp * (3 - 2 * ramp);
        }
        pitch += 22 * std::sin(2 * std::numbers::pi * 5.0 * time);
        const double hz = 440.0 * std::exp2((pitch - 6900) / 1200.0);
        mono[frame] = static_cast<float>(0.2 * std::sin(phase) + 0.11 * std::sin(phase * 2));
        phase = std::remainder(phase + 2 * std::numbers::pi * hz / rate, 2 * std::numbers::pi);
    }
    const auto result = analyzePitch(mono, rate);
    check(result && result.analysis->notes.size() == pitches.size(), "slow legato transitions split without steady vibrato creating any extra note");
    if (!result) return;
    if (result.analysis->notes.size() != pitches.size()) {
        for (const auto& note : result.analysis->notes)
            std::printf("  legato note %.1f–%.1f ms at %.1f cents\n", 1000 * static_cast<double>(note.startFrame) / rate,
                        1000 * static_cast<double>(note.endFrame) / rate, note.pitchCents);
        return;
    }
    for (std::size_t index = 0; index < pitches.size(); ++index) {
        const auto& note = result.analysis->notes[index];
        const auto expectedStart = static_cast<double>(index * rate / 2);
        check(std::abs(static_cast<double>(note.startFrame) - expectedStart) / rate < 0.025 &&
              std::abs(note.pitchCents - pitches[index]) < 15, "continuous legato note keeps its measured pitch and boundary");
    }
}

std::uint64_t uncertaintyDistance(const PitchAnalysis& analysis, PitchBoundaryReason reason, std::uint64_t expected) {
    auto nearest = std::numeric_limits<std::uint64_t>::max();
    if (!analysis.boundaryUncertainty) return nearest;
    for (const auto& hint : *analysis.boundaryUncertainty) {
        if (hasPitchBoundaryReason(hint.reasons, reason))
            nearest = std::min(nearest, hint.frame >= expected ? hint.frame - expected : expected - hint.frame);
    }
    return nearest;
}

void boundaryEvidence() {
    constexpr std::uint32_t rate = 48000;
    const auto tone = [](std::size_t frame, double hz) {
        return std::sin(2 * std::numbers::pi * hz * static_cast<double>(frame) / rate);
    };
    std::vector<float> harmonic(rate / 2);
    for (std::size_t frame = 0; frame < harmonic.size(); ++frame)
        harmonic[frame] = static_cast<float>(0.2 * tone(frame, 220) + 0.31 * tone(frame, 440) + 0.12 * tone(frame, 660));
    const auto stable = analyzePitch(harmonic, rate);
    check(stable && stable.analysis->notes.size() == 1 && stable.analysis->boundaryUncertainty &&
          stable.analysis->boundaryUncertainty->empty(), "stable harmonics are not competing periods");

    constexpr auto onset = rate / 8;
    constexpr auto release = rate * 3 / 8;
    std::vector<float> breath(rate / 2);
    std::uint32_t random = 27;
    for (std::size_t frame = onset - rate / 20; frame < release + rate / 20; ++frame) {
        random = random * 1664525u + 1013904223u;
        const double noise = static_cast<double>(random >> 8) / 16777216.0 - 0.5;
        breath[frame] = static_cast<float>(frame >= onset && frame < release ? 0.35 * tone(frame, 220) : 0.9 * noise);
    }
    const auto edged = analyzePitch(breath, rate);
    check(edged && !edged.analysis->notes.empty(), "aperiodic edge fixture retains a voiced note");
    if (edged) {
        const auto first = uncertaintyDistance(*edged.analysis, PitchBoundaryReason::AperiodicTransition, onset);
        const auto last = uncertaintyDistance(*edged.analysis, PitchBoundaryReason::AperiodicTransition, release);
        check(first < rate / 40 && last < rate / 40, "audible unpitched material marks both note edges for review");
        if (first < rate / 40 && last < rate / 40)
            std::printf("Aperiodic candidate edges at %.2f/%.2f ms absolute from the known changes\n",
                        1000.0 * static_cast<double>(first) / rate, 1000.0 * static_cast<double>(last) / rate);
    }

    constexpr std::array pitches{60, 62, 65, 63, 67, 64, 69};
    constexpr std::size_t shortNote = rate * 65 / 1000;
    std::vector<float> quick(shortNote * pitches.size());
    double phase = 0;
    for (std::size_t frame = 0; frame < quick.size(); ++frame) {
        const double hz = 440.0 * std::exp2((pitches[frame / shortNote] - 69.0) / 12);
        quick[frame] = static_cast<float>(0.24 * std::sin(phase) + 0.1 * std::sin(2 * phase));
        phase = std::remainder(phase + 2 * std::numbers::pi * hz / rate, 2 * std::numbers::pi);
    }
    const auto fast = analyzePitch(quick, rate);
    check(fast && fast.analysis->notes.size() < pitches.size(), "short legato runs exercise ambiguous unsegmented events");
    if (fast) {
        std::size_t covered = 0;
        std::uint64_t worst = 0;
        for (std::size_t index = 1; index < pitches.size(); ++index) {
            const auto error = uncertaintyDistance(*fast.analysis, PitchBoundaryReason::RapidPitchChange, index * shortNote);
            if (error > rate / 50) continue;
            ++covered;
            worst = std::max(worst, error);
        }
        check(covered >= 5, "rapid suggestions expose skipped candidate boundaries without introducing notes");
        std::printf("65 ms run: %zu/%zu suggested edges within 20 ms, worst matched %.2f ms\n", covered, pitches.size() - 1,
                    1000.0 * static_cast<double>(worst) / rate);
    }

    constexpr auto doubledStart = rate / 3;
    constexpr auto doubledEnd = rate * 2 / 3;
    std::vector<float> doubled(rate);
    for (std::size_t frame = 0; frame < doubled.size(); ++frame) {
        const double second = frame >= doubledStart && frame < doubledEnd ? 0.19 * tone(frame, 350) : 0;
        doubled[frame] = static_cast<float>(0.24 * tone(frame, 220) + second);
    }
    const auto unsure = analyzePitch(doubled, rate);
    check(static_cast<bool>(unsure), "a summed two-period fixture still returns monophonic analysis");
    if (unsure) {
        const auto first = uncertaintyDistance(*unsure.analysis, PitchBoundaryReason::CompetingPeriodicity, doubledStart);
        const auto last = uncertaintyDistance(*unsure.analysis, PitchBoundaryReason::CompetingPeriodicity, doubledEnd);
        check(first < rate / 25 && last < rate / 25, "non-harmonic alternate periods mark the mixed portion's boundaries");
        if (first < rate / 25 && last < rate / 25)
            std::printf("Competing periods in two-tone overlap marked at %.2f/%.2f ms absolute from the known edges\n",
                        1000.0 * static_cast<double>(first) / rate, 1000.0 * static_cast<double>(last) / rate);
        check(validPitchAnalysis(*unsure.analysis), "uncertainties share a bounded, unique source coordinate order");
    }
}

void invalidUnvoicedAndGuards() {
    const auto silent = analyzePitch(std::vector<float>(1600), 8000);
    check(silent && silent.analysis->notes.empty(), "silence is a successful empty note analysis");
    std::vector<float> noise(10000);
    std::uint32_t random = 42;
    for (auto& sample : noise) {
        random = random * 1664525u + 1013904223u;
        sample = static_cast<float>(random >> 8) / 16777216.0f - 0.5f;
    }
    const auto unvoiced = analyzePitch(noise, 8000);
    check(unvoiced && unvoiced.analysis->notes.empty(), "broadband noise is unvoiced");
    check(analyzePitch({}, 48000).error == PitchAnalysisError::InvalidSource &&
          analyzePitch(noise, 7999).error == PitchAnalysisError::InvalidSource &&
          analyzePitch(noise, 384001).error == PitchAnalysisError::InvalidSource, "empty source and unsupported rates refuse");
    noise.back() = std::numeric_limits<float>::quiet_NaN();
    check(analyzePitch(noise, 8000).error == PitchAnalysisError::InvalidSource, "nonfinite input refuses without partial notes");
    std::stop_source cancelled;
    cancelled.request_stop();
    const auto stopped = analyzePitch(noise, 8000, cancelled.get_token());
    check(stopped.error == PitchAnalysisError::Cancelled && !stopped.analysis, "cancelled work has no partial analysis");

    PitchAnalysisResult callback;
    const auto io = callbackFileIo.load();
    const auto allocations = callbackAllocations.load();
    {
        CallbackScope scope;
        callback = analyzePitch(noise, 8000);
    }
    check(callback.error == PitchAnalysisError::Realtime && !callback.analysis &&
          callbackFileIo.load() == io && callbackAllocations.load() == allocations,
          "pitch analysis refuses on the audio thread without allocating or doing I/O");

    PitchAnalysis invalid{48000, 1000, {PitchNote{100, 250, 6000, 0, 0, {{150, 6000}, {200, 6002}}}}};
    check(validPitchAnalysis(invalid), "validation accepts a bounded note and ordered curve");
    invalid.notes.front().curve.back().frame = 150;
    check(!validPitchAnalysis(invalid), "validation refuses repeated frame coordinates");
    invalid.notes.front().curve.back().frame = 250;
    check(!validPitchAnalysis(invalid), "validation refuses a curve point on its exclusive end");
    invalid.notes.front().curve.back().frame = 200;
    invalid.notes.push_back(PitchNote{249, 251, 6000, 0, 0, {{250, 6000}}});
    check(!validPitchAnalysis(invalid), "validation refuses overlapping source notes");
    invalid.notes.pop_back();
    invalid.notes.front().curve.front().cents = std::numeric_limits<double>::max();
    check(!validPitchAnalysis(invalid), "validation refuses out-of-domain values before contour arithmetic");

    invalid.notes.front().curve.front().cents = 6000;
    invalid.boundaryUncertainty.emplace().push_back({1000, PitchBoundaryReason::AperiodicTransition});
    check(validPitchAnalysis(invalid), "uncertainty can mark the exclusive source end");
    invalid.boundaryUncertainty->push_back({1000, PitchBoundaryReason::RapidPitchChange});
    check(!validPitchAnalysis(invalid), "uncertainty refuses duplicate coordinates");
    invalid.boundaryUncertainty->pop_back();
    invalid.boundaryUncertainty->front().reasons = static_cast<PitchBoundaryReason>(128);
    check(!validPitchAnalysis(invalid), "uncertainty refuses unknown evidence bits");
}
} // namespace

int main() {
    scale();
    contoursAndRests();
    smoothLegato();
    boundaryEvidence();
    invalidUnvoicedAndGuards();
    return failures != 0;
}
