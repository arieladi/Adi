// SPDX-License-Identifier: AGPL-3.0-or-later
#define ADI_TEST_COUNT_REPLACED_DELETE
#include "replace_operator_new.hpp"
#include "temp_directory.hpp"

#include "adi/audio/io_audit.hpp"
#include "adi/audio/warp.hpp"
#include "adi/audio/warp_map.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/clip_commands.hpp"
#include "adi/engine/session.hpp"
#include "adi/engine/warp_clip_player.hpp"
#include "adi/media/blake3.hpp"
#include "adi/ops.hpp"
#include "adi/render/queue.hpp"
#include "adi/store.hpp"
#include "adi/textproj.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#if defined(ADI_TEST_WARP_WRAP_CALLBACK)
#include <pthread.h>
#endif

namespace {
std::atomic<unsigned> allocations{0}, releases{0};
#if defined(ADI_TEST_WARP_WRAP_CALLBACK)
std::atomic<unsigned> cAllocations{0}, cReleases{0}, locks{0};
#endif
}

#if defined(ADI_TEST_WARP_WRAP_CALLBACK)
extern "C" {
void* __real_malloc(std::size_t size);
void* __real_calloc(std::size_t count, std::size_t size);
void* __real_realloc(void* memory, std::size_t size);
void __real_free(void* memory);
int __real_posix_memalign(void** result, std::size_t alignment, std::size_t size);
int __real_pthread_mutex_lock(pthread_mutex_t* mutex);
int __real_pthread_mutex_trylock(pthread_mutex_t* mutex);

void* __wrap_malloc(std::size_t size) {
    if (adi::audio::onAudioThread) cAllocations.fetch_add(1, std::memory_order_relaxed);
    return __real_malloc(size);
}
void* __wrap_calloc(std::size_t count, std::size_t size) {
    if (adi::audio::onAudioThread) cAllocations.fetch_add(1, std::memory_order_relaxed);
    return __real_calloc(count, size);
}
void* __wrap_realloc(void* memory, std::size_t size) {
    if (adi::audio::onAudioThread) cAllocations.fetch_add(1, std::memory_order_relaxed);
    return __real_realloc(memory, size);
}
void __wrap_free(void* memory) {
    if (memory && adi::audio::onAudioThread) cReleases.fetch_add(1, std::memory_order_relaxed);
    __real_free(memory);
}
int __wrap_posix_memalign(void** result, std::size_t alignment, std::size_t size) {
    if (adi::audio::onAudioThread) cAllocations.fetch_add(1, std::memory_order_relaxed);
    return __real_posix_memalign(result, alignment, size);
}
int __wrap_pthread_mutex_lock(pthread_mutex_t* mutex) {
    if (adi::audio::onAudioThread) locks.fetch_add(1, std::memory_order_relaxed);
    return __real_pthread_mutex_lock(mutex);
}
int __wrap_pthread_mutex_trylock(pthread_mutex_t* mutex) {
    if (adi::audio::onAudioThread) locks.fetch_add(1, std::memory_order_relaxed);
    return __real_pthread_mutex_trylock(mutex);
}
}
#endif

void adi::test::countReplacedNew() noexcept {
    if (adi::audio::onAudioThread) allocations.fetch_add(1, std::memory_order_relaxed);
}

void adi::test::countReplacedDelete(void* memory) noexcept {
    if (memory && adi::audio::onAudioThread) releases.fetch_add(1, std::memory_order_relaxed);
}

namespace {
using namespace adi;
using namespace adi::engine;

constexpr std::uint32_t sourceRate = 48000;
constexpr std::uint32_t framesPerBeat = sourceRate / 2;
constexpr std::int64_t quarter = textproj::kPPQ;
constexpr std::uint32_t sourceBeats = 8;
constexpr std::uint32_t arrangementBeats = 2 * sourceBeats;
constexpr std::uint32_t sourceFrames = framesPerBeat * sourceBeats;
constexpr std::int32_t maxFrames = 512;

int failures = 0;

void check(bool ok, const char* label) {
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", label);
    }
}

struct Project {
    test::TempDirectory temporary{"streaming_warp", "project"};
    std::filesystem::path source = temporary.path() / "source.wav";
    std::unique_ptr<Store> store;
    std::vector<WarpMarker> markers;

    explicit Project(std::string_view mode) {
        StoreError error{};
        store = Store::createEmpty(temporary.path() / "song.adi", error);
        if (!store) throw std::runtime_error(toString(error));
        std::vector<float> samples(sourceFrames * 2);
        constexpr auto pulseHalfWidth = 48u;
        for (std::uint32_t frame = 0; frame < sourceFrames - pulseHalfWidth; ++frame) {
            // Center each pulse on its source beat. The source contains no attack
            // before its end, so the final clip cutoff does not count as a beat.
            const auto distance = std::min(frame % framesPerBeat, framesPerBeat - frame % framesPerBeat);
            if (distance >= pulseHalfWidth) continue;
            const auto pulse = static_cast<float>(0.4 * (1 + std::cos(std::numbers::pi * distance / pulseHalfWidth)));
            samples[frame * 2] = pulse;
            samples[frame * 2 + 1] = -pulse;
        }
        {
            audio::WavWriter writer(source, sourceRate, 2, audio::WavFormat::Float32);
            writer.write(samples.data(), sourceFrames);
            writer.close();
        }
        const auto digest = media::blake3File(source);
        if (!digest) throw std::runtime_error("hash Warp proof fixture");
        auto& db = store->db();
        db.exec("INSERT INTO project(id,name,sample_rate) VALUES(1,'Warp proof',48000);"
                "INSERT INTO tracks(id,kind,name) VALUES(1,'audio','Warp'),(9,'master','Main')");
        SQLite::Statement mediaRow(db, "INSERT INTO media_files(id,hash_blake3,rel_path,format,sample_rate,channels,frames) "
                                        "VALUES(1,?,'source.wav','wav',48000,2,?)");
        mediaRow.bind(1, digest.hex);
        mediaRow.bind(2, sourceFrames);
        mediaRow.exec();
        SQLite::Statement clip(db, "INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks,loop_enabled,loop_start_ticks,loop_len_ticks) "
                                    "VALUES(7,1,'audio',0,?,1,0,?)");
        clip.bind(1, arrangementBeats * quarter);
        clip.bind(2, sourceBeats * quarter);
        clip.exec();
        // Re-Pitch must track the entire ramp even with only two warp anchors.
        const auto beatsPerAnchor = mode == "re-pitch" ? sourceBeats : 1u;
        for (std::uint32_t beat = 0; beat <= sourceBeats; beat += beatsPerAnchor) {
            markers.push_back({beat * framesPerBeat, beat * quarter});
        }
        const auto map = audio::writeWarpMap(markers, sourceFrames);
        if (!map) throw std::runtime_error("write Warp proof markers");
        SQLite::Statement warp(db, "INSERT INTO audio_clips(clip_id,media_id,src_len_frames,warp_enabled,warp_mode,warp_markers) "
                                    "VALUES(7,1,?,1,?,?)");
        warp.bind(1, sourceFrames);
        warp.bind(2, std::string(mode));
        warp.bind(3, map.bytes.data(), static_cast<int>(map.bytes.size()));
        warp.exec();
    }

    void tempo(int initialCurve, std::int64_t boundary, double bpm, double initialBpm = 120.0,
               double tension = 0.0) {
        auto& db = store->db();
        db.exec("DELETE FROM tempo_map");
        SQLite::Statement row(db, "INSERT INTO tempo_map(pos_ticks,bpm,curve,tension) VALUES(?,?,?,?)");
        row.bind(1, 0);
        row.bind(2, initialBpm);
        row.bind(3, initialCurve);
        row.bind(4, tension);
        row.exec();
        row.reset();
        row.bind(1, boundary);
        row.bind(2, bpm);
        row.bind(3, 0);
        row.bind(4, 0.0);
        row.exec();
    }

