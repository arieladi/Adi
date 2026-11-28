// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/auto_warp.hpp"
#include "adi/audio/spectral_onsets.hpp"
#include "adi/audio/transients.hpp"
#include "adi/audio/warp_beat_candidate.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/textproj.hpp"

#include <algorithm>
#include <charconv>
#include <iomanip>
#include <iostream>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {
using namespace adi::audio;

void emit(std::string_view method, std::string_view field, double value) {
    std::cout << method << '\t' << field << '\t' << value << '\n';
}

void reportBaseline(WavReader& reader) {
    const auto rate = reader.sampleRate();
    const auto onset = detectTransients(reader, 0, reader.frames());
    if (!onset) throw std::runtime_error("could not run baseline transient detector");
    for (auto frame : onset.frames) emit("baseline", "onset", static_cast<double>(frame) / rate);
    const auto fitted = autoWarpFromOnsets(onset.frames, reader.frames(), rate);
    if (!fitted) return;
    emit("baseline", "bpm", fitted.estimatedBpm);
    for (auto it = fitted.markers.begin(); it != fitted.markers.end() - 1; ++it) {
        if (it->tick % adi::textproj::kWhole == 0)
            emit("baseline", "downbeat", (static_cast<double>(it->sourceFrame) +
                static_cast<double>(fitted.trimFrames)) / rate);
    }
    std::size_t right = 1;
    for (std::int64_t tick = 0; tick < fitted.markers.back().tick; tick += adi::textproj::kPPQ) {
        while (fitted.markers[right].tick < tick) ++right;
        const auto& a = fitted.markers[right - 1];
        const auto& b = fitted.markers[right];
        const double fraction = static_cast<double>(tick - a.tick) / static_cast<double>(b.tick - a.tick);
        const double frame = static_cast<double>(a.sourceFrame) + fraction * static_cast<double>(b.sourceFrame - a.sourceFrame);
        emit("baseline", "beat", (frame + static_cast<double>(fitted.trimFrames)) / rate);
    }
}

void reportCandidate(WavReader& reader, WarpBeatCandidateAnalyzer& analyzer) {
    reader.seek(0);
    constexpr std::uint32_t blockFrames = 4096;
    const auto channels = reader.channels();
    WarpAnalysisPcm pcm(reader.sampleRate(), channels, [&](std::span<const float> samples) {
        return analyzer.append(samples);
    });
    SpectralOnsetAnalyzer spectral(reader.sampleRate(), channels);
    std::vector<float> buffer(static_cast<std::size_t>(blockFrames) * channels);
    for (auto remaining = reader.frames(); remaining != 0;) {
        const auto count = static_cast<std::uint32_t>(std::min<std::uint64_t>(remaining, blockFrames));
        if (reader.read(buffer.data(), count) != count) throw std::runtime_error("could not read candidate PCM");
        const auto samples = std::span(buffer).first(static_cast<std::size_t>(count) * channels);
        if (!pcm.append(samples) || !spectral.append(samples)) throw std::runtime_error("candidate rejected PCM");
        remaining -= count;
    }
    if (!pcm.finish()) throw std::runtime_error("could not finish candidate resampling");
    const auto onsets = spectral.finish();
    if (!onsets) throw std::runtime_error("could not finish spectral onset analysis");
    for (const auto& onset : onsets.onsets)
        emit("candidate", "onset", static_cast<double>(onset.analysisFrame) / kWarpAnalysisRate);
    const auto result = analyzer.finish();
    if (result.error == WarpBeatCandidateError::NoBeat) return;
    if (!result) throw std::runtime_error("candidate analysis failed");
    emit("candidate", "bpm", result.medianBpm);
    for (auto frame : result.beats) emit("candidate", "beat", static_cast<double>(frame) / kWarpAnalysisRate);
    for (auto index : result.downbeats)
        emit("candidate", "downbeat", static_cast<double>(result.beats[index]) / kWarpAnalysisRate);
}

unsigned int meterNumber(std::string_view raw) {
    unsigned int value{};
    const auto parsed = std::from_chars(raw.data(), raw.data() + raw.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != raw.data() + raw.size())
        throw std::runtime_error("invalid numeric meter or tempo");
    return value;
}

double tempoNumber(std::string_view raw) {
    // Floating from_chars is unavailable on older macOS deployment targets.
    std::istringstream input{std::string(raw)};
    input.imbue(std::locale::classic());
    double value = 0;
    if (raw.empty() || raw.front() == '+' || raw.find_first_not_of("-+eE.0123456789") != raw.npos ||
        !(input >> std::noskipws >> value) || !input.eof())
        throw std::runtime_error("invalid numeric meter or tempo");
    return value;
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2 && argc != 4 && argc != 5) {
        std::cerr << "Usage: adi_warp_bench AUDIO.wav [NUMERATOR DENOMINATOR [SOURCE_QUARTER_BPM]]\n";
        return 2;
    }
    try {
        std::cout << std::setprecision(12);
        WavReader reader(argv[1]);
        reportBaseline(reader);
        if (argc == 2) {
            WarpBeatCandidateAnalyzer analyzer;
            reportCandidate(reader, analyzer);
        } else {
            WarpBeatMeter meter{meterNumber(argv[2]), meterNumber(argv[3]), {}};
            if (argc == 5) meter.quarterBpm = tempoNumber(argv[4]);
            WarpBeatCandidateAnalyzer analyzer(meter);
            reportCandidate(reader, analyzer);
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
