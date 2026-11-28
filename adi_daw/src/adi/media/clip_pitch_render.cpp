// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/media/clip_pitch_render.hpp"

#include "adi/audio/decode.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/audio/pitch_compare.hpp"
#include "adi/audio/pitch_render.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/media/blake3.hpp"
#include "adi/media/clip_pitch.hpp"
#include "adi/pitch_edit_store.hpp"

#include <samplerate.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>
#else
#include <cerrno>
#include <sys/stat.h>
#endif

namespace adi::media {
namespace fs = std::filesystem;

PreparedClipPitch::PreparedClipPitch(fs::path path, std::uint32_t sampleRate, std::uint16_t channelCount,
                                     std::uint64_t frameCount)
    : file(path), rate(sampleRate), channels(channelCount), frames(frameCount),
      stageDirectory_(path.parent_path()), stageFile_(std::move(path)) {}

PreparedClipPitch::PreparedClipPitch(fs::path stage, std::shared_ptr<const audio::PreparedWarp> warp)
    : file(warp->file), rate(warp->rate), channels(warp->channels), frames(warp->frames),
      stageDirectory_(stage.parent_path()), stageFile_(std::move(stage)), warp_(std::move(warp)) {}

PreparedClipPitch::~PreparedClipPitch() {
    warp_.reset();
    std::error_code error;
    fs::remove(stageFile_, error);
    fs::remove(stageDirectory_, error);
}

struct ClipPitchPreparation {
    static std::shared_ptr<const PreparedClipPitch> stage(fs::path path, const audio::WavReader& source,
                                                          std::uint64_t frames) {
        return std::shared_ptr<const PreparedClipPitch>(new PreparedClipPitch(
            std::move(path), source.sampleRate(), source.channels(), frames));
    }
    static std::shared_ptr<const PreparedClipPitch> warped(fs::path stage, std::shared_ptr<const audio::PreparedWarp> warp) {
        return std::shared_ptr<const PreparedClipPitch>(new PreparedClipPitch(std::move(stage), std::move(warp)));
    }
};

namespace {

constexpr std::uint32_t blockFrames = 4096;
constexpr std::uint64_t maxSourceSamples = 64u * 1024 * 1024; // 256 MiB interleaved PCM

struct Failure { ClipPitchRenderProblem problem; const char* detail; };

ClipPitchRenderResult fail(ClipPitchRenderProblem problem, const char* detail = nullptr) {
    if (!detail) return {problem, {}, {}};
    try {
        return {problem, std::string(detail), {}};
    } catch (const std::bad_alloc&) {
        return {ClipPitchRenderProblem::Resource, {}, {}};
    }
}

void checkCancelled(std::stop_token stop) {
    if (stop.stop_requested()) throw Failure{ClipPitchRenderProblem::Cancelled, nullptr};
}

void verifySource(const fs::path& source, std::string_view expected, std::stop_token stop) {
    const auto actual = blake3File(source, stop);
    checkCancelled(stop);
    if (actual.error == HashError::resource) throw Failure{ClipPitchRenderProblem::Resource, nullptr};
    if (!actual) throw Failure{ClipPitchRenderProblem::Source, "cannot hash Pitch Editor source"};
    if (actual.hex != expected) throw Failure{ClipPitchRenderProblem::Source, "Pitch Editor source hash differs from saved edits"};
}

struct DecodePin {
    audio::DecodeCache& cache;
    const fs::path& file;
    bool pinned;
    ~DecodePin() { if (pinned) try { cache.release(file); } catch (...) {} }
};

bool createOwnerDirectory(const fs::path& path, std::error_code& error) {
#if defined(_WIN32)
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    // Protected DACL, inheritable full access granted to the owner only.
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;OICI;FA;;;OW)", SDDL_REVISION_1,
                                                                 &descriptor, nullptr)) {
        error = {static_cast<int>(::GetLastError()), std::system_category()};
        return false;
    }
    const std::unique_ptr<void, decltype(&::LocalFree)> release(descriptor, &::LocalFree);
    SECURITY_ATTRIBUTES security{static_cast<DWORD>(sizeof(SECURITY_ATTRIBUTES)), descriptor, FALSE};
    if (::CreateDirectoryW(path.c_str(), &security)) return true;
    error = {static_cast<int>(::GetLastError()), std::system_category()};
