// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/wav_file.hpp"
#include "adi/measure/loopback.hpp"
#include "adi/measure/residual.hpp"
#include "adi/measure/sync_spike.hpp"
#include "adi/material_commands.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/media/publish_file.hpp"
#include "adi/ops.hpp"
#include "adi/render/queue.hpp"
#include "adi/store.hpp"

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
using adi::measure::SyncSpikeStatus;
using nlohmann::json;
constexpr std::uint32_t blockFrames = 4096;
constexpr std::uint64_t maximumSyncFrames = 1'048'576;

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
        fs::remove_all(directory, ignored);
    }
};

void reserve(Stage& stage, const fs::path& target, const char* basename) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned attempt = 0; attempt < 128; ++attempt) {
        auto directory = target.parent_path() / (".adi-measure-" + std::to_string(stamp) + "-" + std::to_string(attempt));
        auto file = directory / basename;
        std::error_code error;
        if (fs::create_directory(directory, error)) {
            stage.directory = std::move(directory);
            stage.file = std::move(file);
            break;
        }
        if (error && error != std::errc::file_exists) throw std::system_error(error, "reserve measurement stage");
    }
    if (stage.directory.empty()) throw std::runtime_error("cannot reserve the measurement stage");
    fs::permissions(stage.directory, fs::perms::owner_all, fs::perm_options::replace);
}

