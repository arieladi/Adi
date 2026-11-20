// SPDX-License-Identifier: AGPL-3.0-or-later
// Original host acceptance from the director's research. No external host data.
#include "adi/audio/wav_file.hpp"
#include "adi/engine/session.hpp"
#include "adi/render/queue.hpp"
#include "adi/store.hpp"
#include "adi/textproj_store.hpp"
#include "juce/clap_host.hpp"
#include "juce/vst3_host.hpp"
#include "temp_directory.hpp"
#include "../plugins/probe_shared.h"
#include "../plugins/time/probe/time_shared.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace adi;
constexpr double rate = 48000.0;
constexpr std::int32_t block = 2048;
constexpr std::uint32_t playing = 2, cycling = 4, musicValid = 512;
constexpr std::uint32_t tempoValid = 1024, barValid = 2048, cycleValid = 4096, meterValid = 8192;
int checks = 0;
void require(bool ok, const std::string& message) {
    ++checks;
    if (!ok) throw std::runtime_error(message);
}
bool withinTolerance(double a, double b) { return std::abs(a-b) < 1e-8; }
std::int64_t wrap(std::int64_t n, std::int64_t begin, std::int64_t end) {
    const auto length=end-begin;
    auto phase=(n-begin)%length;
    if(phase<0)phase+=length;
    return begin+phase;
}
// Independent closed-form expectations: 120 BPM until frame 48000, then 90.
// No production TempoMap, context conversion or graph-arrival helper is used.
double music(std::int64_t sample, bool change) {
    return !change || sample<48000 ? static_cast<double>(sample)/24000.0
        : 2.0+static_cast<double>(sample-48000)/32000.0;
}
double bpm(std::int64_t sample,bool change) { return change&&sample>=48000?90.0:120.0; }
double barStart(double beats) { return std::floor(beats/4.0)*4.0; }

class Capture {
public:
    Capture() {
        wchar_t old[512]{};
        const auto size=GetEnvironmentVariableW(L"ADI_TIME_MAP",old,512);
        require(size<512,"previous trace environment is too long");
        existed_=size!=0; previous_=old;
        static unsigned serial=0;
        const auto name=L"Local\\ADI_DFE21_"+std::to_wstring(GetCurrentProcessId())+L"_"+std::to_wstring(++serial);
        map_=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,
                               static_cast<DWORD>(TIME_MAP_BYTES),name.c_str());
        require(map_!=nullptr,"create private trace mapping");
        if(GetLastError()==ERROR_ALREADY_EXISTS){CloseHandle(map_);map_=nullptr;throw std::runtime_error("trace name collision; refusing to alter an existing mapping");}
        base_=MapViewOfFile(map_,FILE_MAP_ALL_ACCESS,0,0,TIME_MAP_BYTES);
        require(base_!=nullptr,"map private trace memory");
        header_=new(base_) TimeHeader{};
        std::memcpy(header_->magic,"ADITIME1",8);
        header_->version=1;header_->record_size=static_cast<std::uint32_t>(sizeof(TimeRecord));header_->capacity=TIME_CAP;
        LARGE_INTEGER frequency{};QueryPerformanceFrequency(&frequency);header_->frequency=frequency.QuadPart;
        require(SetEnvironmentVariableW(L"ADI_TIME_MAP",name.c_str())!=0,"publish private trace name");
    }
    ~Capture() {
        if(header_)InterlockedExchange(&header_->enabled,0);
        SetEnvironmentVariableW(L"ADI_TIME_MAP",existed_?previous_.c_str():nullptr);
        if(base_)UnmapViewOfFile(base_);
        if(map_)CloseHandle(map_);
    }
    Capture(const Capture&)=delete;Capture& operator=(const Capture&)=delete;
    void begin() { InterlockedExchange(&header_->enabled,0);header_->next=0;header_->lost=0;InterlockedExchange(&header_->enabled,1); }
    std::vector<TimeRecord> finish() {
        InterlockedExchange(&header_->enabled,0);
        require(header_->lost==0,"bounded trace overflow");
        const auto n=header_->next;
        require(n>=0&&static_cast<std::uint64_t>(n)<=TIME_CAP,"trace count in bounds");
        std::vector<TimeRecord> result;
        result.reserve(static_cast<std::size_t>(n));
        const auto* records=reinterpret_cast<const TimeRecord*>(static_cast<const char*>(base_)+TIME_HEADER_BYTES);
        for(LONG64 i=0;i<n;++i) {
            require(records[i].committed==i+1,"complete trace record");
            result.push_back(records[i]);
        }
        return result;
    }