    void load(Session& session, Precision precision, std::uint32_t rate) {
        SessionSpec spec{2, static_cast<double>(rate), maxFrames};
        spec.precision = precision;
        session.graph().setFadeFrames(0);
        if (!session.load(*store, {}, spec)) throw std::runtime_error(session.error());
        session.transport().locate(0);
        session.transport().play();
    }
};

struct StereoFrame {
    float left = 0, right = 0;
    friend bool operator==(const StereoFrame&, const StereoFrame&) = default;
};

void render(Session& session, Precision width, std::span<StereoFrame> output, std::int32_t blockFrames,
            bool primeFirst = true) {
    std::array<float, maxFrames> left{}, right{};
    std::array<double, maxFrames> leftWide{}, rightWide{};
    float* channels[] = {left.data(), right.data()};
    double* channelsWide[] = {leftWide.data(), rightWide.data()};
    auto streamTime = session.transport().position();
    for (std::size_t first = 0; first < output.size();) {
        if ((first != 0 || primeFirst) && !session.clips()->prime(std::chrono::seconds(5))) {
            throw std::runtime_error("prime Warp proof source");
        }
        const auto frames = std::min(static_cast<std::size_t>(blockFrames), output.size() - first);
        AudioIo io;
        io.frames = static_cast<std::int32_t>(frames);
        io.numOut = 2;
        io.streamTimeSamples = streamTime;
        if (width == Precision::Float64) io.out64 = channelsWide;
        else io.out = channels;
        {
            const audio::CallbackScope callback;
            session.process(io);
        }
        for (std::size_t frame = 0; frame < frames; ++frame) {
            output[first + frame] = width == Precision::Float64
                ? StereoFrame{static_cast<float>(leftWide[frame]), static_cast<float>(rightWide[frame])}
                : StereoFrame{left[frame], right[frame]};
        }
        first += frames;
        streamTime += io.frames;
    }
}

template<class SecondsAtBeat>
std::int64_t beatLandmarks(std::span<const StereoFrame> output, std::uint32_t rate,
                          SecondsAtBeat secondsAtBeat, const char* label) {
    std::vector<std::int64_t> peaks;
    float peak = 0;
    std::int64_t last = -static_cast<std::int64_t>(rate), peakFrame = 0;
    bool coherent = true;
    for (std::size_t frame = 0; frame < output.size(); ++frame) {
        const auto& sample = output[frame];
        coherent = coherent && std::isfinite(sample.left) && std::isfinite(sample.right)
            && std::abs(sample.left + sample.right) < 1e-6f;
        const auto level = std::abs(sample.left);
        if (level < 0.05f) continue;
        const auto at = static_cast<std::int64_t>(frame);
        if (at - last > rate / 8) {
            if (peak > 0) peaks.push_back(peakFrame);
            peak = 0;
        }
        if (level > peak) {
            peak = level;
            peakFrame = at;
        }
        last = at;
    }
    if (peak > 0) peaks.push_back(peakFrame);
    auto worst = std::int64_t{0};
    for (std::size_t beat = 0; beat < std::min<std::size_t>(arrangementBeats, peaks.size()); ++beat) {
        const auto expected = std::llround(rate * secondsAtBeat(static_cast<double>(beat)));
        worst = std::max(worst, static_cast<std::int64_t>(std::abs(peaks[beat] - expected)));
    }
    std::printf("%s: %zu/%u beats, max grid error %lld samples (%.3f ms) at %u Hz\n",
                label, peaks.size(), arrangementBeats, static_cast<long long>(worst),
                1000.0 * static_cast<double>(worst) / rate, rate);
    check(coherent, "warped pulses stay finite and keep their stereo polarity");
    check(peaks.size() == arrangementBeats, "every beat plays once across the clip loop");
    return worst;
}

double rampSecondsAtBeat(double beat) {
    constexpr auto slope = 20.0 / arrangementBeats;
    return 60.0 / slope * std::log1p(slope * beat / 120.0);
}

void tempoRamp(std::string_view mode, Precision width, std::uint32_t rate, std::int64_t tolerance) {
    Project project(mode);
    project.tempo(1, arrangementBeats * quarter, 140);
    Session session;
    project.load(session, width, rate);
    const auto frames = static_cast<std::size_t>(std::llround(rate * rampSecondsAtBeat(arrangementBeats))) + rate / 10;
    std::vector<StereoFrame> heard(frames);
    render(session, width, heard, 257);
    check(beatLandmarks(heard, rate, rampSecondsAtBeat, mode.data()) <= tolerance,
          "the warped loop follows the analytically integrated 120 to 140 BPM ramp");
    const auto end = static_cast<std::size_t>(std::llround(rate * rampSecondsAtBeat(arrangementBeats)));
    check(std::all_of(heard.begin() + static_cast<std::ptrdiff_t>(end), heard.end(),
                      [](const StereoFrame& sample) { return sample == StereoFrame{}; }),
          "the warped arrangement clip ends on its tempo-ramp boundary");
    const auto loopEdge = static_cast<std::size_t>(std::llround(rate * rampSecondsAtBeat(sourceBeats)));
    constexpr std::size_t leadIn = 512;
    std::array<StereoFrame, 2048> sought{};
    session.transport().play(false);
    session.transport().locate(static_cast<std::int64_t>(loopEdge - leadIn));
    session.transport().play();
    render(session, width, sought, 113);
    check(std::equal(sought.begin(), sought.end(), heard.begin() + static_cast<std::ptrdiff_t>(loopEdge - leadIn)),
          "stopping and seeking across the loop seam reproduces the same audio at a different callback partition");
    check(session.clips()->underrunSamples() == 0 && session.clips()->readErrors() == 0,
          "streaming warp reads the test loop without underruns or disk errors");
}

void typedTempo(std::string_view mode, std::int64_t changeFrame, std::int64_t tolerance) {
    Project project(mode);
    const auto changeTick = static_cast<std::int64_t>(std::llround(static_cast<double>(changeFrame) * quarter / framesPerBeat));
    const auto changeBeat = static_cast<double>(changeTick) / quarter;
    project.tempo(0, changeTick, 120);
    Session session;
    project.load(session, Precision::Float32, sourceRate);
    const auto delay = static_cast<std::size_t>(session.graph().currentGraph()->latencySamples());
    const auto secondsAtBeat = [changeBeat](double beat) {
        if (beat <= changeBeat) return beat / 2;
        return changeBeat / 2.0 + (beat - changeBeat) * 60.0 / 140.0;
    };
    const auto frames = static_cast<std::size_t>(std::llround(sourceRate * secondsAtBeat(arrangementBeats))) + sourceRate / 10 + delay;
    std::vector<StereoFrame> heard(frames);
    std::span<StereoFrame> output = heard;
    const auto switchAt = static_cast<std::size_t>(changeFrame);
    render(session, Precision::Float32, output.first(switchAt), 257);

    // The same journal batch the tempo field uses when replacing the event in force.
    std::array<OpRequest, 2> edit;
    edit[0].opType = "project.removeTempoEvent";
    edit[0].payload = {{"pos", changeTick}};
    edit[1].opType = "project.insertTempoEvent";
    edit[1].payload = {{"pos", changeTick}, {"bpm", 140.0}, {"curve", 0}};
    if (!OpJournal(*project.store).commit(edit).ok || !session.refresh(*project.store)) {
        throw std::runtime_error("publish a typed tempo edit while the transport plays");
    }
    check(session.transport().playing() && session.transport().position() == changeFrame,
          "a typed tempo edit preserves the running transport at the edit boundary");
    // At the R3 test position, the current output is still reading the
    // previous PCM page through the live compensation clock.
    render(session, Precision::Float32, output.subspan(switchAt, 257), 257, false);
    check(session.clips()->underrunSamples() == 0, "the first callback after a typed tempo has ready source audio");
    render(session, Precision::Float32, output.subspan(switchAt + 257), 257);
    const std::string label = "typed " + std::string(mode);
    check(beatLandmarks(output.subspan(delay), sourceRate, secondsAtBeat, label.c_str()) <= tolerance,
          "the typed tempo places each following beat without dropping a landmark");
    check(session.clips()->underrunSamples() == 0 && session.clips()->readErrors() == 0,
          "typed tempo playback has no missing source pages");
}