#else
    if (::mkdir(path.c_str(), 0700) == 0) return true;
    error = {errno, std::generic_category()};
#endif
    return false;
}

class PrivateStage {
public:
    explicit PrivateStage(const fs::path& folder) {
        fs::create_directories(folder);
        static std::atomic<std::uint64_t> serial{0};
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned attempt = 0; attempt < 128; ++attempt) {
            auto candidate = folder / ("pitch-" + std::to_string(now) + "-" + std::to_string(serial.fetch_add(1)));
            auto candidateFile = candidate / "prepared.wav";
            std::error_code error;
            if (createOwnerDirectory(candidate, error)) {
                directory = std::move(candidate);
                file = std::move(candidateFile);
                return;
            }
            if (error && error != std::errc::file_exists)
                throw fs::filesystem_error("cannot create private Pitch Editor stage", candidate, error);
        }
        throw Failure{ClipPitchRenderProblem::Io, "cannot reserve private Pitch Editor stage"};
    }
    ~PrivateStage() {
        if (keep) return;
        std::error_code error;
        fs::remove(file, error);
        fs::remove(directory, error);
    }
    fs::path directory, file;
    bool keep = false;
};

std::vector<float> readPcm(audio::WavReader& reader, std::stop_token stop) {
    const auto channels = reader.channels();
    const auto frames = static_cast<std::size_t>(reader.frames());
    std::vector<float> samples(frames * channels);
    for (std::size_t frame = 0; frame < frames;) {
        checkCancelled(stop);
        const auto count = static_cast<std::uint32_t>(std::min<std::size_t>(blockFrames, frames - frame));
        if (reader.read(samples.data() + frame * channels, count) != count)
            throw Failure{ClipPitchRenderProblem::Io, "short Pitch Editor source read"};
        frame += count;
    }
    return samples;
}

void downmix(std::span<const float> source, std::span<float> mono, std::uint16_t channels, std::stop_token stop) {
    for (std::size_t frame = 0; frame < mono.size(); ++frame) {
        if (frame % blockFrames == 0) checkCancelled(stop);
        double sum = 0;
        for (std::size_t channel = 0; channel < channels; ++channel) sum += source[frame * channels + channel];
        mono[frame] = static_cast<float>(sum / channels);
    }
}

std::unique_ptr<const std::vector<float>> renderChannels(std::span<const float> pcm, std::uint16_t channels,
                                                         const audio::PitchAnalysis& analysis,
                                                         const audio::PitchEditState& edits, std::stop_token stop) {
    auto rendered = audio::renderPitchEdits(pcm, channels, analysis, edits, stop);
    switch (rendered.error) {
    case audio::PitchRenderError::None: if (rendered) return std::move(rendered.samples); break;
    case audio::PitchRenderError::Cancelled: throw Failure{ClipPitchRenderProblem::Cancelled, nullptr};
    case audio::PitchRenderError::Resource: throw Failure{ClipPitchRenderProblem::Resource, nullptr};
    case audio::PitchRenderError::Unsupported: throw Failure{ClipPitchRenderProblem::Unsupported, "unsupported Pitch Editor rate or ratio"};
    case audio::PitchRenderError::InvalidEdits: throw Failure{ClipPitchRenderProblem::Edits, "invalid Pitch Editor edit state"};
    default: break;
    }
    throw Failure{ClipPitchRenderProblem::Dsp, "cannot render Pitch Editor note edits"};
}