private:
    HANDLE map_=nullptr;void* base_=nullptr;TimeHeader* header_=nullptr;
    std::wstring previous_;bool existed_=false;
};

struct Catalog {
    test::TempDirectory dir{"df_e21","discovery"};
    device::Vst3Host host;
    device::ClapHost clap;
    device::ClapPluginRef clapObserver;
    juce::PluginDescription observer,delay1024,delay4800;
    Catalog() {
        scan::Config config;
        config.executable=ADI_DF_E21_SCANNER_PATH;
        config.registry=dir.path()/"registry.sqlite";
        juce::KnownPluginList list;
        const auto result=host.scanFile(juce::String(ADI_DF_E21_PROBE_PATH),list,config);
        require(result.complete&&result.failures.empty(),"owned fixture discovery through isolated scanner");
        bool seenObserver=false,seen1024=false,seen4800=false;
        for(const auto& description:list.getTypes()) {
            if(description.name=="ADI Time Observer"){observer=description;seenObserver=true;}
            if(description.name=="ADI Pure Delay 1024"){delay1024=description;seen1024=true;}
            if(description.name=="ADI Pure Delay 4800"){delay4800=description;seen4800=true;}
        }
        require(seenObserver&&seen1024&&seen4800,"all three owned VST3 classes discovered");
        std::string error;
        auto scanner = scan::Scanner::open(config, error);
        require(scanner != nullptr, "open isolated CLAP fixture scanner: " + error);
        const auto clapResult = scanner->probe("clap", ADI_DF_E21_CLAP_PATH);
        require(clapResult.state == scan::State::Ready, "owned CLAP discovery: " + clapResult.error);
        for (const auto& description : clapResult.plugins) {
            if (description.id != "org.adi.research.engine-probe") continue;
            clapObserver = {clapResult.path, description.id, description.name, description.vendor,
                            description.version, description.features, description.instrument};
        }
        require(!clapObserver.id.empty(), "isolated scanner found the exact CLAP observer class");
    }
    engine::DeviceLoader loader(int latency) {
        return [this,latency](const engine::DeviceRequest& request,std::string& error) {
            if (request.ref && request.ref->uid == clapObserver.id) {
                auto instance = clap.makeDevice(clapObserver, request.sampleRate, request.maxFrames, error);
                require(instance && instance->loaded() && instance->latencySamples() == 0,
                        "real zero-latency CLAP observer loaded: " + error);
                return instance;
            }
            const bool isObserver=request.ref&&request.ref->uid=="fixture.observer";
            const auto& description=isObserver?observer:(latency==1024?delay1024:delay4800);
            auto instance=host.makeDevice(description,request.sampleRate,request.maxFrames,error);
            if(!instance||!instance->loaded())return std::unique_ptr<device::DeviceInstance>{};
            // JUCE caches the processor's latency during prepareToPlay, not merely
            // construction. Prepare through the real adapter before checking it.
            instance->prepare(request.sampleRate,request.maxFrames);
            require(instance->latencySamples()==(isObserver?0:latency),
                "prepared "+description.name.toStdString()+" reports latency "+std::to_string(instance->latencySamples()));
            std::printf("LOAD %s prepared latency=%d\n",description.name.toRawUTF8(),instance->latencySamples());
            return instance;
        };
    }
};

struct Source final : engine::Node {
    void process(const engine::NodeIo& io) noexcept override {
        for(std::int32_t c=0;c<io.channels;++c)
            for(std::int32_t i=0;i<io.frames;++i)io.out[c][io.blockOffset+i]=0.125f;
    }
};

