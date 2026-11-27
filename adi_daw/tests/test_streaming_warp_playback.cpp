// SPDX-License-Identifier: AGPL-3.0-or-later
#define ADI_TEST_COUNT_REPLACED_DELETE
#include "replace_operator_new.hpp"
#include "temp_directory.hpp"

#include "adi/audio/io_audit.hpp"
#include "adi/audio/warp.hpp"
#include "adi/audio/warp_map.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/engine/session.hpp"
#include "adi/media/blake3.hpp"
#include "adi/ops.hpp"
#include "adi/render/queue.hpp"
#include "adi/store.hpp"
#include "adi/textproj.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <atomic>
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

void typedTempo() {
    Project project("beats");
    constexpr std::int64_t changeBeat = 4;
    constexpr auto changeTick = changeBeat * quarter;
    constexpr auto changeFrame = framesPerBeat * changeBeat;
    project.tempo(0, changeTick, 120);
    Session session;
    project.load(session, Precision::Float32, sourceRate);
    const auto secondsAtBeat = [](double beat) {
        if (beat <= changeBeat) return beat / 2;
        return changeBeat / 2.0 + (beat - changeBeat) * 60.0 / 140.0;
    };
    const auto frames = static_cast<std::size_t>(std::llround(sourceRate * secondsAtBeat(arrangementBeats))) + sourceRate / 10;
    std::vector<StereoFrame> heard(frames);
    std::span<StereoFrame> output = heard;
    render(session, Precision::Float32, output.first(changeFrame), 257);

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
    render(session, Precision::Float32, output.subspan(changeFrame), 257, false);
    check(beatLandmarks(heard, sourceRate, secondsAtBeat, "typed Beats") <= 4,
          "the next callback hears the typed tempo without a pre-render wait or a lost beat");
    check(session.clips()->underrunSamples() == 0 && session.clips()->readErrors() == 0,
          "the first callback after a typed tempo has ready source audio");
}

std::vector<StereoFrame> compensatedPlayback(Session& session, std::size_t frames, std::int32_t blockFrames) {
    const auto* graph = session.graph().currentGraph();
    if (!graph || graph->latencySamples() < 0) throw std::runtime_error("read Warp graph latency");
    const auto latency = static_cast<std::size_t>(graph->latencySamples());
    std::vector<StereoFrame> raw(frames + latency);
    render(session, Precision::Float32, raw, blockFrames);
    return {raw.begin() + static_cast<std::ptrdiff_t>(latency), raw.end()};
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
        typedTempo();
        steadyTempo("beats", 120.0);
        steadyTempo("re-pitch", 100.0);
        steadyTempo("texture", 120.0);
        // Real-time and offline R3 use different spectral histories around
        // transients. Compare at -22 dBFS peak and -48 dBFS RMS error.
        steadyTempo("complex", 120.0, 0.08f, 0.004);
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
