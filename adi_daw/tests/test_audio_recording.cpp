// SPDX-License-Identifier: AGPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/engine/audio_recording.hpp"
#include "adi/engine/transport.hpp"
#include "adi/engine/session.hpp"
#include "adi/engine/midi_input_capture.hpp"
#include "adi/ops.hpp"
#include "adi/store.hpp"
#include "adi/textproj.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include "adi/audio/io_audit.hpp"
#include "juce/device_core.hpp"
#include <algorithm>
#include <atomic>
#include <array>
#include <bit>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <limits>
#include <new>
#include <thread>
#include <utility>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
namespace {std::atomic<unsigned> callbackAllocations{0};}
#if defined(__GNUC__) && !defined(__clang__)
#define ADI_TEST_NOINLINE __attribute__((noinline))
#else
#define ADI_TEST_NOINLINE
#endif
ADI_TEST_NOINLINE void* operator new(std::size_t size){if(adi::audio::onAudioThread)++callbackAllocations;if(void* p=std::malloc(size?size:1))return p;throw std::bad_alloc();}
ADI_TEST_NOINLINE void* operator new[](std::size_t size){return operator new(size);}
ADI_TEST_NOINLINE void operator delete(void* p)noexcept{std::free(p);}ADI_TEST_NOINLINE void operator delete[](void* p)noexcept{std::free(p);}
ADI_TEST_NOINLINE void operator delete(void* p,std::size_t)noexcept{std::free(p);}ADI_TEST_NOINLINE void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
#undef ADI_TEST_NOINLINE
namespace {
using namespace adi;using namespace adi::engine;
int checks=0,failures=0;
void check(bool ok,const char* what){++checks;if(!ok){++failures;std::printf("FAIL %s\n",what);}}
AudioRecordingResult finish(AudioInputRecorder& recorder,bool stopped=false) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(std::chrono::steady_clock::now()<deadline){if(auto result=recorder.poll(stopped))return *result;std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    throw std::runtime_error("recording did not finalize");
}
void input(AudioInputRecorder& recorder,Transport& transport,const AudioIo& io) {
    {audio::CallbackScope callback;recorder.capture(io,transport);}transport.advance(io.frames);
}
AudioRecordingSpec spec(const std::filesystem::path& folder) {
    AudioRecordingSpec result;result.workFolder=folder;result.maxFrames=128;result.ringFrames=4096;result.targets={{21,{2,0}},{22,{0}}};return result;
}
void actualFilesAndAcknowledgment() {
    for(const auto rate:{44100U,48000U}) {
        test::TempDirectory directory{"audio_recording","inputs"};AudioInputRecorder recorder;auto request=spec(directory.path());request.sampleRate=rate;std::string error;
        check(recorder.start(request,error),"start real multitrack WAV capture before callback publication");
        Transport transport;transport.locate(123);transport.play();std::array<float,128> first{},third{};const float* channels[]={first.data(),nullptr,third.data()};
        std::vector<float> expectedMono,expectedStereo;
        std::uint64_t stream=0;
        for(const auto count:{17,128,1,63,127,5}) {
            for(int frame=0;frame<count;++frame){first[static_cast<std::size_t>(frame)]=static_cast<float>(stream+static_cast<std::uint64_t>(frame))/1024;third[static_cast<std::size_t>(frame)]=-first[static_cast<std::size_t>(frame)]-0.25f;expectedMono.push_back(first[static_cast<std::size_t>(frame)]);expectedStereo.push_back(third[static_cast<std::size_t>(frame)]);expectedStereo.push_back(first[static_cast<std::size_t>(frame)]);}
            AudioIo io;io.in=channels;io.numIn=3;io.frames=count;io.streamTimeSamples=static_cast<std::int64_t>(stream);input(recorder,transport,io);stream+=static_cast<std::uint64_t>(count);
        }
        check(recorder.acceptedFrames()==stream,"variable callbacks accept exactly the supplied frames");recorder.stop();
        check(!recorder.poll(),"stop cannot finalize while the callback may still hold the old capture generation");
        AudioIo idle;idle.frames=1;input(recorder,transport,idle);auto result=finish(recorder);
        check(result.failure==RecordingFailure::None && result.frames==stream && result.startSample==123 && result.sampleRate==rate && result.tracks.size()==2,"acknowledged stop drains exact start, rate, frame count and all tracks");
        std::vector<float> mono(expectedMono.size()),stereo(expectedStereo.size());
        {audio::WavReader reader(result.tracks[0].file);check(reader.channels()==2 && reader.frames()==stream && reader.sampleRate()==rate && reader.read(stereo.data(),static_cast<std::uint32_t>(stream))==stream,"stereo take has finalized native WAV dimensions");}
        {audio::WavReader reader(result.tracks[1].file);check(reader.channels()==1 && reader.read(mono.data(),static_cast<std::uint32_t>(stream))==stream,"second take reads the chosen shared mono input");}
        check(stereo==expectedStereo && mono==expectedMono,"actual recorded Float32 bits preserve nonadjacent channel ordering and every final partial block");
        check(!recorder.pending() && !recorder.poll(),"completed recording is returned once and releases pending state");
        const auto owned=result.directory();check(std::filesystem::exists(owned),"completed files remain available for captured import or recovery");result.discard();
        check(!std::filesystem::exists(owned),"explicit discard removes only the recording's owned staging directory");
    }
}
void stopDrainsLastShortBlock() {
    test::TempDirectory directory{"audio_recording","stop_short_block"};
    auto request=spec(directory.path());
    request.targets={{21,{0}}};
    const std::array<float,8> expected{-0.25f,0.125f,0.5f,0.0f,-0.75f,0.25f,-0.5f,0.75f};
    const float* channels[]={expected.data()};
    AudioIo block;block.in=channels;block.numIn=1;block.frames=8;
    for(const bool deviceStopped:{false,true}) {
        for(unsigned take=0;take<4;++take) {
            AudioInputRecorder recorder;
            Transport transport;
            transport.play();
            std::string error;
            if(!recorder.start(request,error))throw std::runtime_error(error);
            input(recorder,transport,block);
            recorder.stop();
            if(!deviceStopped) {
                AudioIo idle;idle.frames=1;
                input(recorder,transport,idle);
            }
            auto result=finish(recorder,deviceStopped);
            check(result.failure==RecordingFailure::None && result.frames==expected.size(),
                  "immediate Stop finalizes all accepted recording frames");
            {
                audio::WavReader reader(result.tracks.at(0).file);
                std::array<float,8> heard{};
                const auto frames=reader.read(heard.data(),8);
                check(reader.channels()==1 && reader.frames()==expected.size() && frames==expected.size() && heard==expected,
                      "the finalized short WAV includes the exact last block");
            }
            result.discard();
        }
    }
}
void formatAndInputFailures() {
    test::TempDirectory directory{"audio_recording","failures"};AudioInputRecorder recorder;std::string error;auto request=spec(directory.path());
    auto invalid=request;invalid.targets[1].trackId=21;check(!recorder.start(invalid,error),"duplicate destination track refuses before publication");
    invalid=request;invalid.targets[0].channels={0,0};check(!recorder.start(invalid,error),"duplicate input in one take refuses");
    invalid=request;invalid.ringFrames=1;check(!recorder.start(invalid,error),"ring smaller than maximum callback refuses");
    invalid=request;invalid.ringFrames=0xffffffffU;check(!recorder.start(invalid,error),"excessive ring memory refuses before allocation");
    check(recorder.start(request,error),"start a capture to test missing hardware inputs");Transport transport;transport.play();AudioIo io;io.frames=8;input(recorder,transport,io);recorder.stop();input(recorder,transport,io);
    auto missing=finish(recorder);check(missing.failure==RecordingFailure::MissingInput && missing.frames==0,"missing selected hardware input is an explicit failure without invented silence");missing.discard();
    check(recorder.start(request,error),"start a capture before cancelling an input failure");input(recorder,transport,io);recorder.stop(true);input(recorder,transport,io);auto discardedFailure=finish(recorder);
    check(discardedFailure.failure==RecordingFailure::Cancelled && discardedFailure.directory().empty(),"explicit cancel also cleans an already failed partial take after quiescence");
    check(recorder.start(request,error),"start an empty capture");recorder.stop();input(recorder,transport,io);auto empty=finish(recorder);check(empty.failure==RecordingFailure::NoFrames,"empty capture is never presented as a playable recording");empty.discard();
    check(recorder.start(request,error),"start a capture that is cancelled");recorder.stop(true);input(recorder,transport,io);auto cancelled=finish(recorder);check(cancelled.failure==RecordingFailure::Cancelled && cancelled.directory().empty(),"cancel waits for acknowledgment/finalization before deleting its owned work");
}
struct Blocked {std::mutex mutex;std::condition_variable condition;bool entered=false,release=false;};
class BlockingSink final:public AudioRecordingSink {
    std::shared_ptr<Blocked> blocked_;
public:
    explicit BlockingSink(std::shared_ptr<Blocked> blocked):blocked_(std::move(blocked)){}
    void write(std::span<const float>,std::uint32_t)override{std::unique_lock lock(blocked_->mutex);blocked_->entered=true;blocked_->condition.notify_all();blocked_->condition.wait(lock,[&]{return blocked_->release;});}
    void close()override{}
};
class ObservedWaveSink final:public AudioRecordingSink {
    audio::WavWriter writer_;std::shared_ptr<std::atomic<std::uint64_t>> drained_;
public:
    ObservedWaveSink(const std::filesystem::path& file,std::uint32_t rate,std::uint16_t channels,audio::WavFormat format,std::shared_ptr<std::atomic<std::uint64_t>> drained):writer_(file,rate,channels,format),drained_(std::move(drained)){}
    void write(std::span<const float> values,std::uint32_t frames)override{writer_.write(values.data(),frames);drained_->fetch_add(frames,std::memory_order_release);}
    void close()override{writer_.close();}
};
void actualQueueWraps() {
    test::TempDirectory directory{"audio_recording","wraps"};AudioInputRecorder recorder;auto request=spec(directory.path());request.targets={{21,{0}}};request.ringFrames=128;
    const auto drained=std::make_shared<std::atomic<std::uint64_t>>(0);std::string error;
    check(recorder.start(request,error,[drained](const auto& file,auto rate,auto channels,auto format){return std::make_unique<ObservedWaveSink>(file,rate,channels,format,drained);}),"start actual WAV recording through a small wrapping queue");
    Transport transport;transport.play();std::array<float,128> values{};const float* channels[]={values.data()};std::vector<float> expected;
    for(unsigned block=0;block<48;++block) {
        const auto frames=17+block%112;
        for(unsigned frame=0;frame<frames;++frame){values[frame]=static_cast<float>(expected.size()%29)/64-0.125f;expected.push_back(values[frame]);}
        AudioIo io;io.in=channels;io.numIn=1;io.frames=static_cast<std::int32_t>(frames);io.streamTimeSamples=transport.position();input(recorder,transport,io);
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(drained->load(std::memory_order_acquire)<expected.size() && std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if(drained->load(std::memory_order_acquire)!=expected.size())throw std::runtime_error("writer did not drain");
    }
    recorder.stop();AudioIo idle;input(recorder,transport,idle);auto result=finish(recorder);std::vector<float> heard(expected.size());
    {audio::WavReader reader(result.tracks[0].file);reader.read(heard.data(),static_cast<std::uint32_t>(heard.size()));}
    check(result.failure==RecordingFailure::None && result.frames==expected.size() && heard==expected,"many queue wraps retain exact source order and final partial-block samples in an actual WAV");result.discard();
}
void waveformPreviewUsesCapturedFrames() {
    test::TempDirectory directory{"audio_recording","waveform_preview"};
    AudioInputRecorder recorder;
    auto request=spec(directory.path());
    request.sampleRate=48000;
    request.targets={{21,{2,0}},{22,{0}}}; // shared input, different track channel order
    std::string error;
    if(!recorder.start(request,error))throw std::runtime_error(error);
    const auto beforeInput=recorder.preview();
    check(!beforeInput || beforeInput->peaks.frames==0,"no waveform exists before any input is written");

    constexpr std::uint32_t firstFrames=79, secondFrames=37, lastFrames=13;
    constexpr std::uint32_t totalFrames=firstFrames+secondFrames+lastFrames;
    constexpr std::int64_t startSample=512;
    std::array<float,totalFrames> channel0{},channel2{};
    for(std::size_t i=0;i<totalFrames;++i) {
        channel0[i]=-0.25f-static_cast<float>(i%11)/16.f;
        channel2[i]=0.25f+static_cast<float>(i%13)/16.f;
    }
    channel0.back()=-1.25f;
    channel2.back()=1.125f; // an extreme in the last partial block cannot be dropped

    Transport transport;
    transport.locate(startSample);
    transport.play();
    const auto capture=[&](std::uint32_t from,std::uint32_t frames) {
        const float* inputs[]{channel0.data()+from,nullptr,channel2.data()+from};
        AudioIo io;
        io.in=inputs;
        io.numIn=3;
        io.frames=static_cast<std::int32_t>(frames);
        io.streamTimeSamples=from;
        input(recorder,transport,io);
    };
    const auto exact=[&](const AudioRecordingPreview& preview,std::uint64_t frames) {
        const auto& peaks=preview.peaks;
        if(preview.startSample!=startSample || peaks.frames!=frames || peaks.channels!=2 ||
           peaks.sampleRate!=request.sampleRate || peaks.framesPerBin==0 || preview.tracks.size()!=2)
            return false;
        const auto track=[&](std::int64_t id) {
            return std::find_if(preview.tracks.begin(),preview.tracks.end(),
                                [id](const auto& entry){return entry.trackId==id;});
        };
        const auto first=track(21), second=track(22);
        if(first==preview.tracks.end() || second==preview.tracks.end() ||
           first->channels.size()!=2 || second->channels.size()!=1)
            return false;
        const auto index2=first->channels[0], index0=first->channels[1];
        if(index2>=peaks.channels || index0>=peaks.channels || index2==index0 ||
           second->channels.front()!=index0)
            return false;
        const auto count=1+(frames-1)/peaks.framesPerBin;
        if(peaks.bins.size()!=count*peaks.channels)
            return false;
        const auto hasExtrema=[&](const auto& source,std::uint32_t channel,std::uint64_t bin) {
            const auto from=bin*peaks.framesPerBin;
            const auto to=from+std::min(peaks.framesPerBin,frames-from);
            const auto [minimum,maximum]=std::minmax_element(source.data()+from,source.data()+to);
            const auto& extrema=peaks.bins[static_cast<std::size_t>(bin*peaks.channels+channel)];
            return extrema.minimum==*minimum && extrema.maximum==*maximum;
        };
        for(std::uint64_t bin=0;bin<count;++bin)
            if(!hasExtrema(channel0,index0,bin) || !hasExtrema(channel2,index2,bin))
                return false;
        return true;
    };
    capture(0,firstFrames);
    std::shared_ptr<const AudioRecordingPreview> first;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    do {
        first=recorder.preview();
        if(first && first->peaks.frames!=0)break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while(std::chrono::steady_clock::now()<deadline);
    check(first && exact(*first,firstFrames),
          "the first writer batch publishes exact per-input extrema, shared channel mapping and the real song start");

    capture(firstFrames,secondFrames);
    capture(firstFrames+secondFrames,lastFrames);
    recorder.stop();
    AudioIo idle;
    input(recorder,transport,idle);
    auto result=finish(recorder);
    check(result.failure==RecordingFailure::None && result.frames==totalFrames && result.preview &&
          exact(*result.preview,totalFrames),
          "the final preview covers every accepted sample, including extrema in the final partial block");
    check(first && exact(*first,firstFrames),"holding the first publication keeps its original frames and peaks");
    result.discard();
}
class FailingSink final:public AudioRecordingSink {
public:
    void write(std::span<const float>,std::uint32_t)override{throw std::runtime_error("injected disk write failure");}
    void close()override{}
};
void discontinuityAndWriterFailures() {
    test::TempDirectory directory{"audio_recording","discontinuity"};AudioInputRecorder recorder;auto request=spec(directory.path());request.targets={{21,{0}}};request.maxFrames=8;std::string error;
    std::array<float,8> values{};const float* channels[]={values.data()};AudioIo io;io.in=channels;io.numIn=1;io.frames=8;
    Transport transport;transport.play();check(recorder.start(request,error),"start recording before a transport seek");input(recorder,transport,io);transport.locate(100);io.streamTimeSamples=8;input(recorder,transport,io);recorder.stop();input(recorder,transport,io);
    auto seek=finish(recorder);check(seek.failure==RecordingFailure::Discontinuity && seek.frames==8 && seek.hasAttachableAudio(),"seek reports a discontinuity and keeps the finalized prefix");seek.discard();
    transport.locate(0);transport.loop(0,4);io.streamTimeSamples=0;check(recorder.start(request,error),"start recording with an interior loop boundary");input(recorder,transport,io);recorder.stop();input(recorder,transport,io);
    auto loop=finish(recorder);check(loop.failure==RecordingFailure::Discontinuity && loop.frames==0 && !loop.hasAttachableAudio(),"a loop wrap within the callback is rejected before mixed-position samples enter a take");loop.discard();
    transport.loop(0,4,false);transport.locate(0);check(recorder.start(request,error,[](const auto&,auto,auto,auto){return std::make_unique<FailingSink>();}),"start a writer that will report an I/O failure");input(recorder,transport,io);
    AudioIo idle;const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);std::optional<AudioRecordingResult> failed;
    while(std::chrono::steady_clock::now()<deadline && !failed){failed=recorder.poll();input(recorder,transport,idle);std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    check(failed && failed->failure==RecordingFailure::Io && failed->error=="injected disk write failure" && !failed->hasAttachableAudio(),"worker I/O failures reach the owner with their reason after callback quiescence");if(failed)failed->discard();
}
class CloseFailingWaveSink final:public AudioRecordingSink {
    audio::WavWriter writer_;
    unsigned& closeAttempts_;
    const bool fails_;
public:
    CloseFailingWaveSink(const std::filesystem::path& file,std::uint32_t rate,std::uint16_t channels,
                         audio::WavFormat format,unsigned& closeAttempts,bool fails)
        :writer_(file,rate,channels,format),closeAttempts_(closeAttempts),fails_(fails) {}
    void write(std::span<const float> samples,std::uint32_t frames)override {
        writer_.write(samples.data(),frames);
    }
    void close()override {
        ++closeAttempts_;
        writer_.close();
        if(fails_)throw std::runtime_error("injected close failure after a locate");
    }
};
void closeFailureAfterDiscontinuity() {
    test::TempDirectory directory{"audio_recording","failed_close_after_seek"};
    unsigned closeAttempts=0;
    AudioInputRecorder recorder;
    auto request=spec(directory.path());
    request.targets={{21,{0}},{22,{0}}};
    request.maxFrames=8;
    std::string error;
    if(!recorder.start(request,error,[&](const auto& file,auto rate,auto channels,auto format) {
        return std::make_unique<CloseFailingWaveSink>(file,rate,channels,format,closeAttempts,file.filename()=="take-0.wav");
    }))throw std::runtime_error(error);
    const std::array<float,8> samples{0.25f,-0.5f,0.0f,0.5f,-0.125f,0.0f,0.0f,0.125f};
    const float* channels[]={samples.data()};
    AudioIo io;io.in=channels;io.numIn=1;io.frames=static_cast<int>(samples.size());
    Transport transport;transport.play();
    input(recorder,transport,io);
    transport.locate(100);
    io.streamTimeSamples=8;
    input(recorder,transport,io);
    recorder.stop();
    AudioIo idle;input(recorder,transport,idle);
    auto result=finish(recorder);
    check(result.failure==RecordingFailure::Discontinuity && result.error=="injected close failure after a locate" &&
          result.frames==samples.size() && !result.hasAttachableAudio(),
          "a sink close failure prevents prefix attachment even when discontinuity was reported first");
    check(closeAttempts==2 && !recorder.pending() && !result.discard(),
          "both sinks were closed, and their temporary directory can be removed after the failed capture");
}
struct DiskFailure {
    std::atomic<unsigned> firstWrites{0};
    std::atomic<unsigned> closeAttempts{0};
    std::atomic<bool> failing{false};
};
class PartialWaveSink final:public AudioRecordingSink {
    audio::WavWriter writer_;
    DiskFailure& state_;
    const bool fails_;
    bool wrote_=false;
public:
    PartialWaveSink(const std::filesystem::path& file,std::uint32_t rate,std::uint16_t channels,
                    audio::WavFormat format,DiskFailure& state,bool fails)
        : writer_(file,rate,channels,format),state_(state),fails_(fails) {}
    void write(std::span<const float> values,std::uint32_t frames)override {
        if(wrote_ && fails_) {
            state_.failing.store(true,std::memory_order_release);
            throw std::runtime_error("injected disk failure after the first write");
        }
        writer_.write(values.data(),frames);
        if(!wrote_)state_.firstWrites.fetch_add(1,std::memory_order_release);
        wrote_=true;
    }
    void close()override {
        state_.closeAttempts.fetch_add(1,std::memory_order_relaxed);
        if(fails_)throw std::runtime_error("injected failure flushing a damaged writer");
        writer_.close();
    }
};
void keepPrefixAfterPartialDiskFailure() {
    test::TempDirectory directory{"audio_recording","disk_error_keep"};
    DiskFailure disk;
    AudioInputRecorder recorder;
    auto request=spec(directory.path());
    request.targets={{21,{0}}};
    std::string error;
    if(!recorder.start(request,error,[&](const auto& file,auto rate,auto channels,auto format) {
        return std::make_unique<PartialWaveSink>(file,rate,channels,format,disk,true);
    }))throw std::runtime_error(error);
    Transport transport;
    transport.play();
    const std::array<float,8> samples{.25f,-.5f,0.f,.5f,-.125f,0.f,0.f,.125f};
    const std::array<float,8> unwritten{1.f,1.f,1.f,1.f,1.f,1.f,1.f,1.f};
    const float* channels[]={samples.data()};
    AudioIo io;
    io.in=channels;
    io.numIn=1;
    io.frames=static_cast<int>(samples.size());
    input(recorder,transport,io);
    // Allow the first WAV write to finish before offering a block that will fail.
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(!disk.firstWrites.load(std::memory_order_acquire) && std::chrono::steady_clock::now()<deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if(!disk.firstWrites.load(std::memory_order_acquire))
        throw std::runtime_error("the first WAV block was not written");
    channels[0]=unwritten.data();
    io.streamTimeSamples=io.frames;
    input(recorder,transport,io);
    const auto result=finish(recorder,true);
    check(result.failure==RecordingFailure::Io && result.error=="injected disk failure after the first write" &&
          !recorder.pending(),"a non-cancelled disk failure finishes with its original writer error");
    const auto file=result.directory()/"take-0.wav";
    const bool retained=!result.directory().empty() && std::filesystem::is_regular_file(file);
    check(retained,"a failed recording keeps its WAV even when closing the sink fails");
    if(retained) {
        audio::WavReader reader(file);
        std::array<float,8> saved{};
        check(reader.frames()==saved.size() &&
              reader.read(saved.data(),static_cast<std::uint32_t>(saved.size()))==saved.size() && saved==samples,
              "the first completed block survives and the failed block is absent");
    }
}
void cancelAfterPartialDiskFailure() {
    for(const bool deviceStopped:{false,true}) {
        test::TempDirectory directory{"audio_recording","disk_error_cancel"};
        DiskFailure disk;
        AudioInputRecorder recorder;
        auto request=spec(directory.path());
        request.targets={{21,{0}},{22,{0}}};
        std::string error;
        std::filesystem::path capture;
        if(!recorder.start(request,error,[&](const auto& file,auto rate,auto channels,auto format) {
            capture=file.parent_path();
            return std::make_unique<PartialWaveSink>(file,rate,channels,format,disk,file.filename()=="take-0.wav");
        }))throw std::runtime_error(error);
        Transport transport;
        transport.play();
        const std::array<float,8> samples{0.25f,-0.5f,0.0f,0.5f,-0.125f,0.0f,0.0f,0.125f};
        const float* channels[]={samples.data()};
        AudioIo io;
        io.in=channels;
        io.numIn=1;
        io.frames=static_cast<int>(samples.size());
        const auto waitFor=[](const auto& ready) {
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            while(!ready() && std::chrono::steady_clock::now()<deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if(!ready())throw std::runtime_error("partial WAV writer did not signal");
        };
        input(recorder,transport,io);
        waitFor([&]{return disk.firstWrites.load(std::memory_order_acquire)==2;});
        io.streamTimeSamples=io.frames;
        input(recorder,transport,io);
        waitFor([&]{return disk.failing.load(std::memory_order_acquire);});
        recorder.stop(true);
        if(!deviceStopped) {
            AudioIo idle;
            input(recorder,transport,idle);
        }
        std::optional<AudioRecordingResult> result;
        try {
            result=finish(recorder,deviceStopped);
        } catch(const std::filesystem::filesystem_error&) {
            check(false,"cancelling a failed disk write must not throw");
        }
        check(disk.closeAttempts.load()==2,"both WAV sinks are closed even if writing and closing one fails");
        check(result && result->failure==RecordingFailure::Cancelled && result->directory().empty(),
              "cancel returns once and removes both partial WAVs");
        check(!recorder.pending() && !std::filesystem::exists(capture),
              "a cancelled disk error clears the pending job and its private directory");
        const auto restarted=recorder.start(request,error);
        check(restarted,"a second recording can start without restarting ADI");
        if(restarted) {
            recorder.stop(true);
            const auto retry=finish(recorder,true);
            check(retry.failure==RecordingFailure::Cancelled && !recorder.pending(),
                  "the restarted recorder completes independently");
        }
    }
}
#if defined(_WIN32)
void cancelReportsCleanupDenial() {
    test::TempDirectory directory{"audio_recording","cleanup_denied"};
    AudioInputRecorder recorder;
    const auto request=spec(directory.path());
    std::string error;
    const auto drained=std::make_shared<std::atomic<std::uint64_t>>(0);
    std::filesystem::path capture;
    if(!recorder.start(request,error,[&](const auto& file,auto rate,auto channels,auto format) {
        capture=file.parent_path();
        return std::make_unique<ObservedWaveSink>(file,rate,channels,format,drained);
    }))throw std::runtime_error(error);
    const auto file=capture/"deletion-blocked";
    const auto lock=CreateFileW(file.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL,nullptr);
    if(lock==INVALID_HANDLE_VALUE)throw std::runtime_error("cannot create a file that denies deletion");
    recorder.stop(true);
    std::optional<AudioRecordingResult> retained;
    try {
        retained=finish(recorder,true);
        check(retained->failure==RecordingFailure::Cancelled && !retained->directory().empty() &&
              retained->error.find("Could not remove recording files")!=std::string::npos,
              "cancellation reports the cleanup denial and retains the directory for retry");
        check(static_cast<bool>(retained->discard()),"an explicit discard reports a removal error too");
    } catch(...) {
        CloseHandle(lock);
        throw;
    }
    CloseHandle(lock);
    check(!recorder.pending() && !retained->discard() && retained->directory().empty(),
          "a cleanup denial leaves the recorder ready and cleanup can be retried");
}
#endif
class CountingSink final:public AudioRecordingSink {
    std::shared_ptr<std::atomic<unsigned>> destroyed_;
public:
    explicit CountingSink(std::shared_ptr<std::atomic<unsigned>> destroyed):destroyed_(std::move(destroyed)){}
    ~CountingSink()override{++*destroyed_;}
    void write(std::span<const float>,std::uint32_t)override{}
    void close()override{}
};
void stoppedDriverReleasesRetiredCapture() {
    test::TempDirectory directory{"audio_recording","quiescent"};AudioInputRecorder recorder;auto request=spec(directory.path());request.targets={{21,{0}}};std::string error;
    const auto destroyed=std::make_shared<std::atomic<unsigned>>(0);
    for(unsigned index=0;index<3;++index) {
        check(recorder.start(request,error,[destroyed](const auto&,auto,auto,auto){return std::make_unique<CountingSink>(destroyed);}),"start another capture with the device stopped");
        recorder.stop();auto result=finish(recorder,true);
        check(destroyed->load()==index+1,"explicit quiescence releases retired job buffers and writers without waiting for a callback");result.discard();
    }
}
void actualSessionBothWidths() {
    for(const auto width:{Precision::Float32,Precision::Float64}) {
        test::TempDirectory directory{"audio_recording","session"};StoreError error{};auto store=Store::createEmpty(directory.path()/"song.adi",error);
        if(!store)throw std::runtime_error(toString(error));
        store->db().exec("INSERT INTO tracks(id,kind,name,record_armed) VALUES(21,'audio','Armed',1),(22,'audio','Not armed',0),(9,'master','Main',0)");
        Session session;SessionSpec format{2,48000,128};format.precision=width;check(session.load(*store,{},format),"load actual Session for input capture at either graph width");
        auto request=spec(directory.path());request.sampleRate=48000;request.targets={{21,{1}}};std::string reason;
        auto wrong=request;wrong.sampleRate=44100;check(!session.startRecording(wrong,reason),"Session refuses a recording format different from its active driver");
        wrong=request;wrong.targets={{22,{0}}};check(!session.startRecording(wrong,reason),"Session refuses an unarmed destination");
        check(session.startRecording(request,reason),"Session starts armed hardware-input recording");session.transport().locate(777);session.transport().play();
        std::array<float,128> source{},other{};const float* in[]={other.data(),source.data()};std::array<float,256> narrow{};std::array<double,256> wide{};float* out[]={narrow.data(),narrow.data()+128};double* out64[]={wide.data(),wide.data()+128};
        std::vector<float> expected;std::uint64_t stream=0;
        for(const auto frames:{19,128,3,81}) {
            for(int frame=0;frame<frames;++frame){source[static_cast<std::size_t>(frame)]=static_cast<float>(stream+static_cast<std::uint64_t>(frame))/512-0.25f;expected.push_back(source[static_cast<std::size_t>(frame)]);}
            AudioIo io;io.frames=frames;io.in=in;io.numIn=2;io.numOut=2;io.out=out;if(width==Precision::Float64)io.out64=out64;io.streamTimeSamples=static_cast<std::int64_t>(stream);session.process(io);stream+=static_cast<std::uint64_t>(frames);
        }
        session.recording().stop();AudioIo idle;idle.frames=1;idle.numOut=2;idle.out=out;if(width==Precision::Float64)idle.out64=out64;session.process(idle);auto result=finish(session.recording());
        std::vector<float> heard(expected.size());{audio::WavReader reader(result.tracks[0].file);reader.read(heard.data(),static_cast<std::uint32_t>(heard.size()));}
        check(result.failure==RecordingFailure::None && result.startSample==777 && result.frames==stream && heard==expected,"actual Session captures selected input bits and exact placement in either processing precision");result.discard();
    }
}
void punchCapturesOnlyTheBrace() {
    test::TempDirectory directory{"audio_recording","punch"};
    StoreError storeError{};
    auto store=Store::createEmpty(directory.path()/"song.adi",storeError);
    if(!store)throw std::runtime_error(toString(storeError));
    store->db().exec("INSERT INTO tracks(id,kind,name,record_armed) VALUES"
                     "(21,'audio','Audio',1),(22,'midi','MIDI',1),(9,'master','Main',0)");
    Session session;
    if(!session.load(*store,{},SessionSpec{2,48000,128}))throw std::runtime_error(session.error());
    constexpr std::int64_t start=13,end=45;
    constexpr auto frames=static_cast<std::uint32_t>(end-start);
    OpRequest brace;
    brace.opType="transport.setLoop";
    const auto [startTick,endTick]=[&] {
        auto clock=session.beginRender();
        return std::pair{clock.tickAtSample(start),clock.tickAtSample(end)};
    }();
    brace.payload={{"on",false},{"start",startTick},{"end",endTick}};
    OpRequest punch;
    punch.opType="transport.setPunch";
    punch.payload={{"in",true},{"out",true}};
    if(!OpJournal(*store).commit(brace).ok || !OpJournal(*store).commit(punch).ok || !session.refresh(*store))
        throw std::runtime_error("could not set the linear punch range");

    auto request=spec(directory.path());
    request.sampleRate=48000;
    request.targets={{21,{0}}};
    const std::array<std::int64_t,1> midiTrack{22};
    auto startBoth=[&] {
        std::string reason;
        if(!session.startRecording(request,reason) || !session.startMidiInputCapture(midiTrack,reason))
            throw std::runtime_error(reason);
        session.transport().locate(0);
        session.transport().play();
    };
    startBoth();
    for(const int frame:{4,18,61})
        check(session.enqueueMidiInput(frame,{0,EventType::NoteOn,1,64,static_cast<std::uint64_t>(frame),0,1.0}),
              "queue a note before, inside or after the punch");
    std::array<float,128> signal{},left{},right{};
    for(std::size_t i=0;i<signal.size();++i)signal[i]=static_cast<float>(i)/128.f;
    const float* inputs[]{signal.data()};
    float* outputs[]{left.data(),right.data()};
    AudioIo io{outputs,inputs,2,1,128,0};
    session.process(io);
    check(session.transport().position()==128 && session.transport().playing() &&
          session.recording().acceptedFrames()==frames && session.midiInputCapture().state()==MidiCaptureState::Complete,
          "both punch edges split one callback without stopping the transport");
    std::array<MidiCapturePacket,8> packets{};
    const auto count=session.midiInputCapture().drain(packets);
    check(count==2 && packets[0].kind==MidiCapturePacket::Kind::Span &&
          packets[0].timelineSample==start && packets[0].frames==end-start &&
          packets[1].kind==MidiCapturePacket::Kind::Event && packets[1].timelineSample==18,
          "the MIDI tap keeps the exact punched interval and skips events outside it");
    auto& recorder=session.recording();
    (void)recorder.poll();
    io.streamTimeSamples=128;
    session.process(io);
    auto result=finish(recorder);
    check(result.failure==RecordingFailure::None && result.startSample==start && result.frames==frames,
          "the audio writer finalizes the punched prefix without an explicit Stop");
    if(result.hasAttachableAudio()) {
        std::array<float,frames> recorded{};
        audio::WavReader wav(result.tracks[0].file);
        check(wav.read(recorded.data(),static_cast<std::uint32_t>(recorded.size()))==recorded.size() &&
              std::equal(recorded.begin(),recorded.end(),signal.begin()+start),
              "the WAV contains only samples between the two brace markers");
    }
    result.discard();

    startBoth();
    io.frames=4;
    io.streamTimeSamples=256;
    session.process(io);
    recorder.stop();
    session.midiInputCapture().stop();
    io.streamTimeSamples+=io.frames;
    session.process(io);
    auto empty=finish(recorder);
    check(empty.failure==RecordingFailure::NoFrames && session.midiInputCapture().state()==MidiCaptureState::Complete &&
          session.midiInputCapture().drain(packets)==0,
          "explicit Stop settles both recorders even before Punch-In with no captured input");
    empty.discard();

    std::string reason;
    if(!session.startRecording(request,reason))
        throw std::runtime_error(reason);
    session.transport().locate(0);
    session.transport().play();
    io.frames=32;
    io.streamTimeSamples+=4;
    session.process(io); // The linear take has accepted [13,32).
    brace.payload["start"] = brace.payload["start"].get<std::int64_t>() + 1;
    if(!OpJournal(*store).commit(brace).ok || !session.refresh(*store))
        throw std::runtime_error("could not move the punch brace during capture");
    io.streamTimeSamples+=io.frames;
    session.process(io);
    (void)recorder.poll();
    io.streamTimeSamples+=io.frames;
    session.process(io);
    auto prefix=finish(recorder);
    check(prefix.failure==RecordingFailure::Discontinuity && prefix.hasAttachableAudio() &&
              prefix.frames==static_cast<std::uint64_t>(32-start) && prefix.punchWindow().in &&
              prefix.punchWindow().in->tick==brace.payload["start"].get<std::int64_t>()-1,
          "changing a punch brace retains the first bounds and the previously accepted WAV prefix");
    prefix.discard();

    startBoth();
    io.streamTimeSamples+=io.frames;
    session.process(io); // Both recorders now own [13,32).
    punch.payload["out"] = false; // Changes the window without relocating the transport.
    if(!OpJournal(*store).commit(punch).ok || !session.refresh(*store))
        throw std::runtime_error("could not turn Punch-Out off during mixed capture");
    io.streamTimeSamples+=io.frames;
    session.process(io);
    RecordingPunchWindow frozen;
    const auto mixedCount=session.midiInputCapture().drain(packets,&frozen);
    check(session.midiInputCapture().state()==MidiCaptureState::Discontinuity && mixedCount==1 &&
              packets[0].timelineSample==start && packets[0].frames==32-start &&
              frozen.in && frozen.in->tick==brace.payload["start"].get<std::int64_t>() &&
              frozen.out && frozen.out->tick==brace.payload["end"].get<std::int64_t>(),
          "changing a punch switch preserves the authored bounds and ends MIDI with the accepted prefix");
    (void)recorder.poll();
    io.streamTimeSamples+=io.frames;
    session.process(io);
    auto mixedPrefix=finish(recorder);
    check(mixedPrefix.failure==RecordingFailure::Discontinuity &&
              mixedPrefix.frames==static_cast<std::uint64_t>(32-start) && mixedPrefix.punchWindow()==frozen,
          "the mixed audio and MIDI prefixes stop at the same boundary");
    mixedPrefix.discard();
    punch.payload["out"] = true;
    if(!OpJournal(*store).commit(punch).ok || !session.refresh(*store))
        throw std::runtime_error("could not restore Punch-Out");

    // Both authored ticks map to the same sample. The one-frame minimum for
    // running a loop must not allow a linear take through the punch boundary.
    brace.payload["start"] = 0;
    brace.payload["end"] = 1;
    if(!OpJournal(*store).commit(brace).ok || !session.refresh(*store))
        throw std::runtime_error("could not set a sub-sample punch brace");
    startBoth();
    io.streamTimeSamples+=io.frames;
    session.process(io);
    recorder.stop();
    session.midiInputCapture().stop();
    io.streamTimeSamples+=io.frames;
    session.process(io);
    auto subSample=finish(recorder);
    check(subSample.failure==RecordingFailure::NoFrames && session.midiInputCapture().drain(packets)==0,
          "a sub-sample linear punch brace accepts neither audio nor MIDI");
    subSample.discard();
}
void oversizedDeviceBlockEndsTake() {
    test::TempDirectory directory{"audio_recording","over-grant"};
    struct Processor final : BlockProcessor {
        AudioInputRecorder recorder;
        Transport transport;
        void prepare(double,std::int32_t) override {}
        void process(const AudioIo& io) noexcept override {input(recorder,transport,io);}
        void release() override {}
    } processor;
    device::DeviceCore driver;
    check(driver.open(processor,48000,8,8).opened,"prepare an eight-frame device");
    auto request=spec(directory.path());
    request.maxFrames=8;
    request.targets={{21,{0}}};
    request.format=audio::WavFormat::Float32;
    std::string error;
    if(!processor.recorder.start(request,error))throw std::runtime_error(error);
    processor.transport.play();
    const std::array<float,9> prefix={.125f,.25f,.375f,.5f,-.125f,-.25f,-.375f,-.5f,0.f};
    std::array<float,9> later{};
    const float* channels[]={prefix.data()};
    check(driver.process(nullptr,0,channels,1,8)==device::CallbackResult::Processed,
          "the granted callback reaches the recorder");
    channels[0]=later.data();
    device::CallbackResult rejected;
    {
        audio::CallbackScope callback;
        rejected=driver.process(nullptr,0,channels,1,9);
        if(rejected==device::CallbackResult::Oversized)processor.recorder.reportOversizedCallback();
    }
    check(rejected==device::CallbackResult::Oversized && driver.oversizeRefusals()==1,
          "the bridge receives explicit notification when the core has to drop input");
    driver.process(nullptr,0,channels,1,8);
    processor.recorder.stop();
    driver.process(nullptr,0,channels,1,8);
    auto result=finish(processor.recorder);
    check(result.failure==RecordingFailure::BlockSize && result.frames==8 && result.hasAttachableAudio(),
          "a dropped hardware block ends the take and reports the intact prefix");
    if(result.hasAttachableAudio()) {
        audio::WavReader wav(result.tracks.front().file);
        std::array<float,8> recorded{};
        check(wav.frames()==8 && wav.read(recorded.data(),8)==8 &&
              std::equal(recorded.begin(),recorded.end(),prefix.begin()),
              "the interrupted take contains exactly the original eight samples");
    }
    result.discard();
    driver.close();
}
void boundedOverflow() {
    for(const bool looping:{false,true}) {
    test::TempDirectory directory{"audio_recording","overflow"};AudioInputRecorder recorder;std::string error;auto request=spec(directory.path());request.maxFrames=8;request.ringFrames=8;request.targets={{21,{0}}};
    if(looping)request.loop=AudioRecordingLoop{0,5,8};
    const auto blocked=std::make_shared<Blocked>();
    struct ReleaseOnExit {
        std::shared_ptr<Blocked> state;
        ~ReleaseOnExit(){std::lock_guard lock(state->mutex);state->release=true;state->condition.notify_all();}
    } release{blocked};
    check(recorder.start(request,error,[blocked](const auto&,auto,auto,auto){return std::make_unique<BlockingSink>(blocked);}),"start with an off-thread blocked writer");
    Transport transport;if(looping)transport.loop(0,5);transport.play();std::array<float,8> values{};const float* channels[]={values.data()};AudioIo io;io.in=channels;io.numIn=1;io.frames=8;input(recorder,transport,io);
    {std::unique_lock lock(blocked->mutex);if(!blocked->condition.wait_for(lock,std::chrono::seconds(5),[&]{return blocked->entered;}))throw std::runtime_error("writer did not enter");}
    io.streamTimeSamples=8;input(recorder,transport,io);io.streamTimeSamples=16;input(recorder,transport,io);
    recorder.stop();io.streamTimeSamples=24;input(recorder,transport,io);
    {std::lock_guard lock(blocked->mutex);blocked->release=true;}blocked->condition.notify_all();
    auto result=finish(recorder);check(result.failure==RecordingFailure::Overflow && result.frames==16 && result.hasAttachableAudio(),
        "overflow reports the failure and offers only the finalized prefix for attachment");
    if(looping)check(result.passes().size()==4 && result.passes().back().sourceStartFrame==15 && result.passes().back().frames==1 && result.passes().back().timelineEndSample==1,"ring overflow does not append or extend pass metadata for rejected input");
    result.discard();
    }
}
bool passEquals(const RecordedPass& actual,std::uint64_t source,std::uint64_t frames,std::int64_t first,std::int64_t end) {
    return actual.sourceStartFrame==source && actual.frames==frames && actual.timelineStartSample==first && actual.timelineEndSample==end;
}
void loopClockChangeKeepsTheCapturedPrefix() {
    test::TempDirectory directory{"audio_recording","loop_clock_change"};
    constexpr std::int64_t quarter=textproj::kPPQ, loopEnd=48000;
    const auto authored=std::make_shared<const TempoMap>(TempoMap{{{0,120,0},{quarter,120,0}}});
    const auto republished=*authored;
    const TempoMap retimed{{{0,180,0},{quarter,90,0}}};
    auto request=spec(directory.path());
    request.sampleRate=48000;
    request.targets={{21,{0}}};
    request.loop=AudioRecordingLoop{0,loopEnd,8,authored};
    AudioInputRecorder recorder;
    std::string error;
    if(!recorder.start(request,error))throw std::runtime_error(error);
    Transport transport;
    transport.loop(0,loopEnd);
    transport.play();
    std::array<float,8> source{};
    const float* channels[]{source.data()};
    AudioIo io;
    io.in=channels;
    io.numIn=1;
    io.frames=static_cast<std::int32_t>(source.size());
    const auto captureWithClock=[&](const TempoMap& clock,std::uint64_t generation) {
        {
            audio::CallbackScope callback;
            recorder.capture(io,transport,RecordingPunch::Within,{},&clock,generation);
        }
        transport.advance(io.frames);
        io.streamTimeSamples+=io.frames;
    };
    captureWithClock(*authored,1);
    captureWithClock(republished,2);
    captureWithClock(retimed,3); // Two beats still last one second.
    auto recorded=finish(recorder,true);
    check(recorded.failure==RecordingFailure::Discontinuity && recorded.frames==2*source.size() &&
          recorded.passes().size()==1 && passEquals(recorded.passes()[0],0,16,0,16),
          "an equivalent clock keeps capturing and an interior retime preserves the earlier loop audio");
    recorded.discard();
}
void loopPunchKeepsEveryPass() {
    for(const bool punchIn:{false,true}) {
        test::TempDirectory directory{"audio_recording","loop_punch"};
        StoreError error{};
        auto store=Store::createEmpty(directory.path()/"song.adi",error);
        if(!store)throw std::runtime_error(toString(error));
        store->db().exec("INSERT INTO tracks(id,kind,name,record_armed) VALUES"
                         "(21,'audio','Audio',1),(9,'master','Main',0)");
        Session session;
        if(!session.load(*store,{},SessionSpec{2,48000,128}))throw std::runtime_error(session.error());
        constexpr std::int64_t start=19,end=33;
        OpRequest brace;
        brace.opType="transport.setLoop";
        const auto [startTick,endTick]=[&] {
            auto clock=session.beginRender();
            return std::pair{clock.tickAtSample(start),clock.tickAtSample(end)};
        }();
        brace.payload={{"on",true},{"start",startTick},{"end",endTick}};
        OpRequest punch;
        punch.opType="transport.setPunch";
        punch.payload={{"in",punchIn},{"out",true}};
        if(!OpJournal(*store).commit(brace).ok || !OpJournal(*store).commit(punch).ok || !session.refresh(*store))
            throw std::runtime_error("could not set the punched loop range");
        auto request=spec(directory.path());
        request.sampleRate=48000;
        request.targets={{21,{0}}};
        request.loop=AudioRecordingLoop{start,end,8};
        std::string reason;
        if(!session.startRecording(request,reason))throw std::runtime_error(reason);

        // The physical block crosses seven count-in frames, the initial lead-in, and three wraps.
        session.transport().locate(0);
        session.transport().countIn(7);
        std::array<float,71> signal{},left{},right{};
        for(std::size_t i=0;i<signal.size();++i)signal[i]=static_cast<float>(i)/128.f;
        const float* inputs[]{signal.data()};
        float* outputs[]{left.data(),right.data()};
        AudioIo io{outputs,inputs,2,1,71,0};
        session.process(io);
        auto& recorder=session.recording();
        const std::uint64_t frames=punchIn?45u:64u;
        check(recorder.pending() && recorder.acceptedFrames()==frames &&
              session.transport().playing() && session.transport().position()==22,
              "Punch-In excludes just the lead-in and Punch-Out leaves the loop running");
        recorder.stop();
        io.streamTimeSamples=io.frames;
        session.process(io);
        auto result=finish(recorder);
        const auto passes=result.passes();
        const auto first=punchIn?start:0;
        const auto firstFrames=static_cast<std::uint64_t>(end-first);
        check(result.failure==RecordingFailure::None && result.startSample==first &&
              result.frames==frames && passes.size()==4 &&
              passEquals(passes[0],0,firstFrames,first,end) &&
              passEquals(passes[1],firstFrames,14,start,end) &&
              passEquals(passes[2],firstFrames+14,14,start,end) &&
              passEquals(passes[3],firstFrames+28,3,start,22),
              "only the first pass changes with Punch-In, and every full wrap and the last partial pass survive");
        if(result.hasAttachableAudio()) {
            std::vector<float> captured(static_cast<std::size_t>(frames));
            audio::WavReader wav(result.tracks[0].file);
            check(wav.read(captured.data(),static_cast<std::uint32_t>(frames))==frames &&
                  std::equal(captured.begin(),captured.end(),signal.end()-static_cast<std::ptrdiff_t>(frames)),
                  "the recorded WAV is the exact contiguous suffix after count-in and optional Punch-In");
        }
        result.discard();
    }
}
void sessionLoopPasses() {
    for(const auto width:{Precision::Float32,Precision::Float64})for(const bool partialFirst:{false,true}) {
        test::TempDirectory directory{"audio_recording","session_loop"};StoreError storeError{};auto store=Store::createEmpty(directory.path()/"song.adi",storeError);
        if(!store)throw std::runtime_error(toString(storeError));
        store->db().exec("INSERT INTO tracks(id,kind,name,record_armed) VALUES(21,'audio','Armed',1),(9,'master','Main',0)");
        Session session;SessionSpec format{2,48000,128};format.precision=width;
        check(session.load(*store,{},format),"load real Session for loop capture");
        auto request=spec(directory.path());request.sampleRate=48000;request.targets={{21,{1}}};request.loop=AudioRecordingLoop{100,132,4};std::string error;
        check(session.startRecording(request,error),"explicit loop capture starts on real armed Session");
        session.transport().loop(100,132);session.transport().locate(partialFirst?108:100);
        if(partialFirst)session.transport().countIn(7);else session.transport().play();
        std::array<float,128> source{},other{},left{},right{};std::array<double,128> left64{},right64{};
        const float* in[]={other.data(),source.data()};float* out[]={left.data(),right.data()};double* out64[]={left64.data(),right64.data()};
        std::vector<float> expected;std::int64_t physical=0;
        const std::array counts{19,partialFirst?37:30,3,60}; // 112 accepted frames, 3.5 loop lengths
        for(const auto frames:counts) {
            for(int frame=0;frame<frames;++frame) {
                source[static_cast<std::size_t>(frame)]=static_cast<float>(physical+frame)/512-0.125f;
                if(physical+frame>=(partialFirst?7:0))expected.push_back(source[static_cast<std::size_t>(frame)]);
            }
            AudioIo io;io.frames=frames;io.in=in;io.numIn=2;io.out=out;io.numOut=2;io.streamTimeSamples=1000+physical;if(width==Precision::Float64)io.out64=out64;
            session.process(io);physical+=frames;
        }
        check(session.recording().acceptedFrames()==112,"only actual playing input after count-in enters the continuous source");
        session.recording().stop();check(!session.recording().poll(),"loop result also waits for callback quiescence");
        AudioIo idle;idle.frames=1;idle.out=out;idle.numOut=2;if(width==Precision::Float64)idle.out64=out64;session.process(idle);
        auto result=finish(session.recording());const auto passes=result.passes();
        check(result.failure==RecordingFailure::None && result.frames==112 && result.tracks.size()==1 && passes.size()==4,"3.5 loop lengths produce four nonempty passes in one owned source");
        check(result.loopSpec() && result.loopSpec()->beginSample==100 && result.loopSpec()->endSample==132 && result.startSample==(partialFirst?108:100),"completed result retains frozen loop contract and actual first placement");
        const auto first=partialFirst?24u:32u,last=partialFirst?24u:16u;
        check(passes.size()==4 && passEquals(passes[0],0,first,partialFirst?108:100,132) &&
              passEquals(passes[1],first,32,100,132) && passEquals(passes[2],first+32,32,100,132) &&
              passEquals(passes[3],first+64,last,100,100+last),"pass offsets and half-open timeline intervals retain exact partial first and last passes");
        std::vector<float> heard(expected.size());
        {audio::WavReader reader(result.tracks[0].file);check(reader.frames()==112 && reader.channels()==1 && reader.read(heard.data(),112)==112,"loop source is one finalized real WAV");}
        check(heard==expected,"continuous source preserves every selected input bit through multiple wraps and count-in slicing");
        std::size_t files=0;for(const auto& file:std::filesystem::directory_iterator(result.directory()))if(file.is_regular_file())++files;
        check(files==1,"passes create no per-pass source files");result.discard();
    }
}
void loopBoundsAndCapacity() {
    test::TempDirectory directory{"audio_recording","loop_bounds"};AudioInputRecorder recorder;auto request=spec(directory.path());request.targets={{21,{0}}};request.loop=AudioRecordingLoop{100,104,2};std::string error;
    for(const auto& invalidLoop:{AudioRecordingLoop{-1,4,2},AudioRecordingLoop{4,4,2},AudioRecordingLoop{4,3,2},AudioRecordingLoop{0,4,0},AudioRecordingLoop{0,4,65537}}) {
        auto invalid=request;invalid.loop=invalidLoop;check(!recorder.start(invalid,error) && !recorder.pending(),"invalid loop bounds or excessive pass allocation refuses before publication");
    }
    std::array<float,128> values{};for(std::size_t index=0;index<values.size();++index)values[index]=static_cast<float>(index)/256;
    const float* channels[]={values.data()};AudioIo io;io.in=channels;io.numIn=1;
    for(const bool exactBoundary:{false,true}) {
        Transport transport;transport.loop(100,104);transport.locate(100);transport.play();check(recorder.start(request,error),"start bounded pass capture");
        io.frames=exactBoundary?8:3;io.streamTimeSamples=0;input(recorder,transport,io);
        const auto retained=static_cast<std::uint64_t>(io.frames);
        io.frames=0;io.streamTimeSamples=static_cast<std::int64_t>(retained);input(recorder,transport,io);
        io.frames=exactBoundary?1:6;input(recorder,transport,io);recorder.stop();input(recorder,transport,io);auto result=finish(recorder);
        check(result.failure==RecordingFailure::PassLimit && result.frames==retained,"pass capacity exhaustion rejects the whole callback without accepting its initial fitting portion");
        check(exactBoundary?(result.passes().size()==2 && passEquals(result.passes()[1],4,4,100,104)):
              (result.passes().size()==1 && passEquals(result.passes()[0],0,3,100,103)),"pass limit leaves prior exact-end/partial pass metadata unchanged and never appends an empty pass");
        {audio::WavReader reader(result.tracks[0].file);check(reader.frames()==retained,"failed capture keeps file and pass log on the same accepted prefix");}result.discard();
    }
    // All 128 one-sample passes occur inside one physical callback.
    request.loop=AudioRecordingLoop{100,101,128};Transport many;many.loop(100,101);many.locate(100);many.play();check(recorder.start(request,error),"start one-sample loop stress");
    io.frames=128;io.streamTimeSamples=0;input(recorder,many,io);recorder.stop();input(recorder,many,io);auto tiny=finish(recorder);
    bool exact=tiny.failure==RecordingFailure::None && tiny.passes().size()==128;
    for(std::size_t index=0;index<tiny.passes().size();++index)exact=exact && passEquals(tiny.passes()[index],index,1,100,101);
    check(exact,"every wrap inside one variable callback has an independently correct source interval");tiny.discard();
    // Pre-loop audio is kept as an exact interval; later publication decides its lane policy.
    request.loop=AudioRecordingLoop{100,104,4};Transport pre;pre.loop(100,104);pre.locate(98);pre.play();check(recorder.start(request,error),"start before loop begin");
    io.frames=9;io.streamTimeSamples=0;input(recorder,pre,io);recorder.stop();input(recorder,pre,io);auto preroll=finish(recorder);
    check(preroll.failure==RecordingFailure::None && preroll.passes().size()==2 && passEquals(preroll.passes()[0],0,6,98,104) && passEquals(preroll.passes()[1],6,3,100,103),"pre-loop input retains actual placement and a negative derived loop offset");preroll.discard();
    const auto ceiling=std::numeric_limits<std::int64_t>::max();request.loop=AudioRecordingLoop{ceiling-5,ceiling,4};Transport high;high.loop(ceiling-5,ceiling);high.locate(ceiling-2);high.play();check(recorder.start(request,error),"start loop near sample-clock ceiling");
    io.frames=8;io.streamTimeSamples=0;input(recorder,high,io);recorder.stop();input(recorder,high,io);auto bounded=finish(recorder);
    check(bounded.failure==RecordingFailure::None && bounded.passes().size()==3 && passEquals(bounded.passes()[0],0,2,ceiling-2,ceiling) && passEquals(bounded.passes()[1],2,5,ceiling-5,ceiling) && passEquals(bounded.passes()[2],7,1,ceiling-5,ceiling-4),"loop arithmetic does not overflow when the linear position-plus-block would overflow");bounded.discard();
}
void loopDiscontinuities() {
    test::TempDirectory directory{"audio_recording","loop_discontinuities"};std::array<float,8> values{};const float* channels[]={values.data()};
    for(unsigned change=0;change<7;++change) {
        AudioInputRecorder recorder;auto request=spec(directory.path());request.targets={{21,{0}}};request.loop=AudioRecordingLoop{100,132,8};std::string error;
        Transport transport;transport.loop(100,132);transport.locate(100);transport.play();check(recorder.start(request,error),"start discontinuity control");
        AudioIo io;io.in=channels;io.numIn=1;io.frames=8;io.streamTimeSamples=0;input(recorder,transport,io);io.streamTimeSamples=8;
        switch(change) {
            case 0:transport.locate(109);break;
            case 1:transport.locate(108);break; // same position, different revision
            case 2:++io.streamTimeSamples;break;
            case 3:transport.loop(100,132);break; // same bounds, explicit reconfigure
            case 4:transport.retimeLoop(100,133);break; // no revision change
            case 5:transport.loop(100,132,false);break;
            default:transport.play(false);transport.play();break;
        }
        input(recorder,transport,io);recorder.stop();input(recorder,transport,io);auto result=finish(recorder);
        check(result.failure==RecordingFailure::Discontinuity && result.frames==8 && result.passes().size()==1 && passEquals(result.passes()[0],0,8,100,108),"seek, same-position locate, stream gap, loop mutation and stop/restart refuse without relabelling accepted samples");result.discard();
    }
    for(unsigned mode=0;mode<3;++mode) {
        AudioInputRecorder recorder;auto request=spec(directory.path());request.targets={{21,{0}}};request.loop=AudioRecordingLoop{100,132,8};std::string error;
        Transport transport;transport.loop(100,mode==1?133:132,mode!=0);transport.locate(100);transport.play();
        check(recorder.start(request,error),"prepare loop mode before first driver block");AudioIo io;io.in=channels;io.numIn=1;io.frames=8;input(recorder,transport,io);
        if(mode==2){transport.play(false);io.streamTimeSamples=8;input(recorder,transport,io);}
        recorder.stop();input(recorder,transport,io);auto result=finish(recorder);
        if(mode<2)check(result.failure==RecordingFailure::Discontinuity && result.frames==0 && result.passes().empty(),"first callback must match the explicitly requested enabled loop bounds");
        else check(result.failure==RecordingFailure::None && result.frames==8 && result.passes().size()==1 && passEquals(result.passes()[0],0,8,100,108),"ordinary transport stop keeps a valid incomplete last pass");
        result.discard();
    }
}
}
int main() {
    try {
        actualFilesAndAcknowledgment();stopDrainsLastShortBlock();formatAndInputFailures();
        oversizedDeviceBlockEndsTake();boundedOverflow();actualQueueWraps();waveformPreviewUsesCapturedFrames();
        discontinuityAndWriterFailures();closeFailureAfterDiscontinuity();
        keepPrefixAfterPartialDiskFailure();cancelAfterPartialDiskFailure();
#if defined(_WIN32)
        cancelReportsCleanupDenial();
#endif
        stoppedDriverReleasesRetiredCapture();actualSessionBothWidths();punchCapturesOnlyTheBrace();
        sessionLoopPasses();loopPunchKeepsEveryPass();loopClockChangeKeepsTheCapturedPrefix();
        loopBoundsAndCapacity();loopDiscontinuities();
    } catch(const std::exception& error) {
        check(false,error.what());
    }
    check(callbackAllocations.load()==0 && audio::callbackFileIo.load()==0,
          "actual recording callback allocates nothing and performs no file I/O");
    std::printf("%d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