struct Fixture {
    test::TempDirectory dir{"df_e21","session"};
    std::unique_ptr<Store> store;
    Source source;engine::Session session;
    explicit Fixture(bool change, bool clap = false) {
        StoreError error{};store=Store::createEmpty(dir.path()/"test.adi",error);
        require(store!=nullptr,"create isolated project");
        store->db().exec("INSERT INTO project(id,name,sample_rate) VALUES(1,'DF-E21',48000);"
            "INSERT INTO tempo_map(id,pos_ticks,bpm) VALUES(1,0,120);"
            "INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(1,'audio','Signal',0),(9,'master','Master',1);"
            "INSERT INTO plugin_refs(id,format,uid,name,subtype) VALUES"
            "(1,'vst3','fixture.delay','Delay','effect'),(2,'vst3','fixture.observer','Observer','effect');"
            "INSERT INTO device_chains(id,track_id) VALUES(1,1);"
            "INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name,always_process) "
            "VALUES(1,1,0,1,'Delay',1),(2,1,1,2,'Observer',1);");
        if (clap) store->db().exec("UPDATE plugin_refs SET format='clap',"
                                   "uid='org.adi.research.engine-probe' WHERE id=2");
        if(change)store->db().exec("INSERT INTO tempo_map(id,pos_ticks,bpm) VALUES(2,"+
            std::to_string(2*textproj::kPPQ)+",90);");
        session.graph().setFadeFrames(0);
        session.setSourcesFor([this](std::int64_t track) {
            return track==1?std::vector<engine::Node*>{&source}:std::vector<engine::Node*>{};
        });
    }
    void load(Catalog& catalog,int latency,bool compensation) {
        engine::SessionSpec spec;spec.sampleRate=rate;spec.maxFrames=block;spec.delayCompensation=compensation;
        require(session.load(*store,catalog.loader(latency),spec),"load actual Session: "+session.error());
        require(session.entryCount()==2,"Session owns both plug-ins");
        for(std::size_t i=0;i<session.entryCount();++i)require(!session.entryAt(i).placeholder,"fixture must not become missing-device placeholder");
    }
    void render() {
        std::array<float,static_cast<std::size_t>(block)*2> data{};
        float* pointers[]={data.data(),data.data()+block};
        engine::AudioIo io;io.out=pointers;io.numOut=2;io.frames=block;session.process(io);
    }
    void makeOfflineMedia() {
        store->db().exec("INSERT INTO media_files(id,hash_blake3,rel_path,format) VALUES(1,'fixture','source.wav','wav');"
            "INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns) VALUES(1,1,'audio',1,0,2000000000);"
            "INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(1,1,0,96000);");
        std::vector<float> samples(192000,0.0f);
        for(std::int64_t n=997;n<96000;n+=997) {
            const auto value=static_cast<float>(0.125+static_cast<double>(n/997)/1024.0);
            samples[static_cast<std::size_t>(n)*2]=value;samples[static_cast<std::size_t>(n)*2+1]=-value;
        }
        audio::WavWriter wave(dir.path()/"source.wav",48000,2,audio::WavFormat::Float32);
        wave.write(samples.data(),96000);wave.close();
        std::filesystem::create_directory(dir.path()/"out");
    }
};

void verifyFields(const TimeRecord& record,std::int64_t expected,bool change,bool loop,bool offline) {
    require(record.context_present!=0,"DF-E21: VST3 ProcessContext is missing");
    require(record.sample_rate==rate,"sample rate");
    require(record.project_samples==expected,"input sample position: got "+std::to_string(record.project_samples)+" expected "+std::to_string(expected));
    require((record.context_state&(musicValid|tempoValid|barValid|meterValid))==(musicValid|tempoValid|barValid|meterValid),"required context validity flags");
    require((record.context_state&playing)!=0,"playing state");
    require(withinTolerance(record.project_music,music(expected,change)),"input musical time from independent piecewise tempo oracle");
    require(withinTolerance(record.tempo,bpm(expected,change)),"tempo at the device input");
    require(withinTolerance(record.bar_music,barStart(music(expected,change))),"bar origin, including negative positions");
    require(record.numerator==4&&record.denominator==4,"time signature");
    require(record.mode==(offline?2U:0U),"real-time/offline process mode");
    require(((record.context_state&cycling)!=0)==loop,"active loop state");
}

