// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/sample_preview.hpp"
#include "adi/audio/aac_codec.hpp"
#include "adi/audio/pcm_stream.hpp"
#include "adi/dsp/biquad.hpp"
#include "adi/mix/filters/FlushDenormals.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

namespace adi::engine {
namespace {
namespace fs = std::filesystem;
using namespace std::chrono_literals;
constexpr std::size_t kChunkFrames = 1024;
constexpr std::size_t kQueueChunks = 128; // Enough source frames for a 4096-frame callback at 768 -> 44.1 kHz.
constexpr std::size_t kWaveformBins = 512;
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);

struct Cancelled {};
void checkCancelled(std::stop_token stop) {
    if (stop.stop_requested())
        throw Cancelled{};
}

// The native AAC APIs may rewrap encoded packets in a private transport file.
// This never holds decoded PCM and is unrelated to the decoding cache.
class CodecScratch {
  public:
    CodecScratch() {
        static std::atomic<std::uint64_t> serial{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto root = fs::temp_directory_path();
        for (unsigned attempt = 0; attempt != 128; ++attempt) {
            auto candidate = root / ("adi-preview-" + std::to_string(stamp) + "-" +
                                     std::to_string(serial.fetch_add(1, std::memory_order_relaxed)));
            std::error_code error;
            if (fs::create_directory(candidate, error)) {
                directory_ = std::move(candidate);
                return;
            }
            if (error && error != std::errc::file_exists)
                throw std::runtime_error("Could not create a temporary AAC transport folder.");
        }
        throw std::runtime_error("Could not reserve a temporary AAC transport folder.");
    }
    ~CodecScratch() {
        std::error_code error;
        fs::remove_all(directory_, error);
    }
    fs::path file() const { return directory_ / "transport"; }

  private:
    fs::path directory_;
};

template <typename Info, typename Read>
void streamPcm(const fs::path &path, std::stop_token stop, Info &&info, Read &&read) {
    checkCancelled(stop);
    const auto format = audio::sniff(path);
    if (format == audio::SourceFormat::Mp4 || format == audio::SourceFormat::Adts) {
        auto track = [&] {
            if (format == audio::SourceFormat::Adts)
                return audio::readAdts(path);
            auto movie = audio::readAacAudioMovie(path);
            return std::move(movie.tracks.front());
        }();
        const auto config = audio::aacConfig(track.config);
        info(audio::PcmStreamInfo{format, config.sampleRate, config.channels, track.frames});
        checkCancelled(stop);
        const CodecScratch scratch;
        audio::decodeAacPcm(track, scratch.file(), std::forward<Read>(read), [&] { checkCancelled(stop); });
        return;
    }
    auto opened = audio::openPcmStream(path, stop);
    checkCancelled(stop);
    if (!opened.ready())
        throw std::runtime_error(opened.error.empty() ? "This file cannot be previewed." : opened.error);
    const auto &source = opened.stream->info();
    info(source);
    std::vector<float> buffer(kChunkFrames * source.channels);
    for (;;) {
        checkCancelled(stop);
        const auto next = opened.stream->read(buffer, stop);
        if (next.status == audio::PcmReadStatus::End)
            return;
        checkCancelled(stop);
        if (next.status != audio::PcmReadStatus::Frames)
            throw std::runtime_error(opened.stream->error().empty() ? "The preview could not read audio."
                                                                   : opened.stream->error());
        read(std::span<const float>(buffer).first(static_cast<std::size_t>(next.frames) * source.channels));
    }
}

struct Stereo {
    float left = 0, right = 0;
};
Stereo previewSample(std::span<const float> frame) noexcept {
    // Use the first stereo pair and duplicate a mono source.
    const auto left = std::isfinite(frame[0]) ? frame[0] : 0.f;
    if (frame.size() == 1)
        return {left, left};
    return {left, std::isfinite(frame[1]) ? frame[1] : 0.f};
}

struct Request {
    fs::path path;
    std::uint64_t revision = 0;
    double fraction = 0.;
    std::optional<std::uint64_t> knownFrames;
    std::stop_token stop;
};
struct WorkSlot {
    std::stop_source cancel;
    std::optional<Request> pending;