void writeWindow(audio::WavWriter& writer, std::span<const float> samples, std::uint16_t channels,
                 bool reverse, std::stop_token stop) {
    const auto frames = samples.size() / channels;
    std::vector<float> reversed(reverse ? static_cast<std::size_t>(blockFrames) * channels : 0);
    for (std::size_t frame = 0; frame < frames;) {
        checkCancelled(stop);
        const auto count = static_cast<std::uint32_t>(std::min<std::size_t>(blockFrames, frames - frame));
        const auto* first = samples.data() + frame * channels;
        if (reverse) {
            for (std::uint32_t index = 0; index < count; ++index)
                std::copy_n(samples.data() + (frames - 1 - (frame + index)) * channels,
                            channels, reversed.data() + static_cast<std::size_t>(index) * channels);
            first = reversed.data();
        }
        writer.write(first, count);
        frame += count;
    }
    writer.close();
}

ClipPitchRenderResult prepareWindow(const ClipPitchRenderPlan& plan, std::span<const float> window,
                                    const audio::WavReader& reader, const std::optional<audio::WarpRenderPlan>& warp,
                                    std::stop_token stop) {
    checkCancelled(stop);
    const auto folder = plan.workFolder.empty() ? fs::temp_directory_path() / "adi-pitch" : plan.workFolder;
    PrivateStage stage(folder);
    {
        audio::WavWriter writer(stage.file, reader.sampleRate(), reader.channels(), audio::WavFormat::Float32);
        writeWindow(writer, window, reader.channels(), plan.reverse, stop);
    }
    checkCancelled(stop);
    if (warp) {
        // MIDI extraction uses the same directed window clock. The staged
        // frames already follow reverse, so the supplied Warp map stays intact.
        auto recipe = *warp;
        recipe.source = stage.file;
        recipe.workFolder = stage.directory;
        recipe.sourceStart = 0;
        recipe.sourceFrames = plan.sourceFrames;
        auto warped = audio::prepareWarp(recipe, stop);
        checkCancelled(stop);
        if (!warped) return fail(ClipPitchRenderProblem::Dsp, warped.detail.c_str());
        fs::remove(stage.file);
        auto prepared = ClipPitchPreparation::warped(stage.file, std::move(warped.prepared));
        stage.keep = true;
        return {ClipPitchRenderProblem::None, {}, std::move(prepared)};
    }
    auto prepared = ClipPitchPreparation::stage(stage.file, reader, plan.sourceFrames);
    stage.keep = true;
    return {ClipPitchRenderProblem::None, {}, std::move(prepared)};
}

struct ComparisonResampleInput {
    audio::WavReader& reader;
    std::stop_token stop;
    std::vector<float> samples;
    std::uint64_t remaining;
    std::uint32_t padding;
    bool failed = false;

    static long supply(void* opaque, float** buffer) noexcept {
        auto& input = *static_cast<ComparisonResampleInput*>(opaque);
        *buffer = input.samples.data();
        if (input.stop.stop_requested() || input.failed) return 0;
        try {
            const auto channels = input.reader.channels();
            if (input.remaining != 0) {
                const auto count = static_cast<std::uint32_t>(std::min<std::uint64_t>(input.remaining, blockFrames));
                if (input.reader.read(input.samples.data(), count) != count) {
                    input.failed = true;
                    return 0;
                }
                for (std::size_t index = 0; index < static_cast<std::size_t>(count) * channels; ++index)
                    if (!std::isfinite(input.samples[index])) input.samples[index] = 0.0f;
                input.remaining -= count;
                return static_cast<long>(count);
            }
            const auto count = std::min(input.padding, blockFrames);
            std::fill_n(input.samples.data(), static_cast<std::size_t>(count) * channels, 0.0f);
            input.padding -= count;
            return static_cast<long>(count);
        } catch (...) {
            input.failed = true;
            return 0;
        }
    }
};

struct ComparisonRateRatios {
    double first = 1, second = 1;
};

ComparisonRateRatios comparisonRateRatios(double ratio) {
    if (src_is_valid_ratio(ratio)) return {ratio, 1};
    // Match the paged clip converter. The minimum effective rate is 500 Hz,
    // so up to 1536x is needed. Supported clips downsample by at most 139.32x.
    constexpr double upsample = 128;
    const double second = ratio / upsample;
    if (ratio > 1 && src_is_valid_ratio(upsample) && src_is_valid_ratio(second)) return {upsample, second};
    throw Failure{ClipPitchRenderProblem::Unsupported, "Pitch Editor comparison sample-rate ratio is unsupported"};
}

