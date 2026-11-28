// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/cue_settings.hpp"
#include "adi/dsp/stereo_envelope_meter.hpp"
#include "adi/engine/audio_recording.hpp"
#include "adi/engine/midi_input_capture.hpp"
#include "adi/engine/sample_preview.hpp"
#include "adi/engine/session.hpp"
#include "adi/history.hpp"
#include "adi/store.hpp"
#include "adi/textproj.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <array>
#include <algorithm>
#include <cstdio>
#include <chrono>
#include <thread>
#include <atomic>
#include <cstdlib>
#include <new>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>
namespace {std::atomic<long> allocations{0};std::atomic<bool> countAllocations{false};}
#if defined(__GNUC__) && !defined(__clang__)
#define ADI_TEST_NOINLINE __attribute__((noinline))
#else
#define ADI_TEST_NOINLINE
#endif
ADI_TEST_NOINLINE void* operator new(std::size_t n){if(countAllocations.load(std::memory_order_relaxed))allocations.fetch_add(1,std::memory_order_relaxed);void* p=std::malloc(n==0?1:n);if(!p)throw std::bad_alloc();return p;}
ADI_TEST_NOINLINE void* operator new[](std::size_t n){return operator new(n);}
ADI_TEST_NOINLINE void operator delete(void* p) noexcept {std::free(p);}
ADI_TEST_NOINLINE void operator delete[](void* p) noexcept {std::free(p);}
ADI_TEST_NOINLINE void operator delete(void* p,std::size_t) noexcept {std::free(p);}
ADI_TEST_NOINLINE void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
#undef ADI_TEST_NOINLINE
namespace {
using namespace adi;using namespace adi::engine;
int checks=0,failures=0;
void check(bool good,const char* name){++checks;if(!good){++failures;std::printf("FAIL: %s\n",name);}}
bool near(double a,double b,double tolerance=1e-6){return std::abs(a-b)<tolerance;}
struct Tone final:Node {
    double value; double rightFactor=1.; std::int64_t frames=0;
    explicit Tone(double v):value(v){}
    bool processesDouble() const noexcept override{return true;}
    void process(const NodeIo& io) noexcept override {
        for(int c=0;c<io.channels;++c)for(int i=0;i<io.frames;++i)if(io.wide())io.out64[c][io.blockOffset+i]=value*(c?rightFactor:1.);else io.out[c][io.blockOffset+i]=static_cast<float>(value*(c?rightFactor:1.));
        frames+=io.frames;
    }
};
struct Effect final:device::DeviceInstance {
    device::DeviceIdentity identity_;double gain;int delay;std::int64_t& frames;
    std::array<std::array<double,512>,2> history{};int head=0;
    Effect(double g,int d,std::int64_t& f):gain(g),delay(d),frames(f){}
    const device::DeviceIdentity& identity() const noexcept override{return identity_;}
    bool loaded() const noexcept override{return true;}
    bool processesDouble() const noexcept override{return true;}
    std::int32_t latencySamples() const noexcept override{return delay;}
    void process(const NodeIo& io) noexcept override {
        for(int i=0;i<io.frames;++i){for(int c=0;c<io.channels;++c){
            const int at=io.blockOffset+i;const double x=io.wide()?(io.in64?io.in64[c][at]:0):(io.in?static_cast<double>(io.in[c][at]):0);
            auto& ring=history[static_cast<std::size_t>(c)];
            const double y=delay?ring[static_cast<std::size_t>((head+512-delay)%512)]:x;
            ring[static_cast<std::size_t>(head)]=x;
            if(io.wide())io.out64[c][at]=y*gain;else io.out[c][at]=static_cast<float>(y*gain);
        }head=(head+1)%512;}frames+=io.frames;
    }
};
struct Block {
    std::array<std::array<float,512>,4> narrow{};std::array<std::array<double,512>,4> wide{};
    float* outputs[4];double* doubles[4];AudioIo io;
    explicit Block(int frames=512,int channels=4){for(int c=0;c<4;++c){outputs[c]=narrow[static_cast<std::size_t>(c)].data();doubles[c]=wide[static_cast<std::size_t>(c)].data();}io.out=outputs;io.out64=doubles;io.numOut=channels;io.frames=frames;}
};
struct Fixture {
    test::TempDirectory temp{"cue_monitor","project"};std::unique_ptr<Store> store;
    Tone main{0.25},reference{0.5};std::int64_t refFrames=0,masterFrames=0;
    Session session;
    Fixture(){StoreError error{};store=Store::create(temp.path()/"song.adi",error);if(!store)throw std::runtime_error("fixture store");
        store->db().exec("INSERT INTO tracks(id,kind,name) VALUES(1,'audio','Project'),(2,'audio','Reference'),(9,'master','Main');"
            "INSERT INTO mixer_strip(track_id,volume_db) VALUES(1,0),(2,0),(9,-20);"
            "INSERT INTO tempo_map(id,pos_ticks,bpm,curve) VALUES(1,0,120,0)");}
    bool op(const char* name,Payload payload,bool refresh=true){OpRequest r;r.opType=name;r.payload=std::move(payload);const auto result=OpJournal(*store).commit(r);if(!result.ok){std::printf("op diagnostic %s: %s\n",name,result.error.c_str());return false;}return !refresh || !session.loaded() || session.refresh(*store);}
    void effects(int delay=0){store->db().exec("INSERT INTO plugin_refs(id,format,uid,name) VALUES(1,'vst3','cue.test','Test');"
        "INSERT INTO device_chains(id,track_id) VALUES(2,2),(9,9);"
        "INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name) VALUES(2,2,0,1,'Reference FX'),(9,9,0,1,'Main FX')");masterDelay=delay;}
    int masterDelay=0;
    bool load(Precision precision=Precision::Float32,bool performance=true, bool exporting=false){session.setSourcesFor([this](std::int64_t id){return id==1?std::vector<Node*>{&main}:id==2?std::vector<Node*>{&reference}:std::vector<Node*>{};});
        SessionSpec spec;spec.maxFrames=512;spec.precision=precision;spec.performanceState=performance;spec.exporting=exporting;
        return session.load(*store,[this](const DeviceRequest& request,std::string&){return std::make_unique<Effect>(request.device->id==2?2.0:0.1,request.device->id==9?masterDelay:0,request.device->id==2?refFrames:masterFrames);},spec);}
    void process(Block& b){session.process(b.io);}
};
void settings() {
    Fixture f;const auto initial=SnapshotBuilder::fromStore(*f.store);
    check(!initial->cue.cueMode && initial->cue.soloInPlace && initial->cue.volumeDb==0,"Cue defaults match saved Solo/SIP and unity cue volume");
    check(f.op("cue.setMode",{{"on",true}}) && f.op("cue.setOutput",{{"first",2}}) && f.op("cue.setVolume",{{"db",-6.0}}),"Cue mode, output and volume commit");
    check(f.op("mixer.setSoloInPlace",{{"on",false}}) && f.op("cue.setReference",{{"on",false},{"id",2}}),"SIP and reference designation commit");
    const auto next=SnapshotBuilder::fromStore(*f.store,*initial);
    check(next->cue.cueMode && !next->cue.soloInPlace && next->cue.firstOutputChannel==2 && next->cue.volumeDb==-6 && next->cue.referenceTrack==2 && !next->cue.referenceOn,"snapshot exposes every cue field");
    check(next->tracks==initial->tracks,"cue settings preserve unchanged track sharing");
    check(!History(*f.store).canUndo(),"cue session commands never become undo steps");
    const auto saved=readCueSettings(*f.store);OpRequest bad;bad.opType="cue.setOutput";bad.payload={{"first",3}};
    check(!OpJournal(*f.store).commit(bad).ok && readCueSettings(*f.store)==saved,"unrepresentable stereo pair is refused atomically");
    bad.payload={{"first",std::int64_t{4294967296LL}}};check(!OpJournal(*f.store).commit(bad).ok,"large output index cannot truncate to a valid pair");
    bad.opType="cue.setVolume";bad.payload={{"db",99.0}};check(!OpJournal(*f.store).commit(bad).ok,"cue volume outside the fader range is refused");
    bad.opType="cue.setReference";bad.payload={{"on",true},{"id",9}};check(!OpJournal(*f.store).commit(bad).ok,"Main cannot be its own exclusive reference");
    bad.payload={{"on",true},{"id",123}};check(!OpJournal(*f.store).commit(bad).ok,"missing reference target is refused");
    check(f.store->close(),"Cue fixture closes");const auto path=f.store->path();f.store.reset();StoreError error{};f.store=Store::open(path,error,true);
    check(f.store && readCueSettings(*f.store)==saved,"Cue choices survive read-only reopen");
}
void referenceSwap(Precision precision) {
    Fixture f;f.effects();f.store->db().exec("UPDATE tracks SET muted=1 WHERE id=2");f.op("cue.setOutput",{{"first",2}});f.op("cue.setReference",{{"on",false},{"id",2}});
    check(f.load(precision),"reference fixture loads");Block b;f.process(b);
    check(near(b.wide[0][511],0.0025) && b.wide[2][511]==0,"designated reference stays outside Main while off");
    const auto before=f.refFrames;
    check(f.op("cue.setReference",{{"on",true}}),"reference on publishes");f.process(b);
    check(f.refFrames==before+512 && f.masterFrames==f.refFrames,"device DSP runs once per callback with monitoring enabled");
    const double project=0.0025,ref=1;
    check(near(b.wide[0][0],project*(239.0/240)+ref/240),"reference starts with the five-ms complementary swap");
    check(near(b.wide[0][239],ref) && near(b.wide[0][511],ref),"reference fully bypasses Main's FX and fader after five ms");
    check(near(b.wide[2][239],ref) && b.wide[2]==b.wide[3],"exclusive reference reaches the selected Cue Out pair too");
    check(std::equal(b.wide[0].begin(),b.wide[0].end(),b.narrow[0].begin(),[](double x,float y){return near(x,y,2e-7);}),"both output widths carry the same reference swap");
    check(f.op("cue.setVolume",{{"db",-6.020599913279624}}),"Cue Volume updates");f.process(b);
    check(near(b.wide[0][511],0.5) && near(b.wide[2][511],0.5),"Cue Volume scales reference on monitor and Cue Out");
    check(f.op("mixer.setVolume",{{"id",9},{"db",-60.0}}),"Main fader changes behind reference");f.process(b);
    check(near(b.wide[0][511],0.5),"Main fader never changes active reference level");
    check(f.op("cue.setReference",{{"on",false}}),"reference off publishes");f.process(b);
    check(b.wide[0][0]>0.49 && near(b.wide[0][239],0.000025),"five-ms release keeps reference audio until Main returns");
    check(b.wide[2][511]==0 && near(b.wide[0][511],0.000025),"reference off restores the project and clears headphones");
    check(f.session.refresh(*f.store),"unchanged cue rebuild succeeds");f.process(b);
    check(near(b.wide[0][511],0.000025),"unchanged rebuild does not retrigger a reference swap");
    check(f.op("cue.setOutput",{{"first",0}}) && f.op("cue.setReference",{{"on",true}}),"reference can share the monitor stereo pair");f.process(b);
    check(near(b.wide[0][511],0.5) && b.wide[2][511]==0,"shared monitor/Cue Out receives reference once at the right level");
}
void soloAndCue() {
    Fixture f;f.store->db().exec("INSERT INTO tracks(id,kind,name) VALUES(3,'return','Return');INSERT INTO mixer_strip(track_id) VALUES(3);"
        "INSERT INTO routing(id,src_kind,src_id,dst_kind,dst_id,kind,gain_db) VALUES(10,'track',1,'track',3,'send',0)");
    f.op("track.setSolo",{{"id",1},{"soloed",true}});check(f.load(),"solo/send fixture loads");Block b;f.process(b);
    check(near(b.wide[0][511],0.05),"Solo in Place keeps the soloed source's return audible");
    check(f.op("mixer.setSoloInPlace",{{"on",false}}),"Solo in Place off publishes");f.process(b);
    check(near(b.wide[0][511],0.025),"Solo in Place off closes that return");
    check(f.op("cue.setOutput",{{"first",2}}) && f.op("cue.setMode",{{"on",true}}) && f.op("cue.setTrack",{{"id",1},{"on",true}}),"switch from Solo to Cue publishes");f.process(b);
    check(near(b.wide[0][511],0.1) && near(b.wide[2][511],0.25),"Cue selection leaves Main's mix open and hears PFL on headphones");
    f.session.setMainAudition(MainAudition::Side);
    f.process(b);
    check(b.wide[0][511] == 0. && b.wide[1][511] == 0. &&
              near(b.wide[2][511], .25) && near(b.wide[3][511], .25),
          "Main Side affects its speaker pair while an audible separate stereo Cue pair is unchanged");
    f.session.setMainAudition(MainAudition::FilterFlat);
    f.process(b);
    check(near(b.wide[0][511], .045) && near(b.wide[1][511], .045) &&
              near(b.wide[2][511], .25) && near(b.wide[3][511], .25),
          "Main Filter Listen leaves the separately routed Cue pair unchanged");
    f.session.setMainAudition(MainAudition::Stereo);
    check(f.op("track.setMute",{{"id",1},{"muted",true}}) && f.op("mixer.setVolume",{{"id",1},{"db",-60.0}}),"mute/fader change on a cued track");f.process(b);
    check(near(b.wide[0][511],0.05) && near(b.wide[2][511],0.25),"PFL cue remains audible despite track mute and fader");
    check(f.op("cue.setTrack",{{"id",1},{"on",false}}),"Cue button releases");f.process(b);
    check(b.wide[2][0]>0.24 && b.wide[2][239]==0 && b.wide[2][511]==0,"released cue ramps over five ms and clears the destination");
}
void compensatedAndClick() {
    Fixture f;f.effects(100);f.op("cue.setOutput",{{"first",2}});f.op("cue.setMode",{{"on",true}});f.op("cue.setTrack",{{"id",1},{"on",true}});
    check(f.load(),"compensated cue fixture loads");Block b;f.process(b);
    check(std::all_of(b.wide[2].begin(),b.wide[2].begin()+100,[](double v){return v==0;}) && b.wide[2][100]>0 && near(b.wide[2][511],0.25),"Cue PFL is aligned to Main's compensated output clock");
    check(f.session.refresh(*f.store),"cue graph rebuild during playback");f.process(b);
    check(std::all_of(b.wide[2].begin(),b.wide[2].end(),[](double v){return near(v,0.25);}),"cue delay history survives the graph swap");
    Fixture click;click.main.value=click.reference.value=0;click.op("cue.setOutput",{{"first",2}});click.op("transport.setMetronome",{{"on",true}});
    check(click.load(),"Cue metronome fixture loads");click.session.transport().play();click.process(b);
    check(std::all_of(b.wide[0].begin(),b.wide[0].end(),[](double v){return v==0;}) && std::any_of(b.wide[2].begin(),b.wide[2].end(),[](double v){return std::abs(v)>0.01;}),"metronome follows Cue Out and leaves Main untouched");
}
void samplePreview() {
    SamplePreview preview;
    Fixture f;
    f.main.value = f.reference.value = 0;
    f.store->db().exec("UPDATE tracks SET record_armed=1 WHERE id=1");
    f.session.setSamplePreview(&preview);
    check(f.op("cue.setOutput", {{"first", 6}}) && f.op("cue.setVolume", {{"db", -6.020599913279624}}) && f.load(),
          "sample preview uses the song's Cue Out and Cue Volume");
    const auto source = f.temp.path() / "audition.wav";
    std::array<float, 8192> pcm;
    for (std::size_t i = 0; i < pcm.size(); i += 2) {
        pcm[i] = .5f;
        pcm[i + 1] = -.25f;
    }
    audio::WavWriter writer(source, 48000, 2, audio::WavFormat::Float32);
    writer.write(pcm.data(), static_cast<std::uint32_t>(pcm.size() / 2));
    writer.close();

    Block block;
    std::array<float, 512> input;
    input.fill(.125f);
    const float* inputs[]{input.data()};
    block.io.in = inputs;
    block.io.numIn = 1;
    block.io.outputChannels.enabled = 0x3c; // physical 3/4 and 5/6
    auto process = [&] {
        f.process(block);
        block.io.streamTimeSamples += block.io.frames;
    };
    process();
    std::string why;
    AudioRecordingSpec take;
    take.workFolder = f.temp.path();
    take.sampleRate = 48000;
    take.targets = {{1, {0}}};
    check(f.session.startRecording(take, why), "start a concurrent audio take");
    f.session.transport().play();
    preview.play(source);
    process();
    check(preview.status().position == 0. && block.wide[2].back() == 0. && block.wide[3].back() == 0.,
          "SC-431: a song's preview stays silent while its physical Cue Out is missing");
    block.io.outputChannels.enabled = 0xcc; // physical 3/4 and 7/8
    using namespace std::chrono_literals;
    const auto waitUntil = [](auto ready) {
        const auto end = std::chrono::steady_clock::now() + 5s;
        do {
            if (ready())
                return true;
            std::this_thread::sleep_for(1ms);
        } while (std::chrono::steady_clock::now() < end);
        return false;
    };
    const auto heard = waitUntil([&] {
        process();
        return near(block.wide[2].back(), .25) && near(block.wide[3].back(), -.125);
    });
    check(heard && block.wide[0].back() == 0 && block.wide[1].back() == 0,
          "SC-431: sample preview plays at half gain on physical Cue 7/8 and leaves Main silent");
    f.session.recording().stop();
    process();
    std::optional<AudioRecordingResult> result;
    check(waitUntil([&] {
        result = f.session.recording().poll();
        return result.has_value();
    }) && result->hasAttachableAudio() && result->tracks.size() == 1,
          "finish and inspect the concurrent take");
    if (result && result->hasAttachableAudio() && result->tracks.size() == 1) {
        audio::WavReader reader(result->tracks.front().file);
        std::vector<float> recorded(static_cast<std::size_t>(reader.frames()));
        check(reader.frames() > 0 && reader.read(recorded.data(), static_cast<std::uint32_t>(recorded.size())) == recorded.size() &&
                  std::all_of(recorded.begin(), recorded.end(), [](float sample) { return sample == .125f; }),
              "the take contains the input samples only while audition is audible");
    }
}
void sparseCueOutput() {
    const auto render = [](int first, std::uint64_t enabled) {
        Fixture fixture;
        fixture.main.value = fixture.reference.value = 0.;
        check(fixture.op("cue.setOutput", {{"first", first}}) &&
                  fixture.op("transport.setMetronome", {{"on", true}}) && fixture.load(),
              "SC-431: load the metronome with a physical Cue Out");
        Block block(512, std::popcount(enabled));
        block.io.outputChannels.enabled = enabled;
        fixture.session.transport().play();
        fixture.process(block);
        std::array<bool, 4> audible{};
        for (std::size_t channel = 0; channel < audible.size(); ++channel)
            audible[channel] = std::ranges::any_of(block.wide[channel], [](double value) { return std::abs(value) > 0.01; });
        return audible;
    };
    constexpr std::uint64_t physical1and2and7and8 = 0xc3;
    constexpr std::uint64_t physical1and2and3and7 = 0x47;
    check(render(2, physical1and2and7and8) == std::array{false, false, false, false},
          "SC-431: a missing Cue Out cannot leak the metronome into the next enabled pair");
    check(render(2, physical1and2and3and7) == std::array{false, false, false, false},
          "SC-431: one connected half of a missing stereo pair stays silent");
    check(render(6, physical1and2and7and8) == std::array{false, false, true, true},
          "SC-431: Cue Out 7/8 reaches its packed buffer positions past holes");
    check(render(6, 0xc1) == std::array{false, true, true, false},
          "SC-431: a single earlier output shifts the packed pair by one channel");
}
void metronomeOptions() {
    Fixture f;
    f.main.value = f.reference.value = 0;
    f.store->db().exec("UPDATE tracks SET record_armed=1 WHERE id=1;"
                       "INSERT INTO tracks(id,kind,name,record_armed) VALUES(3,'midi','MIDI capture',1);"
                       "INSERT INTO mixer_strip(track_id) VALUES(3)");
    constexpr auto shortBar = 4 * textproj::kPPQ;
    constexpr auto regularBar = shortBar + 3 * textproj::kWhole / 16;
    f.store->db().exec("INSERT INTO time_signature_map(pos_ticks,numerator,denominator) VALUES (" +
                       std::to_string(shortBar) + ",3,16),(" + std::to_string(regularBar) + ",4,4)");
    check(f.op("cue.setOutput", {{"first", 2}}) && f.op("transport.setMetronome", {{"on", true}}) &&
              f.op("transport.setCountIn", {{"bars", 1}}) && f.load(),
          "SC-256: load the metronome and capture fixture with a 3/16 bar");
    Block b(128);
    const auto hasClick = [&] {
        return std::any_of(b.wide[2].begin(), b.wide[2].begin() + b.io.frames,
                           [](double value) { return std::abs(value) > 0.01; });
    };
    const auto at = [&](std::int64_t sample) {
        f.session.transport().locate(sample);
        f.session.transport().play();
        f.process(b);
        b.io.streamTimeSamples += b.io.frames;
    };
    at(12000);
    check(!hasClick(), "Auto in 4/4 leaves the first eighth-note offbeat quiet");
    check(f.op("transport.setMetronomeOptions", {{"eighthNotes", true}}), "save eighth-note clicks");
    at(12000);
    check(hasClick(), "the eighth-note option adds a click on the offbeat");
    const auto attackAt = [&](std::int64_t sample) {
        constexpr int lead = 8;
        f.session.transport().play(false);
        f.process(b);
        b.io.streamTimeSamples += b.io.frames;
        at(sample - lead);
        return std::all_of(b.wide[2].begin(), b.wide[2].begin() + lead + 1,
                           [](double value) { return value == 0.0; }) &&
               std::abs(b.wide[2][lead + 1]) > 0.01;
    };
    check(attackAt(96000 + 6000), "an eighth note does not divide a 3/16 bar, so Auto clicks on the second sixteenth");
    check(attackAt(114000 + 12000) && readTransportSettings(*f.store).metronomeOptions.eighthNotes,
          "the saved eighth-note rhythm resumes on the first offbeat of the next 4/4 bar");
    at(0);
    const auto classic = b.wide[2];
    check(f.op("transport.setMetronomeOptions", {{"sound", 1}}), "select Wood");
    at(0);
    const auto wood = b.wide[2];
    check(f.op("transport.setMetronomeOptions", {{"sound", 2}}), "select Bell");
    at(0);
    check(hasClick() && classic != wood && classic != b.wide[2] && wood != b.wide[2],
          "the three synthesized sounds render distinct PCM");
    check(f.op("transport.setMetronomeOptions", {{"recordingOnly", true}}), "select recording-only playback");
    const auto saved = readTransportSettings(*f.store).metronomeOptions;
    check(saved.recordingOnly && saved.eighthNotes && saved.sound == MetronomeSound::Bell && !History(*f.store).canUndo(),
          "each option persists independently and stays outside undo");
    OpRequest invalid;
    invalid.opType = "transport.setMetronomeOptions";
    invalid.payload = {{"sound", 9}};
    check(!OpJournal(*f.store).commit(invalid).ok && readTransportSettings(*f.store).metronomeOptions == saved,
          "unsupported sounds are refused without changing options");
    at(0);
    check(!hasClick(), "recording-only mode is silent during ordinary playback");

    std::string why;
    AudioRecordingSpec take;
    take.workFolder = f.temp.path();
    take.sampleRate = 48000;
    take.targets = {{1, {0}}};
    const std::array<float, 128> input{};
    const float* channels[]{input.data()};
    b.io.in = channels;
    b.io.numIn = 1;
    check(f.op("transport.setLoop", {{"on", false}, {"start", textproj::kPPQ / 2}, {"end", textproj::kPPQ}}) &&
              f.op("transport.setPunch", {{"in", true}, {"out", false}}),
          "set Punch-In at the first eighth-note offbeat");
    const auto punchSettings = readTransportSettings(*f.store);
    check(punchSettings.punchIn && !punchSettings.loopEnabled && punchSettings.metronomeOptions == saved,
          "loop, punch and metronome settings read back together");
    check(f.session.startRecording(take, why), "start real audio capture");
    at(0);
    check(!hasClick(), "the rolling click stays quiet through the armed punch lead-in");
    at(12000 - b.io.frames / 2);
    check(std::all_of(b.wide[2].begin(), b.wide[2].begin() + b.io.frames / 2,
                      [](double value) { return value == 0.0; }) && hasClick(),
          "a callback spanning Punch-In sounds the eighth-note click only on its captured half");
    f.session.recording().stop(true);
    at(12000);
    check(!hasClick(), "the recording-only click stops when audio capture ends");
    check(f.op("transport.setPunch", {{"in", false}, {"out", false}}), "clear punch before MIDI capture");

    const std::array<std::int64_t, 1> midiTracks{3};
    check(f.session.startMidiInputCapture(midiTracks, why), "start real MIDI capture");
    at(0);
    check(hasClick(), "the rolling click sounds during MIDI-only capture");
    f.session.midiInputCapture().stop();
    at(0);
    check(!hasClick(), "the recording-only click stops when MIDI capture ends");
    f.session.transport().play(false);
    {
        auto block = f.session.beginRender();
        check(block.startPlayback(0, true), "start the meter-based Count-In");
    }
    f.process(b);
    check(hasClick() && f.session.transport().countInRemaining() > 0,
          "Count-In still sounds with the recording-only gate closed");
    for (int elapsed = b.io.frames; elapsed <= 12000; elapsed += b.io.frames)
        f.process(b);
    check(!hasClick(), "eighth-note playback keeps the Count-In on signature beats");
}
void automationOnlyRecordingClick() {
    Fixture f;
    f.main.value = f.reference.value = 0;
    check(f.op("cue.setOutput", {{"first", 2}}) && f.op("transport.setMetronome", {{"on", true}}) &&
              f.op("transport.setMetronomeOptions", {{"recordingOnly", true}}) && f.load(),
          "load a recording-only click with no armed audio or MIDI tracks");
    Block block(64);
    const auto hasClick = [&](bool playing) {
        f.session.transport().locate(0);
        f.session.transport().play(playing);
        auto render = f.session.beginRender();
        check(render.process(block.io, false), "render without audio or MIDI capture");
        block.io.streamTimeSamples += block.io.frames;
        return std::any_of(block.wide[2].begin(), block.wide[2].begin() + block.io.frames,
                           [](double sample) { return std::abs(sample) > .01; });
    };
    f.session.setAutomationWriteState(true, true);
    f.session.setRecordingForDevices(true);
    check(!hasClick(false), "a recording request waits for playback before sounding the click");
    check(hasClick(true), "a rolling automation-only recording sounds the recording-only click");
    f.session.setRecordingForDevices(false);
    f.session.setAutomationWriteState(true, false);
    check(!hasClick(true), "the recording-only click stops when automation recording ends");
}
void compensatedPunchClick() {
    constexpr int latency = 100, punchFrames = 24, callbackFrames = 64;
    constexpr auto punchInTick = textproj::kPPQ / 2;
    constexpr auto punchOutTick = punchInTick + textproj::kPPQ / 1000; // rounds to 24 frames at 48 kHz
    constexpr auto punchInSample = 12000;
    Fixture f;
    f.main.value = f.reference.value = 0;
    f.effects(latency);
    f.store->db().exec("UPDATE tracks SET record_armed=1 WHERE id=1");
    check(f.op("cue.setOutput", {{"first", 2}}) &&
          f.op("transport.setMetronome", {{"on", true}}) &&
          f.op("transport.setMetronomeOptions", {{"eighthNotes", true}, {"recordingOnly", true}}) &&
          f.op("transport.setLoop", {{"on", false}, {"start", punchInTick}, {"end", punchOutTick}}) &&
          f.op("transport.setPunch", {{"in", true}, {"out", true}}) && f.load(),
          "load a recording-only click for a punch shorter than graph latency");

    AudioRecordingSpec take;
    take.workFolder = f.temp.path();
    take.sampleRate = 48000;
    take.targets = {{1, {0}}};
    std::string why;
    check(f.session.startRecording(take, why), "arm a real capture for the compensated punch");
    const std::array<float, callbackFrames> input{};
    const float* channels[]{input.data()};
    Block b(callbackFrames);
    b.io.in = channels;
    b.io.numIn = 1;
    const auto step = [&] {
        f.process(b);
        b.io.streamTimeSamples += b.io.frames;
    };
    const auto silent = [&](int first, int end) {
        return std::all_of(b.wide[2].begin() + first, b.wide[2].begin() + end,
                           [](double sample) { return sample == 0.0; });
    };
    f.session.transport().locate(punchInSample - callbackFrames);
    f.session.transport().play();
    step();
    check(silent(0, callbackFrames), "the compensated punch lead-in is silent");
    step(); // Both capture edges pass in this callback, while the heard clock is still behind Punch-In.
    check(silent(0, callbackFrames), "the armed take stays quiet until the punch is heard");
    step();
    constexpr int heardIn = latency - callbackFrames, heardOut = heardIn + punchFrames;
    check(silent(0, heardIn + 1) && std::abs(b.wide[2][heardIn + 1]) > 0.01 &&
          std::abs(b.wide[2][heardOut - 1]) > 0.01 && silent(heardOut, callbackFrames),
          "the delayed click begins at heard Punch-In and closes at heard Punch-Out");
    step();
    check(silent(0, callbackFrames), "the click remains quiet after heard Punch-Out");
}
void locateOutsideMidiPunchSilencesClick() {
    constexpr int frames = 64;
    constexpr auto punchInTick = textproj::kPPQ / 2;
    constexpr int punchInSample = 12000;
    Fixture f;
    f.main.value = f.reference.value = 0;
    f.store->db().exec("INSERT INTO tracks(id,kind,name,record_armed) VALUES(3,'midi','MIDI capture',1);"
                       "INSERT INTO mixer_strip(track_id) VALUES(3)");
    check(f.op("cue.setOutput", {{"first", 2}}) &&
          f.op("transport.setMetronome", {{"on", true}}) &&
          f.op("transport.setMetronomeOptions", {{"eighthNotes", true}, {"recordingOnly", true}}) &&
          f.op("transport.setLoop", {{"on", false}, {"start", punchInTick}, {"end", 2 * punchInTick}}) &&
          f.op("transport.setPunch", {{"in", true}, {"out", true}}) && f.load(),
          "load a recording-only click for a MIDI punch");
    const std::array<std::int64_t, 1> tracks{3};
    std::string why;
    check(f.session.startMidiInputCapture(tracks, why), "arm MIDI capture");
    Block b(frames);
    f.session.transport().locate(punchInSample);
    f.session.transport().play();
    f.process(b);
    check(f.session.midiInputCapture().state() == MidiCaptureState::Recording &&
              b.wide[2][0] == 0.0 && std::abs(b.wide[2][1]) > 0.01,
          "MIDI capture hears the click attack at Punch-In");
    b.io.streamTimeSamples += frames;
    f.session.transport().locate(0);
    f.process(b);
    check(std::all_of(b.wide[2].begin(), b.wide[2].begin() + frames,
                      [](double sample) { return sample == 0.0; }),
          "locating an active MIDI take before Punch-In excludes every frame of the new lead-in");
}
void formatChangeClearsPendingRecordingClick() {
    constexpr int latency = 100, frames = 64;
    Fixture f;
    f.main.value = f.reference.value = 0;
    f.effects(latency);
    f.store->db().exec("INSERT INTO tracks(id,kind,name,record_armed) VALUES(3,'midi','MIDI capture',1);"
                       "INSERT INTO mixer_strip(track_id) VALUES(3)");
    check(f.op("cue.setOutput", {{"first", 2}}) &&
          f.op("transport.setMetronome", {{"on", true}}) &&
          f.op("transport.setMetronomeOptions", {{"recordingOnly", true}}) && f.load(),
          "load a delayed recording-only click");
    const std::array<std::int64_t, 1> tracks{3};
    std::string why;
    check(f.session.startMidiInputCapture(tracks, why), "arm MIDI recording ahead of the format change");
    Block b(frames);
    f.session.transport().play();
    f.process(b);
    const auto silent = [&] {
        return std::all_of(b.wide[2].begin(), b.wide[2].begin() + frames,
                           [](double sample) { return sample == 0.0; });
    };
    check(f.session.midiInputCapture().state() == MidiCaptureState::Recording && silent(),
          "the first beat is queued behind the delay while MIDI is captured");
    b.io.streamTimeSamples += frames;
    f.session.prepare(44100, 256);
    f.process(b);
    check(f.session.transport().playing() && f.session.midiInputCapture().state() != MidiCaptureState::Recording &&
              silent(),
          "a live format change leaves playback running and discards the old recording click");
}
void offline() {
    Fixture f;f.effects();f.store->db().exec("UPDATE tracks SET muted=1 WHERE id=2");f.op("cue.setOutput",{{"first",2}});f.op("cue.setMode",{{"on",true}});f.op("cue.setTrack",{{"id",1},{"on",true}});
    f.op("cue.setReference",{{"on",true},{"id",2}});
    check(f.load(Precision::Float64,false),"offline session loads saved Cue/reference choices");
    f.session.setMainAudition(MainAudition::Side);
    Block b;f.process(b);
    check(near(b.wide[0][511],0.0025) && b.wide[2][511]==0,"offline rendering excludes reference and ignores Cue audition/mode solo gates");
}
void selectionAndRole() {
    Fixture f;f.op("cue.setReference",{{"on",false},{"id",2}});check(f.load(),"generic audition fixture loads");Block b;f.process(b);
    check(near(b.wide[0][511],0.075),"performance audition designation alone does not alter the project mix");
    check(f.op("cue.setOutput",{{"first",2}}) && f.op("cue.setMode",{{"on",true}}) && f.op("cue.setTrack",{{"id",1},{"on",true}}),"independent Cue selection commits");
    check(!f.session.model().tracks.front().soloed && readCueSettings(*f.store).selectedTracks==std::vector<std::int64_t>{1},"Cue selection never overwrites a stored solo flag");
    const auto snapshot=SnapshotBuilder::fromStore(*f.store);check(snapshot->cue.selectedTracks==std::vector<std::int64_t>{1},"snapshot carries selected Cue track ids");
    // Read compatibility only: win's N38 owns the real DDL/version/migration.
    f.store->db().exec("ALTER TABLE tracks ADD COLUMN role TEXT;UPDATE tracks SET role='reference' WHERE id=2");
    check(f.session.refresh(*f.store),"optional native role can be read when the format supplies it");f.process(b);
    check(readCueSettings(*f.store).projectReferenceTrack==2 && near(b.wide[0][511],0.025),"native reference role excludes its track from Main");
    check(f.op("cue.setReference",{{"on",true}}),"native reference audition engages");f.process(b);
    check(near(b.wide[0][511],0.5),"native reference auditions past Main");
}
void defaultRole() {
    Fixture f;f.store->db().exec("ALTER TABLE tracks ADD COLUMN role TEXT;UPDATE tracks SET role='reference' WHERE id=2");
    check(readCueSettings(*f.store).referenceTrack==2 && readCueSettings(*f.store).referenceFromRole,"native role supplies the default audition target");
    check(f.op("cue.setVolume",{{"db",-3.0}}),"unrelated Cue control saves with a native role present");
    f.store->db().exec("UPDATE tracks SET role=NULL;UPDATE tracks SET role='reference' WHERE id=1");
    check(readCueSettings(*f.store).referenceTrack==1,"Cue preference save does not pin an old native reference role");
    check(f.op("cue.setReference",{{"on",true}}),"reference enable can use the new native role without an explicit id");
}
void automationLifetime() {
    rows::Model model;rows::Track ref;ref.id=2;ref.kind="audio";model.tracks.push_back(ref);
    rows::Track main;main.id=9;main.kind="master";model.tracks.push_back(main);
    CueSettings settings;settings.referenceTrack=2;Transport transport;CueMonitor monitor;
    auto owner=std::make_shared<StripAutomation>();owner->byTrack[2]={};
    std::weak_ptr<StripAutomation> weak=owner;
    monitor.configure(model,settings,&transport,owner,true);
    GraphHost graph;RealizeOptions options;options.monitors=monitor.points({});
    check(graph.rebuild(model,options,48000,64),"published reference automation fixture builds");
    Block block(64);graph.process(block.io);
    monitor.configure(model,settings,&transport,{},true);owner.reset();monitor.collect();
    check(!weak.expired(),"retired reference lanes stay alive until the audio reader moves");
    graph.process(block.io);monitor.collect();
    check(weak.expired(),"reference lane owners are reclaimed off audio after the new parameters run");
}
void automationPlays() {
    Fixture f;f.effects();f.store->db().exec("UPDATE tracks SET muted=1 WHERE id=2;"
        "INSERT INTO automation_lanes(id,owner_kind,owner_id,param_ref,time_base,value_domain) VALUES(100,'track',2,'volume',1,'real'),(101,'track',2,'mute',1,'real')");
    auto lane=[&](std::int64_t id,double value){AutomationPoint point{};point.point_id=1;point.value=value;
        const std::array<AutomationPoint,1> points{point};
        const auto bytes=writeStream<AutomationPoint>(FourCC::Automation,points,1,StreamFlags::SortedByTime|StreamFlags::TimeIsNanos);
        if(!f.store->putAutomationData(id,std::nullopt,bytes))throw std::runtime_error("cue automation fixture");};
    lane(100,-6.020599913279624);lane(101,1);
    f.op("cue.setReference",{{"on",true},{"id",2}});check(f.load(Precision::Float64),"reference volume/mute automation fixture loads");Block b;f.process(b);
    check(near(b.wide[0][511],0.5),"reference follows volume automation while PFL ignores Main mute automation");
    lane(100,-12.041199826559248);check(f.session.refresh(*f.store),"changed reference lane publishes");f.process(b);f.session.tick(1000);
    check(near(b.wide[0][511],0.25),"newly published reference automation plays after the graph swap");
}
void mainListening(Precision precision, bool exporting) {
    Fixture f;
    f.store->db().exec("UPDATE mixer_strip SET volume_db=0 WHERE track_id=9");
    f.main.rightFactor = -0.5;
    f.reference.value = 0.;
    check(f.load(precision, true, exporting), "Main listening fixture loads");
    const auto tap = f.session.openScope(9, .25, ScopePoint::PostFader);
    check(tap != nullptr, "Main post-fader tap observes the stored mix before the monitor pair");
    f.session.transport().play();
    Block block(128);
    f.process(block);
    const auto sample = [&](int channel, int i) {
        const auto ch = static_cast<std::size_t>(channel), at = static_cast<std::size_t>(i);
        return precision == Precision::Float32 ? static_cast<double>(block.narrow[ch][at]) : block.wide[ch][at];
    };
    const double l = sample(0,127), r = sample(1,127);
    check(l > 0. && r < 0., "left and right contain different signed programme");
    for (const auto mode : {MainAudition::Left, MainAudition::Right, MainAudition::Mid, MainAudition::Side, MainAudition::Stereo}) {
        f.session.setMainAudition(mode);
        allocations.store(0); countAllocations.store(true);
        f.process(block);
        countAllocations.store(false);
        const double solo = mode == MainAudition::Left ? l : mode == MainAudition::Right ? r :
                            mode == MainAudition::Mid ? 0.5*(l+r) : 0.5*(l-r);
        const bool dry = exporting || mode == MainAudition::Stereo;
        bool match = true;
        for (int i=0;i<128;++i)
            match = match && sample(0,i) == (dry?l:solo) && sample(1,i) == (dry?r:solo)
                          && sample(2,i)==0. && sample(3,i)==0.;
        check(match && !allocations.load(), "Main audition matrix is exact, physical pair only, export independent and allocation free");
        std::array<float, 128> storedLeft{}, storedRight{};
        std::int64_t stamp = 0;
        check(tap && tap->read(storedLeft, storedRight, stamp) &&
                  std::all_of(storedLeft.begin(), storedLeft.end(), [&](float x) { return x == static_cast<float>(l); }) &&
                  std::all_of(storedRight.begin(), storedRight.end(), [&](float x) { return x == static_cast<float>(r); }),
              "Main post-fader capture is identical in every monitor mode");
    }
    using Filter = dsp::StereoMeterWeighting;
    using Weighting = Filter::Weighting;
    constexpr std::array modes{MainAudition::FilterFlat, MainAudition::FilterA,
                               MainAudition::FilterC, MainAudition::FilterK};
    constexpr std::array weights{Weighting::Standard, Weighting::A, Weighting::C, Weighting::K};
    for (std::size_t m = 0; m < modes.size(); ++m) {
        f.session.setMainAudition(modes[m]);
        allocations.store(0); countAllocations.store(true);
        f.process(block);
        countAllocations.store(false);
        Filter filter;
        std::array<Filter::History, 2> history{};
        check(filter.prepare(48000., weights[m]), "Main listen reference coefficients prepare at the session rate");
        const auto matches = [&](double actual, double original, Filter::History &state) {
            const auto expected = exporting ? original : .45 * filter.process(original, state);
            return exporting ? actual == original : near(actual, expected, 5e-8);
        };
        bool match = true;
        for (int i = 0; i < 128; ++i)
            match = matches(sample(0,i), l, history[0]) && matches(sample(1,i), r, history[1]) &&
                    sample(2,i) == 0. && sample(3,i) == 0. && match;
        check(match && !allocations.load(), "Main Filter Listen applies its selected profile allocation-free to physical stereo only");
        std::array<float, 128> storedLeft{}, storedRight{};
        std::int64_t stamp = 0;
        check(tap->read(storedLeft, storedRight, stamp) &&
                  std::all_of(storedLeft.begin(), storedLeft.end(), [&](float x) { return x == static_cast<float>(l); }) &&
                  std::all_of(storedRight.begin(), storedRight.end(), [&](float x) { return x == static_cast<float>(r); }),
              "Main Filter Listen has no effect on the post-fader tap");
    }
}
void filterListenLifecycle() {
    CueMonitor monitor;
    const CueSettings settings;
    GraphHost graph;
    Block block(64);
    using Filter = dsp::StereoMeterWeighting;
    Filter reference;
    std::array<Filter::History, 4> referenceHistory{};
    const auto prepare = [&](double rate) {
        check(monitor.publish(settings, rate, 64, 2, true) &&
                  reference.prepare(rate, Filter::Weighting::C),
              "monitor filters prepare before audio");
        referenceHistory = {};
    };
    const auto run = [&](bool enabled, bool exporting = false) {
        constexpr std::array narrow{.91f, -.328f, .125f, -.18f};
        constexpr std::array wide{.1023, .7512, -.001, .375};
        for (std::size_t channel = 0; channel < narrow.size(); ++channel) {
            block.narrow[channel].fill(narrow[channel]);
            block.wide[channel].fill(wide[channel]);
        }
        block.io.exporting = exporting;
        monitor.process(graph, block.io, {});
        bool match = true;
        for (std::size_t channel = 0; channel < narrow.size(); ++channel)
            for (std::size_t i = 0; i < 64; ++i) {
                if (enabled && channel < 2) {
                    match = near(block.narrow[channel][i], .45 * reference.process(narrow[channel], referenceHistory[channel]), 5e-8) &&
                            near(block.wide[channel][i], .45 * reference.process(wide[channel], referenceHistory[channel + 2]), 1e-10) && match;
                } else {
                    match = block.narrow[channel][i] == narrow[channel] && block.wide[channel][i] == wide[channel] && match;
                }
            }
        return match;
    };
    prepare(48000.);
    monitor.setMainAudition(MainAudition::FilterC);
    check(run(true) && run(true), "four independent histories continue across callbacks");
    check(run(false, true) && run(true), "export neither hears nor advances the active monitor filter");
    check(monitor.publish(settings, 48000., 64, 2, true) && run(true),
          "same-format publication preserves the active monitor stream");
    monitor.setMainAudition(MainAudition::Stereo);
    check(run(false), "leaving Filter Listen returns bit-exact direct audio");
    monitor.setMainAudition(MainAudition::FilterC);
    referenceHistory = {};
    check(run(true), "reentering Filter Listen starts with clean state");
    prepare(44100.);
    check(run(true), "rate-only change uses the new coefficients and clean histories");
    monitor.release();
    prepare(44100.);
    check(run(true), "same-format device restart resets monitor histories");
    prepare(96000.);
    check(run(true), "96 kHz monitor follows its prepared weighting");
}
void hardwareMainOut() {
    Fixture f;
    f.main.rightFactor = -0.5;
    check(f.op("mixer.setMainOutput", {{"first", 6}}) && f.load(Precision::Float64),
          "SC-261: the physical Main Out pair is saved");
    Block block;
    block.io.outputChannels.enabled = 0b11000011; // physical 1/2 and 7/8, packed into four pointers
    f.process(block);
    check(near(block.narrow[2][511], .075) && near(block.wide[3][511], .0375) &&
          block.wide[0][511] == 0 && block.narrow[1][511] == 0,
          "SC-261: stereo Main follows the selected pair on a device with sparse physical outputs");
    check(f.op("cue.setOutput", {{"first", 6}}) &&
          f.op("cue.setReference", {{"on", true}, {"id", 2}}),
          "SC-261: Cue and reference may share the remapped Main pair");
    f.process(block);
    check(near(block.wide[2][511], 0.5) && near(block.narrow[3][511], 0.5) && block.wide[0][511] == 0,
          "SC-261: shared Main and Cue play the exclusive reference once");
    Fixture offline;
    offline.main.rightFactor = -0.5;
    check(offline.op("mixer.setMainOutput", {{"first", 6}}) &&
          offline.op("cue.setReference", {{"on", true}, {"id", 2}}) &&
          offline.load(Precision::Float64, false, true), "SC-261: the project renders without performance settings");
    offline.process(block);
    check(near(block.wide[0][511], .075) && near(block.wide[1][511], .0375) && block.wide[2][511] == 0,
          "SC-261: physical Main Out and reference never change the exported channels");
    Fixture missing;
    check(missing.op("mixer.setMainOutput", {{"first", 6}}) && missing.op("cue.setOutput", {{"first", 4}}) &&
          missing.load(Precision::Float64),
          "SC-261: a song may name a Main Out pair that the device has not opened");
    Block sparse;
    sparse.io.outputChannels.enabled = 0b00111100; // physical 3/4 and 5/6: Main Out 7/8 is missing
    const auto silent = [&](std::size_t channel) {
        return std::ranges::all_of(sparse.wide[channel], [](double sample) { return sample == 0.; }) &&
               std::ranges::all_of(sparse.narrow[channel], [](float sample) { return sample == 0.f; });
    };
    missing.process(sparse);
    check(silent(0) && silent(1) && silent(2) && silent(3),
          "SC-261: a missing Main Out pair is silent and never plays on the device's first outputs");
    check(missing.op("cue.setReference", {{"on", true}, {"id", 2}}),
          "SC-261: the reference engages while Main Out is missing");
    missing.process(sparse);
    check(silent(0) && silent(1) && near(sparse.wide[2][511], 0.5) && near(sparse.narrow[3][511], 0.5),
          "SC-261: with Main Out missing the reference reaches Cue Out and no other output");
    check(missing.op("cue.setOutput", {{"first", 2}}),
          "SC-261: Cue Out can occupy the first two packed channels when Main Out is missing");
    missing.process(sparse);
    check(near(sparse.wide[0][511], 0.5) && near(sparse.narrow[1][511], 0.5) && silent(2) && silent(3),
          "SC-261: reference reaches the first packed Cue pair exactly once");
    Fixture single;
    check(single.load(Precision::Float64), "SC-261: load Main Out 1/2 for devices that lack output 2");
    Block mono(512, 1);
    mono.io.outputChannels.enabled = 0b1;
    single.process(mono);
    check(near(mono.wide[0][511], .075) && near(mono.narrow[0][511], .075),
          "SC-261: a one-channel device still plays Main's left on its own output 1");
    Block half(512, 3);
    half.io.outputChannels.enabled = 0b1101; // physical 1, 3 and 4: the right of Main Out 1/2 is missing
    single.process(half);
    check(near(half.wide[0][511], .075) && half.wide[1][511] == 0. && half.narrow[1][511] == 0.f &&
          half.wide[2][511] == 0.,
          "SC-261: Main's right never plays on another pair's output when only its left is open");
}
void inactiveBits() {
    CueMonitor monitor;CueSettings settings;monitor.publish(settings,48000,64,2,true);GraphHost graph;Block b(64);
    for(auto& channel:b.narrow)for(auto& sample:channel)sample=std::bit_cast<float>(std::uint32_t{0x80000000u});
    for(auto& channel:b.wide)for(auto& sample:channel)sample=std::bit_cast<double>(std::uint64_t{0x8000000000000000ull});
    monitor.process(graph,b.io,{});
    check(std::bit_cast<std::uint32_t>(b.narrow[0][0])==0x80000000u && std::bit_cast<std::uint64_t>(b.wide[0][0])==0x8000000000000000ull,
          "inactive monitoring preserves negative-zero output bits");
}
void coldReference() {
    Fixture f;f.effects(100);check(f.load(),"cold reference with delayed Main loads");Block b;f.process(b);
    check(f.op("cue.setReference",{{"on",true},{"id",2}}),"select and engage a new reference while the project plays");f.process(b);
    check(std::all_of(b.wide[0].begin(),b.wide[0].begin()+100,[](double v){return near(v,0.0125);}),"cold cue delay fills before Main starts fading");
    check(near(b.wide[0][100],0.0125*(239.0/240)+1.0/240) && near(b.wide[0][339],1),"five-ms swap starts exactly when the new reference's compensated audio is ready");
}
void allocation() {
    Fixture f;f.effects(100);f.op("cue.setMode",{{"on",true}});f.op("cue.setTrack",{{"id",1},{"on",true}});
    f.op("cue.setOutput",{{"first",2}});f.op("cue.setReference",{{"on",true},{"id",2}});f.op("transport.setMetronome",{{"on",true}});
    f.op("transport.setMetronomeOptions", {{"sound", 2}, {"eighthNotes", true}});
    f.op("mixer.setMainOutput", {{"first", 2}});
    check(f.load(Precision::Float64),"allocation fixture loads off-thread");f.session.transport().play();Block b(128);
    allocations.store(0);countAllocations.store(true);for(int i=0;i<50;++i)f.process(b);countAllocations.store(false);
    check(allocations.load()==0,"Cue graph copy, reference ramp, volume and metronome allocate nothing");
}

}
int main(){try{settings();referenceSwap(Precision::Float32);referenceSwap(Precision::Float64);soloAndCue();compensatedAndClick();samplePreview();sparseCueOutput();metronomeOptions();automationOnlyRecordingClick();compensatedPunchClick();locateOutsideMidiPunchSilencesClick();formatChangeClearsPendingRecordingClick();offline();selectionAndRole();defaultRole();for(auto precision:{Precision::Float32,Precision::Float64}) { mainListening(precision,false); mainListening(precision,true); }filterListenLifecycle();automationLifetime();automationPlays();hardwareMainOut();inactiveBits();coldReference();allocation();}
    catch(const std::exception& e){check(false,e.what());}std::printf("%s -- %d checks, %d failure(s)\n",failures?"FAIL":"PASS",checks,failures);return failures?1:0;}
