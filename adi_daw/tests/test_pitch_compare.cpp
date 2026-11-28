// SPDX-License-Identifier: AGPL-3.0-or-later
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
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <numbers>
#include <utility>

namespace { std::atomic<unsigned> callbackAllocations{0}; }
#if defined(__GNUC__) && !defined(__clang__)
#define ADI_TEST_NOINLINE __attribute__((noinline))
#else
#define ADI_TEST_NOINLINE
#endif
ADI_TEST_NOINLINE void* operator new(std::size_t n) {
    if (adi::audio::onAudioThread) ++callbackAllocations;
    if (auto* allocated = std::malloc(n == 0 ? 1 : n)) return allocated;
    throw std::bad_alloc();
}
ADI_TEST_NOINLINE void* operator new[](std::size_t n) { return operator new(n); }
ADI_TEST_NOINLINE void operator delete(void* p) noexcept { std::free(p); }
ADI_TEST_NOINLINE void operator delete[](void* p) noexcept { std::free(p); }
ADI_TEST_NOINLINE void operator delete(void* p, std::size_t) noexcept { std::free(p); }
ADI_TEST_NOINLINE void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
#undef ADI_TEST_NOINLINE

namespace {
using namespace adi;
using namespace adi::audio;
int checks = 0, failures = 0;

void check(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL %s\n", message);
    }
}

void sameAddressAndFade() {
    std::vector<float> original(256), corrected(256);
    for (std::size_t frame = 0; frame < original.size() / 2; ++frame) {
        original[2 * frame] = static_cast<float>(frame);
        corrected[2 * frame] = static_cast<float>(frame + 100);
        original[2 * frame + 1] = -original[2 * frame];
        corrected[2 * frame + 1] = -corrected[2 * frame];
    }
    const auto pair = PitchComparePcm::prepare(48000, 2, std::move(original), std::move(corrected));
    check(pair && pair->rate() == 48000 && pair->channels() == 2 && pair->frames() == 128,
          "one immutable resident pair owns the complete interleaved clock");
    if (!pair) return;
    PitchComparePlayback immediate(pair);
    std::array<float, 128> buffer{};
    bool identicalAddresses = true;
    const unsigned allocations = callbackAllocations.load();
    const unsigned ioBefore = callbackFileIo.load();
    constexpr std::size_t switches = 10000;
    const auto start = std::chrono::steady_clock::now();
    {
        const CallbackScope callback;
        for (std::size_t pass = 0; pass < switches; ++pass) {
            const bool useOriginal = (pass % 2) != 0;
            immediate.select(useOriginal ? PitchCompareSide::Original : PitchCompareSide::Corrected);
            const float expected = useOriginal ? 7.0f : 107.0f;
            const bool read = immediate.readAt(7, buffer);
            const auto* out = buffer.data();
            identicalAddresses = identicalAddresses && read && out[0] == expected && out[1] == -expected &&
                                 out[126] == expected + 63.0f && out[127] == -(expected + 63.0f);
        }
    }
    const double microseconds = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
    std::printf("held-frame comparison: %zu switches at frame 7, %.3f microseconds per 64-frame stereo block, %u callback allocations, %u file I/O calls\n",
                switches, microseconds / static_cast<double>(switches), callbackAllocations.load() - allocations,
                callbackFileIo.load() - ioBefore);
    check(identicalAddresses, "every immediate switch reads the same held playhead without advancing it");
    check(callbackAllocations.load() == allocations && callbackFileIo.load() == ioBefore,
          "same-clock callback switching and reads allocate nothing and perform no file I/O");

    PitchComparePlayback faded(pair, 4);
    faded.select(PitchCompareSide::Original);
    std::array<float, 2> frame{};
    check(faded.readAt(3, frame) && frame == std::array<float, 2>{3, -3},
          "the initial requested lane starts without a startup crossfade");
    faded.select(PitchCompareSide::Corrected);
    const auto beforeRefusal = frame;
    check(!faded.readAt(128, frame) && frame == beforeRefusal && faded.readAt(128, {}),
          "a failed read leaves destination unchanged and exact EOF accepts an empty read");
    std::array<float, 10> ramp{};
    check(faded.readAt(4, ramp) && ramp == std::array<float, 10>{4, -4, 30, -30, 56, -56, 82, -82, 108, -108},
          "four-frame anti-click blends matching sample addresses with one weight per stereo frame");
    float maximumStep = 0;
    for (std::size_t sample = 2; sample < ramp.size(); sample += 2)
        maximumStep = std::max(maximumStep, std::abs(ramp[sample] - ramp[sample - 2]));
    std::printf("crossfade: maximum step %.1f across a raw 101.0 jump, 4-frame fade, no address shift\n", maximumStep);
    check(maximumStep == 26, "optional fade bounds the switch discontinuity without a clock offset");

    faded.select(PitchCompareSide::Original);
    std::array<float, 4> partial{};
    check(faded.readAt(18, partial) && partial[0] == 118 && partial[2] == 94, "a new fade spans chunks");
    faded.select(PitchCompareSide::Corrected);
    check(faded.readAt(18, frame) && frame[0] == 68, "reversing mid-fade retains the current weight at the held frame");

    const auto malformed = PitchComparePcm::prepare(48000, 2, {1.0f, 0.0f}, {1.0f});
    check(!malformed, "mismatched payloads cannot form a comparison generation");
    const auto finite = PitchComparePcm::prepare(48000, 1, {std::numeric_limits<float>::quiet_NaN()},
                                                {std::numeric_limits<float>::infinity()});
    PitchComparePlayback sanitized(finite);
    std::array<float, 1> sample{42};
    const bool correctedZero = sanitized.readAt(0, sample) && sample[0] == 0;
    sanitized.select(PitchCompareSide::Original);
    check(correctedZero && sanitized.readAt(0, sample) && sample[0] == 0,
          "worker canonicalizes non-finite PCM on both sides before publication");
}