void realtime(Catalog& catalog,int latency,const std::string& scenario) {
    Capture trace;const bool change=scenario=="tempo",loop=scenario=="loop",pdc=scenario!="pdc-off";
    Fixture fixture(change);fixture.load(catalog,latency,pdc);
    auto& transport=fixture.session.transport();
    constexpr std::int64_t loopBegin=997,loopEnd=2528; // length1531 <4800, neither aligned to2048
    transport.locate(loop?loopBegin:0);transport.play();
    if(loop) {
        transport.loop(loopBegin,loopEnd,true);
        // Fill before testing modulo history; startup negativity is a separate case.
        for(int i=0;i<12;++i)fixture.render();
    }
    std::int64_t timeline=transport.position();
    bool sawNegative=false,sawSplit=false,sawTempoAfter=false;
    const int callbacks=change?32:8;
    for(int callback=0;callback<callbacks;++callback) {
        if(scenario=="seek"&&callback==3){timeline=54321;transport.locate(timeline);}
        require(transport.position()==timeline,"driver position follows independent callback accounting");
        trace.begin();fixture.render();const auto records=trace.finish();
        std::int64_t observedFrames=0;bool sawObserver=false;
        for(const auto& record:records) {
            if(record.variant!=0||record.frames==0)continue;
            sawObserver=true;
            auto expected=timeline+observedFrames-(pdc?latency:0);
            if(loop)expected=wrap(expected,loopBegin,loopEnd);
            verifyFields(record,expected,change,loop,false);
            if(loop) {
                require((record.context_state&cycleValid)!=0,"loop-bound validity");
                require(withinTolerance(record.cycle_start_music,music(loopBegin,false))&&withinTolerance(record.cycle_end_music,music(loopEnd,false)),"loop bounds");
                require(expected+record.frames<=loopEnd,"callback crosses this device's input-clock loop boundary");
                sawSplit=sawSplit||record.frames<static_cast<std::uint32_t>(block);
            }
            sawNegative=sawNegative||expected<0;sawTempoAfter=sawTempoAfter||expected>=48000;
            observedFrames+=record.frames;
        }
        require(sawObserver&&observedFrames==block,"observer covers every frame of the callback");
        timeline+=block;if(loop)timeline=wrap(timeline,loopBegin,loopEnd);
    }
    if(scenario=="preroll")require(sawNegative,"negative input clock was exercised");
    if(loop)require(sawSplit,"loop crossed an actual device callback");
    if(change)require(sawTempoAfter,"tempo change was reached");
}

class ClapCapture {
public:
    ClapCapture() {
        const auto name = "Local\\ADI_DFE21_CLAP_" + std::to_string(adi_platform::pid()) +
                          "_" + std::to_string(adi_platform::ticks());
        require(mapping_.open(name, ADI_MAP_BYTES, true), "create private CLAP trace");
        header_ = new(mapping_.data()) ADIHeader{};
        std::memcpy(header_->magic, "ADIENG01", 8);
        header_->version = 1;
        header_->record_size = static_cast<std::uint32_t>(sizeof(ADIRecord));
        header_->capacity = ADI_CAPACITY;
        require(adi_platform::setEnvironment("ADI_ENGINE_MAP", name), "publish private CLAP trace name");
    }

    void begin() {
        adi_platform::publish(&header_->next, 0);
        adi_platform::publish(&header_->lost, 0);
        adi_platform::publish(&header_->enabled, 2);
    }

    std::vector<ADIRecord> finish() {
        adi_platform::publish(&header_->enabled, 0);
        const auto count = adi_platform::acquire(&header_->next);
        require(adi_platform::acquire(&header_->lost) == 0 && count >= 0 &&
                static_cast<std::uint64_t>(count) <= ADI_CAPACITY, "CLAP trace bounds");
        std::vector<ADIRecord> records;
        const auto* first = adi_platform::mappedAt<ADIRecord, ADI_HEADER_BYTES>(header_);
        for (std::int64_t index = 0; index < count; ++index) {
            require(adi_platform::acquire(&first[index].committed) == index + 1, "committed CLAP record");
            if (first[index].kind == 3) records.push_back(first[index]);
        }
        return records;
    }

private:
    adi_platform::Environment environment_{"ADI_ENGINE_MAP"};
    adi_platform::Mapping mapping_;
    ADIHeader* header_ = nullptr;
};