std::vector<StereoFrame> compensatedPlayback(Session& session, std::size_t frames, std::int32_t blockFrames) {
    const auto* graph = session.graph().currentGraph();
    if (!graph || graph->latencySamples() < 0) throw std::runtime_error("read Warp graph latency");
    const auto latency = static_cast<std::size_t>(graph->latencySamples());
    std::vector<StereoFrame> raw(frames + latency);
    render(session, Precision::Float32, raw, blockFrames);
    return {raw.begin() + static_cast<std::ptrdiff_t>(latency), raw.end()};
}

void r3UsesLocalTempoDomain() {
    for (const auto changeBeat : {arrangementBeats, arrangementBeats + 64}) {
        Project project("complex");
        project.tempo(0, changeBeat * quarter, 300);
        Session session;
        project.load(session, Precision::Float32, sourceRate);
        const auto latency = session.graph().currentGraph()->latencySamples();
        const auto heard = compensatedPlayback(session, framesPerBeat + 1024, maxFrames);
        const auto begin = heard.begin() + framesPerBeat - 4;
        const auto end = begin + 9;
        const auto peak = std::max_element(begin, end, [](const auto& a, const auto& b) { return a.left < b.left; });
        std::printf("Complex clip followed by 300 BPM at beat %u: graph latency %d samples, beat peak %.3f FS\n",
                    changeBeat, latency, static_cast<double>(peak->left));
        check(latency > 0 && peak->left > 0.5f && session.clips()->underrunSamples() == 0,
              "a tempo step at or after the placement end preserves audible streaming R3");
    }

    Project within("complex");
    within.tempo(0, 4 * quarter, 300);
    Session session;
    within.load(session, Precision::Float32, sourceRate);
    check(session.graph().currentGraph()->latencySamples() == 0,
          "a genuinely unsupported tempo step inside the placement retains the prepared fallback");
}

void steadyTempo(std::string_view mode, double bpm, float peakTolerance = 0.002f, double rmsTolerance = 0.0001) {
    Project project(mode);
    project.tempo(0, arrangementBeats * quarter, bpm, bpm);
    Session session;
    project.load(session, Precision::Float32, sourceRate);
    const auto outputFrames = static_cast<std::uint32_t>(std::llround(sourceFrames * 120.0 / bpm));
    const auto heard = compensatedPlayback(session, outputFrames, maxFrames);
    audio::WarpRenderPlan plan;
    plan.source = project.source;
    plan.workFolder = project.temporary.path() / "prepared";
    plan.sourceFrames = sourceFrames;
    plan.outputRate = sourceRate;
    plan.mode = *audio::parseWarpMode(mode);
    for (const auto& marker : project.markers) {
        const auto outputFrame = std::llround(static_cast<double>(marker.sourceFrame) * 120.0 / bpm);
        plan.anchors.push_back({static_cast<std::uint64_t>(marker.sourceFrame), static_cast<std::uint64_t>(outputFrame)});
    }
    const auto prepared = audio::prepareWarp(plan);
    if (!prepared) throw std::runtime_error("render the steady-tempo Warp reference");
    audio::WavReader reader(prepared.prepared->file);
    std::vector<float> reference(outputFrames * 2);
    if (reader.read(reference.data(), outputFrames) != outputFrames) {
        throw std::runtime_error("read the steady-tempo Warp reference");
    }
    double power = 0;
    float peakError = 0;
    // Ignore the clip's outer edge where the first and last filters can see
    // a different boundary. The source loop repeats the same interior.
    constexpr std::uint32_t edgeFrames = 2048;
    for (std::size_t frame = edgeFrames; frame < outputFrames - edgeFrames; ++frame) {
        const auto difference = heard[frame].left - reference[frame * 2];
        power += difference * difference;
        peakError = std::max(peakError, std::abs(difference));
    }
    const auto rms = std::sqrt(power / (outputFrames - 2 * edgeFrames));
    std::printf("steady %s at %.0f BPM: streamed/prepared peak difference %.6f, RMS %.6f\n",
                mode.data(), bpm, peakError, rms);
    check(peakError <= peakTolerance && rms <= rmsTolerance,
          "steady-tempo streaming matches the prepared reference within tolerance");
}

constexpr std::uint32_t reverseStart = 257;
constexpr std::uint32_t reverseLength = 72000;
constexpr std::array reverseOriginalMarkers{WarpMarker{0, 0}, WarpMarker{14000, quarter},
    WarpMarker{46000, 2 * quarter}, WarpMarker{reverseLength, 4 * quarter}};
constexpr std::array reverseExpectedMarkers{WarpMarker{0, 0}, WarpMarker{26000, 2 * quarter},
    WarpMarker{58000, 3 * quarter}, WarpMarker{reverseLength, 4 * quarter}};
constexpr std::array reverseAttacks{6500u, 42000u, 50000u, 65000u};
constexpr std::array reverseBeats{0.5, 2.5, 2.75, 3.5};

enum class ReverseSignal { Pulses, SingleAttacks, Sustained };