    void replace(Request request) {
        cancel.request_stop();
        cancel = std::stop_source{};
        request.stop = cancel.get_token();
        pending = std::move(request);
    }
    void clear() noexcept {
        cancel.request_stop();
        pending.reset();
    }
};
struct Chunk {
    std::uint64_t revision = 0, first = 0;
    std::uint32_t rate = 0;
    std::size_t frames = 0;
    std::array<Stereo, kChunkFrames> pcm;
};

// Filter every source frame before linear interpolation can skip it. A
// twelve-pole Butterworth leaves a transition band below the device's Nyquist.
// The two latest filtered frames cover interpolation across queue boundaries.
struct PreviewLowPass {
    struct History {
        double z1 = 0., z2 = 0.;
        void reset(const dsp::Biquad &coefficients, double input) noexcept {
            z1 = (1. - coefficients.b0) * input;
            z2 = (coefficients.b2 - coefficients.a2) * input;
        }
        double step(const dsp::Biquad &coefficients, double input) noexcept {
            const auto output = coefficients.b0 * input + z1;
            z1 = coefficients.b1 * input - coefficients.a1 * output + z2;
            z2 = coefficients.b2 * input - coefficients.a2 * output;
            return output;
        }
    };
    struct Section {
        dsp::Biquad coefficients;
        History left, right;
    };
    static constexpr int kSections = 6;
    std::array<Section, kSections> sections;
    std::uint32_t sourceRate = 0;
    double outputRate = 0.;
    std::uint64_t nextFrame = 0;
    Stereo previous, latest;

    void reset() noexcept { sourceRate = 0; }

    Stereo frameAt(const Chunk &chunk, std::size_t at, double deviceRate) noexcept {
        if (sourceRate != chunk.rate || outputRate != deviceRate) {
            sourceRate = chunk.rate;
            outputRate = deviceRate;
            nextFrame = chunk.first;
            const auto start = chunk.pcm[0];
            for (int index = 0; index < kSections; ++index) {
                auto &section = sections[static_cast<std::size_t>(index)];
                section.coefficients = dsp::lowPass(sourceRate, deviceRate * .40,
                                                     dsp::butterworthQ(2 * kSections, kSections - index));
                section.left.reset(section.coefficients, start.left);
                section.right.reset(section.coefficients, start.right);
            }
        }
        const auto through = chunk.first + at + 1;
        for (; nextFrame < through; ++nextFrame) {
            const auto input = chunk.pcm[static_cast<std::size_t>(nextFrame - chunk.first)];
            double left = input.left, right = input.right;
            for (auto &section : sections) {
                left = section.left.step(section.coefficients, left);
                right = section.right.step(section.coefficients, right);
            }
            previous = latest;
            latest = {static_cast<float>(left), static_cast<float>(right)};
        }
        return through == nextFrame ? latest : previous;
    }
};

void appendPeaks(SamplePreview::Waveform &wave, std::span<const float> pcm, std::size_t channels) {
    for (std::size_t at = 0; at < pcm.size(); at += channels) {
        const auto sample = previewSample(pcm.subspan(at, channels));
        if (wave.frames % wave.framesPerBin == 0) {
            if (wave.bins.size() == kWaveformBins) {
                for (std::size_t i = 0; i < kWaveformBins / 2; ++i)
                    wave.bins[i] = {std::min(wave.bins[i * 2].minimum, wave.bins[i * 2 + 1].minimum),
                                    std::max(wave.bins[i * 2].maximum, wave.bins[i * 2 + 1].maximum)};
                wave.bins.resize(kWaveformBins / 2);
                wave.framesPerBin *= 2;
            }
            wave.bins.push_back({std::min(sample.left, sample.right), std::max(sample.left, sample.right)});
        } else {
            auto &bin = wave.bins.back();
            bin.minimum = std::min({bin.minimum, sample.left, sample.right});
            bin.maximum = std::max({bin.maximum, sample.left, sample.right});
        }
        ++wave.frames;
    }
}

template <typename T> bool outputPair(T *const *out, int first) noexcept {
    return out && out[first] && out[first + 1] && out[first] != out[first + 1];
}
} // namespace

struct SamplePreview::Impl {
    mutable std::mutex mutex;
    std::condition_variable_any wake;
    WorkSlot playback, selection;
    std::uint64_t nextRevision = 0, selectionRevision = 0;
    Status snapshot;
    fs::path selectedPath;
    std::uint64_t totalFrames = 0;
    double requestedFraction = 0.;
    std::string waveformError;