double frequency(std::span<const float> interleaved, std::uint32_t rate) {
    std::size_t crossings = 0;
    double first = 0, last = 0;
    for (std::size_t frame = 1; frame < interleaved.size() / 2; ++frame) {
        const auto a = interleaved[(frame - 1) * 2], b = interleaved[frame * 2];
        if (a <= 0 && b > 0) {
            const double crossing = static_cast<double>(frame - 1) - a / (static_cast<double>(b) - a);
            if (crossings == 0) first = crossing;
            last = crossing;
            ++crossings;
        }
    }
    return crossings > 1 ? static_cast<double>(crossings - 1) * rate / (last - first) : 0;
}

std::shared_ptr<const clip_edit::ClipPitchEdits> shiftedNote(const media::AnalysisIdentity& source,
    std::span<const float> mono, media::AnalysisCache& cache, std::int64_t clipId, double shiftCents) {
    const auto analyzed = media::buildSourcePitchAnalysisFromMono(source, mono, &cache);
    if (!analyzed || analyzed.analysis->notes.size() != 1) throw std::runtime_error("analyze source fixture");
    const auto& note = analyzed.analysis->notes.front();
    auto edit = resolvePitchNoteEdit(*analyzed.analysis, {}, 0);
    edit.pitchShiftCents = shiftCents;
    auto stored = std::make_shared<clip_edit::ClipPitchEdits>();
    stored->clipId = clipId;
    stored->sourceHash = source.hash;
    stored->sampleRate = source.sampleRate;
    stored->sourceFrames = source.frames;
    stored->analysisVersion = media::kPitchAnalysisVersion;
    stored->analysisConfiguration = media::kPitchAnalysisConfiguration;
    stored->rows.push_back({note.startFrame, note.endFrame, edit});
    return stored;
}