using ComparisonResampler = std::unique_ptr<SRC_STATE, decltype(&src_delete)>;

ComparisonResampler comparisonResampler(src_callback_t supply, void* input, double ratio, std::uint16_t channels) {
    int error = 0;
    ComparisonResampler state(src_callback_new(supply, SRC_SINC_BEST_QUALITY, channels, &error, input), &src_delete);
    if (!state || src_set_ratio(state.get(), ratio) != 0)
        throw Failure{ClipPitchRenderProblem::Dsp, "cannot prepare Pitch Editor comparison resampler"};
    return state;
}

struct ComparisonResampleBridge {
    SRC_STATE* first;
    double ratio;
    std::vector<float> samples;

    static long supply(void* opaque, float** buffer) noexcept {
        auto& bridge = *static_cast<ComparisonResampleBridge*>(opaque);
        *buffer = bridge.samples.data();
        return src_callback_read(bridge.first, bridge.ratio, blockFrames, *buffer);
    }
};

std::vector<float> readComparisonSide(const PreparedClipPitch& prepared, ComparisonRateRatios ratios,
                                      std::uint64_t frames, std::stop_token stop) {
    audio::WavReader reader(prepared.file);
    if (reader.sampleRate() != prepared.rate || reader.channels() != prepared.channels || reader.frames() != prepared.frames)
        throw Failure{ClipPitchRenderProblem::Io, "prepared Pitch Editor comparison side changed"};
    if (ratios.first == 1) return readPcm(reader, stop);

    // Use the exact device/source ratio. Round only the number of requested
    // frames, so the fractional sample clock does not drift to meet an end
    // anchor. Zero extension keeps sinc context beyond the trimmed boundary.
    const auto channels = prepared.channels;
    const auto blockSamples = static_cast<std::size_t>(blockFrames) * channels;
    const auto padding = blockFrames * static_cast<std::uint32_t>(std::ceil(std::max(1.0, 1 / ratios.first)));
    ComparisonResampleInput input{reader, stop, std::vector<float>(blockSamples), prepared.frames, padding};
    auto first = comparisonResampler(&ComparisonResampleInput::supply, &input, ratios.first, channels);
    std::optional<ComparisonResampleBridge> bridge;
    ComparisonResampler second(nullptr, &src_delete);
    if (ratios.second != 1) {
        // Stream stage one's filter tail as well as its in-window samples.
        // Stage two then sees a continuous, zero-extended source clock.
        bridge.emplace(first.get(), ratios.first, std::vector<float>(blockSamples));
        second = comparisonResampler(&ComparisonResampleBridge::supply, &*bridge, ratios.second, channels);
    }
    auto* const state = second ? second.get() : first.get();
    const auto ratio = second ? ratios.second : ratios.first;
    std::vector<float> output(static_cast<std::size_t>(frames) * channels);
    for (std::uint64_t frame = 0; frame < frames;) {
        checkCancelled(stop);
        const auto count = static_cast<long>(std::min<std::uint64_t>(frames - frame, blockFrames));
        const auto read = src_callback_read(state, ratio, count, output.data() + static_cast<std::size_t>(frame) * channels);
        checkCancelled(stop);
        if (input.failed) throw Failure{ClipPitchRenderProblem::Io, "cannot read Pitch Editor comparison resample input"};
        if (read != count || (second && src_error(first.get()) != 0))
            throw Failure{ClipPitchRenderProblem::Dsp, "Pitch Editor comparison resampler ended early"};
        frame += static_cast<std::uint64_t>(count);
    }
    return output;
}

