// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/warp_analysis_settings.hpp"
#include "adi/audio/warp_source_analysis.hpp"
#include "adi/media/blake3.hpp"
#include "temp_directory.hpp"
#include <cmath>
#include <cstdio>

namespace {
using namespace adi;
constexpr std::uint32_t rate = 48000;
int errors = 0;
void check(bool ok, const char* label) {
    if (!ok) { ++errors; std::printf("FAIL %s\n", label); }
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
    return errors ? 1 : 0;
}