void preparedPair() {
    constexpr std::uint32_t rate = 44100, frames = 30000, start = 2000, length = 24000;
    constexpr std::uint32_t marker = 700, bodyFirst = 8000;
    constexpr std::uint16_t channels = 2;
    test::TempDirectory temp{"pitch_compare", "verified"};
    const auto path = temp.path() / "voice.wav";
    auto stereo = std::vector<float>(static_cast<std::size_t>(frames) * channels);
    auto mono = std::vector<float>(frames);
    for (std::uint32_t frame = 4500; frame < 22500; ++frame) {
        const double gain = std::min(1.0, static_cast<double>(std::min(frame - 4500, 22500 - frame)) / 900);
        const double phase = 2 * std::numbers::pi * 220 * frame / rate;
        const auto value = static_cast<float>(gain * 0.42 * std::sin(phase));
        stereo[2 * frame] = value;
        stereo[2 * frame + 1] = value * 0.4f;
        mono[frame] = static_cast<float>((static_cast<double>(value) + stereo[2 * frame + 1]) / 2);
    }
    stereo[2 * (start + marker)] = 0.6f;
    stereo[2 * (start + marker) + 1] = -0.6f;
    {
        WavWriter writer(path, rate, channels, WavFormat::Float32);
        writer.write(stereo.data(), frames);
        writer.close();
    }
    const auto hash = media::blake3File(path);
    if (!hash) throw std::runtime_error("hash source fixture");
    DecodeCache decoded(temp.path() / "decoded");
    media::AnalysisCache cache(temp.path() / "analysed");
    const auto stored = shiftedNote({hash.hex, rate, channels, frames}, mono, cache, 45, 100);
    const media::ClipPitchRenderPlan plan{path, temp.path() / "work", stored, start, length, false, {}};

    const auto beginPreparation = std::chrono::steady_clock::now();
    const auto base = media::prepareClipPitchCompare(plan, {}, &decoded, &cache);
    const auto preparationMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - beginPreparation).count();
    std::printf("prepared pair: %.3f ms for %u stereo source-window frames\n", preparationMs, length);
    check(base && base.prepared->frames() == length && base.prepared->rate() == rate && base.prepared->channels() == channels,
          "same-rate comparison reports exact trimmed output dimensions");
    if (!base) {
        if (base.detail) std::printf("prepare error: %s\n", base.detail->c_str());
        return;
    }
    PitchComparePlayback native(base.prepared);
    std::array<float, 2> correctedMarker{}, originalMarker{};
    const bool readCorrected = native.readAt(marker, correctedMarker);
    native.select(PitchCompareSide::Original);
    check(readCorrected && native.readAt(marker, originalMarker) && correctedMarker == originalMarker &&
          originalMarker == std::array<float, 2>{0.6f, -0.6f},
          "the unchanged impulse stays on the exact trimmed frame in both generations");
    check(std::filesystem::is_empty(plan.workFolder), "resident comparison retains neither private staged file");

    auto reversed = plan;
    reversed.reverse = true;
    const auto reverse = media::prepareClipPitchCompare(reversed, {}, &decoded, &cache);
    check(static_cast<bool>(reverse), "unwarped reversed comparison prepares");
    if (reverse) {
        PitchComparePlayback playback(reverse.prepared);
        const bool edited = playback.readAt(length - 1 - marker, correctedMarker);
        playback.select(PitchCompareSide::Original);
        check(edited && playback.readAt(length - 1 - marker, originalMarker) &&
              correctedMarker == originalMarker && originalMarker == std::array<float, 2>{0.6f, -0.6f},
              "reverse applies the exact same frame/channel transform to both sources");
    }

    auto warped = plan;
    warped.playbackRate = 2 * rate;
    auto& warp = warped.warp.emplace();
    warp.mode = WarpMode::RePitch;
    warp.outputRate = 48000;
    warp.anchors = {{0, 0}, {length, 2 * length}};
    const auto pair = media::prepareClipPitchCompare(warped, {}, &decoded, &cache, 48000);
    check(pair && pair.prepared->rate() == 48000 && pair.prepared->frames() == 2 * length,
          "both sides keep the captured Warp clock even with an unwarped playback rate supplied");
    if (pair) {
        PitchComparePlayback playback(pair.prepared);
        std::vector<float> corrected(12000), original(12000);
        const bool edited = playback.readAt(2 * bodyFirst, corrected);
        playback.select(PitchCompareSide::Original);
        const bool raw = playback.readAt(2 * bodyFirst, original);
        const double originalHz = frequency(original, 48000);
        const double editedHz = frequency(corrected, 48000);
        constexpr double expectedRaw = 220.0 * 48000 / (2 * rate);
        std::printf("one warped playhead: %.4f Hz original, %.4f Hz corrected, %.3f cents apart\n", originalHz, editedHz,
                    1200 * std::log2(editedHz / originalHz));
        check(edited && raw && std::abs(originalHz - expectedRaw) < 0.5 &&
              std::abs(editedHz - expectedRaw * std::exp2(1.0 / 12)) < 0.5,
              "original bypasses pitch editing while both sides keep the same project Warp");
        playback.select(PitchCompareSide::Corrected);
        std::array<float, 10> correctedImpulse{}, originalImpulse{};
        const bool editedLandmark = playback.readAt(2 * marker - 2, correctedImpulse);
        playback.select(PitchCompareSide::Original);
        check(editedLandmark && playback.readAt(2 * marker - 2, originalImpulse) && correctedImpulse == originalImpulse &&
              std::distance(originalImpulse.begin(), std::max_element(originalImpulse.begin(), originalImpulse.end())) == 4,
              "shared Warp places the unchanged impulse at the same exact output frame");
    }
    const auto resampled = media::prepareClipPitchCompare(plan, {}, &decoded, &cache, 48000);
    constexpr auto resampledLength = (static_cast<std::uint64_t>(length) * 48000 + rate / 2) / rate;
    check(resampled && resampled.prepared->rate() == 48000 && resampled.prepared->frames() == resampledLength,
          "unwarped pair converts to the exact rounded graph sample clock");
    if (resampled) {
        PitchComparePlayback playback(resampled.prepared);
        std::vector<float> corrected(16000), original(16000);
        const bool edited = playback.readAt(bodyFirst, corrected);
        playback.select(PitchCompareSide::Original);
        const bool raw = playback.readAt(bodyFirst, original);
        const double originalHz = frequency(original, 48000), editedHz = frequency(corrected, 48000);
        std::printf("44.1 to 48 kHz compare: %llu frames, %.4f Hz original, %.4f Hz corrected\n",
                    static_cast<unsigned long long>(resampled.prepared->frames()), originalHz, editedHz);
        check(edited && raw && std::abs(originalHz - 220) < 0.2 && std::abs(editedHz - 233.08188) < 0.2,
              "owner rate conversion preserves each lane's own pitch");
    }

    auto transposed = plan;
    transposed.playbackRate = 2 * rate;
    transposed.sourceFrames = length - 1;
    const auto effective = media::prepareClipPitchCompare(transposed, {}, &decoded, &cache);
    check(effective && effective.prepared->rate() == 2 * rate && effective.prepared->frames() == length - 1,
          "unwarped comparison defaults to the effective rate with unchanged source-frame addresses");
    const auto graph = media::prepareClipPitchCompare(transposed, {}, &decoded, &cache, rate);
    check(graph && graph.prepared->rate() == rate && graph.prepared->frames() == length / 2,
          "a graph rate equal to the WAV rate still converts the odd-length transposed window");
    if (graph) {
        PitchComparePlayback playback(graph.prepared);
        std::vector<float> editedSignal(8000), rawSignal(8000);
        const bool edited = playback.readAt(bodyFirst / 2, editedSignal);
        playback.select(PitchCompareSide::Original);
        const bool raw = playback.readAt(bodyFirst / 2, rawSignal);
        check(edited && raw && std::abs(frequency(rawSignal, rate) - 440) < 0.4 &&
              std::abs(frequency(editedSignal, rate) - 466.16376) < 0.4,
              "both graph lanes retain clip transpose and only the corrected lane adds the note edits");
        std::array<float, 10> rawLandmark{}, editedLandmark{};
        const bool readRawLandmark = playback.readAt(marker / 2 - 2, rawLandmark);
        playback.select(PitchCompareSide::Corrected);
        check(readRawLandmark && playback.readAt(marker / 2 - 2, editedLandmark) && rawLandmark == editedLandmark &&
              std::distance(rawLandmark.begin(), std::max_element(rawLandmark.begin(), rawLandmark.end())) == 4,
              "the original and corrected lanes share the transposed graph sample address");
    }
    const auto nativeRender = media::prepareClipPitchRender(transposed, {}, &decoded, &cache);
    check(nativeRender && nativeRender.prepared->rate == rate && nativeRender.prepared->frames == length - 1,
          "ordinary render retains the physical WAV clock despite a comparison playback rate");

    const auto wrongRate = media::prepareClipPitchCompare(warped, {}, &decoded, &cache, 44100);
    check(wrongRate.problem == media::ClipPitchRenderProblem::Unsupported && !wrongRate.prepared,
          "a requested rate cannot silently replace an existing projected Warp clock");

    warped.warp->anchors.back().outputFrame = kPitchCompareMaxResidentSamples;
    const auto tooLarge = media::prepareClipPitchCompare(warped, {}, &decoded, &cache);
    check(tooLarge.problem == media::ClipPitchRenderProblem::Resource && !tooLarge.prepared,
          "oversized pair is refused before rendering either side");
    auto stale = plan;
    auto wrongHash = std::make_shared<clip_edit::ClipPitchEdits>(*stored);
    wrongHash->sourceHash[0] = wrongHash->sourceHash[0] == '0' ? '1' : '0';
    stale.edits = std::move(wrongHash);
    const auto replaced = media::prepareClipPitchCompare(stale, {}, &decoded, &cache);
    check(replaced.problem == media::ClipPitchRenderProblem::Source && !replaced.prepared,
          "wrong source identity cannot publish either lane of the pair");
    std::stop_source stop;
    stop.request_stop();
    check(media::prepareClipPitchCompare(plan, stop.get_token(), &decoded, &cache).problem == media::ClipPitchRenderProblem::Cancelled,
          "cancelled worker publishes no comparison");
    media::ClipPitchCompareResult realtime;
    const auto oldAllocations = callbackAllocations.load();
    {
        const CallbackScope callback;
        realtime = media::prepareClipPitchCompare(plan);
    }
    check(realtime.problem == media::ClipPitchRenderProblem::Realtime && !realtime.prepared &&
          callbackAllocations.load() == oldAllocations, "worker preparation refuses on the callback without allocating");
    check(media::blake3File(path).hex == hash.hex, "comparison leaves the source bytes intact");
}

