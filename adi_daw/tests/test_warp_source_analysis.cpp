// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/warp_analysis_settings.hpp"
#include "adi/audio/warp_source_analysis.hpp"
#include "adi/audio/decode.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/library/sample_analysis.hpp"
#include "adi/media/blake3.hpp"
#include "temp_directory.hpp"
#include <array>
#include <barrier>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <latch>
#include <thread>

namespace {
using namespace adi;
constexpr std::uint32_t rate = 48000;
int errors = 0;
void check(bool ok, const char* label) {
    if (!ok) { ++errors; std::printf("FAIL %s\n", label); }
}

void overlappingColdTiming(const std::filesystem::path& source, const std::string& hash,
                           const std::filesystem::path& analysisFolder) {
    std::array<audio::WavReader, 2> readers{audio::WavReader(source), audio::WavReader(source)};
    // Separate cache handles used by workers may address the same folder.
    media::AnalysisCache first(analysisFolder), second(analysisFolder);
    const std::array caches{&first, &second};
    std::array<std::optional<audio::WarpTimingAnalysis>, 2> results;
    std::array<std::uint32_t, 2> remaining{};
    std::barrier start(3);
    std::array<std::jthread, 2> workers;
    for (std::size_t index = 0; index < workers.size(); ++index) {
        readers[index].seek(readers[index].frames() - 1);
        workers[index] = std::jthread([&, index] {
            start.arrive_and_wait();
            results[index] = audio::analyzeWarpTiming(readers[index], hash, {4, 68}, {}, caches[index]);
            float last = 0;
            remaining[index] = readers[index].read(&last, 1);
        });
    }
    start.arrive_and_wait();
    for (auto& worker : workers) worker.join();
    check(results[0] && results[1] && !results[0]->beats.empty() &&
        results[0]->beats == results[1]->beats && results[0]->downbeats == results[1]->downbeats &&
        results[0]->onsets == results[1]->onsets, "concurrent preparations observe one identical timing result");
    check(remaining[0] + remaining[1] == 1, "exactly one cold preparation consumes PCM");
    std::printf("Concurrent cold timing reads %u PCM pass(es)\n", 2u - remaining[0] - remaining[1]);
}

void timingFlightLifecycle(const std::filesystem::path& source, const std::string& hash,
                           const std::filesystem::path& folder) {
    const audio::WarpTimingSettings settings{4, 68};
    media::AnalysisCache cache(folder / "leased"), alias(folder / "leased" / "");
    auto first = audio::acquireWarpTimingFlight(cache, hash, settings);
    check(first.has_value(), "a new content and configuration can be leased");
    if (!first) return;
    check(!audio::acquireWarpTimingFlight(alias, hash, settings),
          "recursive same-thread acquisition refuses across equivalent cache handles");

    const auto different = audio::acquireWarpTimingFlight(cache, hash, {4, 69});
    media::AnalysisCache other(folder / "elsewhere");
    const auto independent = audio::acquireWarpTimingFlight(other, hash, settings);
    check(different && independent, "different cache folders and analyser configurations remain independent");

    audio::DecodeCache decoded(folder / "decoded");
    library::SampleAnalysisService service(alias, decoded);
    check(service.analyze(source, hash, {}, settings).problem == library::SampleAnalysisProblem::Read,
          "library analysis and native Warp use the same acquisition boundary");

    std::latch started(1);
    bool leftReaderUntouched = false;
    std::jthread waiter([&](std::stop_token stop) {
        audio::WavReader reader(source);
        reader.seek(reader.frames() - 1);
        started.count_down();
        const auto outcome = audio::analyzeWarpTiming(reader, hash, settings, stop, &alias);
        float last = 0;
        leftReaderUntouched = !outcome && reader.read(&last, 1) == 1;
    });
    started.wait();
    waiter.request_stop();
    waiter.join();
    check(leftReaderUntouched, "cancelling a waiter releases it before reading source PCM");
    first.reset(); // Simulate a producer that never published its analysis.
    check(audio::acquireWarpTimingFlight(alias, hash, settings).has_value(),
          "a cancelled or failed publication leaves its key available to retry");

    media::AnalysisCache disabled(std::filesystem::path{});
    const auto uncached = audio::acquireWarpTimingFlight(disabled, hash, settings);
    check(uncached && audio::acquireWarpTimingFlight(disabled, hash, settings),
          "disabled persistence allows uncached processing");
    const audio::CallbackScope callback;
    check(!audio::acquireWarpTimingFlight(cache, hash, settings), "callback acquisition refuses");
}

void timingWithUnwritableCache(const std::filesystem::path& folder) {
    const auto wav = folder / "brief.wav";
    const auto blocked = folder / "occupied";
    { std::ofstream regularFile(blocked); regularFile << "not a cache directory"; }
    std::vector<float> pcm(rate / 5);
    pcm[rate / 20] = 0.9f;
    {
        audio::WavWriter writer(wav, rate, 1, audio::WavFormat::Float32);
        writer.write(pcm.data(), static_cast<std::uint32_t>(pcm.size()));
        writer.close();
    }
    auto hash = media::blake3File(wav);
    media::AnalysisCache cache(blocked);
    audio::WavReader reader(wav);
    const auto first = audio::analyzeWarpTiming(reader, hash.hex, {4, 68}, {}, &cache);
    const auto second = audio::analyzeWarpTiming(reader, hash.hex, {4, 68}, {}, &cache);
    check(hash && first && second && !first->onsets.empty() && first->onsets == second->onsets &&
        !audio::readWarpTiming(cache, {hash.hex, rate, 1, reader.frames()}, {4, 68}),
        "a failed cache publication still returns timing and leaves the request able to retry");
}

void unaccentedGridKeepsPickup(const std::filesystem::path& source, const std::string& hash,
                               const std::filesystem::path& cacheFolder) {
    constexpr auto analysisRate = audio::kWarpAnalysisRate;
    audio::WavReader reader(source);
    media::AnalysisCache cache(cacheFolder);
    audio::WarpTimingAnalysis timing;
    timing.medianBpm = 120;
    timing.tempoRegions = {{0, 120}};
    for (std::uint64_t beat = 0; beat < 32; ++beat) {
        const auto frame = analysisRate / 20 + beat * (analysisRate / 2);
        timing.beats.push_back(frame);
        timing.onsets.push_back({frame, .5f, -12.f});
    }
    std::string error;
    check(audio::storeWarpTiming(cache, {hash, rate, 1, reader.frames()}, {4, 50}, timing, error) ==
        media::AnalysisCacheProblem::None, "store a steady pulse with no measured bar phase");
    audio::AutoWarpOptions options;
    options.useCachedBeatGrid = true;
    options.fromFirstBeat = true;
    const auto plan = audio::analyzeWarpSource(reader, hash, 0, reader.frames(), options, {}, &cache);
    check(plan && plan.trimFrames == 0 && plan.estimatedBpm == 120 && plan.markers.front() == WarpMarker{0, 0},
          "requested first downbeat keeps the full source when only a steady pulse was measured");
}
}

