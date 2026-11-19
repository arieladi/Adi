// SPDX-License-Identifier: AGPL-3.0-or-later
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
#include <cstdio>
#include <cstdlib>
#include <new>
#include <set>
#include <stdexcept>
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
    check(builtinCatalog().size()==18 && !findBuiltin("unregistered"),"catalogue has stable known entries and rejects unknown UID");
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
    check(available==17,"all registered native canonical factories are available");
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
          !prepareBuiltinInsert(store,"org.adidaw.builtin.hybrid-reverb",1).ok &&
          !prepareBuiltinInsert(store,"org.adidaw.builtin.utility",9,prepared.deviceId).ok,"unknown/unavailable devices, missing track and selection on another track refuse");
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
int main(){try{catalogueAndHost();paramsAndState();insertionAndHistory();playingSession();}catch(const std::exception& exception){counting=false;check(false,exception.what());}std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
