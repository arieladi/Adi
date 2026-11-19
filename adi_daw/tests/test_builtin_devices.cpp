// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/sample_slots.hpp"
#include "adi/blob.hpp"
#include <cstring>
#include "temp_directory.hpp"
#include "adi/builtin_catalog.hpp"
#include "juce/builtin_host.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/history.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <set>
#include <stdexcept>
#include <utility>
namespace {
std::atomic<int> allocations{0};thread_local bool counting=false;
}
void* operator new(std::size_t bytes){if(counting)allocations.fetch_add(1,std::memory_order_relaxed);if(void* value=std::malloc(bytes?bytes:1))return value;throw std::bad_alloc();}
void* operator new[](std::size_t bytes){return operator new(bytes);}
void operator delete(void* value)noexcept{std::free(value);}
void operator delete[](void* value)noexcept{std::free(value);}
void operator delete(void* value,std::size_t)noexcept{std::free(value);}
void operator delete[](void* value,std::size_t)noexcept{std::free(value);}
namespace {
using namespace adi;using namespace adi::device;using namespace adi::engine;
int checks=0,failures=0;
void check(bool ok,const char* what){++checks;if(!ok){++failures;std::printf("FAIL %s\n",what);}}
rows::PluginRef reference(std::string uid) {
    rows::PluginRef result;result.id=1;result.format="internal";result.uid=std::move(uid);result.name="Built-in";result.vendor="ADI";result.version="1";return result;
}
#if ADI_WITH_PD
std::unique_ptr<DeviceInstance> load(const char* uid) {
    auto ref=reference(uid);std::string error;DeviceRequest request;request.ref=&ref;
    auto instance=makeBuiltinDevice(request,error);
    if(!instance)throw std::runtime_error(error);
    return instance;
}
struct Project {
    test::TempDirectory temporary{"builtin_devices","project"};std::unique_ptr<Store> store;
    Project() {
        StoreError error{};store=Store::create(temporary.path()/"song.adi",error);
        if(!store)throw std::runtime_error(toString(error));
        store->db().exec("INSERT INTO tracks(id,kind,name) VALUES(1,'audio','Audio'),(9,'master','Main')");
    }
};
#endif
void catalogueAndHost() {
    check(builtinCatalog().size()==19 && !findBuiltin("unregistered"),"catalogue has stable known entries and rejects unknown UID");
    std::set<std::string_view> identities;int available=0;
    for(const auto& info:builtinCatalog()) {
        check(identities.insert(info.uid).second && !info.name.empty() && !info.category.empty() && !info.group.empty(),"catalogue identities are unique and labels/category/folder are present");
        std::string error;auto ref=reference(std::string(info.uid));DeviceRequest request;request.ref=&ref;
        auto instance=makeBuiltinDevice(request,error);
        if(!info.available){check(!instance && !error.empty(),"unavailable backend refuses by name");continue;}
        ++available;check(instance && instance->loaded() && instance->identity().uid==info.uid && !builtinCanvas(info.uid).empty(),"canonical factory creates supported device without a binary scan or file path");
        if(!instance)continue;
        check(instance->paramCount()>0 && instance->paramAt(0) && instance->findParam(instance->paramAt(0)->id),"declared parameter identities and descriptors are readable");
        instance->prepare(48000,64);
        std::array<std::array<float,64>,2> input{},output{};float* in[]={input[0].data(),input[1].data()};float* out[]={output[0].data(),output[1].data()};
        NodeIo io;io.in=in;io.out=out;io.channels=2;io.frames=64;
        instance->process(io);
        allocations.store(0,std::memory_order_relaxed);counting=true;
        {audio::CallbackScope callback;for(int block=0;block<4;++block)instance->process(io);}
        counting=false;
        check(allocations.load(std::memory_order_relaxed)==0,"canonical built-in processing allocates nothing");
    }
#if ADI_WITH_PD
    check(available==19,"all registered native canonical factories are available");
#else
    check(available==0,"PD-disabled build advertises no unavailable backend as loadable");
#endif
    auto fake=reference("not.a.builtin");DeviceRequest request;request.ref=&fake;std::string error;
    check(!makeBuiltinDevice(request,error) && !error.empty(),"unknown internal reference stays a named placeholder failure");
    int delegated=0;
    auto composed=withBuiltinDevices([&](const DeviceRequest&,std::string& message){++delegated;message="external";return std::unique_ptr<DeviceInstance>{};});
    fake.format="clap";(void)composed(request,error);
    check(delegated==1 && error=="external","composition retains external-format loader");
}
void paramsAndState() {
#if ADI_WITH_PD
    auto instance=load("org.adidaw.builtin.utility");
    check(instance->findParam("1")->shape==ParamShape::Switch && instance->findParam("3")->shape==ParamShape::Menu &&
          instance->findParam("11")->shape==ParamShape::Continuous && instance->findParam("11")->hasRealRange,"control shapes and real units follow declarations");
    check(instance->paramText("3",1)=="Stereo" && instance->paramText("9",0).find("Hz")!=std::string::npos,"host renders menu items and declared units");
    check(instance->setParam("11",ParamValue::fromNormalized(0.8)) && std::abs(instance->getParam("11").real-4)<1e-4,"normalized setter exposes its real value before the next process block");
    const auto saved=instance->saveState("state");auto payload=Payload::from_cbor(saved);payload["future"]={{"kept",7}};payload["params"]["future-param"]=0.3;
    check(instance->loadState("state",Payload::to_cbor(payload)),"known state accepts and retains future fields and unknown parameter IDs");
    const auto kept=Payload::from_cbor(instance->saveState("state"));
    check(kept.at("future")==payload.at("future") && kept.at("params").at("future-param")==0.3,"state round trip preserves future opaque fields");
    payload["params"]["1"]=1.0;payload["params"]["11"]=-1.0;
    const auto before=instance->getParam("1").normalized;
    check(!instance->loadState("state",Payload::to_cbor(payload)) && instance->getParam("1").normalized==before,"bad later state value refuses before any parameter changes");
    check(!instance->setParam("missing",ParamValue{}) && !instance->setParam("11",ParamValue::fromNormalized(2)) &&
          !instance->loadState("state",{1,2,3}) && !instance->loadState("component",saved),"missing parameters, bad values and unsupported/malformed state refuse");
    auto restored=load("org.adidaw.builtin.utility");
    check(restored->loadState("state",saved) && restored->getParam("11").normalized==instance->getParam("11").normalized,"stored state restores parameter intent exactly");
    check(!restored->setExpressionRoute(RouteChoice::NoteExpression) && restored->setExpressionRoute(RouteChoice::MpeMidi),"format route supports its declared MIDI boundary");
#endif
}
void insertionAndHistory() {
#if ADI_WITH_PD
    Project project;auto& store=*project.store;auto& db=store.db();
    const auto prepared=prepareBuiltinInsert(store,"org.adidaw.builtin.utility",1);
    check(prepared.ok && prepared.options.expectHead && !prepared.options.expectHead->has_value() &&
          db.execAndGet("SELECT count(*) FROM plugin_refs").getInt()==0 && db.execAndGet("SELECT count(*) FROM devices").getInt()==0,"browser preparation captures root/head/ref/chain/IDs without project writes");
    const auto result=commitBuiltinInsert(store,prepared);
    if(!result.ok)std::printf("insert: %s\n",result.error.c_str());
    check(result.ok && result.seqs.size()==3 && db.execAndGet("SELECT count(*) FROM devices").getInt()==1,"missing plug-in ref, chain and device are one native undo step");
    check(History(store).undo().ok && db.execAndGet("SELECT count(*) FROM devices").getInt()==0 &&
          db.execAndGet("SELECT count(*) FROM plugin_refs").getInt()==0 && db.execAndGet("SELECT count(*) FROM device_chains").getInt()==0,"undo removes exactly the rows the insertion created");
    check(History(store).redo().ok && db.execAndGet("SELECT plugin_ref_id FROM devices").getInt64()==prepared.referenceId,"redo restores captured identities and links");
    const auto second=prepareBuiltinInsert(store,"org.adidaw.builtin.utility",1,prepared.deviceId);
    check(second.ok && second.referenceId==prepared.referenceId && second.chainId==prepared.chainId &&
          commitBuiltinInsert(store,second).ok,"reuse existing ref/chain and insert before selected device");
    check(db.execAndGet("SELECT id FROM devices ORDER BY ord,id LIMIT 1").getInt64()==second.deviceId &&
          db.execAndGet("SELECT count(*) FROM plugin_refs").getInt()==1,"browser ordering and reference deduplication are deterministic");
    db.exec("UPDATE devices SET ord=9223372036854775807");
    const auto tied=prepareBuiltinInsert(store,"org.adidaw.builtin.delay",1,prepared.deviceId);
    check(tied.ok && commitBuiltinInsert(store,tied).ok && History(store).undo().ok &&
        db.execAndGet("SELECT count(*) FROM devices WHERE ord=9223372036854775807").getInt()==2,
        "insertion normalizes tied maximum ordinals and undo restores exact original values");
    db.exec("ALTER TABLE plugin_refs ADD COLUMN future_data BLOB");
    db.exec("UPDATE plugin_refs SET future_data=x'00ff00'");
    OpRequest remove;remove.opType="pluginRef.unregister";remove.payload={{"id",prepared.referenceId}};
    check(!OpJournal(store).commit(remove).ok && db.execAndGet("SELECT count(*) FROM plugin_refs").getInt()==1,
        "used reference removal refuses atomically through foreign keys");
    const auto future=prepareBuiltinInsert(store,"org.adidaw.builtin.reverb",1);
    check(future.ok && future.requests.front().payload.at("row").at("future_data").is_null(),
        "new reference captures defaults of future typed columns");
    const auto stale=prepareBuiltinInsert(store,"org.adidaw.builtin.reverb",1);
    OpRequest rename;rename.opType="track.rename";rename.payload={{"id",1},{"name","Newer"}};
    check(OpJournal(store).commit(rename).ok,"intervening browser insertion fixture edit");
    const auto refused=commitBuiltinInsert(store,stale);
    check(!refused.ok && refused.stale && db.execAndGet("SELECT count(*) FROM devices").getInt()==2,"stale browser result refuses all new rows");
    check(!prepareBuiltinInsert(store,"unknown",1).ok && !prepareBuiltinInsert(store,"org.adidaw.builtin.utility",999).ok &&
          !prepareBuiltinInsert(store,"org.adidaw.builtin.utility",9,prepared.deviceId).ok,"unknown devices, a missing track and a selection on another track refuse (every catalogue entry is available with Pd; the PD-disabled build refuses them all)");
#endif
}
void playingSession() {
#if ADI_WITH_PD
    Project project;auto& store=*project.store;auto& db=store.db();
    const auto prepared=prepareBuiltinInsert(store,"org.adidaw.builtin.utility",1);check(commitBuiltinInsert(store,prepared).ok,"session fixture inserts builtin");
    std::vector<float> samples(8192,0.25f);audio::WavWriter writer(project.temporary.path()/"audio.wav",48000,2,audio::WavFormat::Float32);writer.write(samples.data(),4096);writer.close();
    db.exec("INSERT INTO media_files(id,hash_blake3,rel_path,format) VALUES(1,'test','audio.wav','wav')");
    db.exec("INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns) VALUES(1,1,'audio',1,0,1000000000)");
    db.exec("INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(1,1,0,4096)");
    OpRequest gain;gain.opType="device.setParam";gain.payload={{"dev",prepared.deviceId},{"param","11"},{"norm",126.0/155}};
    check(OpJournal(store).commit(gain).ok,"persist builtin control as ordinary parameter op");
    Session session;session.graph().setFadeFrames(0);
    check(session.load(store,withBuiltinDevices(),{2,48000,64}) && session.stats().loaded==1,"project internal reference becomes loaded device in ordinary Session");
    if(!session.error().empty())std::printf("session: %s\n",session.error().c_str());
    session.transport().play();std::array<std::array<float,64>,2> output{};float* out[]={output[0].data(),output[1].data()};
    bool primed=true;
    for(int block=0;block<20;++block) {primed=primed && session.clips()->prime();AudioIo io;io.out=out;io.numOut=2;io.frames=64;session.process(io);}
    check(primed && std::abs(output[0][63]-0.25f*std::pow(10.0f,6.0f/20))<2e-5f,"stored built-in gain processes real clip audio through the complete graph");
#endif
}
}
// DEV-21: OneShot is a loadable instrument, so a MIDI clip sounds. Through
// the product's own calls: the browser insert (registered as an instrument),
// the sample slot assignment (#414), an ordinary Session with a MIDI clip.
// DEV-22: Hybrid Reverb is loadable. Through the product's calls: the browser
// insert, an impulse response assigned to its slot (#414), an ordinary Session
// at 48 kHz, the Convolution route alone, fully wet, so a constant 0.25 comes
// out at 0.25 times the IR's DC gain once the 256-sample partition has passed.
// Two IRs, each one impulse of 0.5: at the session's rate on its first sample
// (nothing resampled: exact), and at 44.1 kHz 64 samples in (resampled with its
// DC gain kept). An impulse on the very first sample of an IR at another rate
// loses the half of its resampled pulse that falls before time zero.
void hybridSession() {
#if ADI_WITH_PD
    const auto* info=findBuiltin("org.adidaw.builtin.hybrid-reverb");
    check(info && info->available,"Hybrid Reverb is available in the catalogue");
    if(!info || !info->available)return;
    struct Case{std::uint32_t rate;std::size_t at;float tolerance;const char* what;};
    for(const Case& c:{Case{48000,0,1e-6f,"an IR at the session's rate convolves exactly"},
                       Case{44100,64,1e-3f,"an IR at 44.1 kHz is resampled to 48 kHz with its DC gain kept"}}) {
        Project project;auto& store=*project.store;auto& db=store.db();
        const auto inserted=prepareBuiltinInsert(store,"org.adidaw.builtin.hybrid-reverb",1);
        check(inserted.ok && commitBuiltinInsert(store,inserted).ok,"the browser inserts Hybrid Reverb on an audio track");
        const auto ir=project.temporary.path()/"impulse.wav";
        {
            std::vector<float> impulse(512,0.f);impulse[c.at]=.5f;
            audio::WavWriter writer(ir,c.rate,1,audio::WavFormat::Float32);writer.write(impulse.data(),512);writer.close();
        }
        const auto assigned=prepareSampleAssignment(store,inserted.deviceId,1,ir,48000,7);
        if(!assigned.ok)std::printf("assign: %s\n",assigned.error.c_str());
        check(assigned.ok && commitSampleAssignment(store,assigned).ok,"an impulse response is assigned to Hybrid Reverb's slot as one undo step");
        for(const auto& [param,norm]:{std::pair<const char*,double>{"1",1.0},{"14",1.0}}) {  // Route: Convolution; Dry/Wet 100 %
            OpRequest set;set.opType="device.setParam";set.payload={{"dev",inserted.deviceId},{"param",param},{"norm",norm}};
            check(OpJournal(store).commit(set).ok,"Hybrid Reverb parameter set");
        }
        std::vector<float> samples(96000,0.25f);
        audio::WavWriter writer(project.temporary.path()/"audio.wav",48000,2,audio::WavFormat::Float32);writer.write(samples.data(),48000);writer.close();
        db.exec("INSERT INTO media_files(id,hash_blake3,rel_path,format) VALUES(2,'test','audio.wav','wav')");
        db.exec("INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns) VALUES(1,1,'audio',1,0,1000000000)");
        db.exec("INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(1,2,0,48000)");
        Session session;session.graph().setFadeFrames(0);
        const bool loaded=session.load(store,withBuiltinDevices(),{2,48000,256});
        if(!session.error().empty())std::printf("session: %s\n",session.error().c_str());
        check(loaded && session.stats().loaded==1,"the project with Hybrid Reverb loads in an ordinary Session");
        session.transport().play();
        std::vector<float> left(256),right(256);float* out[]={left.data(),right.data()};
        for(int block=0;block<40;++block){(void)session.clips()->prime();AudioIo io;io.out=out;io.numOut=2;io.frames=256;session.process(io);}
        std::printf("METRIC Hybrid Reverb, a 0.5 impulse at %u Hz (sample %zu) convolving 0.25 at 48 kHz: %.6f\n",c.rate,c.at,left.back());
        check(std::abs(left.back()-0.125f)<c.tolerance && std::abs(right.back()-0.125f)<c.tolerance,c.what);
    }
#endif
}
void instrumentSession() {
#if ADI_WITH_PD
    const auto* info=findBuiltin("org.adidaw.builtin.oneshot");
    check(info && info->instrument && info->midi && info->inputs==0 && info->available,"OneShot is in the catalogue as an instrument");
    auto instance=load("org.adidaw.builtin.oneshot");
    check(instance->eventFlow()==EventFlow::Consume,"and consumes the notes it is given");
    Project project;auto& store=*project.store;auto& db=store.db();
    db.exec("INSERT INTO tracks(id,kind,name) VALUES(2,'midi','Keys')");
    const auto inserted=prepareBuiltinInsert(store,"org.adidaw.builtin.oneshot",2);
    check(inserted.ok && commitBuiltinInsert(store,inserted).ok,"the browser inserts OneShot on a MIDI track");
    check(db.execAndGet("SELECT subtype FROM plugin_refs WHERE uid='org.adidaw.builtin.oneshot'").getString()=="instrument",
          "registered with subtype instrument");
    // A 1 kHz sine at 0.5, 1 s, stereo.
    const auto file=project.temporary.path()/"tone.wav";
    {
        std::vector<float> tone(96000);
        for(std::size_t i=0;i<48000;++i)tone[2*i]=tone[2*i+1]=static_cast<float>(.5*std::sin(2*3.141592653589793*1000*static_cast<double>(i)/48000));
        audio::WavWriter writer(file,48000,2,audio::WavFormat::Float32);writer.write(tone.data(),48000);writer.close();
    }
    const auto assigned=prepareSampleAssignment(store,inserted.deviceId,1,file,48000,7);
    if(!assigned.ok)std::printf("assign: %s\n",assigned.error.c_str());
    check(assigned.ok && commitSampleAssignment(store,assigned).ok,"the sample is assigned to OneShot's slot as one undo step");
    // One note: C3 (60, OneShot's root) from the start, one quarter, full velocity.
    db.exec("INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks) VALUES(5,2,'midi',0,23063040)");
    {
        StreamHeader header{static_cast<std::uint32_t>(FourCC::Notes),1,40,1,0};
        NoteRecord note{};note.start_ticks=0;note.dur_ticks=5765760;note.note_id=1;note.key=60;note.vel_on=127;
        note.vel_off=64;note.probability=10000;
        std::vector<std::uint8_t> bytes(sizeof header+sizeof note);
        std::memcpy(bytes.data(),&header,sizeof header);std::memcpy(bytes.data()+sizeof header,&note,sizeof note);
        SQLite::Statement statement(db,"INSERT INTO event_streams(id,clip_id,stream_kind,data) VALUES(50,5,'notes',?)");
        statement.bind(1,bytes.data(),static_cast<int>(bytes.size()));statement.exec();
    }
    Session session;session.graph().setFadeFrames(0);
    const bool loaded=session.load(store,withBuiltinDevices(),{2,48000,64});
    if(!session.error().empty())std::printf("session: %s\n",session.error().c_str());
    check(loaded,"the project with OneShot loads in an ordinary Session");
    session.transport().play();
    std::array<std::array<float,64>,2> output{};float* out[]={output[0].data(),output[1].data()};
    double peak=0;int firstSound=-1;
    for(int block=0;block<200;++block) {
        (void)session.clips()->prime();AudioIo io;io.out=out;io.numOut=2;io.frames=64;session.process(io);
        for(int i=0;i<64;++i){const double v=std::abs(static_cast<double>(output[0][static_cast<std::size_t>(i)]));
            if(v>1e-4 && firstSound<0)firstSound=block*64+i;peak=std::max(peak,v);}
    }
    std::printf("METRIC OneShot in a Session: first sound at frame %d, peak %.4f\n",firstSound,peak);
    // At full velocity, OneShot's default Volume (Live's -12 dB) on the 0.5 sine.
    check(firstSound>=0 && firstSound<256 && std::abs(peak-.5*std::pow(10.,-12./20))<2e-3,"the MIDI clip's note plays the sample through OneShot, at its default -12 dB");
#endif
    hybridSession();  // DEV-22
}
int main(){try{catalogueAndHost();paramsAndState();insertionAndHistory();playingSession();instrumentSession();}catch(const std::exception& exception){counting=false;check(false,exception.what());}std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
