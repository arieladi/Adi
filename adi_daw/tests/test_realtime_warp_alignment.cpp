// SPDX-License-Identifier: AGPL-3.0-or-later
#include "temp_directory.hpp"

#include "adi/audio/io_audit.hpp"
#include "adi/audio/warp_map.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/engine/session.hpp"
#include "adi/media/blake3.hpp"
#include "adi/store.hpp"
#include "adi/textproj.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numbers>
#include <span>
#include <stdexcept>
#include <vector>

namespace {
using namespace adi;
using namespace adi::engine;

constexpr std::uint32_t rate = 48000;
constexpr std::int32_t block = 512;
constexpr std::int64_t ticks = textproj::kPPQ;
constexpr std::uint32_t framesPerBeat = rate / 2;
constexpr std::uint32_t sourceFrames = 8 * framesPerBeat;
constexpr std::uint32_t earlyPulse = framesPerBeat;
constexpr std::uint32_t sourceRampPulse = 5 * framesPerBeat;
constexpr int pulseHalfWidth = 110;

std::uint32_t rampPulseFrame() {
    // Tempo starts rising at beat 2, from 120 to 140 over six beats.
    constexpr double slope = 20.0 / 6.0;
    const double seconds = 1.0 + 60.0 / slope * std::log1p(slope * 3 / 120.0);
    return static_cast<std::uint32_t>(std::llround(seconds * rate));
}

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void addMedia(Store& store, int id, const char* filename, std::size_t channel, std::uint32_t secondPulse) {
    const auto path = store.path().parent_path() / filename;
    std::vector<float> samples(2 * sourceFrames);
    for (const auto centre : {earlyPulse, secondPulse}) {
        for (int offset = -pulseHalfWidth; offset <= pulseHalfWidth; ++offset) {
            const auto frame = static_cast<std::size_t>(static_cast<std::int64_t>(centre) + offset);
            const double window = .5 * (1 + std::cos(std::numbers::pi * offset / pulseHalfWidth));
            samples[frame * 2 + channel] = static_cast<float>(0.8 * window);
        }
    }
    audio::WavWriter writer(path, rate, 2, audio::WavFormat::Float32);
    writer.write(samples.data(), sourceFrames);
    writer.close();
    const auto digest = media::blake3File(path);
    require(static_cast<bool>(digest), "hash latency fixture");
    SQLite::Statement row(store.db(), "INSERT INTO media_files(id,hash_blake3,rel_path,format,sample_rate,channels,frames) "
                                     "VALUES(?,?,?,'wav',48000,2,?)");
    row.bind(1, id);
    row.bind(2, digest.hex);
    row.bind(3, filename);
    row.bind(4, sourceFrames);
    row.exec();
}

void arrange(Store& store) {
    addMedia(store, 1, "complex.wav", 0, sourceRampPulse);
    addMedia(store, 2, "dry.wav", 1, rampPulseFrame());
    auto& db = store.db();
    db.exec("DELETE FROM tempo_map; "
            "INSERT INTO tracks(id,kind,name) VALUES(1,'audio','Complex'),(2,'audio','Dry'),(9,'master','Main');");
    SQLite::Statement tempo(db, "INSERT INTO tempo_map(pos_ticks,bpm,curve) VALUES(?,?,?)");
    for (const auto pos : {0 * ticks, 2 * ticks, 8 * ticks}) {
        tempo.bind(1, pos);
        tempo.bind(2, pos == 8 * ticks ? 140.0 : 120.0);
        tempo.bind(3, pos == 2 * ticks ? 1 : 0);
        tempo.exec();
        tempo.reset();
    }
    SQLite::Statement clips(db, "INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks) "
                                "VALUES(1,1,'audio',0,?),(2,2,'audio',0,?)");
    clips.bind(1, 8 * ticks);
    clips.bind(2, 8 * ticks);
    clips.exec();
    const auto markers = audio::writeWarpMap(std::array<WarpMarker, 2>{{{0, 0}, {sourceFrames, 8 * ticks}}},
                                            sourceFrames);
    require(static_cast<bool>(markers), "write latency fixture warp map");
    SQLite::Statement audioClips(db, "INSERT INTO audio_clips(clip_id,media_id,src_len_frames,warp_enabled,warp_mode,warp_markers) "
                                     "VALUES(1,1,?,1,'complex',?),(2,2,?,0,'beats',NULL)");
    audioClips.bind(1, sourceFrames);
    audioClips.bind(2, markers.bytes.data(), static_cast<int>(markers.bytes.size()));
    audioClips.bind(3, sourceFrames);
    audioClips.exec();
}

using Channels = std::array<std::vector<float>, 2>;

Channels render(Session& session, Precision precision, std::int64_t frames) {
    Channels channels{std::vector<float>(static_cast<std::size_t>(frames)),
                      std::vector<float>(static_cast<std::size_t>(frames))};
    std::array<double, block> left{}, right{};
    double* wide[]{left.data(), right.data()};
    for (std::int64_t first = 0; first < frames;) {
        require(session.clips()->prime(std::chrono::seconds(5)), "prime latency proof source pages");
        float* out[]{channels[0].data() + first, channels[1].data() + first};
        AudioIo io;
        if (precision == Precision::Float64) io.out64 = wide;
        else io.out = out;
        io.numOut = 2;
        io.frames = static_cast<std::int32_t>(std::min<std::int64_t>(frames - first, block));
        {
            const audio::CallbackScope callback;
            session.process(io);
        }
        if (precision == Precision::Float64) {
            for (std::size_t channel = 0; channel < channels.size(); ++channel)
                std::transform(wide[channel], wide[channel] + io.frames, out[channel],
                               [](double sample) { return static_cast<float>(sample); });
        }
        first += io.frames;
    }
    require(session.clips()->readErrors() == 0 && session.clips()->underrunSamples() == 0,
            "latency render has no read errors or underruns");
    return channels;
}

std::int64_t peakNear(std::span<const float> samples, std::int64_t expected, std::int64_t radius) {
    const auto begin = samples.begin() + std::max<std::int64_t>(0, expected - radius);
    const auto end = samples.begin() + std::min<std::int64_t>(static_cast<std::int64_t>(samples.size()),
                                                             expected + radius + 1);
    const auto peak = std::max_element(begin, end);
    require(peak != end && *peak > 0.15f, "latency landmark remains audible");
    return peak - samples.begin();
}

double rms(std::span<const float> window) {
    double power = 0;
    for (const float value : window) power += static_cast<double>(value) * value;
    return std::sqrt(power / static_cast<double>(window.size()));
}

void checkAlignment(const Channels& audio, std::int64_t frame, std::int32_t latency,
                    std::int64_t tolerance, const char* label) {
    const auto expected = frame + latency;
    const auto dry = peakNear(audio[1], expected, 1024);
    const auto complex = peakNear(audio[0], expected, 1024);
    const auto error = complex - dry;
    const auto pulseRms = [&](std::size_t channel, std::int64_t peak) {
        return rms(std::span(audio[channel]).subspan(static_cast<std::size_t>(peak - pulseHalfWidth),
                                                    2 * pulseHalfWidth + 1));
    };
    std::printf("%s: graph latency %d samples, dry offset %lld, Complex offset %lld (relative %.3f ms)"
                ", onset RMS dry %.6f / Complex %.6f\n",
                label, latency, static_cast<long long>(dry - frame), static_cast<long long>(complex - frame),
                1000.0 * static_cast<double>(error) / rate, pulseRms(1, dry), pulseRms(0, complex));
    require(dry == expected, "PDC delays the dry track by the reported latency exactly");
    require(std::abs(error) <= tolerance, "Complex and the dry pulse align at the master");
}

void runAlignment(Store& store, Precision precision, bool readAhead) {
    Session session;
    SessionSpec spec{2, rate, block};
    spec.precision = precision;
    spec.synchronousClipReads = !readAhead;
    session.graph().setFadeFrames(0);
    if (!session.load(store, {}, spec)) throw std::runtime_error(session.error());
    const auto* graph = session.graph().currentGraph();
    require(graph != nullptr, "session has a master graph");
    const auto latency = graph->latencySamples();
    std::printf("Complex session graph: %d samples at %u Hz, %s, %s\n", latency, rate,
                precision == Precision::Float64 ? "64-bit" : "32-bit",
                readAhead ? "worker read-ahead" : "synchronous current-block reads");
    require(latency > 0, "real-time Complex declares its source delay to the graph");

    const auto landmark = static_cast<std::int64_t>(rampPulseFrame());
    constexpr auto rampTolerance = rate / 500; // 2 ms at the signal crest
    const auto fileIoBefore = audio::callbackFileIo.load();
    session.transport().locate(0);
    session.transport().play();
    const auto rolling = render(session, precision, landmark + latency + 4096);
    checkAlignment(rolling, earlyPulse, latency, 4, "Constant tempo after start");
    checkAlignment(rolling, landmark, latency, rampTolerance, "Inside tempo ramp while rolling");
    const auto silenceStart = static_cast<std::size_t>((earlyPulse + landmark) / 2 + latency - rate / 2);
    const auto silentRms = rms(std::span(rolling[0]).subspan(silenceStart, rate));
    std::printf("Complex silent middle second: RMS %.4g\n", silentRms);
    require(silentRms < 1e-6, "Complex preserves the silent middle second");

    session.transport().play(false);
    const auto stopped = render(session, precision, block);
    for (const auto& channel : stopped)
        require(std::all_of(channel.begin(), channel.end(), [](float sample) { return sample == 0; }),
                "stop leaves no stale Complex pulse");

    constexpr auto seekLead = 12000;
    session.transport().locate(landmark - seekLead);
    session.transport().play();
    const auto afterSeek = render(session, precision, seekLead + latency + 4096);
    checkAlignment(afterSeek, seekLead, latency, rampTolerance, "Inside tempo ramp after seek");
    require(audio::callbackFileIo.load() == fileIoBefore, "Complex source reads stay off the audio thread");
}
} // namespace

int main() {
    try {
        test::TempDirectory tmp{"realtime_warp_alignment", "project"};
        StoreError error{};
        auto store = Store::createEmpty(tmp.path() / "alignment.adi", error);
        if (!store) throw std::runtime_error(toString(error));
        arrange(*store);
        for (const auto precision : {Precision::Float32, Precision::Float64})
            for (const bool readAhead : {true, false})
                runAlignment(*store, precision, readAhead);
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::printf("FAIL: %s\n", error.what());
        return EXIT_FAILURE;
    }
}