void reverseSource(Project& project, bool physicalReverse, bool ramp, ReverseSignal signal = ReverseSignal::Pulses) {
    constexpr auto fileFrames = reverseStart + reverseLength + 509;
    constexpr auto halfWidth = 96u;
    std::vector<float> pcm(fileFrames * 2, 0.7f);
    std::fill_n(pcm.data() + 2 * reverseStart, 2 * reverseLength, 0.0f);
    for (std::size_t attack = 0; attack < reverseAttacks.size(); ++attack) {
        const auto center = reverseAttacks[attack];
        if (signal == ReverseSignal::SingleAttacks) {
            // Each isolated pulse is exactly at the forward source position
            // corresponding to a known reversed fractional beat.
            const auto input = reverseStart + reverseLength - center;
            pcm[2*input] = 0.5f;
            pcm[2*input+1] = -0.2f;
            continue;
        }
        const auto scale = static_cast<float>(attack + 1) / 4.0f;
        for (auto frame = center - halfWidth; frame <= center + halfWidth; ++frame) {
            const auto distance = static_cast<double>(frame) - center;
            const auto pulse = static_cast<float>(0.25 * (1 + std::cos(std::numbers::pi * distance / halfWidth)));
            const auto input = reverseStart + (physicalReverse ? frame : reverseLength - 1 - frame);
            pcm[2 * input] = scale * pulse;
            pcm[2 * input + 1] = -0.2f * pulse;
        }
    }
    if (signal == ReverseSignal::Sustained) {
        for (std::uint32_t frame = 0; frame<reverseLength; ++frame) {
            const auto input = reverseStart + (physicalReverse ? frame : reverseLength-1-frame);
            const auto at = static_cast<double>(frame)/sourceRate;
            pcm[2*input] = static_cast<float>(0.3*std::sin(2*std::numbers::pi*167*at));
            pcm[2*input+1] = static_cast<float>(0.2*std::cos(2*std::numbers::pi*223*at));
        }
    }
    {
        audio::WavWriter writer(project.source, sourceRate, 2, audio::WavFormat::Float32);
        writer.write(pcm.data(), fileFrames);
        writer.close();
    }
    const auto digest = media::blake3File(project.source);
    if (!digest) throw std::runtime_error("hash reversed Warp source");
    const auto map = audio::writeWarpMap(physicalReverse ? reverseExpectedMarkers : reverseOriginalMarkers,
                                        reverseLength);
    if (!map) throw std::runtime_error("store the asymmetric Reverse Warp map");
    auto& db = project.store->db();
    SQLite::Statement mediaRow(db, "UPDATE media_files SET hash_blake3=?,frames=? WHERE id=1");
    mediaRow.bind(1, digest.hex);
    mediaRow.bind(2, fileFrames);
    mediaRow.exec();
    SQLite::Statement warp(db, "UPDATE audio_clips SET src_start_frames=?,src_len_frames=?,warp_markers=? WHERE clip_id=7");
    warp.bind(1, reverseStart);
    warp.bind(2, reverseLength);
    warp.bind(3, map.bytes.data(), static_cast<int>(map.bytes.size()));
    warp.exec();
    SQLite::Statement clip(db, "UPDATE clips SET length_ticks=?,loop_enabled=0,loop_len_ticks=NULL WHERE id=7");
    clip.bind(1, 4 * quarter);
    clip.exec();
    if (!physicalReverse) {
        constexpr std::array<std::int64_t, 1> clipIds{7};
        const auto edit = clip_edit::prepareReverse(*project.store, clipIds, std::nullopt, sourceRate);
        if (!edit.ok) throw std::runtime_error("prepare Reverse Warp: " + edit.error);
        const auto committed = clip_edit::commitPrepared(*project.store, edit);
        if (!committed.ok) throw std::runtime_error("commit Reverse Warp: " + committed.error);
        SQLite::Statement stored(db, "SELECT reverse,src_start_frames,warp_markers FROM audio_clips WHERE clip_id=7");
        if (!stored.executeStep()) throw std::runtime_error("read stored Reverse Warp clip");
        const auto blob = stored.getColumn(2);
        const auto bytes = std::span(static_cast<const std::byte*>(blob.getBlob()),
                                     static_cast<std::size_t>(blob.getBytes()));
        check(stored.getColumn(0).getInt() == 1 && stored.getColumn(1).getInt64() == reverseStart &&
              std::ranges::equal(bytes, map.bytes),
              "Reverse command keeps the physical start and authored AWRP bytes");
    }
    if (ramp) {
        SQLite::Statement loop(db, "UPDATE clips SET length_ticks=?,loop_enabled=1,loop_start_ticks=0,loop_len_ticks=? WHERE id=7");
        loop.bind(1, 8 * quarter);
        loop.bind(2, 4 * quarter);
        loop.exec();
    }
    project.tempo(ramp ? 1 : 0, 4 * quarter, ramp ? 140.0 : 100.0, ramp ? 120.0 : 100.0);
}

void comparePreparedReverse(const Project& project, std::span<const StereoFrame> output,
                            audio::WarpMode mode, float peakTolerance, double rmsTolerance) {
    // The prepared renderer stretches forward PCM through reflected anchors.
    // Reverse its output for the comparison, as the playback page reader did.
    audio::WarpRenderPlan plan;
    plan.source = project.source;
    plan.workFolder = project.temporary.path() / "prepared-reverse";
    plan.sourceStart = reverseStart;
    plan.sourceFrames = reverseLength;
    plan.outputRate = sourceRate;
    plan.mode = mode;
    plan.anchors = {{0, 0}, {14000, 28800}, {46000, 57600}, {reverseLength, output.size()}};
    const auto prepared = audio::prepareWarp(plan);
    if (!prepared) throw std::runtime_error("prepare reversed Warp reference");
    audio::WavReader reader(prepared.prepared->file);
    std::vector<float> pcm(output.size() * 2);
    if (reader.read(pcm.data(), static_cast<std::uint32_t>(output.size())) != output.size())
        throw std::runtime_error("read reversed Warp reference");
    double power = 0;
    float peak = 0;
    for (std::size_t frame = 0; frame < output.size(); ++frame) {
        const auto i = 2*(output.size()-1-frame);
        const auto left = output[frame].left - pcm[i];
        const auto right = output[frame].right - pcm[i+1];
        power += static_cast<double>(left)*left + static_cast<double>(right)*right;
        peak = std::max({peak,std::abs(left),std::abs(right)});
    }
    const auto rms = std::sqrt(power / (2.0*static_cast<double>(output.size())));
    std::printf("reverse %s against #830 steady: peak %.8f, RMS %.8f\n",
                audio::warpModeName(mode), static_cast<double>(peak), rms);
    check(peak <= peakTolerance && rms <= rmsTolerance,
          "steady reversed Warp follows the prepared result within measured tolerance");
}

void reverseRePitch(bool ramp) {
    Project reversed("re-pitch"), reflected("re-pitch");
    reverseSource(reversed, false, ramp);
    reverseSource(reflected, true, ramp);
    const auto rate = ramp ? 44100u : sourceRate;
    const auto width = ramp ? Precision::Float64 : Precision::Float32;
    const auto secondsAtBeat = [ramp](double beat) {
        if (!ramp) return 0.6 * beat;
        const auto firstLap = 12 * std::log1p(std::min(beat, 4.0) / 24.0);
        return firstLap + std::max(beat - 4.0, 0.0) * 60 / 140;
    };
    const auto outputEnd = static_cast<std::size_t>(std::llround(secondsAtBeat(ramp ? 8.0 : 4.0) * rate));
    std::vector<StereoFrame> output(outputEnd + maxFrames), reference(output.size());
    Session reverseSession, reflectedSession;
    reversed.load(reverseSession, width, rate);
    reflected.load(reflectedSession, width, rate);
    check(reverseSession.clips()->problems().empty(), "reversed Re-Pitch streams without a prepared render");
    render(reverseSession, width, output, 321);
    render(reflectedSession, width, reference, 470);
    check(output == reference, "physically reflected PCM and marker map match the Reverse command bit for bit");
    check(std::all_of(output.data() + outputEnd, output.data() + output.size(),
                      [](const StereoFrame& frame) { return frame == StereoFrame{}; }),
          "reversed Re-Pitch stops at the selected end of its audio");

    std::int64_t worst = 0;
    for (int lap = 0; lap < (ramp ? 2 : 1); ++lap) {
        for (std::size_t attack = 0; attack < reverseBeats.size(); ++attack) {
            const auto expected = static_cast<std::int64_t>(
                std::llround(rate * secondsAtBeat(4.0 * lap + reverseBeats[attack])));
            const auto* begin = output.data() + expected - 512;
            const auto* end = output.data() + expected + 512;
            const auto* peak = std::max_element(begin, end,
                [](const StereoFrame& a, const StereoFrame& b) { return a.left < b.left; });
            worst = std::max(worst, std::abs(static_cast<std::int64_t>(peak - output.data()) - expected));
            check(peak->left > 0.1f && peak->right < -0.09f, "reversed attacks keep their asymmetric stereo values");
        }
    }
    check(worst <= 1, "reversed attacks hit the independent tempo integral on every lap");

    if (!ramp)
        comparePreparedReverse(reversed,std::span(output).first(outputEnd),audio::WarpMode::RePitch,0.01f,0.001);

    // Start just before oriented source frame 40960, a PCM page boundary.
    const auto seek = static_cast<std::size_t>(std::llround(secondsAtBeat(ramp ? 6.5 : 2.5) * rate)) - 1200;
    Session cold;
    reversed.load(cold, width, rate);
    cold.transport().play(false);
    cold.transport().locate(static_cast<std::int64_t>(seek));
    cold.transport().play();
    std::array<StereoFrame, 2048> sought{};
    render(cold, width, sought, 243);
    check(std::equal(sought.begin(), sought.end(), output.data() + seek),
          "stop and cold seek restore the same reverse PCM across pages");
    check(reverseSession.clips()->underrunSamples() == 0 && cold.clips()->underrunSamples() == 0 &&
          reverseSession.clips()->readErrors() == 0 && cold.clips()->readErrors() == 0,
          "reverse PCM and cold seek each complete without page underruns or read errors");
    std::printf("reverse Re-Pitch %s: %d laps at %u Hz, max grid error %lld samples\n",
                ramp ? "ramp" : "steady", ramp ? 2 : 1, rate, static_cast<long long>(worst));
}