ClipPitchRenderResult preparePitch(const ClipPitchRenderPlan& plan, std::stop_token stop,
                                   audio::DecodeCache* decodeCache, AnalysisCache* analysisCache,
                                   std::shared_ptr<const audio::PitchComparePcm>* comparison = nullptr,
                                   std::uint32_t outputRate = 0) {
    if (audio::onAudioThread) return fail(ClipPitchRenderProblem::Realtime);
    if (stop.stop_requested()) return fail(ClipPitchRenderProblem::Cancelled);
    if (!plan.edits || (plan.edits->rows.empty() && !plan.edits->segmentation))
        return fail(ClipPitchRenderProblem::Edits, "no Pitch Editor edit set");
    const auto& saved = *plan.edits;
    if (plan.source.empty() || plan.sourceFrames == 0 || plan.sourceStart > saved.sourceFrames ||
        plan.sourceFrames > saved.sourceFrames - plan.sourceStart)
        return fail(ClipPitchRenderProblem::Source, "Pitch Editor playback window exceeds source");
    if ((outputRate && (outputRate < 44100 || outputRate > 768000)) ||
        (outputRate && plan.warp && outputRate != plan.warp->outputRate))
        return fail(ClipPitchRenderProblem::Unsupported, "Pitch Editor comparison rate differs from the Warp clock or device range");
    if (saved.analysisVersion != kPitchAnalysisVersion || saved.analysisConfiguration != kPitchAnalysisConfiguration)
        return fail(ClipPitchRenderProblem::Edits, "Pitch Editor edit analysis version is unavailable");
    try {
        verifySource(plan.source, saved.sourceHash, stop);
        auto& decoder = decodeCache ? *decodeCache : audio::defaultDecodeCache();
        const auto playable = decoder.playable(plan.source, stop);
        const DecodePin pin{decoder, playable, playable != plan.source};
        audio::WavReader reader(playable);
        const auto channels = reader.channels();
        const auto rate = reader.sampleRate();
        const auto frames = reader.frames();
        if (channels == 0 || channels > audio::kMaxDecodeChannels || rate == 0 || rate != saved.sampleRate || frames != saved.sourceFrames)
            return fail(ClipPitchRenderProblem::Source, "Pitch Editor source dimensions differ from saved edits");
        if (frames > maxSourceSamples / channels)
            return fail(ClipPitchRenderProblem::Resource, "Pitch Editor source exceeds render PCM budget");

        std::uint32_t comparisonRate = 0;
        std::uint64_t comparisonFrames = 0;
        ComparisonRateRatios ratios;
        if (comparison) {
            const auto playbackRate = !plan.warp && plan.playbackRate ? plan.playbackRate : rate;
            comparisonRate = plan.warp ? plan.warp->outputRate : outputRate ? outputRate : playbackRate;
            comparisonFrames = plan.warp && !plan.warp->anchors.empty() ? plan.warp->anchors.back().outputFrame :
                (plan.sourceFrames * comparisonRate + playbackRate / 2) / playbackRate;
            if (!plan.warp) ratios = comparisonRateRatios(static_cast<double>(comparisonRate) / playbackRate);
            if (comparisonFrames == 0) return fail(ClipPitchRenderProblem::Source, "Pitch Editor comparison has no output frames");
            if (comparisonFrames > audio::kPitchCompareMaxResidentSamples / (2 * static_cast<std::uint64_t>(channels)))
                return fail(ClipPitchRenderProblem::Resource, "Pitch Editor comparison exceeds resident PCM budget");
        }

        auto pcm = readPcm(reader, stop);
        verifySource(plan.source, saved.sourceHash, stop);
        std::vector<float> mono;
        std::span<const float> analysisInput(pcm);
        if (channels > 1) {
            mono.resize(static_cast<std::size_t>(frames));
            downmix(pcm, mono, channels, stop);
            analysisInput = mono;
        }
        const AnalysisIdentity identity{saved.sourceHash, rate, channels, frames};
        auto analyzed = buildSourcePitchAnalysisFromMono(identity, analysisInput, analysisCache, stop);
        if (analyzed.error == audio::PitchAnalysisError::Cancelled)
            return fail(ClipPitchRenderProblem::Cancelled);
        if (analyzed.error == audio::PitchAnalysisError::Resource)
            return fail(ClipPitchRenderProblem::Resource);
        if (!analyzed) return fail(ClipPitchRenderProblem::Source, "cannot recover saved Pitch Editor analysis");
        const auto edits = clip_edit::resolveClipPitchEdits(saved, *analyzed.analysis, identity.hash,
                                                           kPitchAnalysisVersion, kPitchAnalysisConfiguration);
        if (!edits.state) return fail(ClipPitchRenderProblem::Edits, edits.error.c_str());
        const auto& effective = edits.adjustedAnalysis ? *edits.adjustedAnalysis : *analyzed.analysis;

        const auto first = static_cast<std::size_t>(plan.sourceStart);
        const auto count = static_cast<std::size_t>(plan.sourceFrames);
        ClipPitchRenderResult original;
        if (comparison) {
            original = prepareWindow(plan, std::span<const float>(pcm).subspan(first * channels, count * channels),
                                     reader, plan.warp, stop);
            if (!original) return original;
        }
        std::unique_ptr<const std::vector<float>> rendered;
        std::span<const float> window;
        if (channels <= 2) {
            // Counted construction can report iterator-proxy allocation failure in MSVC Debug.
            std::vector<float>(std::size_t{0}).swap(mono);
            rendered = renderChannels(pcm, channels, effective, *edits.state, stop);
            window = std::span(*rendered).subspan(first * channels, count * channels);
        } else {
            // Wider files retain the common note clock and independent channel rendering.
            for (std::size_t channel = 0; channel < channels; ++channel) {
                for (std::size_t frame = 0; frame < mono.size(); ++frame) {
                    if (frame % blockFrames == 0) checkCancelled(stop);
                    mono[frame] = pcm[frame * channels + channel];
                }
                const auto changed = renderChannels(mono, 1, effective, *edits.state, stop);
                for (std::size_t frame = first; frame < first + count; ++frame) {
                    if (frame % blockFrames == 0) checkCancelled(stop);
                    pcm[frame * channels + channel] = (*changed)[frame];
                }
            }
            window = std::span(pcm).subspan(first * channels, count * channels);
        }

        auto corrected = prepareWindow(plan, window, reader, plan.warp, stop);
        if (!comparison || !corrected) return corrected;
        if (original.prepared->rate != corrected.prepared->rate || original.prepared->channels != corrected.prepared->channels ||
            original.prepared->frames != corrected.prepared->frames)
            return fail(ClipPitchRenderProblem::Dsp, "Pitch Editor comparison clocks do not match");

        std::vector<float>(std::size_t{0}).swap(pcm);
        std::vector<float>(std::size_t{0}).swap(mono);
        rendered.reset();
        auto dry = readComparisonSide(*original.prepared, ratios, comparisonFrames, stop);
        auto edited = readComparisonSide(*corrected.prepared, ratios, comparisonFrames, stop);
        checkCancelled(stop);
        auto pair = audio::PitchComparePcm::prepare(comparisonRate, corrected.prepared->channels,
                                                     std::move(dry), std::move(edited), stop);
        checkCancelled(stop);
        if (!pair) return fail(ClipPitchRenderProblem::Dsp, "cannot assemble Pitch Editor comparison pair");
        *comparison = std::move(pair);
        return corrected;
    } catch (const Failure& failure) {
        return fail(failure.problem, failure.detail);
    } catch (const std::bad_alloc&) {
        return fail(ClipPitchRenderProblem::Resource);
    } catch (const std::length_error&) {
        return fail(ClipPitchRenderProblem::Resource);
    } catch (const audio::DecodeError& error) {
        return error.problem() == audio::DecodeProblem::Cancelled ? fail(ClipPitchRenderProblem::Cancelled) :
            fail(ClipPitchRenderProblem::Io, error.what());
    } catch (const std::exception& error) {
        return fail(ClipPitchRenderProblem::Io, error.what());
    }
}

} // namespace