int main() {
    test::TempDirectory temp{"warp_timing", "source"};
    const auto source = temp.path() / "accented.wav";
    std::vector<float> pcm(16 * rate + rate / 250);
    const auto length = rate / 500;
    for (std::size_t beat = 0; beat < 32; ++beat)
        for (std::size_t i = 0; i < length; ++i) {
            const auto phase = static_cast<double>(i) / length;
            pcm[beat * (rate / 2) + rate / 20 + i] =
                static_cast<float>((1 - phase) * std::sin(phase * (beat % 4 == 0 ? 13 : 61)));
        }
    {
        audio::WavWriter writer(source, rate, 1, audio::WavFormat::Float32);
        writer.write(pcm.data(), static_cast<std::uint32_t>(pcm.size()));
        writer.close();
    }
    const auto hash = media::blake3File(source);
    media::AnalysisCache cache(temp.path() / "analysed");
    audio::WavReader reader(source);
    audio::AutoWarpOptions options;
    options.useCachedBeatGrid = true;
    options.transientSensitivity = audio::warpTransientSensitivity({{"editing.transientSensitivity", 68}});
    const auto cold = audio::analyzeWarpSource(reader, hash.hex, 0, reader.frames(), options, {}, &cache);
    const auto timing = audio::readWarpTiming(cache, {hash.hex, rate, 1, reader.frames()}, {4, 68});
    std::printf("WAV timing BPM %.4f, %zu beats, %zu downbeats, Warp error %d\n",
                timing ? timing->medianBpm.value_or(0) : 0, timing ? timing->beats.size() : 0,
                timing ? timing->downbeats.size() : 0, static_cast<int>(cold.error));
    check(hash && cold && timing && timing->medianBpm && std::abs(*timing->medianBpm - 120) < 2 &&
          !timing->downbeats.empty(), "real source produces and caches measured beats and downbeats");
    if (timing) {
        const auto hits = audio::warpOnsetsInWindow(*timing, rate, 0, reader.frames());
        check(!hits.empty() && cold && cold.markers.front() == WarpMarker{0, 0} &&
              audio::writeWarpMap(cold.markers, reader.frames() - cold.trimFrames),
              "full-source analysis captures onset positions and a canonical 1.1.1 map");
    }

    reader.seek(reader.frames() - 1);
    const auto warm = audio::analyzeWarpSource(reader, hash.hex, 0, reader.frames(), options, {}, &cache);
    float last = 0;
    check(warm && warm.markers == cold.markers && reader.read(&last, 1) == 1,
          "the second preparation returns the same captured bytes without reading PCM");
    check(!audio::readWarpTiming(cache, {hash.hex, rate, 1, reader.frames()}, {4, 69}),
          "a changed transient-sensitivity config is a real miss");
    overlappingColdTiming(source, hash.hex, temp.path() / "concurrent");
    timingFlightLifecycle(source, hash.hex, temp.path());
    timingWithUnwritableCache(temp.path());
    unaccentedGridKeepsPickup(source, hash.hex, temp.path() / "unaccented");
    return errors ? 1 : 0;
}
