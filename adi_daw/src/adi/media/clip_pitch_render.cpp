// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/media/clip_pitch_render.hpp"

#include "adi/audio/decode.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/audio/pitch_render.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/media/blake3.hpp"
#include "adi/media/clip_pitch.hpp"
#include "adi/pitch_edit_store.hpp"

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

std::unique_ptr<const std::vector<float>> renderChannel(std::span<const float> pcm, const audio::PitchAnalysis& analysis,
                                                        const audio::PitchEditState& edits, std::stop_token stop) {
    auto rendered = audio::renderPitchEdits(pcm, analysis, edits, stop);
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

} // namespace

ClipPitchRenderResult prepareClipPitchRender(const ClipPitchRenderPlan& plan, std::stop_token stop) {
    if (audio::onAudioThread) return fail(ClipPitchRenderProblem::Realtime);
    if (stop.stop_requested()) return fail(ClipPitchRenderProblem::Cancelled);
    return prepareClipPitchRender(plan, stop, nullptr, &defaultAnalysisCache());
}

ClipPitchRenderResult prepareClipPitchRender(const ClipPitchRenderPlan& plan, std::stop_token stop,
                                              audio::DecodeCache* decodeCache, AnalysisCache* analysisCache) {
    if (audio::onAudioThread) return fail(ClipPitchRenderProblem::Realtime);
    if (stop.stop_requested()) return fail(ClipPitchRenderProblem::Cancelled);
    if (!plan.edits || plan.edits->rows.empty()) return fail(ClipPitchRenderProblem::Edits, "no Pitch Editor edit set");
    const auto& saved = *plan.edits;
    if (plan.source.empty() || plan.sourceFrames == 0 || plan.sourceStart > saved.sourceFrames ||
        plan.sourceFrames > saved.sourceFrames - plan.sourceStart)
        return fail(ClipPitchRenderProblem::Source, "Pitch Editor playback window exceeds source");
    if (plan.reverse && plan.warp)
        return fail(ClipPitchRenderProblem::Unsupported, "Pitch Editor playback cannot combine reverse and Warp");
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
        if (channels == 0 || channels > audio::kMaxDecodeChannels || rate != saved.sampleRate || frames != saved.sourceFrames)
            return fail(ClipPitchRenderProblem::Source, "Pitch Editor source dimensions differ from saved edits");
        if (frames > maxSourceSamples / channels)
            return fail(ClipPitchRenderProblem::Resource, "Pitch Editor source exceeds render PCM budget");

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

        const auto first = static_cast<std::size_t>(plan.sourceStart);
        const auto count = static_cast<std::size_t>(plan.sourceFrames);
        std::unique_ptr<const std::vector<float>> renderedMono;
        std::span<const float> window;
        if (channels == 1) {
            renderedMono = renderChannel(pcm, *analyzed.analysis, *edits.state, stop);
            window = std::span(*renderedMono).subspan(first, count);
        } else {
            // Every original channel shares the same note clock and controls.
            // Render individually and write the edited window back interleaved.
            for (std::size_t channel = 0; channel < channels; ++channel) {
                for (std::size_t frame = 0; frame < mono.size(); ++frame) {
                    if (frame % blockFrames == 0) checkCancelled(stop);
                    mono[frame] = pcm[frame * channels + channel];
                }
                const auto changed = renderChannel(mono, *analyzed.analysis, *edits.state, stop);
                for (std::size_t frame = first; frame < first + count; ++frame) {
                    if (frame % blockFrames == 0) checkCancelled(stop);
                    pcm[frame * channels + channel] = (*changed)[frame];
                }
            }
            window = std::span(pcm).subspan(first * channels, count * channels);
        }

        checkCancelled(stop);
        const auto folder = plan.workFolder.empty() ? fs::temp_directory_path() / "adi-pitch" : plan.workFolder;
        PrivateStage stage(folder);
        {
            audio::WavWriter writer(stage.file, rate, channels, audio::WavFormat::Float32);
            writeWindow(writer, window, channels, plan.reverse, stop);
        }
        checkCancelled(stop);
        if (plan.warp) {
            auto recipe = *plan.warp;
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
