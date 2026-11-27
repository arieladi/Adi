// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/audio_recording.hpp"
#include "adi/engine/audio_memory.hpp"
#include "adi/engine/publisher.hpp"
#include "adi/engine/snapshot.hpp"
#include "adi/engine/transport.hpp"
#include "adi/audio/io_audit.hpp"
#include <algorithm>
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
    // A punched take keeps the original sample mapping even when the correction
    // crosses song zero. An unpunched take retains the song-zero placement rule.
    const auto start = punch.active() && offsetSamples > startSample ? startSample-offsetSamples : *placed;
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

AudioRecordingPreview::Range AudioRecordingPreview::latestPass(std::uint64_t acceptedFrames,
    std::optional<std::int64_t> trackOffsetSamples) const noexcept {
    const auto frames = std::max(peaks.frames, acceptedFrames);
    if (!loop && trackOffsetSamples) {
        const auto placed = recordingPlacementRange(rawStartSample, frames, *trackOffsetSamples, {});
        return placed ? Range{placed->timelineStartSample, 0, placed->frames} : Range{startSample, 0, 0};
    }
    if (!loop || loop->beginSample < 0 || loop->endSample <= loop->beginSample ||
        startSample < 0 || startSample >= loop->endSample)
        return {startSample, 0, frames};
    const auto firstFrames = static_cast<std::uint64_t>(loop->endSample - startSample);
    if (frames <= firstFrames)
        return {startSample, 0, frames};
    const auto loopFrames = static_cast<std::uint64_t>(loop->endSample - loop->beginSample);
    // An exact wrap still shows the last complete pass until the next one
    // has audio. Source offsets include a partial first pass.
    const auto passFrames = (frames - firstFrames - 1) % loopFrames + 1;
    return {loop->beginSample, frames - passFrames, passFrames};
}