void reverseBeatsAttacks() {
    constexpr double firstBpm = 120, lastBpm = 140, slope = (lastBpm-firstBpm)/4;
    const auto sampleAtBeat = [&](double beat) {
        const auto seconds = 60/slope*std::log1p(slope*std::min(beat,4.0)/firstBpm) +
            std::max(0.0,beat-4)*60/lastBpm;
        return static_cast<std::size_t>(std::llround(seconds*sourceRate));
    };
    Project project("beats");
    reverseSource(project,false,true,ReverseSignal::SingleAttacks);
    Session session;
    project.load(session,Precision::Float32,sourceRate);
    std::vector<StereoFrame> heard(sampleAtBeat(8));
    render(session,Precision::Float32,heard,289);
    std::size_t worst = 0, attacks = 0;
    for (int lap = 0; lap < 2; ++lap) {
        for (const auto beat : reverseBeats) {
            // Each one-sample source onset is the first frame of its forward
            // slice and therefore the last frame before the reversed boundary.
            const auto expected = sampleAtBeat(4*lap+beat)-1;
            const auto* peak = std::max_element(heard.data()+expected-2,heard.data()+expected+3,
                [](const auto& a, const auto& b) { return a.left < b.left; });
            if (peak->left > 0.49f && peak->right < -0.19f) {
                const auto at = static_cast<std::size_t>(peak-heard.data());
                worst = std::max(worst, at > expected ? at-expected : expected-at);
                ++attacks;
            }
        }
    }
    const auto audibleFrames = std::count_if(heard.begin(),heard.end(),[](const auto& frame) { return frame.left>0.49f; });
    check(attacks == 8 && audibleFrames == 8 && worst <= 1,
          "the reversed Beats loop plays each analytic onset once across the ramp");
    check(session.clips()->problems().empty() && session.clips()->underrunSamples()==0 &&
          session.clips()->readErrors()==0,"reversed Beats streams without page failures or a prepared render");
    std::printf("reverse Beats ramp: %zu/8 attacks, worst grid error %zu samples\n", attacks, worst);
    const auto seek = sampleAtBeat(3.5)-50;
    session.transport().play(false);
    session.transport().locate(static_cast<std::int64_t>(seek));
    session.transport().play();
    std::vector<StereoFrame> sought(sampleAtBeat(4.5)+50-seek);
    render(session,Precision::Float32,sought,113);
    check(std::equal(sought.begin(),sought.end(),heard.data()+seek) && session.clips()->underrunSamples()==0,
          "a reverse Beats cold seek reproduces both attacks around the loop seam");


    Project steady("beats");
    reverseSource(steady,false,false);
    Session flat;
    steady.load(flat,Precision::Float32,sourceRate);
    constexpr std::size_t frames = 115200;
    std::vector<StereoFrame> native(frames);
    render(flat,Precision::Float32,native,313);
    comparePreparedReverse(steady,native,audio::WarpMode::Beats,0.002f,0.0001);
    check(flat.clips()->problems().empty(),"steady reversed Beats streams without a prepared render");
}

void reverseTexture() {
    for (const auto curve : {0,1}) {
        Project project("texture");
        reverseSource(project,false,true);
        const auto boundaryBeat = curve == 0 ? 2 : 4;
        project.tempo(curve, boundaryBeat * quarter, 140);
        const auto before = [&](double beat) {
            return curve == 0 ? beat/2 : 12*std::log1p(beat/24);
        };
        const auto sampleAtBeat = [&](double beat) {
            const auto seconds = before(std::min(beat, static_cast<double>(boundaryBeat))) +
                std::max(0.0, beat - boundaryBeat) * 60 / 140;
            return static_cast<std::size_t>(std::llround(seconds*sourceRate));
        };

        Session session;
        project.load(session,Precision::Float32,sourceRate);
        std::vector<StereoFrame> heard(sampleAtBeat(8));
        render(session,Precision::Float32,heard,311);
        std::size_t worst = 0, landmarks = 0;
        for (int lap = 0; lap<2; ++lap) {
            for (const auto beat : reverseBeats) {
                const auto expected = sampleAtBeat(4*lap+beat);
                const auto* peak = std::min_element(heard.data()+expected-1400,heard.data()+expected+1401,
                    [](const auto& a, const auto& b) { return a.right<b.right; });
                if (peak->right < -0.018f) {
                    const auto at = static_cast<std::size_t>(peak-heard.data());
                    worst = std::max(worst,at>expected ? at-expected : expected-at);
                    ++landmarks;
                }
            }
        }
        std::printf("reverse Texture %s: %zu/8 landmarks, worst %zu samples\n",
                    curve == 0 ? "step" : "ramp",landmarks,worst);
        check(landmarks==8 && worst<=1250,"reversed Texture follows the integrated clock through both loop laps");

        const auto seek = sampleAtBeat(3.5)-1400;
        session.transport().play(false);
        session.transport().locate(static_cast<std::int64_t>(seek));
        session.transport().play();
        std::vector<StereoFrame> sought(sampleAtBeat(4.5)+1400-seek);
        render(session,Precision::Float32,sought,201);
        check(std::equal(sought.begin(),sought.end(),heard.data()+seek),
              "a reverse Texture cold seek preserves grain phase across audible loop boundaries");
        check(session.clips()->problems().empty() && session.clips()->underrunSamples()==0 &&
              session.clips()->readErrors()==0,"reversed Texture plays and seeks with all source PCM available");

        if (curve == 1) {
            const auto saved = project.temporary.path()/"reopened.adi";
            if (!project.store->saveAs(saved).ok) throw std::runtime_error("save reversed Texture project");
            project.store.reset();
            StoreError error{};
            project.store = Store::open(saved,error);
            if (!project.store) throw std::runtime_error("reopen reversed Texture project");
            Session reopened;
            project.load(reopened,Precision::Float32,sourceRate);
            std::vector<StereoFrame> replay(heard.size());
            render(reopened,Precision::Float32,replay,217);
            check(replay==heard,"reopened reversed Texture renders identical PCM");
        }
    }

    Project steady("texture");
    reverseSource(steady,false,false);
    Session flat;
    steady.load(flat,Precision::Float32,sourceRate);
    std::vector<StereoFrame> native(115200);
    render(flat,Precision::Float32,native,313);
    comparePreparedReverse(steady,native,audio::WarpMode::Texture,0.002f,0.0001);
    check(flat.clips()->problems().empty(),"steady reversed Texture streams without a prepared render");
}

