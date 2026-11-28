// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/audio/decode.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/audio/pitch_compare.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/media/blake3.hpp"
#include "adi/media/clip_pitch.hpp"
#include "adi/media/clip_pitch_render.hpp"
#include "adi/pitch_edit_store.hpp"
#include "temp_directory.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <stdexcept>
#include <thread>

namespace {
using namespace adi;
using namespace adi::media;
namespace fs = std::filesystem;
constexpr std::uint32_t rate = 48000;
constexpr std::uint32_t frames = 36000;
constexpr std::uint16_t channels = 2;
constexpr std::uint32_t start = 2200, length = 30000;
int checks = 0, failures = 0;

void check(bool condition, const char* description) {
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL %s\n", description);
    }
}

void write(const fs::path& output, const float* pcm) {
    audio::WavWriter file(output, rate, channels, audio::WavFormat::Float32);
    file.write(pcm, frames);
    file.close();
}

std::vector<float> read(const fs::path& source) {
    audio::WavReader reader(source);
    std::vector<float> samples(static_cast<std::size_t>(reader.frames()) * reader.channels());
    if (reader.read(samples.data(), static_cast<std::uint32_t>(reader.frames())) != reader.frames())
        throw std::runtime_error("short test PCM read");
    return samples;
}

double estimateHz(std::span<const float> pcm, std::size_t channel, std::size_t begin, std::size_t end) {
    std::size_t crossings = 0;
    double first = 0, last = 0;
    for (std::size_t frame = begin + 1; frame < end; ++frame) {
        const auto a = pcm[(frame - 1) * channels + channel];
        const auto b = pcm[frame * channels + channel];
        if (a <= 0 && b > 0) {
            const double position = static_cast<double>(frame - 1) - a / (static_cast<double>(b) - a);
            if (crossings == 0) first = position;
            last = position;
            ++crossings;
        }
    }
    return crossings > 1 ? static_cast<double>(crossings - 1) * rate / (last - first) : 0;
}

double rms(std::span<const float> pcm, std::size_t channel, std::size_t begin, std::size_t end) {
    double sum = 0;
    for (auto frame = begin; frame < end; ++frame) {
        const auto value = static_cast<double>(pcm[frame * channels + channel]);
        sum += value * value;
    }
    return std::sqrt(sum / static_cast<double>(end - begin));
}