void realtimeClap(Catalog& catalog, int latency, bool pdc, bool loop) {
    ClapCapture trace;
    Fixture fixture(false, true);
    fixture.load(catalog, latency, pdc);
    auto& transport = fixture.session.transport();
    constexpr std::int64_t loopBegin = 997, loopEnd = 2528;
    transport.locate(loop ? loopBegin : 0);
    transport.play();
    if (loop) {
        transport.loop(loopBegin, loopEnd, true);
        for (int index = 0; index < 12; ++index) fixture.render();
    }
    auto timeline = transport.position();
    for (int callback = 0; callback < 8; ++callback) {
        if (!loop && callback == 3) {
            timeline = 54321;
            transport.locate(timeline);
        }
        require(transport.position() == timeline, "independent CLAP driver position");
        trace.begin();
        fixture.render();
        std::int64_t frames = 0;
        for (const auto& record : trace.finish()) {
            auto position = timeline + frames - (pdc ? latency : 0);
            if (loop) position = wrap(position, loopBegin, loopEnd);
            constexpr auto valid = CLAP_TRANSPORT_HAS_SECONDS_TIMELINE | CLAP_TRANSPORT_HAS_BEATS_TIMELINE |
                                   CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_IS_PLAYING;
            require((record.flags & valid) == valid, "DF-E21: CLAP transport is missing required flags");
            require(record.rate == rate && record.mode == CLAP_RENDER_REALTIME, "CLAP process rate and mode");
            require(withinTolerance(record.transport, static_cast<double>(position) / rate),
                    "CLAP seconds match the sample time at the observer input");
            require(((record.flags & CLAP_TRANSPORT_IS_LOOP_ACTIVE) != 0) == loop, "CLAP loop flag");
            if (loop) require(position + record.frames <= loopEnd, "CLAP callback splits at its own loop boundary");
            frames += record.frames;
        }
        require(frames == block, "CLAP observer covers the whole driver callback");
        timeline += block;
        if (loop) timeline = wrap(timeline, loopBegin, loopEnd);
    }
}

void verifySparseOutput(const render::Result& result) {
    audio::WavReader output(result.output);
    require(output.frames() == 96000, "export frame count");
    std::vector<float> samples(192000);
    require(output.read(samples.data(), 96000) == 96000, "read actual exported PCM");
    for (std::int64_t sample = 0; sample < 96000; ++sample) {
        const auto expected = sample > 0 && sample % 997 == 0
            ? static_cast<float>(0.125 + static_cast<double>(sample / 997) / 1024.0) : 0.0f;
        const auto index = static_cast<std::size_t>(sample) * 2;
        require(samples[index] == expected && samples[index + 1] == -expected,
                "offline PCM aligned, including zeros");
    }
}

render::Job offlineJob(const Fixture& fixture) {
    render::Job job;
    job.project = fixture.store->path();
    job.directory = fixture.dir.path() / "out";
    job.nameTemplate = "DF-E21";
    job.beginSample = 0;
    job.endSample = 96000;
    job.session.sampleRate = rate;
    job.session.maxFrames = block;
    job.encoding.format = audio::WavFormat::Float32;
    return job;
}

void offlineClap(Catalog& catalog, int latency) {
    ClapCapture trace;
    Fixture fixture(true, true);
    fixture.makeOfflineMedia();
    const auto job = offlineJob(fixture);
    trace.begin();
    const auto result = render::renderWav(job, catalog.loader(latency));
    const auto records = trace.finish();
    require(result.state == render::JobState::Completed, "CLAP export completed: " + result.error);
    std::int64_t position = -latency;
    for (const auto& record : records) {
        constexpr auto valid = CLAP_TRANSPORT_HAS_SECONDS_TIMELINE | CLAP_TRANSPORT_IS_PLAYING;
        require((record.flags & valid) == valid, "CLAP export has playing transport");
        require(record.mode == CLAP_RENDER_OFFLINE, "CLAP process mode is offline");
        require(withinTolerance(record.transport, static_cast<double>(position) / rate),
                "offline CLAP seconds match independent input-sample position");
        position += record.frames;
    }
    require(position == job.endSample, "CLAP export processes the complete compensated input window");
    verifySparseOutput(result);
}

