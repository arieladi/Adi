// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/wav_file.hpp"
#include "adi/measure/loopback.hpp"
#include "adi/measure/residual.hpp"
#include "adi/media/publish_file.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
namespace fs = std::filesystem;
using adi::audio::WavReader;
using adi::measure::LoopbackProbe;
using adi::measure::LoopbackStatus;
using nlohmann::json;
constexpr std::uint32_t blockFrames = 4096;

template<class Number> Number number(std::string_view text, std::string_view name) {
    Number value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        throw std::invalid_argument("invalid " + std::string(name));
    }
    return value;
}

struct Stage {
    fs::path directory, file;
    ~Stage() {
        if (directory.empty()) return;
        std::error_code ignored;
        fs::remove(file, ignored);
        fs::remove(directory, ignored);
    }
};

int generate(const fs::path& selectedPath, std::uint32_t sampleRate, unsigned periods) {
    if (sampleRate == 0 || sampleRate > 768000 || periods == 0 || periods > 64) {
        throw std::invalid_argument("expected 1–64 periods and a sample rate through 768000 Hz");
    }
    const auto target = fs::absolute(selectedPath).lexically_normal();
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    Stage stage;
    for (unsigned attempt = 0; attempt < 128; ++attempt) {
        auto directory = target.parent_path() / (".adi-measure-" + std::to_string(stamp) + "-" + std::to_string(attempt));
        auto file = directory / "probe.wav";
        std::error_code error;
        if (fs::create_directory(directory, error)) {
            stage.directory = std::move(directory);
            stage.file = std::move(file);
            break;
        }
        if (error && error != std::errc::file_exists) throw std::system_error(error, "reserve probe stage");
    }
    if (stage.directory.empty()) throw std::runtime_error("cannot reserve the measurement probe stage");
    fs::permissions(stage.directory, fs::perms::owner_all, fs::perm_options::replace);

    const std::uint64_t frames = periods * LoopbackProbe::period;
    const LoopbackProbe probe;
    std::array<float, blockFrames> block{};
    {
        adi::audio::WavWriter wav(stage.file, sampleRate, 1, adi::audio::WavFormat::Float32);
        for (std::uint64_t frame = 0; frame < frames; frame += blockFrames) {
            probe.render(frame, block);
            wav.write(block.data(), blockFrames);
        }
        wav.close();
    }
    const auto result = adi::media::publishFile(stage.file, target);
    if (result.status != adi::media::PublishStatus::published) {
        throw std::runtime_error(result.status == adi::media::PublishStatus::exists ? "probe destination exists" : result.error);
    }
    std::cout << json{{"sampleRate", sampleRate}, {"frames", frames}, {"warning", result.warning}}.dump() << '\n';
    return 0;
}

std::string_view statusName(LoopbackStatus status) {
    switch (status) {
        case LoopbackStatus::Measured: return "measured";
        case LoopbackStatus::InvalidInput: return "invalid_input";
        case LoopbackStatus::SignalTooLow: return "signal_too_low";
        case LoopbackStatus::Incoherent: return "incoherent";
        case LoopbackStatus::OutsideRange: return "outside_range";
    }
    return "unknown";
}

int latency(const fs::path& capture, std::uint64_t firstFrame, double maximum, unsigned channel) {
    WavReader wav(capture);
    if (channel == 0 || channel > wav.channels()) throw std::invalid_argument("capture channel is out of range");
    if (firstFrame > wav.frames() || wav.frames() - firstFrame < LoopbackProbe::period) {
        throw std::invalid_argument("capture has no complete probe period at the selected frame");
    }
    std::vector<float> block(static_cast<std::size_t>(blockFrames) * wav.channels());
    std::vector<float> mono(LoopbackProbe::period);
    wav.seek(firstFrame);
    for (std::size_t begin = 0; begin < mono.size(); begin += blockFrames) {
        if (wav.read(block.data(), blockFrames) != blockFrames) throw std::runtime_error("short probe input");
        for (std::size_t i = 0; i < blockFrames; ++i) {
            mono[begin + i] = block[i * wav.channels() + channel - 1];
        }
    }
    const auto result = LoopbackProbe{}.resolve(mono, firstFrame, maximum);
    std::cout << json{{"status", statusName(result.status)}, {"sampleRate", wav.sampleRate()},
                      {"windowStart", firstFrame}, {"channel", channel},
                      {"samples", result ? json(result.samples) : json(nullptr)},
                      {"inverted", result.inverted}, {"pilotGain", result.pilotGain},
                      {"coherence", result.coherence}}.dump() << '\n';
    return result ? 0 : 1;
}

int compare(const fs::path& expectedPath, const fs::path& actualPath) {
    WavReader expected(expectedPath), actual(actualPath);
    if (expected.channels() != actual.channels() || expected.sampleRate() != actual.sampleRate() ||
        expected.frames() != actual.frames() || expected.frames() == 0) {
        throw std::invalid_argument("comparison needs equal, nonempty frame, rate and channel dimensions");
    }
    std::vector<float> reference(static_cast<std::size_t>(blockFrames) * expected.channels());
    std::vector<float> recorded(reference.size());
    adi::measure::ResidualAccumulator samples;
    for (std::uint64_t left = expected.frames(); left > 0;) {
        const auto frames = static_cast<std::uint32_t>(std::min<std::uint64_t>(blockFrames, left));
        if (expected.read(reference.data(), frames) != frames || actual.read(recorded.data(), frames) != frames) {
            throw std::runtime_error("short comparison input");
        }
        const auto count = static_cast<std::size_t>(frames) * expected.channels();
        if (!samples.add(std::span(reference).first(count), std::span(recorded).first(count))) {
            throw std::runtime_error("invalid comparison samples");
        }
        left -= frames;
    }
    const auto result = samples.finish().value();
    std::cout << json{{"sampleCount", result.sampleCount}, {"referenceRms", result.referenceRms},
                      {"actualRms", result.actualRms}, {"residualRms", result.residualRms},
                      {"peakResidual", result.peakResidual}, {"bitExact", result.bitExact},
                      {"errorSignalDb", result.errorSignalDb ? json(*result.errorSignalDb) : json(nullptr)}}.dump() << '\n';
    return 0;
}
}

int main(int argc, char** argv) {
    try {
        if (argc >= 2) {
            const std::string_view command(argv[1]);
            if (command == "gen" && (argc == 5 || argc == 6) && std::string_view(argv[2]) == "loopback") {
                return generate(argv[3], number<std::uint32_t>(argv[4], "sample rate"),
                                argc == 6 ? number<unsigned>(argv[5], "period count") : 2U);
            }
            if (command == "latency" && (argc == 5 || argc == 6)) {
                return latency(argv[2], number<std::uint64_t>(argv[3], "starting frame"),
                               number<double>(argv[4], "maximum delay in samples"),
                               argc == 6 ? number<unsigned>(argv[5], "channel") : 1U);
            }
            if (command == "compare" && argc == 4) return compare(argv[2], argv[3]);
        }
    } catch (const std::exception& error) {
        std::cerr << "adi_measure: " << error.what() << '\n';
        return 1;
    }
    std::cerr << "adi_measure gen loopback <output.wav> <sample-rate> [periods]\n"
                 "adi_measure latency <capture.wav> <starting-frame> <maximum-samples> [one-based-channel]\n"
                 "adi_measure compare <expected.wav> <actual.wav>\n";
    return 2;
}
