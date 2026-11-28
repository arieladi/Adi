// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/audio_recording.hpp"
#include "adi/engine/audio_memory.hpp"
#include "adi/engine/publisher.hpp"
#include "adi/engine/snapshot.hpp"
#include "adi/engine/recording_recovery_plan.hpp"
#include "adi/engine/recording_recovery_wave.hpp"
#include "adi/engine/transport.hpp"
#include "adi/audio/io_audit.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <exception>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
namespace adi::engine {
namespace fs=std::filesystem;
std::optional<std::int64_t> recordingPlacementStart(std::int64_t startSample,
                                                   std::int64_t offsetSamples) noexcept {
    constexpr auto ceiling = std::numeric_limits<std::int64_t>::max();
    if (startSample < 0 || (offsetSamples < 0 && startSample > ceiling + offsetSamples))
        return std::nullopt;
    return offsetSamples > startSample ? 0 : startSample - offsetSamples;
}

std::int64_t recordingLatencyCorrection(std::int64_t driverSamples, double sampleRate,
                                        double errorMilliseconds) noexcept {
    if (!std::isfinite(sampleRate) || sampleRate <= 0 || sampleRate > 768000 ||
        !std::isfinite(errorMilliseconds) || std::abs(errorMilliseconds) > 150)
        return driverSamples;
    const auto errorSamples = static_cast<std::int64_t>(std::llround(errorMilliseconds * sampleRate / 1000.0));
    if (errorSamples > 0 && driverSamples > std::numeric_limits<std::int64_t>::max() - errorSamples)
        return std::numeric_limits<std::int64_t>::max();
    if (errorSamples < 0 && driverSamples < std::numeric_limits<std::int64_t>::min() - errorSamples)
        return std::numeric_limits<std::int64_t>::min();
    return driverSamples + errorSamples;
}

std::optional<RecordedPass> recordingPlacementRange(std::int64_t startSample,std::uint64_t frames,
    std::int64_t offsetSamples,const RecordingPunchWindow& punch) noexcept {
    constexpr auto ceiling = std::numeric_limits<std::int64_t>::max();
    const auto placed = recordingPlacementStart(startSample,offsetSamples);
    if(!placed || frames > static_cast<std::uint64_t>(ceiling))
        return {};
    // A take keeps its sample mapping when the correction crosses song zero: the
    // frames it puts before the song are hidden, and the rest stays in time. A
    // whole file moved to zero would sound late by the part that did not fit.
    const auto start = offsetSamples > startSample ? startSample-offsetSamples : *placed;
    const auto count = static_cast<std::int64_t>(frames);
    if(start > ceiling-count)
        return {};
    const auto end = start+count;
    const auto keptStart = std::max({start,std::int64_t{0},punch.in ? punch.in->sample : std::int64_t{0}});
    const auto keptEnd = punch.out ? std::min(end,punch.out->sample) : end;
    if(keptStart >= keptEnd)
        return {};
    return RecordedPass{static_cast<std::uint64_t>(keptStart-start),static_cast<std::uint64_t>(keptEnd-keptStart),
                        keptStart,keptEnd};
}

std::optional<RecordedPass> loopPassPlacement(const RecordedPass& captured,std::uint64_t sourceFrames,
    std::int64_t offsetSamples) noexcept {
    constexpr auto ceiling = std::numeric_limits<std::int64_t>::max();
    if(captured.frames==0 || captured.frames>static_cast<std::uint64_t>(ceiling) ||
       sourceFrames>static_cast<std::uint64_t>(ceiling) || captured.sourceStartFrame>sourceFrames ||
       captured.timelineStartSample>ceiling-static_cast<std::int64_t>(captured.frames))
        return {};
    const auto source = static_cast<std::int64_t>(captured.sourceStartFrame);
    if((offsetSamples>0 && source>ceiling-offsetSamples) || (offsetSamples<0 && offsetSamples<-ceiling))
        return {};
    const auto begin = source+offsetSamples;
    const auto length = static_cast<std::int64_t>(captured.frames);
    const auto end = begin>ceiling-length ? ceiling : begin+length;
    // Keep the timeline the pass was heard at; only the part the file holds is placed.
    const auto keptBegin = std::max<std::int64_t>(begin,0);
    const auto keptEnd = std::min(end,static_cast<std::int64_t>(sourceFrames));
    if(keptBegin>=keptEnd)
        return {};
    const auto timeline = captured.timelineStartSample+(keptBegin-begin);
    return RecordedPass{static_cast<std::uint64_t>(keptBegin),static_cast<std::uint64_t>(keptEnd-keptBegin),
                        timeline,timeline+(keptEnd-keptBegin)};
}

AudioRecordingPreview::Range AudioRecordingPreview::latestPass(std::uint64_t acceptedFrames,
    std::optional<std::int64_t> trackOffsetSamples) const noexcept {
    const auto frames = std::max(peaks.frames, acceptedFrames);
    const auto offset = trackOffsetSamples.value_or(placementOffsetSamples);
    if (!loop) {
        // The same range the landed clip will play, hidden frames before song zero included.
        const auto placed = recordingPlacementRange(rawStartSample, frames, offset, {});
        return placed ? Range{placed->timelineStartSample, placed->sourceStartFrame, placed->frames}
                      : Range{startSample, 0, 0};
    }
    if (loop->beginSample < 0 || loop->endSample <= loop->beginSample ||
        startSample < 0 || startSample >= loop->endSample || frames > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max()) || offset == std::numeric_limits<std::int64_t>::min())
        return {startSample, 0, frames};
    // The passes as they were heard: the file reads `offset` frames later than the timeline,
    // so the last `offset` captured frames belong to no pass yet (loopPassPlacement).
    const auto signedFrames = static_cast<std::int64_t>(frames);
    if (offset >= signedFrames)
        return {startSample, 0, 0};
    if (offset < 0 && signedFrames > std::numeric_limits<std::int64_t>::max() + offset)
        return {startSample, 0, frames};
    const auto heard = static_cast<std::uint64_t>(signedFrames - offset);
    const auto firstFrames = static_cast<std::uint64_t>(loop->endSample - startSample);
    RecordedPass pass{0, heard, startSample, 0};
    if (heard > firstFrames) {
        const auto loopFrames = static_cast<std::uint64_t>(loop->endSample - loop->beginSample);
        // An exact wrap still shows the last complete pass until the next one
        // has audio. Source offsets include a partial first pass.
        const auto passFrames = (heard - firstFrames - 1) % loopFrames + 1;
        pass = {heard - passFrames, passFrames, loop->beginSample, 0};
    }
    pass.timelineEndSample = pass.timelineStartSample + static_cast<std::int64_t>(pass.frames);
    // In heard frames the pass starts at `sourceStartFrame`; in the file, `offset` later.
    const auto heardStart = static_cast<std::int64_t>(pass.sourceStartFrame);
    pass.sourceStartFrame = 0;
    const auto inFile = loopPassPlacement({0, pass.frames, pass.timelineStartSample, pass.timelineEndSample},
                                          frames, heardStart + offset);
    return inFile ? Range{inFile->timelineStartSample, inFile->sourceStartFrame, inFile->frames}
                  : Range{startSample, 0, 0};
}