void reverseTextureTypedTempo() {
    // Start the edit inside a sounding grain and cross a source-page boundary
    // while the following callbacks use the new project clock.
    constexpr auto changeFrame = std::int64_t{3}*8192 + 64;
    constexpr auto changeTick = (changeFrame*quarter + framesPerBeat/2)/framesPerBeat;
    const auto end = static_cast<std::size_t>(std::llround(static_cast<double>(changeFrame) +
        static_cast<double>(8*quarter-changeTick)*60.0*sourceRate/(140.0*quarter)));
    Project typed("texture"), authored("texture");
    reverseSource(typed,false,true,ReverseSignal::Sustained);
    reverseSource(authored,false,true,ReverseSignal::Sustained);
    typed.tempo(0,changeTick,120);
    authored.tempo(0,changeTick,140);
    Session liveSession, referenceSession;
    typed.load(liveSession,Precision::Float32,sourceRate);
    authored.load(referenceSession,Precision::Float32,sourceRate);
    std::vector<StereoFrame> heard(end), reference(end);
    const auto switchAt = static_cast<std::size_t>(changeFrame);
    auto live = std::span(heard);
    render(liveSession,Precision::Float32,live.first(switchAt),257);

    std::array<OpRequest,2> edit;
    edit[0].opType = "project.removeTempoEvent";
    edit[0].payload = {{"pos",changeTick}};
    edit[1].opType = "project.insertTempoEvent";
    edit[1].payload = {{"pos",changeTick},{"bpm",140.0},{"curve",0}};
    if (!OpJournal(*typed.store).commit(edit).ok || !liveSession.refresh(*typed.store))
        throw std::runtime_error("type tempo while reversed Texture plays");
    check(liveSession.transport().playing() && liveSession.transport().position()==changeFrame,
          "typing a tempo preserves the reversed Texture playhead");
    render(liveSession,Precision::Float32,live.subspan(switchAt,257),257,false);
    render(liveSession,Precision::Float32,live.subspan(switchAt+257),257);
    render(referenceSession,Precision::Float32,reference,463);
    const auto changed = live.subspan(switchAt);
    check(std::any_of(changed.begin(),changed.begin()+257,[](const auto& frame) { return std::abs(frame.left)>0.05f; }),
          "the first new-clock callback contains audible reversed grains");
    check(std::equal(changed.begin(),changed.end(),reference.data()+switchAt),
          "typed and authored tempos produce the same reversed grain phase from the boundary callback");
    check(liveSession.clips()->problems().empty() && liveSession.clips()->underrunSamples()==0 &&
          liveSession.clips()->readErrors()==0, "typed reverse Texture plays immediately with ready source pages");
    const auto step = std::max(std::abs(heard[switchAt].left-heard[switchAt-1].left),
                              std::abs(heard[switchAt].right-heard[switchAt-1].right));
    float largestNearby = 0;
    for (auto i = switchAt-257; i<switchAt+257; ++i) {
        if (i == switchAt) continue;
        largestNearby = std::max({largestNearby,std::abs(heard[i].left-heard[i-1].left),
            std::abs(heard[i].right-heard[i-1].right)});
    }
    std::printf("reverse Texture typed at %lld: %zu exact frames, boundary %.6f vs nearby %.6f FS\n",
        static_cast<long long>(changeFrame), changed.size(), static_cast<double>(step),static_cast<double>(largestNearby));
    check(step <= largestNearby,"typed tempo seam stays within nearby waveform variation");
}

void reverseTextureRenderCursor() {
    const TempoMap tempo{{{0, 100, 2, 0.72}, {12 * quarter, 180, 0, 0}}};
    const WarpArrangementClock clock(tempo, sourceRate);
    constexpr auto origin = static_cast<double>(quarter / 3);
    constexpr auto loopStart = static_cast<double>(quarter / 2);
    constexpr auto loopLength = static_cast<double>(4 * quarter);
    std::vector<float> pcm(120000);
    for (std::size_t i = 0; i < pcm.size(); ++i)
        pcm[i] = static_cast<float>(std::sin(static_cast<double>(i) / 31));
    const std::vector<WarpMarker> markers{{0, 0}, {34000, 2 * quarter}, {120000, 5 * quarter}};
    struct ReadProbe {
        std::span<const float> source;
        std::array<std::int64_t, 12> reads{};
        std::size_t count = 0;
        static float sample(void* context, std::int64_t frame, std::uint16_t) noexcept {
            auto& self = *static_cast<ReadProbe*>(context);
            if (self.count < self.reads.size()) self.reads[self.count] = frame;
            ++self.count;
            return self.source[static_cast<std::size_t>(frame)];
        }
    };
    ReadProbe scalarReader{pcm}, cachedReader{pcm};
    const audio::WarpStreamInput scalarInput{&scalarReader, ReadProbe::sample};
    const audio::WarpStreamInput cachedInput{&cachedReader, ReadProbe::sample};
    const auto firstWrap = std::llround(clock.frameAtTick(origin + loopStart + loopLength));
    const auto secondWrap = std::llround(clock.frameAtTick(origin + loopStart + 2 * loopLength));
    for (const auto grain : {0.001, 0.065}) {
        audio::WarpRenderPlan plan;
        plan.mode = audio::WarpMode::Texture;
        plan.sourceFrames = pcm.size();
        plan.outputRate = sourceRate;
        plan.grainSeconds = grain;
        const WarpClipPlayer player(clock, plan, markers, sourceRate, 1, origin, 12 * quarter,
                                    loopStart, loopLength, true);
        WarpClipPlayer::RenderCursor cursor;
        bool matches = true, audible = false;
        {
            const audio::CallbackScope callback;
            // Keep the cursor through two wraps and an out-of-order seek.
            for (const auto begin : {firstWrap - 150, secondWrap - 89, secondWrap, firstWrap - 433}) {
                for (auto frame = begin; frame < begin + 513; ++frame) {
                    std::array<float, 1> scalar{}, cached{};
                    scalarReader.count = cachedReader.count = 0;
                    matches &= player.renderFrame(frame, scalarInput, scalar) &&
                               player.renderFrame(cursor, frame, cachedInput, cached) && scalar == cached;
                    matches &= scalarReader.count <= scalarReader.reads.size() &&
                               scalarReader.count == cachedReader.count &&
                               std::equal(scalarReader.reads.data(), scalarReader.reads.data() + scalarReader.count,
                                          cachedReader.reads.data());
                    audible |= std::abs(scalar[0]) > 0.1f;
                }
            }
        }
        check(audible && matches, "reversed Texture cursor preserves PCM and reads through wraps and a cold seek");
    }
}

constexpr double bezierTension = 0.72;

double bezierSecondsAtBeat(double beat) {
    // Independently integrate dseconds = 60 / bpm dbeats in the authored
    // quadratic Bezier parameter rather than using the engine's clock.
    const auto fraction = beat / arrangementBeats;
    auto lo = 0.0, hi = 1.0;
    for (int step = 0; step < 48; ++step) {
        const auto middle = (lo + hi) / 2;
        const auto x = middle + bezierTension * middle * (1 - middle);
        if (x < fraction) lo = middle;
        else hi = middle;
    }
    const auto integrand = [](double t) {
        const auto y = t - bezierTension * t * (1 - t);
        const auto dx = 1 + bezierTension * (1 - 2 * t);
        return 60 * arrangementBeats * dx / (120 + 20 * y);
    };
    constexpr int intervals = 256;
    const auto dt = (lo + hi) / (2 * intervals);
    auto sum = integrand(0) + integrand(dt * intervals);
    for (int i = 1; i < intervals; ++i) {
        sum += (i % 2 == 0 ? 2 : 4) * integrand(dt * i);
    }
    return dt * sum / 3;
}

std::vector<StereoFrame> exportWav(const std::filesystem::path& project, std::size_t frames) {
    render::Job job;
    job.project = project;
    job.directory = project.parent_path();
    job.endSample = static_cast<std::int64_t>(frames);
    job.session = SessionSpec{2, sourceRate, 179};
    job.encoding.format = audio::WavFormat::Float32;
    const auto result = render::renderMasterWav(job, {});
    if (result.state != render::JobState::Completed || result.framesWritten != frames) {
        throw std::runtime_error("export Warp proof WAV: " + result.error);
    }
    audio::WavReader reader(result.output);
    if (reader.frames() != frames || reader.channels() != 2 || reader.sampleRate() != sourceRate) {
        throw std::runtime_error("verify Warp proof WAV format and length");
    }
    std::vector<float> samples(frames * 2);
    if (reader.read(samples.data(), static_cast<std::uint32_t>(frames)) != frames) {
        throw std::runtime_error("read Warp proof WAV");
    }
    std::vector<StereoFrame> stereo(frames);
    for (std::size_t i = 0; i < frames; ++i) stereo[i] = {samples[2 * i], samples[2 * i + 1]};
    return stereo;
}