void offline(Catalog& catalog,int latency) {
    Capture trace;Fixture fixture(true);fixture.makeOfflineMedia();
    const auto job = offlineJob(fixture);
    trace.begin();const auto result=render::renderWav(job,catalog.loader(latency));const auto records=trace.finish();
    require(result.state==render::JobState::Completed,"real export completed: "+result.error);
    // Anchor sample identity to original input audio, never to a reported context.
    bool anchored=false;std::int64_t referenceStart=0,prefix=0;
    for(const auto& record:records) {
        if(record.variant!=0||record.frames==0)continue;
        if(record.input_count>0) {
            const auto& marker=record.input[0];
            require(std::isfinite(marker.value)&&marker.value>0.125&&marker.value<0.25,"finite original offline marker");
            const auto index=std::llround((marker.value-0.125)*1024.0);
            require(index>0&&index*997<96000,"offline anchor is an original marker");
            require(marker.value==0.125+static_cast<double>(index)/1024.0,"exact offline anchor amplitude");
            referenceStart=index*997-marker.offset-prefix;anchored=true;break;
        }
        prefix+=record.frames;
    }
    require(anchored,"offline marker source reached the real observer");
    std::size_t markers=0,calls=0;std::int64_t next=referenceStart;
    for(const auto& record:records) {
        if(record.variant!=0||record.frames==0)continue;
        ++calls;verifyFields(record,next,true,false,true);
        next+=record.frames;
        require(record.input_total==record.input_count,"offline sparse marker capture complete");
        for(std::uint32_t i=0;i<record.input_count;++i) {
            const auto& marker=record.input[i];
            require(std::isfinite(marker.value)&&marker.value>0.125&&marker.value<0.25,"finite original offline marker");
            const auto index=std::llround((marker.value-0.125)*1024.0);
            require(index>0&&index*997<96000,"original sparse marker identity");
            require(marker.value==0.125+static_cast<double>(index)/1024.0,"exact original marker amplitude");
            require(record.project_samples+marker.offset==index*997,"offline input marker and context aligned");
            ++markers;
        }
    }
    require(calls>0&&markers==96,"offline observer actually processed all original source markers");
    verifySparseOutput(result);
}

void selfTest() {
    require(withinTolerance(music(-4800,false),-.2)&&withinTolerance(barStart(-.2),-4),"negative musical/bar oracle");
    require(withinTolerance(music(48000,true),2)&&withinTolerance(music(80000,true),3),"tempo oracle boundary");
    require(wrap(-3803,997,2528)==2321,"multi-wrap negative input oracle");
    Capture capture;capture.begin();TimeTrace writer;require(writer.open(),"fixture opens private capture mapping");
    TimeRecord record{};record.variant=0;record.frames=17;record.project_samples=-4800;writer.record(record);writer.close();
    const auto records=capture.finish();require(records.size()==1&&records[0].project_samples==-4800&&records[0].frames==17,"bounded fixture readout");
}
}

int main(int argc,char** argv) {
    try {
        if(argc!=2)throw std::runtime_error("use --self-test or --acceptance");
        const std::string mode=argv[1];
        if(mode=="--self-test"){selfTest();std::printf("PASS DF-E21 oracle/readout: %d checks\n",checks);return 0;}
        if(mode!="--acceptance")throw std::runtime_error("unknown mode");
        juce::ScopedJuceInitialiser_GUI initializer;
        Catalog catalog;int failures=0;
        for(const int latency:{1024,4800})for(const auto* scenario:{"preroll","seek","tempo","loop","pdc-off","offline"}) {
            try {
                if(std::string(scenario)=="offline")offline(catalog,latency);else realtime(catalog,latency,scenario);
                std::printf("PASS %s latency=%d\n",scenario,latency);
            }catch(const std::exception& error){++failures;std::printf("FAIL %s latency=%d: %s\n",scenario,latency,error.what());}
        }
        for (const int latency : {1024, 4800}) for (const auto* scenario : {"clock", "loop", "pdc-off", "offline"}) {
            try {
                if (std::strcmp(scenario, "offline") == 0) offlineClap(catalog, latency);
                else realtimeClap(catalog, latency, std::strcmp(scenario, "pdc-off") != 0,
                                  std::strcmp(scenario, "loop") == 0);
                std::printf("PASS CLAP %s latency=%d\n", scenario, latency);
            } catch (const std::exception& error) {
                ++failures;
                std::printf("FAIL CLAP %s latency=%d: %s\n", scenario, latency, error.what());
            }
        }
        std::printf("DF-E21: %d checks, %d failing cases\n",checks,failures);return failures?1:0;
    }catch(const std::exception& error){std::fprintf(stderr,"FAIL DF-E21 setup: %s\n",error.what());return 1;}
}