ClipPitchRenderResult prepareClipPitchRender(const ClipPitchRenderPlan& plan, std::stop_token stop) {
    if (audio::onAudioThread) return fail(ClipPitchRenderProblem::Realtime);
    if (stop.stop_requested()) return fail(ClipPitchRenderProblem::Cancelled);
    return preparePitch(plan, stop, nullptr, &defaultAnalysisCache());
}

ClipPitchRenderResult prepareClipPitchRender(const ClipPitchRenderPlan& plan, std::stop_token stop,
                                              audio::DecodeCache* decodeCache, AnalysisCache* analysisCache) {
    return preparePitch(plan, stop, decodeCache, analysisCache);
}

ClipPitchCompareResult prepareClipPitchCompare(const ClipPitchRenderPlan& plan, std::stop_token stop,
                                               std::uint32_t outputRate) {
    if (audio::onAudioThread) return {ClipPitchRenderProblem::Realtime, {}, {}};
    if (stop.stop_requested()) return {ClipPitchRenderProblem::Cancelled, {}, {}};
    return prepareClipPitchCompare(plan, stop, nullptr, &defaultAnalysisCache(), outputRate);
}

ClipPitchCompareResult prepareClipPitchCompare(const ClipPitchRenderPlan& plan, std::stop_token stop,
                                               audio::DecodeCache* decodeCache, AnalysisCache* analysisCache,
                                               std::uint32_t outputRate) {
    std::shared_ptr<const audio::PitchComparePcm> pair;
    auto rendered = preparePitch(plan, stop, decodeCache, analysisCache, &pair, outputRate);
    return {rendered.problem, std::move(rendered.detail), std::move(pair)};
}