void preparedFallbackNotice() {
    Project project("complex");
    project.tempo(0, 4 * quarter, 300);
    project.store->db().exec("INSERT INTO mixer_strip(track_id) VALUES(1)");
    constexpr std::string_view notice = "clips#7: tempo changes need a new prepared Warp render";
    {
        Session session;
        project.load(session, Precision::Float32, sourceRate);
        const auto* clips = session.clips();
        const auto& diagnostics = clips->problems();
        check(std::find(diagnostics.begin(), diagnostics.end(), notice) != diagnostics.end(),
              "ClipPlayback names the Complex clip using prepared Warp audio");
        check(clips->fatalProblems().empty() && session.problems().empty(),
              "the prepared Warp notice leaves offline preflight resolved");
        const auto frames = 4 * framesPerBeat + sourceRate / 3;
        const auto playback = compensatedPlayback(session, frames, 257);
        const auto exported = exportWav(project.store->path(), frames);
        check(playback == exported && std::any_of(playback.begin(), playback.end(),
              [](const auto& frame) { return std::abs(frame.left) > 0.25f; }),
              "the fallback is audible and exports the same prepared PCM");
        GainStageOptions options;
        options.endSample = static_cast<std::int64_t>(frames);
        const auto gain = session.autoGainStage(*project.store, options);
        if (!gain.commit.ok) std::printf("prepared Warp gain stage: %s\n", gain.commit.error.c_str());
        else if (gain.tracks.size() == 1)
            std::printf("prepared Warp gain-stage RMS %.6f dBFS\n", gain.tracks.front().measuredDb);
        check(gain.commit.ok && gain.tracks.size() == 1 && !gain.tracks.front().silent,
              "auto gain measures the prepared Warp fallback");
    }

    project.store->db().exec("INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks) "
                             "VALUES(8,1,'audio',0,5765760)");
    Session session;
    project.load(session, Precision::Float32, sourceRate);
    const auto fatal = session.clips()->fatalProblems();
    check(fatal.size() == 1 && fatal.front().starts_with("clips#8:") &&
          session.problems().size() == 1 && session.problems().front() == fatal.front(),
          "missing audio remains fatal beside a prepared Warp notice");
}

void playbackExportAndReopen(std::string_view mode, bool bezier) {
    Project project(mode);
    if (bezier) project.tempo(2, arrangementBeats * quarter, 140, 120, bezierTension);
    const auto secondsAtBeat = bezier ? bezierSecondsAtBeat : +[](double beat) { return beat / 2; };
    const auto frames = static_cast<std::size_t>(std::llround(sourceRate * secondsAtBeat(arrangementBeats))) + sourceRate / 10;
    const auto seek = static_cast<std::size_t>(std::llround(sourceRate * secondsAtBeat(3))) - 137;
    const auto savedDirectory = project.temporary.path() / "saved";
    std::filesystem::create_directory(savedDirectory);
    const auto saved = savedDirectory / "copy.adi";
    std::vector<StereoFrame> original;
    bool seekMatches = false;
    {
        Session session;
        project.load(session, Precision::Float32, sourceRate);
        original = compensatedPlayback(session, frames, 257);
        check(mode != "complex" || session.graph().currentGraph()->latencySamples() > 0,
              "steady Complex playback goes through the latency-reporting streaming R3 path");
        const auto error = beatLandmarks(original, sourceRate, secondsAtBeat, mode.data());
        check(error <= (bezier ? 4 : 64), "native Warp acceptance fixture lands on its authored beat grid");

        // Jump back past the page cache's retained tail, then start a different
        // callback partition. The range crosses the pulse 137 frames after seek.
        session.transport().play(false);
        session.transport().locate(static_cast<std::int64_t>(seek));
        session.transport().play();
        const auto sought = compensatedPlayback(session, 4096, 113);
        seekMatches = std::equal(sought.begin(), sought.end(), original.begin() + static_cast<std::ptrdiff_t>(seek));
        check(seekMatches,
              "offline timeline and stop, cold seek, and play return the same compensated PCM");
        check(session.clips()->underrunSamples() == 0 && session.clips()->readErrors() == 0,
              "native playback and cold seek complete without underruns or read errors");
        const auto result = project.store->saveAs(saved);
        if (!result.ok) throw std::runtime_error("save Warp proof project: " + result.error);
    }
    project.store.reset();
    StoreError openError{};
    project.store = Store::open(saved, openError);
    if (!project.store) throw std::runtime_error(toString(openError));
    Session reopenedSession;
    project.load(reopenedSession, Precision::Float32, sourceRate);
    const auto reopened = compensatedPlayback(reopenedSession, frames, 311);
    const auto exported = exportWav(saved, frames);
    const auto reopenMatches = original == reopened;
    const auto exportMatches = original == exported;
    check(reopenMatches, "closing and reopening the saved Warp project preserves compensated PCM bit for bit");
    check(exportMatches, "queued Float32 WAV export equals compensated native playback bit for bit");
    std::printf("M2-A-WARP 7 %s: %zu stereo frames, graph latency %d samples, cold seek %s, reopen %s, export %s\n",
                mode.data(), frames, reopenedSession.graph().currentGraph()->latencySamples(),
                seekMatches ? "exact" : "different", reopenMatches ? "exact" : "different", exportMatches ? "exact" : "different");
}

constexpr std::uint32_t savedRate = 48000, windowStart = 263, windowFrames = 65573, placementFrame = 1379;
constexpr std::int32_t alignment = 8192;

void requireUnity(bool condition,const char* failure) {
    if (!condition) throw std::runtime_error(failure);
}

std::int64_t reversedUnityTick(std::uint32_t at) {
    return static_cast<std::int64_t>(std::llround(static_cast<double>(at)*quarter/(savedRate/2)));
}

struct SavedWindow {
    test::TempDirectory directory{"arrangement-reversed-complex-unity","project"};
    std::unique_ptr<Store> store;

    SavedWindow() {
        const auto song = directory.path()/"unity.adi";
        StoreError error{};
        store = Store::createEmpty(song,error);
        if (!store) throw std::runtime_error(toString(error));
        const auto path = directory.path()/"stereo.wav";
        constexpr auto samples = windowStart+windowFrames+509;
        std::vector<float> input(2*samples,.94f);
        for (std::uint32_t frame=0;frame<windowFrames;++frame) {
            const auto t = static_cast<double>(frame)/savedRate;
            const auto section = static_cast<double>(frame/804);
            const auto phase = (995+section*315)*t*2*std::numbers::pi;
            const auto pulse = frame%alignment < 29 ? .15 : 0.0;
            input[(windowStart+frame)*2] = static_cast<float>(std::pow(std::sin(phase),3)*.6+pulse);
            input[(windowStart+frame)*2+1] = static_cast<float>(std::pow(std::cos(phase/2),3)*-.8-pulse);
        }
        audio::WavWriter writer(path,savedRate,2,audio::WavFormat::Float32);
        writer.write(input.data(),samples);
        writer.close();
        const auto digest = media::blake3File(path);
        requireUnity(static_cast<bool>(digest),"hash the saved source");
        {
            auto& db = store->db();
            db.exec("INSERT INTO project(id,name,sample_rate) VALUES(1,'Reversed Complex',48000);"
                    "INSERT INTO tracks(id,kind,name) VALUES(1,'audio','Reverse Wind'),(9,'master','Main');"
                    "DELETE FROM tempo_map;"
                    "INSERT INTO tempo_map(pos_ticks,bpm,curve) VALUES(0,120,0)");
            SQLite::Statement media(db,"INSERT INTO media_files(id,hash_blake3,rel_path,format,sample_rate,channels,frames) "
                                       "VALUES(1,?,'stereo.wav','wav',48000,2,?)");
            media.bind(1,digest.hex);
            media.bind(2,samples);
            media.exec();
            SQLite::Statement clip(db,"INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks,gain_db) "
                                      "VALUES(7,1,'audio',?,?,-6.020599913279624)");
            clip.bind(1,reversedUnityTick(placementFrame));
            clip.bind(2,reversedUnityTick(windowFrames));
            clip.exec();
            const auto blob = audio::writeWarpMap(std::array{WarpMarker{0,0},WarpMarker{windowFrames,reversedUnityTick(windowFrames)}},
                                                  windowFrames);
            requireUnity(static_cast<bool>(blob),"encode exact unity Warp endpoints");
            SQLite::Statement audioRow(db,"INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames,"
                                          "warp_enabled,warp_mode,warp_markers,reverse) VALUES(7,1,?,?,1,'complex',?,1)");
            audioRow.bind(1,windowStart);
            audioRow.bind(2,windowFrames);
            audioRow.bind(3,blob.bytes.data(),static_cast<int>(blob.bytes.size()));
            audioRow.exec();
        }
        store.reset();
        store = Store::open(song,error);
        if (!store) throw std::runtime_error(toString(error));
    }