void run() {
    test::TempDirectory temporary{"clip_pitch_render", "stereo"};
    const auto path = temporary.path() / "voice.wav";
    const auto work = temporary.path() / "work";
    std::vector<float> stereo(static_cast<std::size_t>(frames) * channels);
    std::vector<float> mono(frames);
    for (std::uint32_t frame = 4800; frame < 24000; ++frame) {
        const auto edge = std::min(frame - 4800, 24000 - frame);
        const auto wet = std::min(1.0, static_cast<double>(edge) / 960);
        const double phase = 2 * std::numbers::pi * 220 * frame / rate;
        const auto tone = static_cast<float>(wet * 0.42 * std::sin(phase));
        stereo[2 * frame] = tone;
        stereo[2 * frame + 1] = tone * 0.38f;
        mono[frame] = static_cast<float>((static_cast<double>(stereo[2 * frame]) + stereo[2 * frame + 1]) / 2);
    }
    stereo[2 * start] = 0.11f;
    stereo[2 * start + 1] = -0.11f;
    stereo[2 * (start + 800)] = 0.17f;
    stereo[2 * (start + 800) + 1] = -0.17f;
    constexpr std::uint32_t landmark = 1600;
    stereo[2 * (start + length - 1 - landmark)] = -0.5f;
    stereo[2 * (start + length - 1 - landmark) + 1] = 0.5f;
    stereo[2 * (start + length - 1)] = -0.22f;
    stereo[2 * (start + length - 1) + 1] = 0.22f;
    write(path, stereo.data());
    const auto hash = blake3File(path);
    check(static_cast<bool>(hash), "hash original source");
    if (!hash) return;

    audio::DecodeCache decoded(temporary.path() / "decoded");
    AnalysisCache cache(temporary.path() / "analysed");
    const AnalysisIdentity identity{hash.hex, rate, channels, frames};
    const auto captured = buildSourcePitchAnalysisFromMono(identity, mono, &cache);
    check(captured && captured.analysis->notes.size() == 1, "fixture has one voiced segment");
    if (!captured || captured.analysis->notes.size() != 1) return;
    const auto& note = captured.analysis->notes.front();
    auto edits = std::make_shared<clip_edit::ClipPitchEdits>();
    edits->clipId = 4;
    edits->sourceHash = hash.hex;
    edits->sampleRate = rate;
    edits->sourceFrames = frames;
    edits->analysisVersion = kPitchAnalysisVersion;
    edits->analysisConfiguration = kPitchAnalysisConfiguration;
    auto edit = audio::resolvePitchNoteEdit(*captured.analysis, {}, 0);
    edit.pitchShiftCents = 100;
    edits->rows.push_back({note.startFrame, note.endFrame, edit});
    ClipPitchRenderPlan plan{path, work, edits, start, length, false, {}};

    const auto ready = prepareClipPitchRender(plan, {}, &decoded, &cache);
    check(ready && ready.prepared->channels == channels && ready.prepared->rate == rate &&
          ready.prepared->frames == length, "render keeps channel count, source rate and exact trimmed extent");
    if (!ready) {
        if (ready.detail) std::printf("render error: %s\n", ready.detail->c_str());
        return;
    }
#if !defined(_WIN32)
    const auto permissions = fs::status(ready.prepared->file.parent_path()).permissions();
    check((permissions & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none,
          "prepared source is inside an owner-only stage");
#endif
    const auto pcm = read(ready.prepared->file);
    constexpr auto bodyStart = 10500u - start, bodyEnd = 19500u - start;
    const auto left = estimateHz(pcm, 0, bodyStart, bodyEnd);
    const auto right = estimateHz(pcm, 1, bodyStart, bodyEnd);
    const auto ratio = rms(pcm, 1, bodyStart, bodyEnd) / rms(pcm, 0, bodyStart, bodyEnd);
    std::printf("stereo pitch: left %.3f Hz, right %.3f Hz, right/left level %.4f\n", left, right, ratio);
    check(std::abs(left - 233.08188) < 1 && std::abs(right - 233.08188) < 1 && std::abs(ratio - 0.38) < 0.01,
          "left and right get one semitone with their original balance");
    check(pcm[0] == stereo[2 * start] && pcm[1] == stereo[2 * start + 1] &&
          pcm[2 * length - 2] == stereo[2 * (start + length - 1)] &&
          pcm[2 * length - 1] == stereo[2 * (start + length - 1) + 1],
          "unedited window boundaries are sample-identical in both channels");

    auto reverse = plan;
    reverse.reverse = true;
    const auto reversed = prepareClipPitchRender(reverse, {}, &decoded, &cache);
    check(static_cast<bool>(reversed), "pitch preparation supports an unwarped reversed window");
    if (reversed) {
        const auto reversedPcm = read(reversed.prepared->file);
        bool matches = true;
        for (std::size_t frame = 0; frame < length; ++frame)
            for (std::size_t channel = 0; channel < channels; ++channel)
                matches = matches && (reversedPcm[frame * channels + channel] == pcm[(length - 1 - frame) * channels + channel]);
        check(matches, "reverse follows pitch and trim and preserves frame/channel pairing");
    }

    auto warp = plan;
    auto& warpPlan = warp.warp.emplace();
    warpPlan.mode = audio::WarpMode::RePitch;
    warpPlan.outputRate = rate;
    warpPlan.anchors = {{0, 0}, {length, 2 * length}};
    fs::path warpedFile;
    {
        const auto warped = prepareClipPitchRender(warp, {}, &decoded, &cache);
        check(warped && warped.prepared->frames == 2 * length && warped.prepared->channels == channels,
              "normal Warp receives pitch-rendered PCM and preserves the projected extent");
        if (warped) {
            warpedFile = warped.prepared->file;
            const auto warpedPcm = read(warped.prepared->file);
            const auto warpedHz = estimateHz(warpedPcm, 1, 2 * bodyStart, 2 * bodyEnd);
            std::printf("Pitch Editor into Re-Pitch Warp: %.3f Hz on right, %llu output frames\n", warpedHz,
                        static_cast<unsigned long long>(warped.prepared->frames));
            check(std::abs(warpedHz - 116.54094) < 1, "Re-Pitch halves the newly transposed frequency");
        } else if (warped.detail) {
            std::printf("warp error: %s\n", warped.detail->c_str());
        }
    }
    check(!warpedFile.empty() && !fs::exists(warpedFile.parent_path().parent_path()),
          "prepared Warp and intermediate stages retire with their owners");
    warp.reverse = true;
    // The first quarter stretches 3x, the rest plays at unity. Mirroring
    // these Warp anchors or reversing the already-warped output would move
    // the asymmetric stereo landmarks to a different output address.
    warpPlan.anchors = {{0, 0}, {length / 4, 3 * length / 4}, {length, 3 * length / 2}};
    const auto reverseWarp = prepareClipPitchRender(warp, {}, &decoded, &cache);
    check(reverseWarp && reverseWarp.prepared->frames == 3 * length / 2,
          "pitch and reverse compose with Warp on its declared timeline");
    if (reverseWarp && reversed) {
        auto referencePlan = warpPlan;
        referencePlan.source = reversed.prepared->file;
        referencePlan.workFolder = work;
        referencePlan.sourceStart = 0;
        referencePlan.sourceFrames = length;
        const auto corrected = audio::prepareWarp(referencePlan);
        const auto expected = corrected ? read(corrected.prepared->file) : std::vector<float>{};
        check(corrected && read(reverseWarp.prepared->file) == expected,
              "corrected waveform equals independently applying unchanged Warp to reversed pitch PCM");

        std::vector<float> raw(static_cast<std::size_t>(length) * channels);
        for (std::size_t frame = 0; frame < length; ++frame)
            std::copy_n(stereo.data() + (start + length - 1 - frame) * channels,
                        channels, raw.data() + frame * channels);
        referencePlan.source = temporary.path() / "original-reverse.wav";
        {
            audio::WavWriter writer(referencePlan.source, rate, channels, audio::WavFormat::Float32);
            writer.write(raw.data(), length);
            writer.close();
        }
        const auto original = audio::prepareWarp(referencePlan);
        const auto comparison = prepareClipPitchCompare(warp, {}, &decoded, &cache);
        check(comparison && original && comparison.prepared->frames() == 3 * length / 2,
              "same-clock comparison prepares both reversed Warp lanes");
        if (comparison && corrected && original) {
            const auto originalPcm = read(original.prepared->file);
            audio::PitchComparePlayback playback(comparison.prepared);
            std::vector<float> samples(expected.size());
            const bool editedMatch = playback.readAt(0, samples) && samples == expected;
            playback.select(audio::PitchCompareSide::Original);
            check(editedMatch && playback.readAt(0, samples) && samples == originalPcm && samples != expected,
                  "both comparison lanes are bit-identical to their independent reverse-then-Warp references");
            constexpr auto peak = 3 * landmark;
            const bool atExpectedFrame = samples[2 * peak + 1] > 0.4f && samples[2 * peak] == -samples[2 * peak + 1] &&
                samples[2 * peak + 1] > samples[2 * (peak - 1) + 1] && samples[2 * peak + 1] > samples[2 * (peak + 1) + 1];
            std::printf("reversed Warp landmark: frame %u, channels %.6f / %.6f\n", peak, samples[2 * peak], samples[2 * peak + 1]);
            check(atExpectedFrame, "asymmetric stereo landmark reverses to frame 1600 and Warps to frame 4800");
        }
    }
    auto invalidMap = warp;
    invalidMap.warp->anchors[1].sourceFrame = invalidMap.warp->anchors[0].sourceFrame;
    const auto failedWarp = prepareClipPitchRender(invalidMap, {}, &decoded, &cache);
    check(failedWarp.problem == ClipPitchRenderProblem::Dsp && !failedWarp.prepared,
          "malformed reversed Warp still refuses publication");

    // The optional analysis cache may be swept at any time. The saved note
    // address must work from freshly computed notes, with no project Store.
    const auto coldRender = prepareClipPitchRender(plan, {}, &decoded, nullptr);
    check(coldRender && read(coldRender.prepared->file) == pcm, "cache miss rebuilds identical pitch-rendered PCM");
    auto stale = plan;
    auto malformed = std::make_shared<clip_edit::ClipPitchEdits>(*edits);
    ++malformed->rows.front().sourceStartFrame;
    stale.edits = malformed;
    const auto staleNote = prepareClipPitchRender(stale, {}, &decoded, &cache);
    check(staleNote.problem == ClipPitchRenderProblem::Edits && !staleNote.prepared,
          "saved note bounds must match the recovered source analysis");

    std::stop_source stop;
    stop.request_stop();
    const auto cancelled = prepareClipPitchRender(plan, stop.get_token(), &decoded, &cache);
    check(cancelled.problem == ClipPitchRenderProblem::Cancelled && !cancelled.prepared,
          "cancelled jobs publish no private PCM");
    ClipPitchRenderResult refused;
    std::shared_ptr<const ClipPitchRenderTask> noTask;
    const auto callbackIo = audio::callbackFileIo.load();
    auto refusePlan = plan;
    {
        const audio::CallbackScope callback;
        refused = prepareClipPitchRender(plan);
        noTask = requestClipPitchRender(std::move(refusePlan));
    }
    check(refused.problem == ClipPitchRenderProblem::Realtime && !noTask && audio::callbackFileIo.load() == callbackIo,
          "prepare and request refuse on the callback before source I/O");

    auto task = requestClipPitchRender(plan);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (task && !task->result() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const auto* async = task ? task->result() : nullptr;
    check(async && *async && read(async->prepared->file) == pcm, "asynchronous worker publishes complete PCM");
    if (async && *async) {
        auto pinned = async->prepared;
        task.reset();
        const auto pinnedFile = pinned->file;
        check(fs::is_regular_file(pinnedFile), "prepared file survives retirement of its task while leased");
        pinned.reset();
        while (fs::exists(pinnedFile) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        check(!fs::exists(pinnedFile), "last reader lease retires only its own prepared file");
    }

    check(blake3File(path).hex == hash.hex, "worker leaves the source and sibling clip media unchanged");
    stereo[25] = 0.42f;
    write(path, stereo.data());
    const auto changed = prepareClipPitchRender(plan, {}, &decoded, &cache);
    check(changed.problem == ClipPitchRenderProblem::Source && !changed.prepared,
          "new bytes with unchanged source dimensions cannot reuse authored pitch data");
}
} // namespace

int main() {
    try {
        run();
    } catch (const std::exception& error) {
        std::printf("FAIL fixture: %s\n", error.what());
        ++failures;
    }
    std::printf("%d clip pitch render checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