template<class Signal> int writeProbe(const fs::path& selectedPath, std::uint32_t sampleRate,
                                      std::uint64_t frames, const Signal& signal) {
    if (sampleRate == 0 || sampleRate > 768000) throw std::invalid_argument("invalid probe sample rate");
    const auto target = fs::absolute(selectedPath).lexically_normal();
    Stage stage;
    reserve(stage, target, "probe.wav");

    std::array<float, blockFrames> block{};
    {
        adi::audio::WavWriter wav(stage.file, sampleRate, 1, adi::audio::WavFormat::Float32);
        for (std::uint64_t frame = 0; frame < frames; frame += blockFrames) {
            const auto count = static_cast<std::uint32_t>(std::min<std::uint64_t>(blockFrames, frames - frame));
            signal(frame, std::span(block).first(count));
            wav.write(block.data(), count);
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

int generate(const fs::path& path, std::uint32_t sampleRate, unsigned periods) {
    if (periods == 0 || periods > 64) throw std::invalid_argument("expected 1–64 loopback periods");
    const LoopbackProbe probe;
    return writeProbe(path, sampleRate, periods * LoopbackProbe::period,
                      [&](std::uint64_t frame, std::span<float> output) { probe.render(frame, output); });
}

int generateSync(const fs::path& path, std::uint32_t sampleRate, std::uint64_t frames,
                 std::uint64_t spike, std::uint64_t seed) {
    if (frames == 0 || frames > maximumSyncFrames || spike >= frames) {
        throw std::invalid_argument("sync probe needs 1–1048576 frames and a marker inside the probe");
    }
    return writeProbe(path, sampleRate, frames, [&](std::uint64_t frame, std::span<float> output) {
        adi::measure::renderSyncSpike(frame, output, spike, seed);
    });
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

std::vector<float> readChannel(WavReader& wav, std::uint64_t firstFrame, std::size_t frames, unsigned channel) {
    if (channel == 0 || channel > wav.channels()) throw std::invalid_argument("capture channel is out of range");
    std::vector<float> block(static_cast<std::size_t>(blockFrames) * wav.channels());
    std::vector<float> mono(frames);
    wav.seek(firstFrame);
    for (std::size_t begin = 0; begin < mono.size(); begin += blockFrames) {
        const auto count = static_cast<std::uint32_t>(std::min<std::size_t>(blockFrames, mono.size() - begin));
        if (wav.read(block.data(), count) != count) throw std::runtime_error("short probe input");
        for (std::size_t i = 0; i < count; ++i) {
            mono[begin + i] = block[i * wav.channels() + channel - 1];
        }
    }
    return mono;
}

int latency(const fs::path& capture, std::uint64_t firstFrame, double maximum, unsigned channel) {
    WavReader wav(capture);
    if (firstFrame > wav.frames() || wav.frames() - firstFrame < LoopbackProbe::period) {
        throw std::invalid_argument("capture has no complete probe period at the selected frame");
    }
    const auto mono = readChannel(wav, firstFrame, LoopbackProbe::period, channel);
    const auto result = LoopbackProbe{}.resolve(mono, firstFrame, maximum);
    std::cout << json{{"status", statusName(result.status)}, {"sampleRate", wav.sampleRate()},
                      {"windowStart", firstFrame}, {"channel", channel},
                      {"samples", result ? json(result.samples) : json(nullptr)},
                      {"inverted", result.inverted}, {"pilotGain", result.pilotGain},
                      {"coherence", result.coherence}}.dump() << '\n';
    return result ? 0 : 1;
}

std::string_view statusName(SyncSpikeStatus status) {
    switch (status) {
        case SyncSpikeStatus::Measured: return "measured";
        case SyncSpikeStatus::SignalTooLow: return "signal_too_low";
        case SyncSpikeStatus::Ambiguous: return "ambiguous";
        case SyncSpikeStatus::InvalidInput: return "invalid_input";
    }
    return "unknown";
}

int sync(const fs::path& capture, std::uint64_t firstFrame, std::uint64_t frames, unsigned channel) {
    WavReader wav(capture);
    if (frames == 0 || frames > maximumSyncFrames || firstFrame > wav.frames() || frames > wav.frames() - firstFrame) {
        throw std::invalid_argument("capture has no complete sync window at the selected frames");
    }
    const auto mono = readChannel(wav, firstFrame, static_cast<std::size_t>(frames), channel);
    const auto result = adi::measure::findSyncSpike(mono);
    std::cout << json{{"status", statusName(result.status)}, {"sampleRate", wav.sampleRate()},
                      {"channel", channel}, {"frame", result ? json(firstFrame + result.frame) : json(nullptr)},
                      {"value", result ? json(result.value) : json(nullptr)}}.dump() << '\n';
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

adi::OpRequest operation(const char* name, adi::Payload fields) {
    adi::OpRequest request;
    request.opType = name;
    request.payload = std::move(fields);
    request.label = "Build Measurement Fixture";
    return request;
}

int build(const fs::path& selectedProject, const fs::path& selectedAudio) {
    const auto destination = fs::absolute(selectedProject).lexically_normal();
    const auto source = fs::absolute(selectedAudio).lexically_normal();
    std::uint64_t frames = 0;
    std::uint32_t sampleRate = 0;
    {
        WavReader wave(source);
        frames = wave.frames();
        sampleRate = wave.sampleRate();
    }
    if (frames == 0) throw std::invalid_argument("measurement source has no audio frames");

    Stage stage;
    reserve(stage, destination, "fixture.adi");
    adi::StoreError error{};
    auto store = adi::Store::create(stage.file, error);
    if (!store) throw std::runtime_error(adi::toString(error));
    constexpr std::int64_t track = 2;
    const std::array requests{
        operation("project.setName", {{"name", adi::media::pathUtf8(destination.stem())}}),
        operation("track.create", {{"id", track}, {"kind", "audio"}, {"name", "Measurement Source"}, {"index", 0}}),
        operation("track.create", {{"id", 1}, {"kind", "master"}, {"name", "Main"}, {"index", 1}})
    };
    const auto tracks = adi::OpJournal(*store).commit(requests);
    if (!tracks.ok) throw std::runtime_error(tracks.error);

    adi::clip_edit::ImportOptions options;
    options.source = source;
    options.track = track;
    options.clipId = 1;
    options.mediaId = 1;
    options.at = {1, 0};
    auto prepared = adi::clip_edit::prepareImportAudio(*store, options, sampleRate);
    if (!prepared.edit.ok) throw std::runtime_error(prepared.edit.error);
    const auto imported = adi::clip_edit::commitMaterial(*store, prepared);
    if (!imported.ok) throw std::runtime_error(imported.error);
    const auto head = adi::OpJournal(*store).headSeq().value_or(0);
    const auto saved = store->saveCopy(destination);
    if (!saved.ok) throw std::runtime_error(saved.error);
    std::cout << json{{"sourceTrack", track}, {"frames", frames}, {"sourceRate", sampleRate},
                      {"journalHead", head}, {"warning", saved.warning}}.dump() << '\n';
    return 0;
}

int render(const fs::path& project, std::int64_t track, std::int64_t from, std::int64_t to,
           const fs::path& selectedOutput, std::uint32_t rate, std::int32_t block, int precision) {
    if (track < 0 || (precision != 32 && precision != 64)) {
        throw std::invalid_argument("track must be nonnegative and precision must be 32 or 64");
    }
    const auto output = fs::absolute(selectedOutput).lexically_normal();
    adi::render::Job job;
    job.project = fs::absolute(project).lexically_normal();
    job.directory = output.parent_path();
    job.exactWavFilename = adi::media::pathUtf8(output.filename());
    job.source.trackId = track;
    job.beginSample = from;
    job.endSample = to;
    job.session.sampleRate = rate;
    job.session.maxFrames = block;
    job.session.precision = precision == 64 ? adi::engine::Precision::Float64 : adi::engine::Precision::Float32;
    job.encoding.format = adi::audio::WavFormat::Float32;
    job.encoding.dither = adi::audio::WavDither::None;
    const auto result = adi::render::renderWav(std::move(job), {});
    if (result.state != adi::render::JobState::Completed || !result.snapshot) {
        throw std::runtime_error(result.error.empty() ? "measurement render failed" : result.error);
    }
    std::cout << json{{"frames", result.framesWritten}, {"track", result.snapshot->source.trackId},
                      {"sampleRate", result.snapshot->sampleRate}, {"journalHead", result.snapshot->journalHead},
                      {"warning", result.publication ? result.publication->warning : std::string{}}}.dump() << '\n';
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
            if (command == "gen" && argc == 8 && std::string_view(argv[2]) == "sync") {
                return generateSync(argv[3], number<std::uint32_t>(argv[4], "sample rate"),
                    number<std::uint64_t>(argv[5], "frames"), number<std::uint64_t>(argv[6], "spike frame"),
                    number<std::uint64_t>(argv[7], "noise seed"));
            }
            if (command == "latency" && (argc == 5 || argc == 6)) {
                return latency(argv[2], number<std::uint64_t>(argv[3], "starting frame"),
                               number<double>(argv[4], "maximum delay in samples"),
                               argc == 6 ? number<unsigned>(argv[5], "channel") : 1U);
            }
            if (command == "sync" && (argc == 5 || argc == 6)) {
                return sync(argv[2], number<std::uint64_t>(argv[3], "starting frame"),
                    number<std::uint64_t>(argv[4], "frames"), argc == 6 ? number<unsigned>(argv[5], "channel") : 1U);
            }
            if (command == "compare" && argc == 4) return compare(argv[2], argv[3]);
            if (command == "build" && argc == 4) return build(argv[2], argv[3]);
            if (command == "render" && (argc == 9 || argc == 10)) {
                return render(argv[2], number<std::int64_t>(argv[3], "track"),
                    number<std::int64_t>(argv[4], "start sample"), number<std::int64_t>(argv[5], "end sample"),
                    argv[6], number<std::uint32_t>(argv[7], "sample rate"), number<std::int32_t>(argv[8], "block size"),
                    argc == 10 ? number<int>(argv[9], "precision") : 32);
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "adi_measure: " << error.what() << '\n';
        return 1;
    }
    std::cerr << "adi_measure gen loopback <output.wav> <sample-rate> [periods]\n"
                 "adi_measure gen sync <output.wav> <sample-rate> <frames> <spike-frame> <seed>\n"
                 "adi_measure latency <capture.wav> <starting-frame> <maximum-samples> [one-based-channel]\n"
                 "adi_measure sync <capture.wav> <starting-frame> <frames> [one-based-channel]\n"
                 "adi_measure compare <expected.wav> <actual.wav>\n"
                 "adi_measure build <fixture.adi> <source.wav>\n"
                 "adi_measure render <fixture.adi> <track-or-0-for-Main> <start> <exclusive-end> <output.wav> "
                 "<sample-rate> <block-size> [32-or-64]\n";
    return 2;
}