    // A fixed single-producer/single-consumer queue. Replacing a request does
    // not free or reallocate its PCM, including while the device is stopped.
    std::array<Chunk, kQueueChunks> queue;
    std::atomic<std::uint64_t> written{0}, consumed{0};
    std::atomic<std::uint64_t> requested{0}, decoderEnded{0}, failed{0};
    std::atomic<std::uint64_t> audible{0}, finished{0}, position{0};
    std::atomic<bool> hasTail{false}; // The last callback contributed audio that Stop may need to fade.
    std::uint64_t rendering = 0; // audio-thread-only
    double offset = 0.;
    double lastLeft = 0., lastRight = 0., releaseLeft = 0., releaseRight = 0.;
    int fadeFrames = 1, releaseRemaining = 0, attackRemaining = 0;
    bool starting = false;
    PreviewLowPass lowPass;

    std::jthread playbackThread, selectionThread; // join before releasing the queue and owner state
    Impl() {
        playbackThread = std::jthread([this](std::stop_token stop) {
            run(playback, stop, &Impl::decodePlayback, true);
        });
        selectionThread = std::jthread([this](std::stop_token stop) {
            run(selection, stop, &Impl::decodeWaveform, false);
        });
    }
    ~Impl() {
        playbackThread.request_stop();
        selectionThread.request_stop();
        std::lock_guard lock(mutex);
        playback.clear();
        selection.clear();
    }

    void run(WorkSlot &slot, std::stop_token stop, void (Impl::*work)(const Request &), bool audition) {
        for (;;) {
            Request request;
            {
                std::unique_lock lock(mutex);
                if (!wake.wait(lock, stop, [&] { return slot.pending.has_value(); }))
                    return;
                request = std::move(*slot.pending);
                slot.pending.reset();
            }
            try {
                (this->*work)(request);
            } catch (const Cancelled &) {
                // A replacement or Stop has already changed the owner state.
            } catch (const std::exception &error) {
                std::lock_guard lock(mutex);
                if (audition && requested.load(std::memory_order_relaxed) == request.revision) {
                    snapshot.error = error.what();
                    failed.store(request.revision, std::memory_order_release);
                } else if (!audition && selectionRevision == request.revision)
                    waveformError = error.what();
            }
        }
    }

    void requestWaveform(const fs::path &path) { // owner, with mutex
        if (path == selectedPath)
            return;
        selectedPath = path;
        snapshot.waveform.reset();
        waveformError.clear();
        selectionRevision = ++nextRevision;
        if (path.empty())
            selection.clear();
        else
            selection.replace({path, selectionRevision, 0., {}, {}});
    }

    void decodeWaveform(const Request &request) {
        auto result = std::make_shared<Waveform>();
        result->path = request.path;
        result->bins.reserve(kWaveformBins);
        std::size_t channels = 0;
        streamPcm(request.path, request.stop, [&](const audio::PcmStreamInfo &info) {
            channels = info.channels;
            result->sampleRate = info.sampleRate;
            if (info.frames && *info.frames > 0)
                result->framesPerBin = 1 + (*info.frames - 1) / kWaveformBins;
        }, [&](std::span<const float> pcm) {
            checkCancelled(request.stop);
            appendPeaks(*result, pcm, channels);
        });
        if (!result->frames)
            throw std::runtime_error("This sample has no audio frames.");
        std::lock_guard lock(mutex);
        if (selectionRevision == request.revision)
            snapshot.waveform = std::move(result);
    }