    void load(Session& session,bool prepared,std::uint32_t first=0) {
        SessionSpec spec{2,savedRate,maxFrames};
        spec.synchronousClipReads = prepared;
        session.graph().setFadeFrames(0);
        if (!session.load(*store,{},spec)) throw std::runtime_error(session.error());
        session.transport().locate(first);
        session.transport().play();
    }
};

bool bitEqual(std::span<const StereoFrame> a,std::span<const StereoFrame> b) {
    return std::equal(a.begin(),a.end(),b.begin(),b.end(),[](const auto& first,const auto& next) {
        return std::bit_cast<std::uint32_t>(first.left) == std::bit_cast<std::uint32_t>(next.left) &&
               std::bit_cast<std::uint32_t>(first.right) == std::bit_cast<std::uint32_t>(next.right);
    });
}

void reversedUnityPreparedParity() {
    SavedWindow source;
    Session native, reference;
    source.load(native,false);
    source.load(reference,true);
    requireUnity(native.graph().currentGraph()->latencySamples()==0 &&
                 reference.graph().currentGraph()->latencySamples()==0,"worker and prepared have zero graph delay");
    requireUnity(reference.clips()->problems().size()==1 && native.clips()->problems().empty(),
                 "only the synchronous reference uses whole-clip preparation");
    constexpr auto count=placementFrame+windowFrames+513;
    const auto heard=compensatedPlayback(native,count,377);
    const auto golden=compensatedPlayback(reference,count,231);
    requireUnity(bitEqual(heard,golden),"all samples including true window edges match the historical reverse");
    requireUnity(std::abs(heard[placementFrame+79].left)>.015f &&
                 std::abs(heard[placementFrame+windowFrames-137].right)>.015f,
                 "the exact match includes audible PCM near each source edge");
    requireUnity(heard[placementFrame-1]==StereoFrame{} && heard[placementFrame+windowFrames]==StereoFrame{},
                 "the page producer emits nothing outside the saved window");

    {
        Session cold;
        constexpr auto seek = placementFrame+2*alignment+alignment-79;
        source.load(cold,false,seek);
        const auto result=compensatedPlayback(cold,4511,311);
        requireUnity(bitEqual(result,std::span(golden).subspan(seek,result.size())),
                     "a fresh Session seeks across a reverse page boundary bit-exactly");
        requireUnity(cold.clips()->underrunSamples()==0 && cold.clips()->readErrors()==0,
                     "cold seek retrieves every exact page without errors");
    }
    requireUnity(native.clips()->underrunSamples()==0 && native.clips()->readErrors()==0,
                 "saved unity playback has no source errors or underruns");

    constexpr auto tempoStart=placementFrame+19003;
    native.transport().locate(tempoStart);
    OpRequest tempo;
    tempo.opType="project.insertTempoEvent";
    tempo.payload={{"pos",reversedUnityTick(tempoStart)},{"bpm",140},{"curve",0}};
    requireUnity(OpJournal(*source.store).commit(tempo).ok && native.refresh(*source.store),
                 "publish changed clock while the reversed clip is playing");
    requireUnity(native.transport().position()==tempoStart && native.transport().playing() &&
                 native.clips()->problems().size()==1 && native.graph().currentGraph()->latencySamples()==0,
                 "changed clock leaves the playhead and the legacy prepared path");
    Session fallback;
    source.load(fallback,true,tempoStart);
    const auto changed=compensatedPlayback(native,12000,467);
    const auto expected=compensatedPlayback(fallback,12000,239);
    requireUnity(bitEqual(changed,expected),"edited clock remains bit-exact to the legacy prepared generation");
    requireUnity(native.clips()->underrunSamples()==0 && native.clips()->readErrors()==0,
                 "prepared fallback renders the changed clock without source problems");
    std::printf("reversed unity Complex: %u saved frames with prefix %u, bit-exact prepared PCM and cold pages, "
                "edited clock prepared, graph latency 0, callback allocation/I/O 0\n",windowFrames,windowStart);
}
}

int main() {
    try {
        test::TempDirectory cache{"streaming_warp", "cache"};
#if defined(_WIN32)
        if (_wputenv_s(L"ADI_HOME", cache.path().c_str()) != 0) throw std::runtime_error("set Warp test cache");
#else
        if (setenv("ADI_HOME", cache.path().c_str(), 1) != 0) throw std::runtime_error("set Warp test cache");
#endif
        tempoRamp("beats", Precision::Float32, sourceRate, 4);
        tempoRamp("re-pitch", Precision::Float64, 44100, 4);
        // Texture moves individual grains on purpose. Its default 65 ms grain
        // and 25% fluctuation allow around 16 ms of displacement before stretch.
        tempoRamp("texture", Precision::Float32, sourceRate, 1024);
        typedTempo("beats", 4 * framesPerBeat, 4);
        typedTempo("complex", 12 * 8192 + 64, 160);
        r3UsesLocalTempoDomain();
        steadyTempo("beats", 120.0);
        steadyTempo("re-pitch", 100.0);
        reverseRePitch(false);
        reverseRePitch(true);
        reverseBeatsAttacks();
        reverseTexture();
        reverseTextureTypedTempo();
        reverseTextureRenderCursor();
        reversedUnityPreparedParity();
        steadyTempo("texture", 120.0);
        // Real-time and offline R3 use different spectral histories around
        // transients. Compare at -22 dBFS peak and -48 dBFS RMS error.
        steadyTempo("complex", 120.0, 0.08f, 0.004);
        preparedFallbackNotice();
        playbackExportAndReopen("complex", false);
        playbackExportAndReopen("beats", true);
    } catch (const std::exception& error) {
        check(false, error.what());
    }
    std::printf("callback C++ allocations %u, releases %u, file I/O calls %u\n",
                allocations.load(), releases.load(), audio::callbackFileIo.load());
    check(allocations.load() == 0 && releases.load() == 0 && audio::callbackFileIo.load() == 0,
          "streaming warp callbacks have no C++ allocation, release, or file I/O");
#if defined(ADI_TEST_WARP_WRAP_CALLBACK)
    std::printf("callback C allocations %u, releases %u, mutex calls %u\n",
                cAllocations.load(), cReleases.load(), locks.load());
    check(cAllocations.load() == 0 && cReleases.load() == 0 && locks.load() == 0,
          "streaming warp callbacks have no linked C allocation, release, or mutex calls");
#endif
    return failures == 0 ? 0 : 1;
}
