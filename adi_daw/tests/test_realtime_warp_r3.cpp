// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/io_audit.hpp"
#include "adi/audio/realtime_warp_r3.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/engine/warp_arrangement_clock.hpp"
#include "adi/engine/warp_r3_pages.hpp"
#include "adi/store_rows.hpp"
#include "adi/textproj.hpp"
#include "temp_directory.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <numbers>
#include <span>
#include <vector>

namespace {

constexpr std::uint32_t sampleRate = 48000;
constexpr double frequency = 440;

void check(bool condition, const char* message) {
    if (condition) return;
    std::cerr << message << '\n';
    std::exit(1);
}

struct Input {
    bool ready = true;
    bool variable = true;
    std::int64_t framesRead = 0;

    static bool read(void* context, std::int64_t frame, std::span<float> output) {
        auto& input = *static_cast<Input*>(context);
        if (!input.ready) return false;
        const auto phase = 2 * std::numbers::pi * frequency * static_cast<double>(frame) / sampleRate;
        output[0] = static_cast<float>(0.5 * std::sin(phase));
        output[1] = 0.5f * output[0];
        ++input.framesRead;
        return true;
    }

    static double ratio(void* context, std::int64_t frame) noexcept {
        if (!static_cast<Input*>(context)->variable) return 1;
        const auto progress = std::clamp(static_cast<double>(frame) / sampleRate, 0.0, 1.0);
        return 120 / (120 + 20 * progress);
    }
};

double measurePitch(std::span<const float> pcm) {
    double first = -1;
    double last = -1;
    std::size_t crossings = 0;
    for (std::size_t i = 1; i < pcm.size(); ++i) {
        if (pcm[i - 1] >= 0 || pcm[i] < 0) continue;
        const auto fraction = static_cast<double>(-pcm[i - 1]) / (pcm[i] - pcm[i - 1]);
        last = static_cast<double>(i - 1) + fraction;
        if (crossings++ == 0) first = last;
    }
    return crossings > 1 ? static_cast<double>(crossings - 1) * sampleRate / (last - first) : 0;
}

void testVoice() {
    using namespace adi::audio;
    WarpRenderPlan plan;
    plan.mode = WarpMode::Complex;
    plan.outputRate = sampleRate;
    RealtimeWarpR3 voice(plan, sampleRate, 2, 1.0);
    check(voice.latencySamples() > 0, "R3 must declare its prepared latency");

    Input input;
    const R3WarpInput source{&input, Input::read, Input::ratio};
    std::array<float, 2> frame{};
    {
        CallbackScope callback;
        check(!voice.pull(frame, source) && !voice.restart(0, 1), "R3 worker boundary");
    }
    input.ready = false;
    check(!voice.pull(frame, source) && frame == std::array<float, 2>{}, "missing source must be silent");
    check(voice.nextInputFrame() == 0, "missing input must remain pending");

    input.ready = true;
    std::vector<float> pcm(40000);
    float stereoError = 0;
    for (auto& sample : pcm) {
        check(voice.pull(frame, source), "primed R3 must deliver continuous output");
        stereoError = std::max(stereoError, std::abs(frame[1] - 0.5f * frame[0]));
        sample = frame[0];
    }
    const auto measured = measurePitch(std::span<const float>(pcm).subspan(8000));
    const auto cents = std::abs(1200 * std::log2(measured / frequency));
    check(cents < 5, "R3 tempo ramp must retain pitch");
    check(stereoError < 1.0e-4f, "R3 channel timing or gain diverged");
    check(input.framesRead > static_cast<std::int64_t>(pcm.size()), "ramping down stretch ratio consumes more input");

    constexpr std::int64_t loc = 17001;
    input.variable = false;
    check(voice.restart(loc, 1), "R3 seek reset");
    RealtimeWarpR3 fresh(plan, sampleRate, 2, 1);
    check(fresh.restart(loc, 1), "fresh R3 seek reset");
    std::array<float, 2> expected{};
    for (int i = 0; i < 9600; ++i) {
        check(voice.pull(frame, source) && fresh.pull(expected, source), "R3 stream after seek");
        check(frame == expected, "R3 seek must discard old phase and audio");
    }
    check(!voice.restart(loc, 0.0), "R3 must refuse unsupported ratios");
    check(voice.restart(loc, std::nextafter(RealtimeWarpR3::maxRatio, 3.0)) &&
          voice.restart(loc, std::nextafter(RealtimeWarpR3::minRatio, 0.0)),
          "R3 accepts mapping roundoff at the exact supported bounds");
    std::cout << "R3 latency " << voice.latencySamples() << " frames at " << sampleRate
              << " Hz, ramp pitch error " << cents << " cents, stereo peak error " << stereoError << '\n';
}

void testRepeatedRampPages() {
    using namespace adi;
    using namespace adi::engine;
    constexpr int projectBeats = 32;
    constexpr int loopBeats = 4;
    constexpr int beatsPerRamp = 8;
    constexpr int framesPerBeat = sampleRate / 2;
    constexpr int sourceFrames = loopBeats * framesPerBeat;
    constexpr std::int64_t beatTicks = textproj::kPPQ;
    constexpr std::uint32_t pageFrames = 8192;
    constexpr std::int64_t sourceOffset = 17;

    std::vector<rows::TempoEvent> events;
    for (int beat = 0; beat <= projectBeats; beat += beatsPerRamp) {
        const auto bpm = beat % (2 * beatsPerRamp) == 0 ? 120.0 : 140.0;
        events.push_back({beat * beatTicks, bpm, 1});
    }
    const WarpArrangementClock tempo(events, sampleRate);
    const std::array<WarpMarker, 2> markers{{{0, 0}, {sourceFrames, loopBeats * beatTicks}}};

    audio::WarpRenderPlan plan;
    plan.mode = audio::WarpMode::Complex;
    plan.outputRate = sampleRate;
    plan.sourceStart = sourceOffset;
    plan.sourceFrames = sourceFrames;
    WarpR3Pages pages(plan, sampleRate, 1, markers, tempo, 0, loopBeats * beatTicks, 0, loopBeats * beatTicks);

    std::vector<float> pcm(sourceOffset + sourceFrames);
    constexpr int pulseHalfWidth = 110;
    for (int beat = 1; beat < loopBeats; ++beat) {
        for (int offset = -pulseHalfWidth; offset <= pulseHalfWidth; ++offset) {
            const auto frame = static_cast<std::size_t>(sourceOffset + beat * framesPerBeat + offset);
            pcm[frame] = static_cast<float>(0.4 * (1 + std::cos(std::numbers::pi * offset / pulseHalfWidth)));
        }
    }
    const WarpR3Source source{&pcm, [](void* context, std::uint64_t first, float* output, std::uint32_t count) {
        const auto& input = *static_cast<const std::vector<float>*>(context);
        check(first >= sourceOffset && first + count <= input.size(), "R3 page read must stay in source window");
        check(count > 0 && count < pageFrames, "R3 pages must read bounded source chunks");
        std::copy_n(input.data() + first, count, output);
    }};

    const auto expectedFrame = [](int beat) {
        // Independently integrate the authored linear-in-beats ramps.
        constexpr double slope = 20.0 / beatsPerRamp;
        const auto fullRamps = beat / beatsPerRamp;
        const auto ascending = fullRamps % 2 == 0;
        const auto signedSlope = ascending ? slope : -slope;
        const auto from = ascending ? 120.0 : 140.0;
        const auto elapsed = fullRamps * (60 / slope) * std::log(140.0 / 120.0) +
            (60 / signedSlope) * std::log1p(signedSlope * (beat % beatsPerRamp) / from);
        return std::llround(elapsed * sampleRate);
    };
    const auto pulseError = [](std::span<const float> output, std::int64_t expected) {
        constexpr auto radius = 1024;
        const auto start = output.begin() + expected - radius;
        const auto end = output.begin() + expected + radius + 1;
        const auto peak = std::max_element(start, end);
        check(*peak > 0.15f, "R3 loop pulse must remain audible");
        return std::abs(peak - output.begin() - expected);
    };
    std::vector<float> output(static_cast<std::size_t>(expectedFrame(projectBeats)));
    for (std::size_t first = 0; first < output.size(); first += pageFrames) {
        const auto count = std::min<std::size_t>(pageFrames, output.size() - first);
        check(pages.render(static_cast<std::int64_t>(first), static_cast<std::uint32_t>(count),
                           output.data() + first, source), "R3 pages must fill through repeated ramps");
    }
    std::int64_t maximum = 0;
    for (int beat = 1; beat < projectBeats; ++beat) {
        if (beat % loopBeats != 0)
            maximum = std::max(maximum, static_cast<std::int64_t>(pulseError(output, expectedFrame(beat))));
    }

    const auto target = expectedFrame(26);
    constexpr std::int64_t lead = 4096;
    std::array<float, pageFrames> cold{};
    check(pages.render(target - lead, pageFrames, cold.data(), source), "R3 cold page after backward seek");
    const auto seekError = pulseError(cold, lead);
    check(maximum <= 64 && seekError <= 64, "R3 loop pulses must stay within 64 frames of the musical grid");
    std::cout << "R3 page render at 48 kHz: maximum grid error " << maximum
              << " frames over " << projectBeats << " beats, cold seek error " << seekError << " frames\n";
}

void testDynamicStereoPlacementEnd() {
    using namespace adi;
    using namespace adi::engine;
    constexpr std::int64_t framesPerBeat = sampleRate / 2, ticksPerBeat = textproj::kPPQ;
    constexpr std::int64_t loopBeats = 8, sourceFrames = loopBeats * framesPerBeat;
    constexpr std::int64_t page = 8192, changeFrame = 12 * page + 65;
    const auto changeTick = std::llround(static_cast<double>(changeFrame) * ticksPerBeat / framesPerBeat);
    const std::array<rows::TempoEvent, 2> events{{{0, 120, 0}, {changeTick, 140, 0}}};
    const WarpArrangementClock tempo(events, sampleRate);
    const std::array<WarpMarker, 2> markers{{{0, 0}, {sourceFrames, loopBeats * ticksPerBeat}}};
    const auto exactEnd = tempo.frameAtTick(2 * loopBeats * ticksPerBeat);
    const auto end = static_cast<std::int64_t>(std::llround(exactEnd));
    check(static_cast<double>(end) > exactEnd, "the fixture rounds its terminal loop edge up in the arrangement");

    std::vector<float> input(static_cast<std::size_t>(sourceFrames) * 2);
    constexpr std::int64_t pulseHalfWidth = 48;
    for (std::int64_t frame = 0; frame < sourceFrames; ++frame) {
        const auto offset = frame % framesPerBeat;
        const auto distance = std::min(offset, framesPerBeat - offset);
        if (distance >= pulseHalfWidth || frame >= sourceFrames - pulseHalfWidth) continue;
        const auto pulse = static_cast<float>(0.4 * (1 + std::cos(std::numbers::pi *
            static_cast<double>(distance) / pulseHalfWidth)));
        const auto index = static_cast<std::size_t>(frame) * 2;
        input[index] = pulse;
        input[index + 1] = -pulse;
    }
    const WarpR3Source source{&input, [](void* context, std::uint64_t first, float* target, std::uint32_t count) {
        const auto& signal = *static_cast<const std::vector<float>*>(context);
        check((first + count) * 2 <= signal.size(), "dynamic R3 stays inside the stored source window");
        std::copy_n(signal.data() + first * 2, count * 2, target);
    }};
    audio::WarpRenderPlan plan;
    plan.mode = audio::WarpMode::Complex;
    plan.outputRate = sampleRate;
    plan.sourceFrames = sourceFrames;
    WarpR3Pages pages(plan, sampleRate, 2, markers, tempo, 0, loopBeats * ticksPerBeat,
                      0, loopBeats * ticksPerBeat, end);

    constexpr std::int64_t terminalWindow = 1024;
    std::vector<float> output(static_cast<std::size_t>(end + terminalWindow) * 2);
    for (std::int64_t first = 0; first < end + terminalWindow; first += page) {
        const auto count = static_cast<std::uint32_t>(std::min(page, end + terminalWindow - first));
        check(pages.render(first, count, output.data() + static_cast<std::size_t>(first) * 2, source),
              "dynamic stereo R3 fills pages through the end of the placement");
    }
    float sidePeak = 0, centerPeak = 0, terminalPeak = 0;
    for (std::size_t i = 0; i < output.size(); i += 2) {
        const auto left = output[i], right = output[i+1];
        sidePeak = std::max(sidePeak, std::abs(left));
        centerPeak = std::max(centerPeak, std::abs(left + right));
        if (i / 2 >= static_cast<std::size_t>(end - terminalWindow))
            terminalPeak = std::max(terminalPeak, std::abs(left));
    }
    check(sidePeak > 0.5f && centerPeak < 1e-6f, "dynamic R3 keeps the stereo polarity of audible attacks");
    check(terminalPeak < 1e-5f, "R3 lookahead cannot bring an omitted loop attack into the cutoff");
    std::cout << "Dynamic stereo R3 step: side peak " << sidePeak << ", center peak " << centerPeak
              << ", terminal peak " << terminalPeak << " FS\n";
}

void testDynamicPageHistory() {
    using namespace adi;
    using namespace adi::engine;
    constexpr std::uint32_t pageFrames = 8192;
    constexpr std::int64_t beatTicks = textproj::kPPQ;
    constexpr std::uint32_t sourceFrames = 2 * sampleRate;
    constexpr std::size_t pageCount = 24;
    const std::array<rows::TempoEvent, 2> events{{{0, 140, 1}, {40 * beatTicks, 120, 0}}};
    const WarpArrangementClock tempo(events, sampleRate);
    const std::array<WarpMarker, 2> markers{{{0, 0}, {sourceFrames, 4 * beatTicks}}};
    const auto end = static_cast<std::int64_t>(std::llround(tempo.frameAtTick(40 * beatTicks)));
    audio::WarpRenderPlan plan;
    plan.mode = audio::WarpMode::Complex;
    plan.sourceFrames = sourceFrames;
    plan.outputRate = sampleRate;

    std::vector<float> sine(sourceFrames);
    for (std::size_t i = 0; i < sine.size(); ++i)
        sine[i] = static_cast<float>(0.3 * std::sin(2 * std::numbers::pi * frequency *
                                                   static_cast<double>(i) / sampleRate));
    const WarpR3Source source{&sine, [](void* context, std::uint64_t first, float* target, std::uint32_t count) {
        const auto& wave = *static_cast<const std::vector<float>*>(context);
        check(first + count <= wave.size(), "history fixture stays inside source PCM");
        std::copy_n(wave.data() + first, count, target);
    }};
    WarpR3Pages rolling(plan, sampleRate, 1, markers, tempo, 0, 4 * beatTicks, 0, 4 * beatTicks, end);
    WarpR3Pages seeking(plan, sampleRate, 1, markers, tempo, 0, 4 * beatTicks, 0, 4 * beatTicks, end);
    std::vector<float> baseline(pageCount * pageFrames);
    std::array<float, pageFrames> sought{};
    for (std::size_t i = 0; i < pageCount; ++i) {
        const auto first = i * pageFrames;
        check(rolling.render(static_cast<std::int64_t>(first), pageFrames, baseline.data() + first, source),
              "dynamic R3 supplies rolling pages during a descending tempo ramp");
    }

    float maximumBoundaryStep = 0, rollingBoundaryStep = 0;
    bool exact = true;
    for (std::size_t i = pageCount - 2; i > 0; --i) {
        const auto first = i * pageFrames;
        check(seeking.render(static_cast<std::int64_t>(first), pageFrames, sought.data(), source),
              "dynamic R3 supplies uncached pages in reverse request order");
        exact = exact && std::equal(sought.begin(), sought.end(), baseline.data() + first);
        maximumBoundaryStep = std::max({maximumBoundaryStep, std::abs(baseline[first-1]-sought.front()),
                                      std::abs(sought.back()-baseline[first+pageFrames])});
        rollingBoundaryStep = std::max({rollingBoundaryStep, std::abs(baseline[first-1]-baseline[first]),
                                      std::abs(baseline[first+pageFrames-1]-baseline[first+pageFrames])});
    }
    std::cout << "Dynamic R3 140 to 120 BPM page boundary: mixed history " << maximumBoundaryStep
              << " FS, rolling " << rollingBoundaryStep << " FS\n";
    check(exact && maximumBoundaryStep <= 0.025f,
          "a cold dynamic page reproduces rolling PCM and joins existing pages without a click");
}

void testSteadyPages() {
    using namespace adi;
    using namespace adi::engine;
    constexpr std::size_t sourceBeat = 18000, outputBeat = sampleRate / 2;
    constexpr std::size_t sourceFrames = 4 * sourceBeat, outputFrames = 4 * outputBeat;
    constexpr std::int64_t beatTicks = textproj::kPPQ;
    const std::array<rows::TempoEvent, 1> events{{{0, 120, 0}}};
    const WarpArrangementClock tempo(events, sampleRate);
    const std::array<WarpMarker, 2> markers{{{0, 0}, {sourceFrames, 4 * beatTicks}}};
    test::TempDirectory tmp{"realtime_warp_r3", "steady"};

    std::vector<float> pcm(sourceFrames);
    for (std::size_t frame = 0; frame < pcm.size(); ++frame) {
        const auto at = static_cast<double>(frame % sourceBeat);
        const auto envelope = std::clamp(std::min(at - 3000, 16000 - at) / 512, 0.0, 1.0);
        const auto phase = 2 * std::numbers::pi * frequency * static_cast<double>(frame) / sampleRate;
        pcm[frame] = static_cast<float>(0.3 * envelope * std::sin(phase));
    }
    audio::WarpRenderPlan plan;
    plan.source = tmp.path() / "steady.wav";
    plan.workFolder = tmp.path();
    plan.grainSeconds = 0.03;
    plan.outputRate = sampleRate;
    plan.sourceFrames = pcm.size();
    plan.anchors = {{0, 0}, {sourceFrames, outputFrames}};
    audio::WavWriter wav(plan.source, sampleRate, 1, audio::WavFormat::Float32);
    wav.write(pcm.data(), static_cast<std::uint32_t>(sourceFrames));
    wav.close();
    const WarpR3Source source{&pcm, [](void* context, std::uint64_t first, float* output, std::uint32_t count) {
        const auto& input = *static_cast<const std::vector<float>*>(context);
        check(first + count <= input.size(), "steady R3 read must stay in its physical window");
        std::copy_n(input.data() + first, count, output);
    }};

    constexpr std::uint32_t request = 4096;
    std::array<float, request> piece{};
    std::vector<float> stream(outputFrames), reference(outputFrames);
    for (const auto mode : {audio::WarpMode::Tones, audio::WarpMode::Complex, audio::WarpMode::ComplexPro}) {
        plan.mode = mode;
        WarpR3Pages sequential(plan, sampleRate, 1, markers, tempo, 0, 4 * beatTicks, 0, 0);
        for (std::size_t first = 0; first < stream.size(); first += request) {
            const auto count = static_cast<std::uint32_t>(std::min<std::size_t>(request, stream.size() - first));
            check(sequential.render(static_cast<std::int64_t>(first), count, stream.data() + first, source),
                  "steady R3 supplies consecutive page requests");
        }
        WarpR3Pages seeking(plan, sampleRate, 1, markers, tempo, 0, 4 * beatTicks, 0, 0);
        for (const auto first : {65001, 7702, 72505, 32001, 2, 65001}) {
            check(seeking.render(first, request, piece.data(), source) &&
                  std::equal(piece.begin(), piece.end(), stream.begin() + first),
                  "steady R3 PCM is exact through cold and repeated out-of-order seeks");
        }
        const auto latency = seeking.latencySamples();
        seeking.releaseIdle();
        check(seeking.latencySamples() == latency && seeking.render(32001, request, piece.data(), source) &&
              std::equal(piece.begin(), piece.end(), stream.begin() + 32001),
              "steady R3 remains exact after idle storage reclamation");

        constexpr std::int64_t moveTicks = 2 * beatTicks, moveFrames = 2 * outputBeat;
        constexpr std::int64_t trim = 5 * outputBeat / 2;
        WarpR3Pages moved(plan, sampleRate, 1, markers, tempo, moveTicks, 5 * beatTicks / 2, 0, 0);
        check(moved.render(moveFrames + trim - request / 2, request, piece.data(), source) &&
              std::equal(piece.begin(), piece.begin() + request / 2, stream.begin() + trim - request / 2) &&
              std::all_of(piece.begin() + request / 2, piece.end(), [](float value) { return value == 0; }),
              "steady R3 move and end trim retain exact source-clock PCM before the cut");

        constexpr std::int64_t loopEnd = 3 * outputBeat;
        WarpR3Pages looped(plan, sampleRate, 1, markers, tempo, 0, 4 * beatTicks, beatTicks, 2 * beatTicks);
        check(looped.render(loopEnd - request / 2, request, piece.data(), source) &&
              std::equal(piece.begin(), piece.begin() + request / 2, stream.begin() + loopEnd - request / 2) &&
              std::equal(piece.begin() + request / 2, piece.end(), stream.begin() + outputBeat),
              "steady R3 loop folds to exact existing samples at the loop boundary");

        const auto prepared = audio::prepareWarp(plan);
        check(static_cast<bool>(prepared) && prepared.prepared->frames == outputFrames,
              "prepare steady offline R3 reference of the same duration");
        audio::WavReader offline(prepared.prepared->file);
        check(offline.read(reference.data(), static_cast<std::uint32_t>(outputFrames)) == outputFrames,
              "read steady R3 reference");
        double streamPower = 0, referencePower = 0, worstLocalLevelDb = 0;
        std::int64_t largestOnsetDelta = 0;
        for (std::size_t beat = 0; beat < 4; ++beat) {
            const auto onset = [beat](std::span<const float> signal) {
                const auto begin = signal.begin() + static_cast<std::ptrdiff_t>(beat * outputBeat + 2500);
                const auto end = begin + 3500;
                const auto found = std::find_if(begin, end, [](float value) { return std::abs(value) > 0.075f; });
                check(found != end, "steady R3 onset remains audible");
                return found - begin;
            };
            largestOnsetDelta = std::max(largestOnsetDelta,
                static_cast<std::int64_t>(std::abs(onset(stream) - onset(reference))));
            constexpr std::size_t window = 256;
            for (auto offset = beat * outputBeat + 7000; offset + window < beat * outputBeat + 19000; offset += window / 2) {
                double streamWindow = 0, referenceWindow = 0;
                for (auto i = offset; i < offset + window; ++i) {
                    streamWindow += static_cast<double>(stream[i])*stream[i];
                    referenceWindow += static_cast<double>(reference[i])*reference[i];
                }
                streamPower += streamWindow;
                referencePower += referenceWindow;
                worstLocalLevelDb = std::max(worstLocalLevelDb, std::abs(10 * std::log10(streamWindow / referenceWindow)));
            }
        }
        const auto pitch = [](std::span<const float> signal) { return measurePitch(signal.subspan(11000, 4500)); };
        const auto pitchCents = std::abs(1200 * std::log2(pitch(stream) / pitch(reference)));
        const auto levelDb = 10 * std::log10(streamPower / referencePower);
        check(pitchCents <= 5 && std::abs(levelDb) < 0.3 && worstLocalLevelDb <= 1.5 && largestOnsetDelta <= 96,
              "steady real-time and offline R3 agree in pitch, RMS and onset timing");
        std::cout << audio::warpModeName(mode) << " steady real-time versus prepared: pitch " << pitchCents
                  << " cents, RMS " << levelDb << " dB, local window " << worstLocalLevelDb
                  << " dB, onset delta " << largestOnsetDelta << " frames\n";
    }
}

} // namespace

int main() {
    testVoice();
    testRepeatedRampPages();
    testDynamicStereoPlacementEnd();
    testDynamicPageHistory();
    testSteadyPages();
}
