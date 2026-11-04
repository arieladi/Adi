// SPDX-License-Identifier: AGPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/engine/audio_recording.hpp"
#include "adi/engine/transport.hpp"
#include "adi/engine/session.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include "adi/audio/io_audit.hpp"
#include <atomic>
#include <array>
#include <bit>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <new>
#include <thread>
namespace {std::atomic<unsigned> callbackAllocations{0};}
void* operator new(std::size_t size){if(adi::audio::onAudioThread)++callbackAllocations;if(void* p=std::malloc(size?size:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t size){return operator new(size);}
void operator delete(void* p)noexcept{std::free(p);}void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
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
class FailingSink final:public AudioRecordingSink {
public:
    void write(std::span<const float>,std::uint32_t)override{throw std::runtime_error("injected disk write failure");}
    void close()override{}
};
void discontinuityAndWriterFailures() {
    test::TempDirectory directory{"audio_recording","discontinuity"};AudioInputRecorder recorder;auto request=spec(directory.path());request.targets={{21,{0}}};request.maxFrames=8;std::string error;
    std::array<float,8> values{};const float* channels[]={values.data()};AudioIo io;io.in=channels;io.numIn=1;io.frames=8;
    Transport transport;transport.play();check(recorder.start(request,error),"start recording before a transport seek");input(recorder,transport,io);transport.locate(100);io.streamTimeSamples=8;input(recorder,transport,io);recorder.stop();input(recorder,transport,io);
    auto seek=finish(recorder);check(seek.failure==RecordingFailure::Discontinuity && seek.frames==8,"seek fails the linear take without attaching audio to a false timeline");seek.discard();
    transport.locate(0);transport.loop(0,4);io.streamTimeSamples=0;check(recorder.start(request,error),"start recording with an interior loop boundary");input(recorder,transport,io);recorder.stop();input(recorder,transport,io);
    auto loop=finish(recorder);check(loop.failure==RecordingFailure::Discontinuity && loop.frames==0,"a loop wrap within the callback is rejected before mixed-position samples enter a take");loop.discard();
    transport.loop(0,4,false);transport.locate(0);check(recorder.start(request,error,[](const auto&,auto,auto,auto){return std::make_unique<FailingSink>();}),"start a writer that will report an I/O failure");input(recorder,transport,io);
    AudioIo idle;const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);std::optional<AudioRecordingResult> failed;
    while(std::chrono::steady_clock::now()<deadline && !failed){failed=recorder.poll();input(recorder,transport,idle);std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    check(failed && failed->failure==RecordingFailure::Io && failed->error=="injected disk write failure","worker I/O failures reach the owner with their reason after callback quiescence");if(failed)failed->discard();
}
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
void boundedOverflow() {
    test::TempDirectory directory{"audio_recording","overflow"};AudioInputRecorder recorder;std::string error;auto request=spec(directory.path());request.maxFrames=8;request.ringFrames=8;request.targets={{21,{0}}};
    const auto blocked=std::make_shared<Blocked>();
    struct ReleaseOnExit {
        std::shared_ptr<Blocked> state;
        ~ReleaseOnExit(){std::lock_guard lock(state->mutex);state->release=true;state->condition.notify_all();}
    } release{blocked};
    check(recorder.start(request,error,[blocked](const auto&,auto,auto,auto){return std::make_unique<BlockingSink>(blocked);}),"start with an off-thread blocked writer");
    Transport transport;transport.play();std::array<float,8> values{};const float* channels[]={values.data()};AudioIo io;io.in=channels;io.numIn=1;io.frames=8;input(recorder,transport,io);
    {std::unique_lock lock(blocked->mutex);if(!blocked->condition.wait_for(lock,std::chrono::seconds(5),[&]{return blocked->entered;}))throw std::runtime_error("writer did not enter");}
    io.streamTimeSamples=8;input(recorder,transport,io);io.streamTimeSamples=16;input(recorder,transport,io);
    recorder.stop();io.streamTimeSamples=24;input(recorder,transport,io);
    {std::lock_guard lock(blocked->mutex);blocked->release=true;}blocked->condition.notify_all();
    auto result=finish(recorder);check(result.failure==RecordingFailure::Overflow && result.frames==16,"full bounded queue reports overflow and retains only accepted frames");result.discard();
}
}
int main(){try{actualFilesAndAcknowledgment();formatAndInputFailures();boundedOverflow();actualQueueWraps();discontinuityAndWriterFailures();stoppedDriverReleasesRetiredCapture();actualSessionBothWidths();}catch(const std::exception& error){check(false,error.what());}check(callbackAllocations.load()==0 && audio::callbackFileIo.load()==0,"actual recording callback allocates nothing and performs no file I/O");std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