    void decodePlayback(const Request &request) {
        std::uint64_t frame = 0, start = 0;
        std::size_t channels = 0;
        std::uint32_t rate = 0;
        auto retry = 2ms;
        streamPcm(request.path, request.stop, [&](const audio::PcmStreamInfo &info) {
            channels = info.channels;
            rate = info.sampleRate;
            const auto length = info.frames ? info.frames : request.knownFrames;
            if (request.fraction > 0. && !length)
                throw std::runtime_error("Wait for the waveform before seeking this sample.");
            if (length && *length > 0) {
                // The waveform's horizontal fraction measures the full
                // interval [0, length). Its right edge plays the last frame.
                start = request.fraction == 1. ? *length - 1 :
                            static_cast<std::uint64_t>(static_cast<long double>(*length) * request.fraction);
                std::lock_guard lock(mutex);
                if (requested.load(std::memory_order_relaxed) == request.revision)
                    totalFrames = *length;
            }
        }, [&](std::span<const float> pcm) {
            const auto frames = pcm.size() / channels;
            std::size_t at = 0;
            if (frame < start) {
                at = static_cast<std::size_t>(std::min<std::uint64_t>(start - frame, frames));
                frame += at;
            }
            while (at < frames) {
                checkCancelled(request.stop);
                const auto next = written.load(std::memory_order_relaxed);
                const auto space = [&] { return next - consumed.load(std::memory_order_acquire) < queue.size(); };
                if (!space()) {
                    // The audio callback only publishes its cursor. Back off
                    // while the device is suspended, and wake on cancellation.
                    std::unique_lock lock(mutex);
                    wake.wait_for(lock, request.stop, retry, space);
                    retry = std::min(2 * retry, 32ms);
                    continue;
                }
                retry = 2ms;
                auto &chunk = queue[static_cast<std::size_t>(next % queue.size())];
                chunk.revision = request.revision;
                chunk.rate = rate;
                chunk.first = frame;
                chunk.frames = std::min(chunk.pcm.size(), frames - at);
                for (std::size_t i = 0; i < chunk.frames; ++i)
                    chunk.pcm[i] = previewSample(pcm.subspan((at + i) * channels, channels));
                at += chunk.frames;
                frame += chunk.frames;
                written.store(next + 1, std::memory_order_release);
            }
        });
        if (frame <= start)
            throw std::runtime_error("This sample has no audio at the preview start.");
        std::lock_guard lock(mutex);
        if (requested.load(std::memory_order_relaxed) == request.revision) {
            totalFrames = frame;
            decoderEnded.store(request.revision, std::memory_order_release);
        }
    }
};

SamplePreview::SamplePreview() : impl_(std::make_unique<Impl>()) {}
SamplePreview::~SamplePreview() = default;

void SamplePreview::select(fs::path path) {
    auto &state = *impl_;
    {
        std::lock_guard lock(state.mutex);
        state.requestWaveform(path);
    }
    state.wake.notify_all();
}
void SamplePreview::play(fs::path path, double fraction) {
    if (path.empty()) {
        stop();
        return;
    }
    auto &state = *impl_;
    {
        std::lock_guard lock(state.mutex);
        // Keep the selection supplied by the caller; a surface may audition a
        // different file without taking the Browser's selection away.
        if (state.selectedPath.empty())
            state.requestWaveform(path);
        const auto frames = state.snapshot.waveform && state.snapshot.waveform->path == path
                                ? std::optional{state.snapshot.waveform->frames} : std::nullopt;
        const auto revision = ++state.nextRevision;
        state.requestedFraction = std::isfinite(fraction) ? std::clamp(fraction, 0., 1.) : 0.;
        state.playback.replace({path, revision, state.requestedFraction, frames, {}});
        state.snapshot.playingPath = std::move(path);
        state.snapshot.error.clear();
        state.totalFrames = 0;
        state.requested.store(revision, std::memory_order_release);
    }
    state.wake.notify_all();
}
void SamplePreview::stop() {
    auto &state = *impl_;
    std::lock_guard lock(state.mutex);
    state.playback.clear();
    state.snapshot.playingPath.clear();
    state.snapshot.error.clear();
    state.requested.store(0, std::memory_order_release);
}
SamplePreview::Status SamplePreview::status() const {
    const auto &state = *impl_;
    std::lock_guard lock(state.mutex);
    auto result = state.snapshot;
    const auto revision = state.requested.load(std::memory_order_relaxed);
    if (revision != 0) {
        result.phase = Phase::Loading;
        result.position = state.requestedFraction;
        if (state.finished.load(std::memory_order_acquire) == revision) {
            result.phase = Phase::Finished;
            result.position = 1.;
        } else if (state.failed.load(std::memory_order_acquire) == revision)
            result.phase = Phase::Failed;
        else if (state.audible.load(std::memory_order_acquire) == revision) {
            result.phase = Phase::Playing;
            auto length = state.totalFrames;
            if (!length && result.waveform && result.waveform->path == result.playingPath)
                length = result.waveform->frames;
            if (length)
                result.position = std::clamp(static_cast<double>(state.position.load(std::memory_order_relaxed)) /
                                             static_cast<double>(length), 0., 1.);
        }
    } else if (!state.waveformError.empty())
        result.error = state.waveformError;
    return result;
}

bool SamplePreview::active() const noexcept {
    const auto &state = *impl_;
    const auto revision = state.requested.load(std::memory_order_acquire);
    return state.hasTail.load(std::memory_order_relaxed) ||
           (revision != 0 && state.finished.load(std::memory_order_relaxed) != revision &&
            state.failed.load(std::memory_order_acquire) != revision);
}
bool SamplePreview::canOutput(const AudioIo &io, int firstChannel) noexcept {
    const auto packed = io.packedOutputPair(firstChannel);
    return !io.exporting && packed >= 0 && (outputPair(io.out, packed) || outputPair(io.out64, packed));
}
void SamplePreview::mix(const AudioIo &io, double deviceRate, int physicalFirst, double gain) noexcept {
    const auto firstChannel = io.packedOutputPair(physicalFirst);
    if (io.frames <= 0 || io.exporting || firstChannel < 0 || !(deviceRate >= 1.) || !std::isfinite(deviceRate) ||
        !(gain >= 0.) || !std::isfinite(gain))
        return;
    const bool narrow = outputPair(io.out, firstChannel), wide = outputPair(io.out64, firstChannel);
    if (!narrow && !wide)
        return;
    auto &state = *impl_;
    auto wanted = state.requested.load(std::memory_order_acquire);
    if (state.finished.load(std::memory_order_relaxed) == wanted ||
        state.failed.load(std::memory_order_acquire) == wanted)
        wanted = 0;
    if (state.rendering != wanted) {
        state.rendering = wanted;
        state.offset = 0.;
        // The worker may not have the replacement ready yet. Release the last
        // audible value now, then attack the replacement when its PCM arrives.
        const bool releasing = state.lastLeft != 0. || state.lastRight != 0.;
        state.fadeFrames = static_cast<int>(std::clamp(deviceRate * .005, 1., 4096.));
        state.releaseLeft = state.lastLeft;
        state.releaseRight = state.lastRight;
        state.releaseRemaining = releasing ? state.fadeFrames : 0;
        state.attackRemaining = releasing && wanted ? state.fadeFrames : 0;
        state.starting = wanted != 0;
        state.lowPass.reset();
    }
    const auto mixFrame = [&](double left, double right, std::int32_t frame) {
        if (state.releaseRemaining > 0) {
            const auto release = static_cast<double>(--state.releaseRemaining) / state.fadeFrames;
            left += state.releaseLeft * release;
            right += state.releaseRight * release;
        }
        if (narrow) {
            io.out[firstChannel][frame] += static_cast<float>(left);
            io.out[firstChannel + 1][frame] += static_cast<float>(right);
        }
        if (wide) {
            io.out64[firstChannel][frame] += left;
            io.out64[firstChannel + 1][frame] += right;
        }
        state.lastLeft = left;
        state.lastRight = right;
    };
    auto next = state.consumed.load(std::memory_order_relaxed);
    const bool ended = state.decoderEnded.load(std::memory_order_acquire) == wanted;
    const auto available = state.written.load(std::memory_order_acquire);
    while (next != available && state.queue[static_cast<std::size_t>(next % state.queue.size())].revision < wanted)
        ++next;
    std::optional<mix::filters::FlushDenormals> flush;
    if (wanted && next != available && state.queue[static_cast<std::size_t>(next % state.queue.size())].rate > deviceRate)
        flush.emplace();
    std::int32_t i = 0;
    for (; wanted && i < io.frames; ++i) {
        while (next != available) {
            const auto &candidate = state.queue[static_cast<std::size_t>(next % state.queue.size())];
            if (candidate.revision != wanted || state.offset < static_cast<double>(candidate.frames))
                break;
            if (candidate.rate > deviceRate)
                state.lowPass.frameAt(candidate, candidate.frames - 1, deviceRate);
            state.offset -= static_cast<double>(candidate.frames);
            ++next;
        }
        if (next == available) {
            if (ended) {
                state.finished.store(wanted, std::memory_order_release);
                state.rendering = 0;
            }
            break;
        }
        const auto &chunk = state.queue[static_cast<std::size_t>(next % state.queue.size())];
        if (chunk.revision != wanted)
            break;
        const auto at = static_cast<std::size_t>(state.offset);
        const double fraction = state.offset - static_cast<double>(at);
        const bool downsample = chunk.rate > deviceRate;
        if (!downsample)
            state.lowPass.reset();
        const auto sample = [&](const Chunk &from, std::size_t index) {
            return downsample ? state.lowPass.frameAt(from, index, deviceRate) : from.pcm[index];
        };
        const auto a = sample(chunk, at);
        auto b = a;
        if (fraction > 0.) {
            if (at + 1 < chunk.frames)
                b = sample(chunk, at + 1);
            else if (next + 1 < available) {
                const auto &following = state.queue[static_cast<std::size_t>((next + 1) % state.queue.size())];
                if (following.revision == wanted)
                    b = sample(following, 0);
            }
            else if (!ended)
                break; // wait for interpolation's next source frame without changing position
        }
        if (std::exchange(state.starting, false) && chunk.first != 0)
            state.attackRemaining = state.fadeFrames;
        double level = gain;
        if (state.attackRemaining > 0)
            level *= 1. - static_cast<double>(--state.attackRemaining) / state.fadeFrames;
        mixFrame(std::lerp(static_cast<double>(a.left), static_cast<double>(b.left), fraction) * level,
                 std::lerp(static_cast<double>(a.right), static_cast<double>(b.right), fraction) * level, i);
        state.offset += static_cast<double>(chunk.rate) / deviceRate;
        state.position.store(chunk.first + static_cast<std::uint64_t>(state.offset), std::memory_order_relaxed);
        state.audible.store(wanted, std::memory_order_release);
    }
    for (; state.releaseRemaining > 0 && i < io.frames; ++i)
        mixFrame(0., 0., i);
    if (i < io.frames)
        state.lastLeft = state.lastRight = 0.;
    state.hasTail.store(state.releaseRemaining > 0 || state.lastLeft != 0. || state.lastRight != 0.,
                        std::memory_order_relaxed);
    state.consumed.store(next, std::memory_order_release);
}
} // namespace adi::engine