struct AudioRecordingResult::Files {fs::path directory;};
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
    fs::remove_all(files_->directory,removalError);
    if(!removalError)files_.reset();
    return removalError;
}
namespace {
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
    std::vector<std::int64_t> trackIds;
    std::vector<std::uint32_t> inputChannels;
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
                const auto offset = spec.loop ? 0 : spec.placementOffsetSamples;
                accumulatingPreview.rawStartSample = start;
                accumulatingPreview.startSample = recordingPlacementStart(start, offset).value_or(start);
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
                    if (!spec.loop)
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
    bool capture(const AudioIo& io,const Transport& transport,RecordingPunch punch,
                 const RecordingPunchWindow& window,const TempoMap* clock,std::uint64_t clockGeneration)noexcept {
        if(failure.load(std::memory_order_relaxed)!=RecordingFailure::None || transportEnded.load(std::memory_order_relaxed))return false;
        if(begun && !spec.loop && window!=punchWindow){fail(RecordingFailure::Discontinuity);return false;}
        if(!transport.playing() || punch==RecordingPunch::After) {
            if(begun)transportEnded.store(true,std::memory_order_release);
            return false;
        }
        if(punch==RecordingPunch::Before) {
            if(begun && spec.loop)fail(RecordingFailure::Discontinuity);
            return false;
        }
        if(io.frames==0)return false;
        if(io.frames<0 || static_cast<std::uint32_t>(io.frames)>spec.maxFrames){fail(RecordingFailure::BlockSize);return false;}
        const auto frames=static_cast<std::uint32_t>(io.frames);
        for(const auto channel:inputChannels)if(!io.in || io.numIn<=0 || channel>=static_cast<std::uint32_t>(io.numIn) || !io.in[channel]){fail(RecordingFailure::MissingInput);return false;}
        const auto position=spec.loop?transport.sampleAt(0):transport.position();
        if(position<0 || io.streamTimeSamples<0 || io.streamTimeSamples>std::numeric_limits<std::int64_t>::max()-frames ||
           (begun && (position!=nextTimeline || io.streamTimeSamples!=nextStream))){fail(RecordingFailure::Discontinuity);return false;}
        if(spec.loop) {
            if(!transport.looping() || transport.loopBegin()!=spec.loop->beginSample || transport.loopEnd()!=spec.loop->endSample ||
               (begun && transport.revision()!=transportRevision)){fail(RecordingFailure::Discontinuity);return false;}
            // Equal loop endpoints can hide a different tempo inside the brace.
            // Check the immutable map once per graph publication.
            if(spec.loop->tempo && verifiedClockGeneration!=clockGeneration) {
                if(!clock || clock->events!=spec.loop->tempo->events) {
                    fail(RecordingFailure::Discontinuity);
                    return false;
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
                if(count<=0){fail(RecordingFailure::Discontinuity);return false;}
                if(previousEnd!=at) {
                    if(needed==passes.size()){fail(RecordingFailure::PassLimit);return false;}
                    ++needed;
                }
                previousEnd=at+count;offset+=count;
            }
        }else if(position>std::numeric_limits<std::int64_t>::max()-frames || transport.contiguousFrames(0,frames)!=frames) {
            fail(RecordingFailure::Discontinuity);return false;
        }
        const auto head=written.load(std::memory_order_relaxed),tail=read.load(std::memory_order_acquire);
        if(head>std::numeric_limits<std::uint64_t>::max()-frames || head-tail+frames>spec.ringFrames){fail(RecordingFailure::Overflow);return false;}
        if(spec.loop)for(std::int64_t offset=0;offset<frames;) {
            const auto at=transport.sampleAt(offset),count=transport.contiguousFrames(offset,frames-offset);
            if(passCount==0 || passes[passCount-1].timelineEndSample!=at)
                passes[passCount++]={head+static_cast<std::uint64_t>(offset),0,at,at};
            auto& pass=passes[passCount-1];pass.frames+=static_cast<std::uint64_t>(count);pass.timelineEndSample=at+count;offset+=count;
        }
        for(std::uint32_t frame=0;frame<frames;++frame)for(std::size_t channel=0;channel<inputChannels.size();++channel)
            ring[static_cast<std::size_t>((head+frame)%spec.ringFrames)*inputChannels.size()+channel]=io.in[inputChannels[channel]][frame];
        if(!begun){
            if(!spec.loop)punchWindow=window;
            firstSample.store(position,std::memory_order_relaxed);transportRevision=transport.revision();begun=true;
        }
        nextTimeline=spec.loop?transport.sampleAt(frames):position+frames;nextStream=io.streamTimeSamples+frames;
        written.store(head+frames,std::memory_order_release);
        return true;
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
            channels.insert(target.channels.begin(),target.channels.end());
        }
        if(static_cast<std::uint64_t>(spec.ringFrames)*channels.size()*sizeof(float)>256ULL*1024*1024)throw std::runtime_error("recording ring exceeds memory bound");
        job->trackIds.assign(targetIds.begin(),targetIds.end());
        job->inputChannels.assign(channels.begin(),channels.end());job->ring.resize(static_cast<std::size_t>(spec.ringFrames)*channels.size());job->readBuffer.resize(4096*channels.size());
        owned=reserve(spec.workFolder);job->directory=owned;
        for(std::size_t index=0;index<spec.targets.size();++index) {
            const auto& target=spec.targets[index];const auto file=owned/("take-"+std::to_string(index)+".wav");
            const auto width=static_cast<std::uint16_t>(target.channels.size());
            job->tracks.push_back({target.trackId,width,file,spec.loop ? std::nullopt : target.placementOffsetSamples});
            std::vector<std::size_t> mapping;
            for(const auto channel:target.channels)mapping.push_back(static_cast<std::size_t>(std::lower_bound(job->inputChannels.begin(),job->inputChannels.end(),channel)-job->inputChannels.begin()));
            job->mappings.push_back(std::move(mapping));job->output.emplace_back(4096*target.channels.size());
            auto writer=factory?factory(file,spec.sampleRate,width,spec.format):std::make_unique<WaveSink>(file,spec.sampleRate,width,spec.format);
            if(!writer)throw std::runtime_error("recording writer was not created");
            job->writers.push_back(std::move(writer));
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
    if(state.valid() && state->job && state->job->capture(io,transport,punch,window,clock,clockGeneration))
        return state->job->trackIds;
    return {};
}
}