struct AudioRecordingResult::Files {
    fs::path directory;
    std::shared_ptr<media::ProjectFileLease> recordingOwner;
    bool hasOriginalPlan=false;
    bool ephemeralStage=false;
    ~Files() {
        if(ephemeralStage && !directory.empty()) {
            try {
                std::error_code ignored;
                fs::remove_all(directory,ignored);
            } catch(...) {
                // A discarded recovery preview cannot affect the original.
            }
        }
    }
};
bool AudioRecordingResult::hasAttachableAudio()const noexcept {
    if(frames==0)return false;
    switch(failure) {
    case RecordingFailure::None:return true;
    case RecordingFailure::Overflow:
    case RecordingFailure::MissingInput:
    case RecordingFailure::BlockSize:
    case RecordingFailure::Discontinuity:
    case RecordingFailure::PassLimit:return finalizedPrefix_;
    default:return false;
    }
}
const fs::path& AudioRecordingResult::directory()const noexcept {static const fs::path empty;return files_?files_->directory:empty;}
std::error_code AudioRecordingResult::discard() {
    if(audio::onAudioThread)return std::make_error_code(std::errc::operation_not_permitted);
    if(!files_)return {};
    std::error_code removalError;
    // If removal of the audio is refused, do not offer a take the owner
    // already discarded. Deleting a link here only removes the link.
    if(files_->hasOriginalPlan) {
        fs::remove(files_->directory/"start.json",removalError);
        if(removalError)return removalError;
        recording_recovery::forgetLaunchTake(recordingId_);
        // The take is no longer recoverable. Close its owner.lock before removing the directory
        // so a Windows delete-pending marker cannot keep that directory nonempty.
        files_->recordingOwner.reset();
    }
    fs::remove_all(files_->directory,removalError);
    if(!removalError) {
        // A recovery preview also holds its original take's lease, outside this staging directory.
        files_->recordingOwner.reset();
        if(files_->ephemeralStage)files_->directory.clear();
        files_.reset();
    }
    return removalError;
}
namespace {
constexpr std::uint32_t kMainChannelBase = 64;
static_assert(std::atomic<std::uint64_t>::is_always_lock_free && std::atomic<std::int64_t>::is_always_lock_free);
static_assert(std::atomic<RecordingFailure>::is_always_lock_free);
class WaveSink final:public AudioRecordingSink {
    audio::WavWriter writer_;
public:
    WaveSink(const fs::path& file,std::uint32_t rate,std::uint16_t channels,audio::WavFormat format):writer_(file,rate,channels,format){}
    void write(std::span<const float> values,std::uint32_t frames)override{writer_.write(values.data(),frames);}
    void close()override{writer_.close();}
};
fs::path reserve(const fs::path& parent) {
    static std::atomic<std::uint64_t> serial{0};const auto absolute=fs::absolute(parent);fs::create_directories(absolute);
    for(unsigned attempt=0;attempt<128;++attempt) {
        auto path=absolute/(".adi-record-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(serial.fetch_add(1)));
        std::error_code error;if(fs::create_directory(path,error))return path;
        if(error && error!=std::errc::file_exists)throw fs::filesystem_error("reserve recording",path,error);
    }
    throw std::runtime_error("cannot reserve recording directory");
}
struct Job {
    static constexpr std::size_t maxPreviewBins = 4096;
    static constexpr std::uint64_t firstPreviewBinFrames = 64;
    static constexpr auto previewInterval = std::chrono::milliseconds(50);
    using Clock = std::chrono::steady_clock;

    AudioRecordingSpec spec;
    fs::path directory;
    std::vector<RecordedTrack> tracks;
    std::vector<std::int64_t> trackIds, resamplingTracks;
    std::vector<std::uint32_t> inputChannels;
    std::size_t physicalChannels = 0;
    std::vector<float> mainBuffer;
    std::array<float*, 2> mainOutput{};
    std::vector<std::vector<std::size_t>> mappings;
    std::vector<std::unique_ptr<AudioRecordingSink>> writers;
    std::vector<std::vector<float>> output;
    std::vector<float> ring,readBuffer;
    std::atomic<std::uint64_t> written{0},read{0};
    std::atomic<std::int64_t> firstSample{-1};
    RecordingPunchWindow punchWindow; // written once before publishing the first captured frames
    std::atomic<RecordingFailure> failure{RecordingFailure::None};
    std::atomic<bool> stopWriter{false},done{false},transportEnded{false};
    std::thread worker;
    std::exception_ptr writerError;
    // Accumulation stays on the writer. Only the immutable pointer crosses to the owner.
    AudioRecordingPreview accumulatingPreview;
    bool previewDisabled=false;
    Clock::time_point nextPreview=Clock::time_point::min();
    mutable std::mutex previewMutex;
    std::shared_ptr<const AudioRecordingPreview> publishedPreview;
    bool finalized=false; // written before publishing done
    std::optional<recording_recovery::OwnedPlan> recovery;
    std::string recoveryWarning;
    bool discardRequested=false; // owner only; cancellation also discards failed takes
    // Callback-owned continuity state; observed only after its quiescence.
    bool begun=false;
    std::int64_t nextTimeline=0,nextStream=0;
    std::uint64_t transportRevision=0;
    std::optional<std::uint64_t> verifiedClockGeneration;
    std::vector<RecordedPass> passes; // sized before publication, never grown here
    std::size_t passCount=0;
    void fail(RecordingFailure value)noexcept{auto expected=RecordingFailure::None;failure.compare_exchange_strong(expected,value,std::memory_order_relaxed);}
    ~Job() {
        stopWriter.store(true,std::memory_order_release);
        if(worker.joinable())worker.join();
        closeWriters(); // also closes sinks opened before a failed start
    }
    void noteWriterError()noexcept {
        if(!writerError)writerError=std::current_exception();
        fail(RecordingFailure::Io);
    }
    void closeWriters()noexcept {
        for(auto& writer:writers) {
            try {
                writer->close();
            } catch(...) {
                noteWriterError();
            }
        }
        // A sink whose close throws may still own a file handle.
        writers.clear();
    }
    [[nodiscard]] std::shared_ptr<const AudioRecordingPreview> latestPreview() const noexcept {
        const std::lock_guard lock(previewMutex);
        return publishedPreview;
    }
    void publishPreview(Clock::time_point now) noexcept {
        if (previewDisabled || accumulatingPreview.peaks.frames == 0)
            return;
        nextPreview = now + previewInterval;
        try {
            auto prepared = std::make_shared<AudioRecordingPreview>(accumulatingPreview);
            // Undithered PCM conversion is monotonic, so encode extrema once per published bin.
            if (spec.format != audio::WavFormat::Float32)
                for (auto &bin : prepared->peaks.bins) {
                    bin.minimum = audio::wavSampleAtDepth(bin.minimum, spec.format);
                    bin.maximum = audio::wavSampleAtDepth(bin.maximum, spec.format);
                }
            std::shared_ptr<const AudioRecordingPreview> snapshot = std::move(prepared);
            {
                const std::lock_guard lock(previewMutex);
                publishedPreview.swap(snapshot);
            }
        } catch (...) {
            // Preview publication is optional. A resource failure cannot stop recording.
        }
    }
    void appendPreview(std::uint32_t frames) noexcept {
        if (previewDisabled)
            return;
        auto &peaks = accumulatingPreview.peaks;
        const auto width = inputChannels.size();
        if (peaks.frames == 0) {
            try {
                const auto start = firstSample.load(std::memory_order_relaxed);
                const auto offset = spec.placementOffsetSamples;
                accumulatingPreview.rawStartSample = start;
                accumulatingPreview.placementOffsetSamples = offset;
                // A loop pass keeps its timeline and reads the file later (loopPassPlacement).
                accumulatingPreview.startSample =
                    spec.loop ? start : recordingPlacementStart(start, offset).value_or(start);
                if (punchWindow.active())
                    accumulatingPreview.punchedPlacement = AudioRecordingPreview::PunchedPlacement{start, offset, punchWindow};
                accumulatingPreview.loop = spec.loop;
                peaks.channels = static_cast<std::uint32_t>(width);
                peaks.sampleRate = spec.sampleRate;
                peaks.framesPerBin = firstPreviewBinFrames;
                peaks.bins.reserve(maxPreviewBins * width);
                accumulatingPreview.tracks.reserve(spec.targets.size());
                for (std::size_t target = 0; target < spec.targets.size(); ++target) {
                    AudioRecordingPreview::Track track;
                    track.trackId = spec.targets[target].trackId;
                    track.placementOffsetSamples = spec.targets[target].placementOffsetSamples;
                    track.channels.reserve(mappings[target].size());
                    for (const auto channel : mappings[target])
                        track.channels.push_back(static_cast<std::uint32_t>(channel));
                    accumulatingPreview.tracks.push_back(std::move(track));
                }
            } catch (...) {
                previewDisabled = true;
                return;
            }
        }

        constexpr auto infinity = std::numeric_limits<float>::infinity();
        for (std::uint32_t offset = 0; offset < frames;) {
            const auto inBin = peaks.frames % peaks.framesPerBin;
            if (inBin == 0) {
                if (peaks.bins.size() == maxPreviewBins * width) {
                    // The buffer is full of complete bins. Merge pairs to retain the whole take.
                    for (std::size_t bin = 0; bin < maxPreviewBins / 2; ++bin)
                        for (std::size_t channel = 0; channel < width; ++channel) {
                            const auto first = peaks.bins[(2 * bin) * width + channel];
                            const auto second = peaks.bins[(2 * bin + 1) * width + channel];
                            peaks.bins[bin * width + channel] = {std::min(first.minimum, second.minimum),
                                                                 std::max(first.maximum, second.maximum)};
                        }
                    peaks.bins.resize(maxPreviewBins / 2 * width);
                    peaks.framesPerBin *= 2;
                }
                peaks.bins.resize(peaks.bins.size() + width, {infinity, -infinity});
            }
            const auto count = static_cast<std::uint32_t>(
                std::min<std::uint64_t>(frames - offset, peaks.framesPerBin - inBin));
            auto *bin = peaks.bins.data() + peaks.bins.size() - width;
            for (auto frame = offset; frame < offset + count; ++frame)
                for (std::size_t channel = 0; channel < width; ++channel) {
                    const float raw = readBuffer[static_cast<std::size_t>(frame) * width + channel];
                    const float value = std::isfinite(raw) ? raw : 0.f;
                    bin[channel].minimum = std::min(bin[channel].minimum, value);
                    bin[channel].maximum = std::max(bin[channel].maximum, value);
                }
            offset += count;
            peaks.frames += count;
        }
        const auto now = Clock::now();
        if (now >= nextPreview)
            publishPreview(now);
    }
    void run()noexcept {
        bool drained=false;
        try {
            while(true) {
                auto head=written.load(std::memory_order_acquire);
                const auto tail=read.load(std::memory_order_relaxed);
                if(head==tail) {
                    if(!stopWriter.load(std::memory_order_acquire)) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        continue;
                    }
                    // Stop is published after the callback quiesces. The first
                    // read of written may predate that callback's final block.
                    head=written.load(std::memory_order_acquire);
                    if(head==tail)break;
                }
                const auto frames=static_cast<std::uint32_t>(std::min<std::uint64_t>(4096,head-tail));
                if(tail==0 && recovery) {
                    auto warning=recording_recovery::writeStart(
                        directory,recovery->plan.recordingId,firstSample.load(std::memory_order_relaxed),punchWindow);
                    if(!warning.empty())recoveryWarning=std::move(warning);
                }
                for(std::uint32_t frame=0;frame<frames;++frame)for(std::size_t channel=0;channel<inputChannels.size();++channel)
                    readBuffer[static_cast<std::size_t>(frame)*inputChannels.size()+channel]=ring[static_cast<std::size_t>((tail+frame)%spec.ringFrames)*inputChannels.size()+channel];
                read.store(tail+frames,std::memory_order_release);
                for(std::size_t target=0;target<writers.size();++target) {
                    const auto& mapping=mappings[target];auto& values=output[target];
                    for(std::uint32_t frame=0;frame<frames;++frame)for(std::size_t channel=0;channel<mapping.size();++channel)
                        values[static_cast<std::size_t>(frame)*mapping.size()+channel]=readBuffer[static_cast<std::size_t>(frame)*inputChannels.size()+mapping[channel]];
                    writers[target]->write(std::span<const float>(values.data(),static_cast<std::size_t>(frames)*mapping.size()),frames);
                }
                appendPreview(frames);
            }
            drained=true;
        } catch(...) {
            noteWriterError();
        }
        closeWriters();
        // A kept prefix needs the whole write loop and every close to succeed.
        finalized=drained && !writerError;
        publishPreview(Clock::now());
        done.store(true,std::memory_order_release);
    }
    struct Capture {
        std::uint64_t head;
        std::int64_t position;
        std::uint32_t frames;
    };
    std::optional<Capture> prepareCapture(const AudioIo& io,const Transport& transport,RecordingPunch punch,
                                          const RecordingPunchWindow& window,const TempoMap* clock,
                                          std::uint64_t clockGeneration) noexcept {
        if(failure.load(std::memory_order_relaxed)!=RecordingFailure::None || transportEnded.load(std::memory_order_relaxed))return std::nullopt;
        if(begun && !spec.loop && window!=punchWindow){fail(RecordingFailure::Discontinuity);return std::nullopt;}
        if(!transport.playing() || punch==RecordingPunch::After) {
            if(begun)transportEnded.store(true,std::memory_order_release);
            return std::nullopt;
        }
        if(punch==RecordingPunch::Before) {
            if(begun && spec.loop)fail(RecordingFailure::Discontinuity);
            return std::nullopt;
        }
        if(io.frames==0)return std::nullopt;
        if(io.frames<0 || static_cast<std::uint32_t>(io.frames)>spec.maxFrames){fail(RecordingFailure::BlockSize);return std::nullopt;}
        const auto frames=static_cast<std::uint32_t>(io.frames);
        for (std::size_t index = 0; index < physicalChannels; ++index) {
            const auto channel = inputChannels[index];
            if (!io.in || io.numIn <= 0 || channel >= static_cast<std::uint32_t>(io.numIn) || !io.in[channel]) {
                fail(RecordingFailure::MissingInput);
                return std::nullopt;
            }
        }
        const auto position=spec.loop?transport.sampleAt(0):transport.position();
        if(position<0 || io.streamTimeSamples<0 || io.streamTimeSamples>std::numeric_limits<std::int64_t>::max()-frames ||
           (begun && (position!=nextTimeline || io.streamTimeSamples!=nextStream))){fail(RecordingFailure::Discontinuity);return std::nullopt;}
        if(spec.loop) {
            if(!transport.looping() || transport.loopBegin()!=spec.loop->beginSample || transport.loopEnd()!=spec.loop->endSample ||
               (begun && transport.revision()!=transportRevision)){fail(RecordingFailure::Discontinuity);return std::nullopt;}
            // Equal loop endpoints can hide a different tempo inside the brace.
            // Check the immutable map once per graph publication.
            if(spec.loop->tempo && verifiedClockGeneration!=clockGeneration) {
                if(!clock || clock->events!=spec.loop->tempo->events) {
                    fail(RecordingFailure::Discontinuity);
                    return std::nullopt;
                }
                verifiedClockGeneration=clockGeneration;
            }
            // Preflight the entire callback before modifying either log or ring.
            // A callback can span many short loops; callback boundaries do not
            // create passes, and ending exactly at loop end creates no empty row.
            auto needed=passCount;
            auto previousEnd=needed?passes[needed-1].timelineEndSample:std::int64_t{-1};
            for(std::int64_t offset=0;offset<frames;) {
                const auto at=transport.sampleAt(offset),count=transport.contiguousFrames(offset,frames-offset);
                if(count<=0){fail(RecordingFailure::Discontinuity);return std::nullopt;}
                if(previousEnd!=at) {
                    if(needed==passes.size()){fail(RecordingFailure::PassLimit);return std::nullopt;}
                    ++needed;
                }
                previousEnd=at+count;offset+=count;
            }
        }else if(position>std::numeric_limits<std::int64_t>::max()-frames || transport.contiguousFrames(0,frames)!=frames) {
            fail(RecordingFailure::Discontinuity);return std::nullopt;
        }
        const auto head=written.load(std::memory_order_relaxed),tail=read.load(std::memory_order_acquire);
        if(head>std::numeric_limits<std::uint64_t>::max()-frames || head-tail+frames>spec.ringFrames){fail(RecordingFailure::Overflow);return std::nullopt;}
        return Capture{head, position, frames};
    }
    void copyInput(const Capture& slice, const float* const* samples, std::uint32_t sourceBase,
                   std::size_t first, std::size_t end) noexcept {
        if (first == end)
            return;
        for (std::uint32_t frame = 0; frame < slice.frames; ++frame) {
            const auto offset = static_cast<std::size_t>((slice.head + frame) % spec.ringFrames) * inputChannels.size();
            for (auto channel = first; channel < end; ++channel)
                ring[offset + channel] = samples[inputChannels[channel] - sourceBase][frame];
        }
    }
    void publishCapture(const Capture& slice, const AudioIo& io, const Transport& transport,
                        const RecordingPunchWindow& window) noexcept {
        const auto [head, position, frames] = slice;
        if(spec.loop)for(std::int64_t offset=0;offset<frames;) {
            const auto at=transport.sampleAt(offset),count=transport.contiguousFrames(offset,frames-offset);
            if(passCount==0 || passes[passCount-1].timelineEndSample!=at)
                passes[passCount++]={head+static_cast<std::uint64_t>(offset),0,at,at};
            auto& pass=passes[passCount-1];pass.frames+=static_cast<std::uint64_t>(count);pass.timelineEndSample=at+count;offset+=count;
        }
        if(!begun){
            if(!spec.loop)punchWindow=window;
            firstSample.store(position,std::memory_order_relaxed);transportRevision=transport.revision();begun=true;
        }
        nextTimeline=spec.loop?transport.sampleAt(frames):position+frames;nextStream=io.streamTimeSamples+frames;
        written.store(head+frames,std::memory_order_release);
    }
};
struct State final:Sequenced {std::shared_ptr<Job> job;};
}
AudioRecordingResult AudioRecordingResult::fromMemory(const AudioMemorySnapshot& memory,const AudioMemoryTakeSpec& spec) {
    AudioRecordingResult result;result.failure=RecordingFailure::InvalidCapture;fs::path owned;
    try {
        if(audio::onAudioThread)throw std::runtime_error("materialize audio memory off callback");
        if(memory.inputId<=0 || memory.epoch==0 || memory.firstSample<0 || memory.sampleRate<8000 || memory.sampleRate>768000 ||
           memory.channels==0 || memory.channels>64 || memory.samples.empty() || memory.samples.size()%memory.channels!=0 ||
           memory.samples.size()>256u*1024u*1024u/sizeof(float) ||
           memory.frames()>static_cast<std::uint64_t>(memory.sampleRate)*30 ||
           memory.frames()>static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()-memory.firstSample))
            throw std::runtime_error("invalid or empty audio-memory snapshot");
        if(spec.trackId<=0 || spec.startSample<0 || spec.sourceProject.empty() || spec.workFolder.empty() ||
           spec.startSample>(std::int64_t{1}<<50) || memory.frames()>static_cast<std::uint64_t>((std::int64_t{1}<<50)-spec.startSample))
            throw std::runtime_error("audio-memory take needs an explicit project, track, work folder and representable placement");
        result.failure=RecordingFailure::Io;owned=reserve(spec.workFolder);
        const auto file=owned/"take-0.wav";
        audio::WavWriter writer(file,memory.sampleRate,static_cast<std::uint16_t>(memory.channels),audio::WavFormat::Float32);
        writer.write(memory.samples.data(),static_cast<std::uint32_t>(memory.frames()));writer.close();
        result.files_=std::make_shared<Files>();result.files_->directory=owned;
        result.sourceProject_=spec.sourceProject;result.sampleRate=memory.sampleRate;result.startSample=spec.startSample;
        result.frames=memory.frames();result.tracks.push_back({spec.trackId,static_cast<std::uint16_t>(memory.channels),file});
        result.failure=RecordingFailure::None;
    } catch(const std::exception& exception) {
        result.error=exception.what();
        if(!owned.empty()){std::error_code ignored;fs::remove_all(owned,ignored);}
        result.files_.reset();result.tracks.clear();result.frames=0;
    }
    return result;
}
AudioRecordingResult AudioRecordingResult::fromInterrupted(recording_recovery::OwnedPlan owned,
    const fs::path& directory,std::string sourceProject,std::stop_token stop) {
    if(audio::onAudioThread)throw std::runtime_error("recover recording off callback");
    AudioRecordingResult result;
    const auto& plan=owned.plan;
    result.recordingId_=plan.recordingId;
    result.recoveryProjectUuid_=plan.projectUuid;
    result.recoveryWarning_=owned.warning;
    result.recovered_=true;
    result.recoverySource_=directory;
    result.sourceProject_=std::move(sourceProject);
    const auto start=recording_recovery::readStart(directory,plan.recordingId);
    result.startSample=start.sample;
    result.placementOffsetSamples=plan.placementOffsetSamples;
    result.punchWindow_=start.punch;
    result.sampleRate=plan.sampleRate;
    result.loop_=plan.loop;
    std::uint64_t frames=std::numeric_limits<std::uint64_t>::max();
    for(std::size_t i=0;i<plan.targets.size();++i) {
        if(stop.stop_requested())throw std::runtime_error("recording recovery cancelled");
        const auto source=directory/("take-"+std::to_string(i)+".wav");
        const auto channels=static_cast<std::uint16_t>(plan.targets[i].channels.size());
        const auto wave=recording_recovery::inspectWave(source,plan.identities.at(i),
                                                         {plan.sampleRate,channels,plan.format});
        frames=std::min(frames,wave.frames);
    }
    if(result.startSample<0 || result.startSample>(std::int64_t{1}<<50) || frames==0 ||
       frames>static_cast<std::uint64_t>((std::int64_t{1}<<50)-result.startSample))
        throw std::runtime_error("No complete, representable recording to recover");
    result.frames=frames;
    if(plan.loop) {
        auto timeline=result.startSample;
        for(std::uint64_t offset=0;offset<frames;) {
            if(timeline>=plan.loop->endSample || result.passes_.size()>=plan.loop->maxPasses)
                throw std::runtime_error("Interrupted recording exceeds its frozen loop plan");
            const auto length=std::min(frames-offset,static_cast<std::uint64_t>(plan.loop->endSample-timeline));
            const auto end=timeline+static_cast<std::int64_t>(length);
            result.passes_.push_back({offset,length,timeline,end});
            offset+=length;
            timeline=plan.loop->beginSample;
        }
    }
    const auto scratch=reserve(directory.parent_path());
    try {
        for(std::size_t i=0;i<plan.targets.size();++i) {
            const auto file="take-"+std::to_string(i)+".wav";
            const auto channels=static_cast<std::uint16_t>(plan.targets[i].channels.size());
            recording_recovery::copyRepaired(directory/file,plan.identities.at(i),
                {plan.sampleRate,channels,plan.format},scratch/file,frames,stop);
            result.tracks.push_back({plan.targets[i].trackId,channels,scratch/file,plan.targets[i].placementOffsetSamples});
        }
        result.files_=std::make_shared<Files>();
        result.files_->directory=scratch;
        result.files_->recordingOwner=std::move(owned.ownership);
        result.files_->ephemeralStage=true;
    } catch(...) {
        std::error_code ignored;
        fs::remove_all(scratch,ignored);
        throw;
    }
    return result;
}
struct AudioInputRecorder::Impl {
    std::unique_ptr<SnapshotPublisher<State>> published=std::make_unique<SnapshotPublisher<State>>();
    std::shared_ptr<Job> job;
    std::uint64_t stoppedSeq=0;
};
AudioInputRecorder::AudioInputRecorder():impl_(std::make_unique<Impl>()){}
AudioInputRecorder::~AudioInputRecorder()=default;
bool AudioInputRecorder::pending()const noexcept{return static_cast<bool>(impl_->job);}
std::uint64_t AudioInputRecorder::acceptedFrames()const noexcept{return impl_->job?impl_->job->written.load(std::memory_order_acquire):0;}
std::shared_ptr<const AudioRecordingPreview> AudioInputRecorder::preview()const noexcept {
    return impl_->job ? impl_->job->latestPreview() : nullptr;
}
bool AudioInputRecorder::start(const AudioRecordingSpec& spec,std::string& error,AudioRecordingSinkFactory factory) {
    fs::path owned;
    try {
        if(audio::onAudioThread)throw std::runtime_error("start recording off callback");
        if(impl_->job)throw std::runtime_error("a recording is still pending");
        if(!spec.recoveryProjectUuid.empty() && (spec.sourceProject.empty() || factory))
            throw std::runtime_error("recoverable recording needs a project and the built-in WAV writer");
        if(spec.workFolder.empty() || spec.sampleRate<8000 || spec.sampleRate>768000 || spec.maxFrames==0 || spec.maxFrames>16384 ||
           spec.ringFrames<spec.maxFrames || spec.targets.empty() || spec.targets.size()>256)throw std::runtime_error("invalid recording format or buffer bounds");
        if(spec.format!=audio::WavFormat::Float32 && spec.format!=audio::WavFormat::Pcm16 && spec.format!=audio::WavFormat::Pcm24)throw std::runtime_error("invalid recording encoding");
        if(spec.loop && (spec.loop->beginSample<0 || spec.loop->endSample<=spec.loop->beginSample || spec.loop->maxPasses==0 || spec.loop->maxPasses>65536))
            throw std::runtime_error("invalid recording loop bounds or pass capacity");
        auto job=std::make_shared<Job>();job->spec=spec;std::set<std::int64_t> targetIds;std::set<std::uint32_t> channels;
        if(spec.loop)job->passes.resize(spec.loop->maxPasses);
        for(const auto& target:spec.targets) {
            if(target.trackId<=0 || !targetIds.insert(target.trackId).second || target.channels.empty() || target.channels.size()>64)throw std::runtime_error("invalid or duplicate recording track");
            std::set<std::uint32_t> unique;
            for(const auto channel:target.channels)if(channel>=64 || !unique.insert(channel).second)throw std::runtime_error("invalid or duplicate input channel");
            if (target.source == RecordingSource::MainMix) {
                if (target.channels.size() != 2 || target.channels[0] != 0 || target.channels[1] != 1)
                    throw std::runtime_error("Resampling needs the stereo Main mix");
                job->resamplingTracks.push_back(target.trackId);
                channels.insert(kMainChannelBase);
                channels.insert(kMainChannelBase + 1);
            } else
                channels.insert(target.channels.begin(),target.channels.end());
        }
        if(static_cast<std::uint64_t>(spec.ringFrames)*channels.size()*sizeof(float)>256ULL*1024*1024)throw std::runtime_error("recording ring exceeds memory bound");
        job->trackIds.assign(targetIds.begin(),targetIds.end());
        job->inputChannels.assign(channels.begin(),channels.end());job->ring.resize(static_cast<std::size_t>(spec.ringFrames)*channels.size());job->readBuffer.resize(4096*channels.size());
        job->physicalChannels = channels.size();
        if (!job->resamplingTracks.empty()) {
            std::sort(job->resamplingTracks.begin(), job->resamplingTracks.end());
            job->physicalChannels -= 2;
            job->mainBuffer.resize(static_cast<std::size_t>(spec.maxFrames) * 2);
            job->mainOutput = {job->mainBuffer.data(), job->mainBuffer.data() + spec.maxFrames};
        }
        owned=reserve(spec.workFolder);
        if(!spec.recoveryProjectUuid.empty())owned=fs::canonical(owned);
        job->directory=owned;
        for(std::size_t index=0;index<spec.targets.size();++index) {
            const auto& target=spec.targets[index];const auto file=owned/("take-"+std::to_string(index)+".wav");
            const auto width=static_cast<std::uint16_t>(target.channels.size());
            job->tracks.push_back({target.trackId,width,file,target.placementOffsetSamples});
            std::vector<std::size_t> mapping;
            const auto base = target.source == RecordingSource::MainMix ? kMainChannelBase : 0U;
            for(const auto channel:target.channels)mapping.push_back(static_cast<std::size_t>(std::lower_bound(job->inputChannels.begin(),job->inputChannels.end(),channel + base)-job->inputChannels.begin()));
            job->mappings.push_back(std::move(mapping));job->output.emplace_back(4096*target.channels.size());
            auto writer=factory?factory(file,spec.sampleRate,width,spec.format):std::make_unique<WaveSink>(file,spec.sampleRate,width,spec.format);
            if(!writer)throw std::runtime_error("recording writer was not created");
            job->writers.push_back(std::move(writer));
        }
        if(!spec.recoveryProjectUuid.empty()) {
            job->recovery=recording_recovery::begin(spec,owned,spec.recoveryProjectUuid);
            job->recoveryWarning=job->recovery->warning;
            auto launchWarning=recording_recovery::registerLaunchTake(owned,job->recovery->plan);
            if(!launchWarning.empty())job->recoveryWarning=std::move(launchWarning);
        }
        job->worker=std::thread([raw=job.get()]{raw->run();});
        auto state=std::make_unique<State>();state->job=job;impl_->published->publish(std::move(state));impl_->job=std::move(job);impl_->stoppedSeq=0;error.clear();return true;
    }catch(const std::exception& exception){error=exception.what();if(!owned.empty()){std::error_code cleanup;fs::remove_all(owned,cleanup);}return false;}
}
void AudioInputRecorder::stop(bool cancel) {
    if(!impl_->job)return;
    if(cancel){impl_->job->discardRequested=true;impl_->job->fail(RecordingFailure::Cancelled);}
    if(impl_->stoppedSeq==0){impl_->published->publish(std::make_unique<State>());impl_->stoppedSeq=impl_->published->publishedSeq();}
}
std::optional<AudioRecordingResult> AudioInputRecorder::poll(bool deviceStopped) {
    auto& job=impl_->job;if(!job)return {};
    if(job->failure.load(std::memory_order_acquire)!=RecordingFailure::None || job->transportEnded.load(std::memory_order_acquire))stop();
    if(!impl_->stoppedSeq || (!deviceStopped && impl_->published->inUseSeq()<impl_->stoppedSeq))return {};
    job->stopWriter.store(true,std::memory_order_release);
    if(!job->done.load(std::memory_order_acquire))return {};
    if(job->worker.joinable())job->worker.join();
    AudioRecordingResult result;result.failure=job->failure.load(std::memory_order_acquire);result.sampleRate=job->spec.sampleRate;
    result.frames=job->written.load(std::memory_order_acquire);result.startSample=job->firstSample.load(std::memory_order_acquire);result.tracks=job->tracks;
    result.placementOffsetSamples=job->spec.placementOffsetSamples;
    result.finalizedPrefix_=job->finalized;
    result.sourceProject_=job->spec.sourceProject;
    result.loop_=job->spec.loop;
    result.punchWindow_=job->punchWindow;
    result.passes_.assign(job->passes.begin(),job->passes.begin()+static_cast<std::ptrdiff_t>(job->passCount));
    if(job->discardRequested) {
        result.failure=RecordingFailure::Cancelled;
    } else if(job->writerError) {
        try {
            std::rethrow_exception(job->writerError);
        } catch(const std::exception& exception) {
            result.error=exception.what();
        } catch(...) {
            result.error="Recording writer failed";
        }
    }
    if(!job->discardRequested)result.preview=job->latestPreview();
    if(result.failure==RecordingFailure::None && result.frames==0)result.failure=RecordingFailure::NoFrames;
    if(result.error.empty())switch(result.failure) {
        case RecordingFailure::None:break;
        case RecordingFailure::Overflow:result.error="Recording writer could not keep up";break;
        case RecordingFailure::MissingInput:result.error="A selected recording input is unavailable";break;
        case RecordingFailure::BlockSize:result.error="Audio callback exceeds the recording block size";break;
        case RecordingFailure::Discontinuity:result.error="Recording stopped at a transport or stream discontinuity";break;
        case RecordingFailure::Cancelled:result.error="Recording cancelled";break;
        case RecordingFailure::Io:result.error="Recording writer failed";break;
        case RecordingFailure::NoFrames:result.error="No audio was recorded";break;
        case RecordingFailure::InvalidCapture:result.error="Invalid retrospective audio capture";break;
        case RecordingFailure::PassLimit:result.error="Recording loop pass capacity exhausted";break;
    }
    result.files_=std::make_shared<AudioRecordingResult::Files>();result.files_->directory=job->directory;
    if(job->recovery) {
        result.recordingId_=job->recovery->plan.recordingId;
        result.recoveryProjectUuid_=job->recovery->plan.projectUuid;
        result.recoveryWarning_=std::move(job->recoveryWarning);
        result.files_->recordingOwner=std::move(job->recovery->ownership);
        result.files_->hasOriginalPlan=true;
    }
    job.reset();
    if(deviceStopped)impl_->published=std::make_unique<SnapshotPublisher<State>>();
    else impl_->published->collect();
    impl_->stoppedSeq=0;
    if(result.failure==RecordingFailure::Cancelled) {
        if(const auto error=result.discard();error)
            result.error+=". Could not remove recording files: "+error.message();
    }
    return result;
}
std::span<const std::int64_t> AudioInputRecorder::capture(const AudioIo& io,const Transport& transport,
                                                        RecordingPunch punch,const RecordingPunchWindow& window,
                                                        const TempoMap* clock,std::uint64_t clockGeneration)noexcept {
    SnapshotPublisher<State>::AudioRead state(*impl_->published);
    if (!state.valid() || !state->job)
        return {};
    auto& job = *state->job;
    if (!job.resamplingTracks.empty()) {
        job.fail(RecordingFailure::MissingInput);
        return {};
    }
    if (const auto slice = job.prepareCapture(io, transport, punch, window, clock, clockGeneration)) {
        job.copyInput(*slice, io.in, 0, 0, job.physicalChannels);
        job.publishCapture(*slice, io, transport, window);
        return job.trackIds;
    }
    return {};
}
AudioInputRecorder::SliceResult AudioInputRecorder::captureAndRender(
    const AudioIo& io, const Transport& transport, RecordingPunch punch, const RecordingPunchWindow& window,
    const TempoMap* clock, std::uint64_t clockGeneration, RenderSlice render) noexcept {
    if (!render.process)
        return {};
    SnapshotPublisher<State>::AudioRead state(*impl_->published);
    if (!state.valid() || !state->job)
        return {render.process(render.context, {}, {}, nullptr), false};
    auto& job = *state->job;
    const auto slice = job.prepareCapture(io, transport, punch, window, clock, clockGeneration);
    if (!slice)
        return {render.process(render.context, {}, {}, nullptr), false};
    // Physical buffers may alias the device's outputs. Stage them before render
    // but publish nothing until every source for this slice has been copied.
    job.copyInput(*slice, io.in, 0, 0, job.physicalChannels);
    if (job.resamplingTracks.empty()) {
        job.publishCapture(*slice, io, transport, window);
        return {render.process(render.context, job.trackIds, {}, nullptr), true};
    }
    AudioIo main;
    main.out = job.mainOutput.data();
    main.numOut = 2;
    main.frames = io.frames;
    if (!render.process(render.context, job.trackIds, job.resamplingTracks, &main)) {
        job.fail(RecordingFailure::MissingInput);
        return {false, false};
    }
    const std::array<const float*, 2> stereo{job.mainOutput[0], job.mainOutput[1]};
    job.copyInput(*slice, stereo.data(), kMainChannelBase, job.physicalChannels, job.inputChannels.size());
    job.publishCapture(*slice, io, transport, window);
    return {true, true};
}
}