struct ClipPitchRenderTask::State {
    std::stop_source stop;
    std::atomic<bool> ready{false};
    ClipPitchRenderResult result;
};

ClipPitchRenderTask::ClipPitchRenderTask(std::shared_ptr<State> state) : state_(std::move(state)) {}

ClipPitchRenderTask::~ClipPitchRenderTask() { state_->stop.request_stop(); }

class ClipPitchRenderWorker {
public:
    ClipPitchRenderWorker(audio::DecodeCache& decode, AnalysisCache& analysis)
        : decode_(decode), analysis_(analysis), worker_([this](std::stop_token stop) { work(stop); }) {}

    std::shared_ptr<const ClipPitchRenderTask> request(const ClipPitchRenderPlan& plan) {
        auto state = std::make_shared<ClipPitchRenderTask::State>();
        auto task = std::shared_ptr<const ClipPitchRenderTask>(new ClipPitchRenderTask(state));
        const std::lock_guard lock(mutex_);
        std::erase_if(queue_, [](const Job& job) { return job.state->stop.stop_requested(); });
        queue_.push_back({plan, std::move(state)});
        wake_.notify_one();
        return task;
    }

private:
    struct Job { ClipPitchRenderPlan plan; std::shared_ptr<ClipPitchRenderTask::State> state; };

    void work(std::stop_token stop) {
        while (!stop.stop_requested()) {
            Job job;
            {
                std::unique_lock lock(mutex_);
                if (!wake_.wait(lock, stop, [&] { return !queue_.empty(); }) || stop.stop_requested()) return;
                job = std::move(queue_.front());
                queue_.pop_front();
            }
            auto& state = *job.state;
            if (state.stop.stop_requested()) continue;
            const std::stop_callback shutdown(stop, [&state] { state.stop.request_stop(); });
            try {
                state.result = prepareClipPitchRender(job.plan, state.stop.get_token(), &decode_, &analysis_);
            } catch (const std::exception& error) {
                state.result = fail(ClipPitchRenderProblem::Io, error.what());
            }
            state.ready.store(true, std::memory_order_release);
        }
    }

    audio::DecodeCache& decode_;
    AnalysisCache& analysis_;
    std::mutex mutex_;
    std::condition_variable_any wake_;
    std::deque<Job> queue_;
    std::jthread worker_;
};

const ClipPitchRenderResult* ClipPitchRenderTask::result() const noexcept {
    return state_->ready.load(std::memory_order_acquire) ? &state_->result : nullptr;
}

std::shared_ptr<const ClipPitchRenderTask> requestClipPitchRender(const ClipPitchRenderPlan& plan) {
    if (audio::onAudioThread) return {};
    auto& decode = audio::defaultDecodeCache();
    auto& analysis = defaultAnalysisCache();
    static ClipPitchRenderWorker worker(decode, analysis);
    return worker.request(plan);
}

} // namespace adi::media
