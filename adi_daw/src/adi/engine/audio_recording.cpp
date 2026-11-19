// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/audio_recording.hpp"
#include "adi/engine/audio_memory.hpp"
#include "adi/engine/publisher.hpp"
#include "adi/engine/transport.hpp"
#include "adi/audio/io_audit.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <set>
#include <stdexcept>
#include <thread>
namespace adi::engine {
namespace fs=std::filesystem;
struct AudioRecordingResult::Files {fs::path directory;};
const fs::path& AudioRecordingResult::directory()const noexcept {static const fs::path empty;return files_?files_->directory:empty;}
void AudioRecordingResult::discard(){if(audio::onAudioThread)throw std::runtime_error("discard recording off callback");if(files_){fs::remove_all(files_->directory);files_.reset();}}
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
    AudioRecordingSpec spec;
    fs::path directory;
    std::vector<RecordedTrack> tracks;
    std::vector<std::uint32_t> inputChannels;
    std::vector<std::vector<std::size_t>> mappings;
    std::vector<std::unique_ptr<AudioRecordingSink>> writers;
    std::vector<std::vector<float>> output;
    std::vector<float> ring,readBuffer;
    std::atomic<std::uint64_t> written{0},read{0};
    std::atomic<std::int64_t> firstSample{-1};
    std::atomic<RecordingFailure> failure{RecordingFailure::None};
    std::atomic<bool> stopWriter{false},done{false},transportEnded{false};
    std::thread worker;
    std::string error;
    bool discardRequested=false; // owner only; cancellation also discards failed takes
    // Callback-owned continuity state; observed only after its quiescence.
    bool begun=false;
    std::int64_t nextTimeline=0,nextStream=0;
    void fail(RecordingFailure value)noexcept{auto expected=RecordingFailure::None;failure.compare_exchange_strong(expected,value,std::memory_order_relaxed);}
    ~Job(){stopWriter.store(true,std::memory_order_release);if(worker.joinable())worker.join();}
    void run()noexcept {
        try {
            while(true) {
                const auto head=written.load(std::memory_order_acquire),tail=read.load(std::memory_order_relaxed);
                if(head==tail) {
                    if(stopWriter.load(std::memory_order_acquire))break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));continue;
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
            }
            for(auto& writer:writers)writer->close();
        } catch(const std::exception& exception){error=exception.what();fail(RecordingFailure::Io);}
        catch(...){error="recording writer failed";fail(RecordingFailure::Io);}
        done.store(true,std::memory_order_release);
    }
    void capture(const AudioIo& io,const Transport& transport)noexcept {
        if(failure.load(std::memory_order_relaxed)!=RecordingFailure::None || transportEnded.load(std::memory_order_relaxed))return;
        if(!transport.playing()){if(begun)transportEnded.store(true,std::memory_order_release);return;}
        if(io.frames==0)return;
        if(io.frames<0 || static_cast<std::uint32_t>(io.frames)>spec.maxFrames){fail(RecordingFailure::BlockSize);return;}
        const auto frames=static_cast<std::uint32_t>(io.frames);
        for(const auto channel:inputChannels)if(!io.in || io.numIn<=0 || channel>=static_cast<std::uint32_t>(io.numIn) || !io.in[channel]){fail(RecordingFailure::MissingInput);return;}
        const auto position=transport.position();
        if(position<0 || io.streamTimeSamples<0 || position>std::numeric_limits<std::int64_t>::max()-frames ||
           io.streamTimeSamples>std::numeric_limits<std::int64_t>::max()-frames || transport.contiguousFrames(0,frames)!=frames ||
           (begun && (position!=nextTimeline || io.streamTimeSamples!=nextStream))){fail(RecordingFailure::Discontinuity);return;}
        const auto head=written.load(std::memory_order_relaxed),tail=read.load(std::memory_order_acquire);
        if(head>std::numeric_limits<std::uint64_t>::max()-frames || head-tail+frames>spec.ringFrames){fail(RecordingFailure::Overflow);return;}
        for(std::uint32_t frame=0;frame<frames;++frame)for(std::size_t channel=0;channel<inputChannels.size();++channel)
            ring[static_cast<std::size_t>((head+frame)%spec.ringFrames)*inputChannels.size()+channel]=io.in[inputChannels[channel]][frame];
        if(!begun){firstSample.store(position,std::memory_order_relaxed);begun=true;}
        nextTimeline=position+frames;nextStream=io.streamTimeSamples+frames;
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
struct AudioInputRecorder::Impl {
    std::unique_ptr<SnapshotPublisher<State>> published=std::make_unique<SnapshotPublisher<State>>();
    std::shared_ptr<Job> job;
    std::uint64_t stoppedSeq=0;
};
AudioInputRecorder::AudioInputRecorder():impl_(std::make_unique<Impl>()){}
AudioInputRecorder::~AudioInputRecorder()=default;
bool AudioInputRecorder::pending()const noexcept{return static_cast<bool>(impl_->job);}
std::uint64_t AudioInputRecorder::acceptedFrames()const noexcept{return impl_->job?impl_->job->written.load(std::memory_order_acquire):0;}
bool AudioInputRecorder::start(const AudioRecordingSpec& spec,std::string& error,AudioRecordingSinkFactory factory) {
    fs::path owned;
    try {
        if(audio::onAudioThread)throw std::runtime_error("start recording off callback");
        if(impl_->job)throw std::runtime_error("a recording is still pending");
        if(spec.workFolder.empty() || spec.sampleRate<8000 || spec.sampleRate>768000 || spec.maxFrames==0 || spec.maxFrames>16384 ||
           spec.ringFrames<spec.maxFrames || spec.targets.empty() || spec.targets.size()>256)throw std::runtime_error("invalid recording format or buffer bounds");
        if(spec.format!=audio::WavFormat::Float32 && spec.format!=audio::WavFormat::Pcm16 && spec.format!=audio::WavFormat::Pcm24)throw std::runtime_error("invalid recording encoding");
        auto job=std::make_shared<Job>();job->spec=spec;std::set<std::int64_t> targetIds;std::set<std::uint32_t> channels;
        for(const auto& target:spec.targets) {
            if(target.trackId<=0 || !targetIds.insert(target.trackId).second || target.channels.empty() || target.channels.size()>64)throw std::runtime_error("invalid or duplicate recording track");
            std::set<std::uint32_t> unique;
            for(const auto channel:target.channels)if(channel>=64 || !unique.insert(channel).second)throw std::runtime_error("invalid or duplicate input channel");
            channels.insert(target.channels.begin(),target.channels.end());
        }
        if(static_cast<std::uint64_t>(spec.ringFrames)*channels.size()*sizeof(float)>256ULL*1024*1024)throw std::runtime_error("recording ring exceeds memory bound");
        job->inputChannels.assign(channels.begin(),channels.end());job->ring.resize(static_cast<std::size_t>(spec.ringFrames)*channels.size());job->readBuffer.resize(4096*channels.size());
        owned=reserve(spec.workFolder);job->directory=owned;
        for(std::size_t index=0;index<spec.targets.size();++index) {
            const auto& target=spec.targets[index];const auto file=owned/("take-"+std::to_string(index)+".wav");
            const auto width=static_cast<std::uint16_t>(target.channels.size());
            job->tracks.push_back({target.trackId,width,file});std::vector<std::size_t> mapping;
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
    AudioRecordingResult result;result.failure=job->failure.load(std::memory_order_acquire);result.error=job->error;result.sampleRate=job->spec.sampleRate;
    result.frames=job->written.load(std::memory_order_acquire);result.startSample=job->firstSample.load(std::memory_order_acquire);result.tracks=job->tracks;
    result.sourceProject_=job->spec.sourceProject;
    if(job->discardRequested){result.failure=RecordingFailure::Cancelled;result.error.clear();}
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
    }
    result.files_=std::make_shared<AudioRecordingResult::Files>();result.files_->directory=job->directory;
    if(result.failure==RecordingFailure::Cancelled)result.discard();
    job.reset();
    if(deviceStopped)impl_->published=std::make_unique<SnapshotPublisher<State>>();
    else impl_->published->collect();
    impl_->stoppedSeq=0;return result;
}
void AudioInputRecorder::capture(const AudioIo& io,const Transport& transport)noexcept {
    SnapshotPublisher<State>::AudioRead state(*impl_->published);
    if(state.valid() && state->job)state->job->capture(io,transport);
}
}
