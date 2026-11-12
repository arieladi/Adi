// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/audio_recording.hpp"
#include "adi/engine/session.hpp"
#include "adi/engine/snapshot.hpp"
#include "adi/history.hpp"
#include "adi/store.hpp"
#include "adi/transport_settings.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <thread>
namespace {
std::atomic<unsigned> callbackStartAllocations{0}, callbackStartFrees{0};
}
void *operator new(std::size_t size) {
    if (adi::audio::onAudioThread)
        ++callbackStartAllocations;
    if (void *p = std::malloc(size ? size : 1))
        return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t size) { return operator new(size); }
void operator delete(void *p) noexcept {
    if (p && adi::audio::onAudioThread)
        ++callbackStartFrees;
    std::free(p);
}
void operator delete[](void *p) noexcept { operator delete(p); }
void operator delete(void *p, std::size_t) noexcept { operator delete(p); }
void operator delete[](void *p, std::size_t) noexcept { operator delete(p); }
namespace {
using namespace adi;using namespace adi::engine;
int checks=0,failures=0;
void check(bool good,const char* name){++checks;if(!good){++failures;std::printf("FAIL: %s\n",name);}}
struct Fixture {
    test::TempDirectory temp{"transport_settings","project"};std::unique_ptr<Store> store;
    Fixture(){StoreError error{};store=Store::create(temp.path()/"song.adi",error);if(!store)throw std::runtime_error("fixture");
        store->db().exec("INSERT INTO tracks(id,kind,name) VALUES(9,'master','Main');INSERT INTO mixer_strip(track_id) VALUES(9);INSERT INTO tempo_map(id,pos_ticks,bpm,curve) VALUES(1,0,120,0)");}
    CommitResult op(const char* name,Payload payload){OpRequest request;request.opType=name;request.payload=std::move(payload);return OpJournal(*store).commit(request);}
};
void storage() {
    Fixture f;const auto initial=SnapshotBuilder::fromStore(*f.store);
    check(!initial->transport.metronome && !initial->transport.loopEnabled,"new project has loop/click off");
    const auto changed=f.op("project.setName",{{"name","Undo this"}});
    const auto head=History(*f.store).head();
    check(changed.ok && f.op("transport.setMetronome",{{"on",true}}).ok,"metronome op applies saved performance state");
    check(f.op("transport.setLoop",{{"on",true},{"start",100},{"end",1000}}).ok,"loop op applies a tick window");
    const auto next=SnapshotBuilder::fromStore(*f.store,*initial);
    check(next->transport.metronome,"snapshot carries metronome state");
    check(next->transport.loopEnabled,"snapshot carries loop switch");
    check(next->transport.loopStartTicks==100,"snapshot carries loop start ticks");
    check(next->transport.loopEndTicks==1000,"snapshot carries loop end ticks");
    check(!next->tracks.empty() && !initial->tracks.empty() &&
              next->tracks.front() == initial->tracks.front(),
          "transport-only snapshot retains unchanged track nodes");
    check(History(*f.store).head()==head && History(*f.store).undo().ok,"performance ops leave undo at the musical edit");
    check(readTransportSettings(*f.store)==next->transport,"undo leaves performance settings intact");
    check(f.op("transport.setLoop",{{"on",false}}).ok,"loop off can retain its window");
    const auto retained=readTransportSettings(*f.store);
    check(!retained.loopEnabled && retained.loopStartTicks==100 && retained.loopEndTicks==1000,"loop bounds survive toggling off");
    const auto count=f.store->db().execAndGet("SELECT count(*) FROM ops").getInt();
    check(!f.op("transport.setLoop",{{"on",true},{"start",1000},{"end",100}}).ok && !f.op("transport.setLoop",{{"on",true},{"start",-1},{"end",100}}).ok,"reversed and negative windows are refused");
    check(f.store->db().execAndGet("SELECT count(*) FROM ops").getInt()==count && readTransportSettings(*f.store)==retained,"invalid loop changes neither rows nor journal");
    OpRequest on;on.opType="transport.setMetronome";on.payload={{"on",false}};
    OpRequest invalid;invalid.opType="transport.setLoop";invalid.payload={{"on",true},{"start",2},{"end",1}};
    check(!OpJournal(*f.store).commit(std::vector<OpRequest>{on,invalid}).ok && readTransportSettings(*f.store).metronome,"invalid batched loop rolls back metronome too");
    check(f.store->close(),"performance fixture closes");const auto path=f.store->path();f.store.reset();
    StoreError error{};f.store=Store::open(path,error,true);
    check(f.store && readTransportSettings(*f.store)==retained,"saved performance state survives reopen read-only");
    f.store->close();f.store.reset();f.store=Store::open(path,error);
    f.store->db().exec("UPDATE session_state SET value='broken' WHERE key='transport.loop'");
    check(!readTransportSettings(*f.store).hasLoop,"malformed session metadata falls back without trusting a bad loop");
}
std::vector<float> render(Session& session,int frames) {
    std::vector<float> result;result.reserve(static_cast<std::size_t>(frames));
    std::array<float,128> l{},r{};float* outputs[]{l.data(),r.data()};AudioIo io;io.out=outputs;io.numOut=2;
    while(frames>0){io.frames=std::min(frames,128);session.process(io);result.insert(result.end(),l.begin(),l.begin()+io.frames);frames-=io.frames;}
    return result;
}
bool audible(const std::vector<float>& audio,std::size_t at,std::size_t count) {
    if (at > audio.size() || count > audio.size() - at)
        return false;
    return std::any_of(audio.begin()+static_cast<std::ptrdiff_t>(at),audio.begin()+static_cast<std::ptrdiff_t>(at+count),[](float v){return std::abs(v)>0.01f;});
}
void playback() {
    Fixture f;check(f.op("transport.setMetronome",{{"on",true}}).ok && f.op("transport.setLoop",{{"on",true},{"start",0},{"end",textproj::kPPQ/2}}).ok,"playback controls commit");
    SessionSpec spec;spec.sampleRate=48000;spec.maxFrames=128;Session session;
    check(session.load(*f.store,{},spec),"session loads saved performance controls");session.transport().play();
    const auto clicks=render(session,30000);
    check(session.transport().position()==6000,"saved quarter-second loop wraps within variable-sized callbacks");
    check(audible(clicks,0,80) && audible(clicks,12000,80) && audible(clicks,24000,80),"metronome clicks on a beat and on each loop's beat restart");
    check(!audible(clicks,3600,80) && !audible(clicks,15600,80),"click decays to silence between beats");
    const auto revision=session.transport().revision();
    check(session.refresh(*f.store),"ordinary rebuild while loop is playing");render(session,1);
    check(session.transport().revision()==revision && session.transport().position()==6001,"same settings do not reset or seek a running loop");
    check(f.op("transport.setLoop",{{"on",false}}).ok && session.refresh(*f.store),"live loop-off publishes");render(session,18000);
    check(session.transport().position()==24001,"loop off advances through the old end");
    check(f.op("transport.setMetronome",{{"on",false}}).ok && session.refresh(*f.store),"live metronome-off publishes");const auto quiet=render(session,1000);
    check(std::all_of(quiet.begin(),quiet.end(),[](float v){return v==0;}),"metronome off leaves silent output untouched");
    check(f.op("transport.setMetronome",{{"on",true}}).ok && session.refresh(*f.store),"turn click back on");session.transport().play(false);const auto stopped=render(session,5000);
    check(std::all_of(stopped.begin(),stopped.end(),[](float v){return v==0;}),"stopped transport does not click");
    check(f.op("transport.setLoop",{{"on",true},{"start",0},{"end",textproj::kPPQ}}).ok,"one beat loop window");
    f.store->db().exec("UPDATE tempo_map SET bpm=60");check(session.refresh(*f.store),"tempo edit rebuilds loop conversion");session.transport().locate(0);session.transport().play();render(session,60000);
    check(session.transport().position()==12000,"loop uses the tempo map instead of fixed 120 BPM");
    Session offline;spec.performanceState=false;check(offline.load(*f.store,{},spec),"offline session can opt out of saved performance controls");offline.transport().play();const auto exported=render(offline,60000);
    check(offline.transport().position()==60000 && std::all_of(exported.begin(),exported.end(),[](float v){return v==0;}),"offline output neither loops nor includes monitor clicks");
}
class LateSilence final:public Node {
public:
    void process(const NodeIo& io) noexcept override{for(int c=0;c<io.channels;++c)for(int i=0;i<io.frames;++i)io.out[c][io.blockOffset+i]=0;}
    std::int32_t latencySamples() const noexcept override{return 100;}
};
void compensation() {
    Fixture f;f.op("transport.setMetronome",{{"on",true}});
    f.op("transport.setLoop",{{"on",true},{"start",0},{"end",textproj::kPPQ/2}});
    LateSilence late;Session session;session.setSourcesFor([&](std::int64_t id){return id==9?std::vector<Node*>{&late}:std::vector<Node*>{};});
    SessionSpec spec;spec.maxFrames=128;
    check(session.load(*f.store,{},spec),"compensated click fixture loads");session.transport().play();
    const auto clicks=render(session,13000);
    check(session.graph().renderedLatency()==100,"rendered graph reports the latency actually in play");
    check(clicks.size() >= 100 &&
              std::all_of(clicks.begin(), clicks.begin() + 100, [](float v) { return v == 0; }) &&
              audible(clicks, 100, 80),
          "click waits for compensated Main audio");
    check(!audible(clicks,12000,80) && audible(clicks,12100,80),"click compensation stays aligned across a loop wrap");
    Transport transport;transport.loop(0,2,true);transport.play();transport.advance(50);
    check(transport.delayedSample(0,100)==-1,"delay longer than a loop does not invent pre-start history");
    transport.advance(50);check(transport.delayedSample(0,100)==0,"delayed clock resolves after multiple wraps");
    f.op("transport.setLoop",{{"on",true},{"start",0},{"end",1}});
    check(session.refresh(*f.store),"sub-sample tick window publishes");session.transport().locate(0);render(session,129);
    check(session.transport().position()==0,"valid sub-sample loop rounds to one frame rather than switching off");
}

void signatureAndWidth() {
    Fixture f;f.op("transport.setMetronome",{{"on",true}});
    f.store->db().exec("INSERT INTO time_signature_map(id,pos_ticks,numerator,denominator) VALUES(1,0,3,8)");
    Session session;SessionSpec spec;spec.maxFrames=128;spec.precision=Precision::Float64;
    check(session.load(*f.store,{},spec),"3/8 double session loads");session.transport().play();
    std::array<double,128> l{},r{};std::array<float,128> lf{},rf{};
    double* wide[]{l.data(),r.data()};float* narrow[]{lf.data(),rf.data()};
    AudioIo io;io.out=narrow;io.out64=wide;io.numOut=2;io.frames=128;session.process(io);
    check(l==r && std::any_of(l.begin(),l.end(),[](double v){return std::abs(v)>0.01;}),"native double output receives the click on both channels");
    check(std::equal(l.begin(),l.end(),lf.begin(),[](double a,float b){return static_cast<float>(a)==b;}),"dual output pointers receive the click once at each width");
    session.transport().locate(0);const auto clicks=render(session,40000);
    check(audible(clicks,12000,80) && audible(clicks,24000,80) && audible(clicks,36000,80),"signature denominator sets the eighth-note click grid");
    check(clicks.size() > 36001 && std::abs(clicks[1]) > 1.8f * std::abs(clicks[12001]) &&
              std::abs(clicks[1] - clicks[36001]) < 1e-6f,
          "signature numerator accents the first of three beats");
}

void startSettings() {
    Fixture f;
    const auto defaults = readTransportSettings(*f.store);
    check(defaults.countInBars == 0 && !defaults.songStartTicks,
          "new transport has no count-in or song-start override");
    check(f.op("project.setName", {{"name", "musical edit"}}).ok,
          "seed musical undo before saved start controls");
    const auto head = History(*f.store).head();
    for (const int bars : {0, 1, 2, 4})
        check(f.op("transport.setCountIn", {{"bars", bars}}).ok &&
                  readTransportSettings(*f.store).countInBars == bars,
              "all supported count-in choices save");
    for (const auto &value : std::array<Payload, 6>{-1, 3, 5, 1.5, "1", true})
        check(!f.op("transport.setCountIn", {{"bars", value}}).ok,
              "invalid count-in choice refuses without coercion");
    check(f.op("transport.setSongStart", {{"on", true}, {"start", 1234}}).ok,
          "song start stores an exact musical tick");
    const auto snapshot = SnapshotBuilder::fromStore(*f.store);
    check(snapshot->transport.countInBars == 4 && snapshot->transport.songStartTicks == 1234,
          "snapshot exposes both saved start controls");
    check(History(*f.store).head() == head && History(*f.store).undo().ok,
          "saved start controls do not become musical undo steps");
    check(readTransportSettings(*f.store).songStartTicks == 1234,
          "musical undo leaves song start intact");
    const auto before = readTransportSettings(*f.store);
    for (const auto &payload : std::array<Payload, 4>{
             Payload{{"on", true}}, Payload{{"on", true}, {"start", -1}},
             Payload{{"on", true}, {"start", 1.5}}, Payload{{"on", true}, {"start", "12"}}})
        check(!f.op("transport.setSongStart", payload).ok,
              "missing/negative/nonintegral song start refuses");
    check(readTransportSettings(*f.store) == before,
          "rejected start controls leave all performance values intact");
    OpRequest first;
    first.opType = "transport.setSongStart";
    first.payload = {{"on", true}, {"start", 99}};
    OpRequest bad;
    bad.opType = "transport.setCountIn";
    bad.payload = {{"bars", 3}};
    check(!OpJournal(*f.store).commit(std::vector<OpRequest>{first, bad}).ok &&
              readTransportSettings(*f.store) == before,
          "failed batch rolls back an earlier song-start write");
    check(f.store->close(), "saved start fixture closes");
    const auto path = f.store->path();
    f.store.reset();
    StoreError error{};
    f.store = Store::open(path, error, true);
    check(f.store && readTransportSettings(*f.store) == before,
          "saved start controls reopen read-only exactly");
    if (!f.store)
        return;
    f.store->close();
    f.store.reset();
    f.store = Store::open(path, error);
    check(f.store && f.op("transport.setSongStart", {{"on", false}}).ok &&
              !readTransportSettings(*f.store).songStartTicks,
          "clearing song start restores insert-marker precedence");
    if (!f.store)
        return;
    for (const auto *invalid :
         {"3", "-1", "1.5", "\"2\"", "9999999999999999999999999999999999", "null"}) {
        SQLite::Statement set(f.store->db(),
                              "UPDATE session_state SET value=? WHERE key='transport.countIn'");
        set.bind(1, invalid);
        set.exec();
        check(readTransportSettings(*f.store).countInBars == 0,
              "corrupt count-in metadata falls back to None");
    }
    for (const auto *invalid : {"-1", "1.5", "\"123\"", "18446744073709551615", "null", "broken"}) {
        SQLite::Statement set(
            f.store->db(),
            "INSERT OR REPLACE INTO session_state(key,value) VALUES('transport.songStart',?)");
        set.bind(1, invalid);
        set.exec();
        check(!readTransportSettings(*f.store).songStartTicks,
              "corrupt song-start metadata cannot become a timeline position");
    }
}
void startPolicy() {
    Fixture f;
    constexpr auto q = textproj::kPPQ;
    check(f.op("transport.setSongStart", {{"on", true}, {"start", 2 * q}}).ok &&
              f.op("transport.setCountIn", {{"bars", 1}}).ok,
          "prepare saved start policy");
    Session session;
    SessionSpec spec;
    spec.maxFrames = 128;
    check(session.load(*f.store, {}, spec), "load start-policy session");
    {
        auto block = session.beginRender();
        check(block.startPlayback(q), "default play accepts saved song start");
    }
    check(session.transport().playing() && session.transport().position() == 48000 &&
              session.transport().countInRemaining() == 0,
          "ordinary play uses saved point immediately, without recording count-in");
    session.transport().locate(17);
    session.transport().play();
    render(session, 1);
    check(session.transport().position() == 18,
          "explicit locate/play bypasses the saved default point");
    {
        auto block = session.beginRender();
        check(!block.startPlayback(-1), "negative fallback refuses without changing transport");
    }
    check(session.transport().position() == 18 && session.transport().playing(),
          "rejected start preserves running state");
    check(f.op("transport.setSongStart", {{"on", false}}).ok && session.refresh(*f.store),
          "cleared start policy publishes");
    {
        auto block = session.beginRender();
        check(block.startPlayback(q), "default play falls back to insert marker after clear");
    }
    check(session.transport().position() == 24000,
          "insert marker determines default start with no override");
    check(f.op("transport.setSongStart", {{"on", true}, {"start", 2 * q}}).ok,
          "restore saved point for offline control");
    Session offline;
    spec.performanceState = false;
    check(offline.load(*f.store, {}, spec), "offline start policy loads");
    {
        auto block = offline.beginRender();
        check(block.startPlayback(q, true), "offline explicit render start is accepted");
    }
    check(offline.transport().playing() && offline.transport().position() == 24000 &&
              offline.transport().countInRemaining() == 0,
          "offline ignores song-start and count-in performance choices");
    for (const int bars : {0, 1, 2, 4}) {
        check(f.op("transport.setCountIn", {{"bars", bars}}).ok && session.refresh(*f.store),
              "updated count-in choice publishes");
        {
            auto block = session.beginRender();
            check(block.startPlayback(0, true), "record start accepts each supported duration");
        }
        check(session.transport().countInRemaining() == static_cast<std::int64_t>(bars) * 96000 &&
                  session.transport().playing() == (bars == 0),
              "all supported durations use complete launch-meter bars");
        session.transport().play(false);
    }
    check(f.op("transport.setLoop", {{"on", true}, {"start", 0}, {"end", 2 * q}}).ok &&
              f.op("transport.setCountIn", {{"bars", 1}}).ok &&
              f.op("transport.setSongStart", {{"on", true}, {"start", q}}).ok &&
              session.refresh(*f.store),
          "prepare a saved song point inside the active loop");
    {
        auto block = session.beginRender();
        check(block.startPlayback(0, true), "looped record start accepts count-in");
    }
    render(session, 96013);
    check(session.transport().position() == 24013 && session.transport().looping(),
          "count-in holds through its full bar and starts inside the authored loop");
    {
        auto block = session.beginRender();
        check(block.startPlayback(0, true), "restart count-in for malformed callback guard");
    }
    std::array<float, 16> rejected;
    rejected.fill(1);
    float *rejectedOut[]{rejected.data()};
    AudioIo invalid;
    invalid.frames = 16;
    invalid.numIn = 65;
    invalid.numOut = 1;
    invalid.out = rejectedOut;
    const auto remaining = session.transport().countInRemaining();
    {
        auto block = session.beginRender();
        check(!block.process(invalid),
              "unsupported count-in input width refuses before a partial slice");
    }
    check(session.transport().countInRemaining() == remaining &&
              std::all_of(rejected.begin(), rejected.end(), [](float value) { return value == 0; }),
          "invalid count-in callback leaves countdown intact and clears its output");
    Transport direct;
    direct.locate(50);
    direct.countIn(3);
    direct.advance(5);
    check(direct.playing() && direct.position() == 52 && direct.countInRemaining() == 0,
          "transport countdown overshoot advances only post-launch timeline frames");
    direct.countIn(100);
    direct.play(false);
    direct.advance(1000);
    check(!direct.playing() && direct.countInRemaining() == 0 && direct.position() == 52,
          "stop cancels a stopped lead-in without later resurrection");
    direct.countIn(100);
    direct.locate(73);
    direct.advance(1000);
    check(!direct.playing() && direct.position() == 73 && direct.countInRemaining() == 0,
          "locate cancels a lead-in");
    direct.countIn(100);
    direct.play();
    direct.advance(1);
    check(direct.playing() && direct.position() == 74 && direct.countInRemaining() == 0,
          "explicit play bypasses a pending countdown");
}
void countInSound() {
    for (const auto width : {Precision::Float32, Precision::Float64}) {
        Fixture f;
        check(f.op("transport.setCountIn", {{"bars", 1}}).ok,
              "count-in saves while ordinary metronome stays off");
        f.store->db().exec("INSERT INTO time_signature_map(id,pos_ticks,numerator,denominator) "
                           "VALUES(1,0,3,8);UPDATE tempo_map SET bpm=60");
        Session session;
        SessionSpec spec;
        spec.maxFrames = 128;
        spec.precision = width;
        check(session.load(*f.store, {}, spec), "3/8 count-in session loads at either width");
        {
            auto block = session.beginRender();
            check(block.startPlayback(0, true), "record start begins a meter-aware count-in");
        }
        check(!session.transport().playing() && session.transport().countInRemaining() == 72000,
              "one 3/8 bar at60BPM holds the song for72000frames");
        const auto audio = render(session, 72001);
        check(audio.size() == 72001 && audible(audio, 0, 80) && audible(audio, 24000, 80) &&
                  audible(audio, 48000, 80),
              "all three count-in beats are audible with ordinary metronome off");
        check(audio.size() == 72001 && !audible(audio, 6000, 80) &&
                  std::abs(audio[1]) > 1.8f * std::abs(audio[24001]),
              "count-in decays and accents its first meter beat");
        check(session.transport().playing() && session.transport().position() == 1 &&
                  session.transport().countInRemaining() == 0,
              "crossing callback starts the timeline on the exact frame");
        const auto after = render(session, 128);
        check(std::all_of(after.begin(), after.end(), [](float value) { return value == 0; }),
              "ordinary metronome remains off after count-in");
        {
            auto block = session.beginRender();
            check(block.startPlayback(0, true),
                  "restart count-in before format-change cancellation");
        }
        session.prepare(44100, 128);
        render(session, 128);
        check(!session.transport().playing() && session.transport().countInRemaining() == 0,
              "device format change cancels rather than retiming a pending lead-in");
        {
            auto block = session.beginRender();
            check(block.startPlayback(0, true), "restart count-in before a same-format device stop");
        }
        render(session, 128);
        check(session.transport().countInRemaining() > 0, "the lead-in is under way");
        session.release();
        session.prepare(44100, 128);
        const auto resumed = render(session, 48000);
        check(!session.transport().playing() && session.transport().countInRemaining() == 0 &&
                  std::all_of(resumed.begin(), resumed.end(), [](float value) { return value == 0; }),
              "a device stop cancels the lead-in: a same-format restart neither clicks nor starts the timeline");
    }
}
AudioRecordingResult finishStartRecording(AudioInputRecorder &recorder) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        if (auto result = recorder.poll(true))
            return *result;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error("count-in recording did not finalize");
}
void countInRecording() {
    for (const auto width : {Precision::Float32, Precision::Float64}) {
        Fixture f;
        f.store->db().exec(
            "INSERT INTO tracks(id,kind,name,record_armed) VALUES(21,'audio','Armed',1);INSERT "
            "INTO mixer_strip(track_id) VALUES(21)");
        check(f.op("transport.setCountIn", {{"bars", 1}}).ok &&
                  f.op("transport.setSongStart", {{"on", true}, {"start", textproj::kPPQ}}).ok,
              "recording fixture saves one-bar count-in and song point");
        Session session;
        SessionSpec spec;
        spec.sampleRate = 48000;
        spec.maxFrames = 128;
        spec.precision = width;
        check(session.load(*f.store, {}, spec), "actual count-in recording session loads");
        AudioRecordingSpec request;
        request.workFolder = f.temp.path();
        request.sampleRate = 48000;
        request.maxFrames = 128;
        request.ringFrames = 4096;
        request.targets = {{21, {1}}};
        std::string error;
        check(session.startRecording(request, error),
              "prepare actual input recorder before driver record start");
        {
            auto block = session.beginRender();
            check(block.startPlayback(0, true), "driver starts count-in with recorder ready");
        }
        std::array<float, 128> left{}, right{}, source{}, other{};
        std::array<double, 128> left64{}, right64{};
        float *out[]{left.data(), right.data()};
        double *wide[]{left64.data(), right64.data()};
        const float *in[]{other.data(), source.data()};
        std::int64_t stream = 1000;
        std::vector<float> expected;
        const auto ioFor = [&](int frames) {
            AudioIo io;
            io.frames = frames;
            io.numIn = 2;
            io.in = in;
            io.numOut = 2;
            io.out = out;
            io.streamTimeSamples = stream;
            if (width == Precision::Float64)
                io.out64 = wide;
            return io;
        };
        session.process(ioFor(17));
        stream += 17;
        check(left == right && std::any_of(left.begin(), left.begin() + 17,
                                           [](float value) { return std::abs(value) > 0.01f; }),
              "count-in click reaches the stereo monitor pair");
        if (width == Precision::Float64)
            check(std::equal(left64.begin(), left64.begin() + 17, left.begin(),
                             [](double a, float b) { return static_cast<float>(a) == b; }) &&
                      left64 == right64,
                  "native64 and simultaneous32 output receive count-in once each");
        else
            check(
                std::all_of(left64.begin(), left64.end(), [](double value) { return value == 0; }),
                "32-bit callback does not touch absent64 outputs");
        for (int frame = 17; frame < 95993;) {
            const auto frames = std::min(128, 95993 - frame);
            session.process(ioFor(frames));
            frame += frames;
            stream += frames;
        }
        check(session.transport().position() == 24000 &&
                  session.transport().countInRemaining() == 7 &&
                  session.recording().acceptedFrames() == 0,
              "no timeline or input frames leak during the lead-in");
        for (std::size_t i = 0; i < source.size(); ++i) {
            source[i] = static_cast<float>(i) / 128;
            other[i] = -source[i];
        }
        for (const auto invalidClock : std::array<std::int64_t, 3>{-1, INT64_MAX, INT64_MAX - 10}) {
            AudioIo overflow = ioFor(19);
            overflow.streamTimeSamples = invalidClock;
            {
                auto block = session.beginRender();
                check(!block.process(overflow),
                      "invalid whole stream interval refuses before either half");
            }
            check(session.transport().countInRemaining() == 7 &&
                      session.recording().acceptedFrames() == 0,
                  "clock refusal cannot consume a prefix or start recording");
        }
        expected.insert(expected.end(), source.begin() + 7, source.begin() + 19);
        const auto alloc = callbackStartAllocations.load(), freed = callbackStartFrees.load();
        const auto ioCount = audio::callbackFileIo.load();
        session.process(ioFor(19));
        stream += 19;
        check(callbackStartAllocations.load() == alloc && callbackStartFrees.load() == freed &&
                  audio::callbackFileIo.load() == ioCount,
              "exact count-in split allocates/frees/opens no files on the callback");
        check(session.transport().position() == 24012 && session.recording().acceptedFrames() == 12,
              "crossing block captures only its12 post-count-in samples");
        expected.insert(expected.end(), source.begin(), source.begin() + 13);
        session.process(ioFor(13));
        stream += 13;
        session.recording().stop();
        session.transport().play(false);
        auto result = finishStartRecording(session.recording());
        check(result.failure == RecordingFailure::None && result.frames == 25 &&
                  result.startSample == 24000 && result.tracks.size() == 1,
              "actual recording begins at song point and survives exact physical stream offset");
        std::vector<float> actual(expected.size());
        if (result.tracks.size() == 1) {
            audio::WavReader reader(result.tracks.front().file);
            check(reader.read(actual.data(), static_cast<std::uint32_t>(actual.size())) ==
                          actual.size() &&
                      actual == expected,
                  "actual WAV excludes lead-in PCM and preserves both suffixes bit for bit");
        } else
            check(false, "recording has one take to inspect");
        result.discard();
    }
}
}
int main() {
    try {
        storage();
        playback();
        compensation();
        signatureAndWidth();
        startSettings();
        startPolicy();
        countInSound();
        countInRecording();
    } catch (const std::exception &e) {
        check(false, e.what());
    }
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