void extremeRatePair() {
    constexpr std::uint32_t rate = 8000, graphRate = 192000, frames = 5500;
    constexpr std::uint64_t start = 43, length = 5301, marker = 170;
    constexpr std::uint16_t channels = 2;
    constexpr double sourceHz = 220, shiftCents = 100;
    const auto playbackRate = static_cast<std::uint32_t>(pitchedSampleRate(rate, -48));
    test::TempDirectory temp{"pitch_compare", "extreme_rate"};
    const auto path = temp.path() / "voice.wav";
    std::vector<float> stereo(frames * channels), mono(frames);
    for (std::uint32_t frame = 850; frame < 5150; ++frame) {
        const double gain = std::min(1.0, static_cast<double>(std::min(frame - 850, 5150 - frame)) / 160);
        const auto sample = static_cast<float>(gain * 0.45 * std::sin(2 * std::numbers::pi * sourceHz * frame / rate));
        stereo[2 * frame] = sample;
        stereo[2 * frame + 1] = sample * 0.5f;
        mono[frame] = static_cast<float>((static_cast<double>(sample) + stereo[2 * frame + 1]) / 2);
    }
    stereo[2 * (start + marker)] = 0.6f;
    stereo[2 * (start + marker) + 1] = -0.6f;
    {
        WavWriter writer(path, rate, channels, WavFormat::Float32);
        writer.write(stereo.data(), frames);
        writer.close();
    }
    const auto hash = media::blake3File(path);
    if (!hash) throw std::runtime_error("hash extreme-rate fixture");
    DecodeCache decoded(temp.path() / "decoded");
    media::AnalysisCache cache(temp.path() / "analysed");
    const auto stored = shiftedNote({hash.hex, rate, channels, frames}, mono, cache, 46, shiftCents);
    const media::ClipPitchRenderPlan plan{path, temp.path() / "work", stored, start, length, false, {}, playbackRate};

    auto invalid = plan;
    invalid.playbackRate = 1;
    invalid.sourceFrames = 1;
    const auto refusal = media::prepareClipPitchCompare(invalid, {}, &decoded, &cache, graphRate);
    check(refusal.problem == media::ClipPitchRenderProblem::Unsupported && !refusal.prepared &&
          !std::filesystem::exists(plan.workFolder), "ratios outside two valid stages are refused before staging");

    const auto begin = std::chrono::steady_clock::now();
    const auto pair = media::prepareClipPitchCompare(plan, {}, &decoded, &cache, graphRate);
    const auto elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    const auto expectedFrames = (length * graphRate + playbackRate / 2) / playbackRate;
    check(playbackRate == 500 && pair && pair.prepared->rate() == graphRate && pair.prepared->channels() == channels &&
          pair.prepared->frames() == expectedFrames, "384x comparison covers the entire odd source window at the graph rate");
    if (!pair) {
        if (pair.detail) std::printf("extreme-rate prepare error: %s\n", pair.detail->c_str());
        return;
    }

    PitchComparePlayback playback(pair.prepared);
    const auto body = 1600u * graphRate / playbackRate;
    const auto count = 2000u * graphRate / playbackRate;
    std::vector<float> editedSignal(count * channels), rawSignal(count * channels);
    const bool readEdited = playback.readAt(body, editedSignal);
    playback.select(PitchCompareSide::Original);
    const bool readRaw = playback.readAt(body, rawSignal);
    const double originalHz = frequency(rawSignal, graphRate), editedHz = frequency(editedSignal, graphRate);
    const double expectedHz = sourceHz * playbackRate / rate;
    std::printf("%u to %u Hz comparison: %llu output frames, %.4f Hz original, %.4f Hz corrected, %.3f ms preparation\n",
                playbackRate, graphRate, static_cast<unsigned long long>(pair.prepared->frames()),
                originalHz, editedHz, elapsedMs);
    check(readEdited && readRaw && std::abs(originalHz - expectedHz) < 0.01 &&
          std::abs(editedHz - expectedHz * std::exp2(shiftCents / 1200)) < 0.1,
          "both stages retain the requested clip pitch and corrected-note semitone");

    const auto impulse = marker * graphRate / playbackRate;
    std::array<float, 10> rawLandmark{}, editedLandmark{};
    const bool readRawLandmark = playback.readAt(impulse - 2, rawLandmark);
    playback.select(PitchCompareSide::Corrected);
    check(readRawLandmark && playback.readAt(impulse - 2, editedLandmark) && rawLandmark == editedLandmark &&
          std::distance(rawLandmark.begin(), std::max_element(rawLandmark.begin(), rawLandmark.end())) == 4,
          "both stages retain the exact rational address of an unchanged impulse");
}
} // namespace

int main() {
    try {
        sameAddressAndFade();
        preparedPair();
        extremeRatePair();
    } catch (const std::exception& error) {
        std::printf("FAIL fixture: %s\n", error.what());
        ++failures;
    }
    std::printf("%d pitch comparison checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
