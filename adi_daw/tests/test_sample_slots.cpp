// SPDX-License-Identifier: AGPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/sample_slots.hpp"
#include "adi/builtin_catalog.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/engine/snapshot.hpp"
#include "adi/history.hpp"
#include "adi/store.hpp"
#include "juce/builtin_host.hpp"
#include "juce/builtin_samples.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <cstdlib>
#include <new>
namespace {thread_local bool countAllocations=false;int callbackAllocations=0;}
void* operator new(std::size_t bytes){if(countAllocations)++callbackAllocations;if(void* p=std::malloc(bytes?bytes:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t bytes){return operator new(bytes);}
void operator delete(void* p)noexcept{std::free(p);}
void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}
void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
namespace {
using namespace adi;using namespace adi::device;using namespace adi::engine;
int checks=0,failures=0;
void check(bool ok,const char* text){++checks;if(!ok){++failures;std::printf("FAIL %s\n",text);}}
#if ADI_WITH_PD
void writeSample(const std::filesystem::path& path) {
    std::vector<float> silence(256);audio::WavWriter writer(path,48000,1,audio::WavFormat::Float32);writer.write(silence.data(),256);writer.close();
}
std::unique_ptr<DeviceInstance> load() {
    rows::PluginRef ref;ref.format="internal";ref.uid="org.adidaw.builtin.color-cab";
    DeviceRequest request;request.ref=&ref;std::string error;auto result=makeBuiltinDevice(request,error);
    if(!result)throw std::runtime_error(error);
    return result;
}
float render(DeviceInstance& instance) {
    std::array<std::array<float,64>,2> input{},output{};for(auto& plane:input)plane.fill(0.25f);
    float* in[]={input[0].data(),input[1].data()};float* out[]={output[0].data(),output[1].data()};
    NodeIo io;io.in=in;io.out=out;io.channels=2;io.frames=64;
    audio::CallbackScope callback;
    countAllocations=true;
    for(int block=0;block<40;++block)instance.process(io);
    countAllocations=false;
    return output[0][63];
}
void assignmentAndPlayback() {
    test::TempDirectory directory{"sample_slots","project"};StoreError error{};auto store=Store::create(directory.path()/"song.adi",error);
    if(!store)throw std::runtime_error(toString(error));
    auto& db=store->db();
    db.exec("INSERT INTO tracks(id,kind,name) VALUES(1,'audio','Audio'),(9,'master','Main')");
    const auto insert=prepareBuiltinInsert(*store,"org.adidaw.builtin.color-cab",1);check(commitBuiltinInsert(*store,insert).ok,"sample fixture inserts Color Cab through public browser API");
    const auto file=directory.path()/"sample.wav";writeSample(file);
    auto instance=load();instance->prepare(48000,64);ParamOps glue;check(glue.attach(insert.deviceId,*instance),"existing parameter/state glue attaches sample roles");
    check(std::abs(render(*instance)-0.25f)<1e-6f,"empty slot is identity before assignment");
    const auto oldHead=OpJournal(*store).headSeq();const auto prepared=prepareSampleAssignment(*store,insert.deviceId,1,file,48000,7);
    if(!prepared.ok)std::printf("prepare: %s\n",prepared.error.c_str());
    check(prepared.ok && prepared.blobs.size()==2 && prepared.requests.size()==3 && OpJournal(*store).headSeq()==oldHead &&
        db.execAndGet("SELECT count(*) FROM media_files").getInt()==0 && db.execAndGet("SELECT count(*) FROM state_blobs").getInt()==0,
        "preparation imports/decodes/designs and captures empty baseline with no project writes");
    const auto committed=commitSampleAssignment(*store,prepared);if(!committed.ok)std::printf("commit: %s\n",committed.error.c_str());
    check(committed.ok && History(*store).nextUndo()->opCount==3 && History(*store).nextUndo()->label=="Assign device sample","import and slot state are one undo step");
    for(const auto& request:prepared.requests)if(request.opType=="device.loadState")glue.appliedState(*store,request.payload,0);
    check(std::abs(render(*instance))<1e-6f,"assigned derived silent profile changes real native processing");
    const auto saved=instance->saveState("sample:1");const auto state=Payload::from_cbor(saved);
    check(state.at("mediaHash").get<std::string>().size()==64 && state.at("derived").contains("target") && state.at("source_f32le").is_binary(),
        "state keeps media identity, derived profile and rate-portable source backup");
    auto first=SnapshotBuilder::fromStore(*store);auto shared=SnapshotBuilder::fromStore(*store,*first);
    check(first->sampleSlots.size()==1 && !first->sampleSlots[0]->empty && !first->sampleSlots[0]->missing && first->sampleSlots[0]->hasDerivedState &&
        first->sampleSlots[0]->mediaId==prepared.mediaId && first->sampleSlots[0]->name=="Impulse" && first->sampleSlots[0]==shared->sampleSlots[0],
        "snapshot exposes complete assigned slot and structurally shares unchanged metadata");
    std::filesystem::remove(file);auto missing=SnapshotBuilder::fromStore(*store,*first);
    check(missing->sampleSlots[0]->missing && !first->sampleSlots[0]->missing && missing->sampleSlots[0]!=first->sampleSlots[0],"missing original marks new snapshot without mutating old frames");
    auto restored=load();restored->prepare(44100,64);
    check(restored->loadState("sample:1",saved) && std::abs(render(*restored))<1e-6f,"derived state and rate-portable backup restore sound without the missing original at another engine rate");
    auto malformed=state;malformed["source_f32le"]=Payload::binary({0,0,128,127});
    check(!instance->loadState("sample:1",Payload::to_cbor(malformed)) && instance->saveState("sample:1")==saved,"nonfinite sample backup refuses atomically");
    malformed=state;malformed["slot"]=2;
    check(!instance->loadState("sample:1",Payload::to_cbor(malformed)) && !instance->loadState("sample:2",saved),"slot identity and role must match declarations");
    malformed=state;malformed["future"]={{"kept",true}};
    check(instance->loadState("sample:1",Payload::to_cbor(malformed)) && Payload::from_cbor(instance->saveState("sample:1")).at("future")==malformed.at("future"),"sample state retains future opaque fields");
    const auto logged=OpJournal(*store).recent(3);
    check(History(*store).undo().ok && db.execAndGet("SELECT count(*) FROM plugin_state").getInt()==0 && db.execAndGet("SELECT count(*) FROM media_files").getInt()==0,
        "undo restores original absent media and state rows");
    for(const auto& op:logged)if(op.opType=="device.loadState" && op.inverse)glue.appliedState(*store,*op.inverse,1);
    check(std::abs(render(*instance)-0.25f)<1e-6f,"undo's captured empty baseline clears the live sample as well as SQL");
    check(History(*store).redo().ok,"sample assignment redo restores content-addressed references");
    for(const auto& request:prepared.requests)if(request.opType=="device.loadState")glue.appliedState(*store,request.payload,2);
    check(std::abs(render(*instance))<1e-6f,"redo restores the sample without file access");
    writeSample(file);const auto again=prepareSampleAssignment(*store,insert.deviceId,1,file,48000,8);
    check(again.ok && again.mediaId==prepared.mediaId && again.requests.size()==1,"same media content deduplicates and existing baseline is not repeated");
    auto poolChanged=SnapshotBuilder::fromStore(*store,*first);
    db.exec("UPDATE media_files SET missing=1");auto flagged=SnapshotBuilder::fromStore(*store,*poolChanged);
    check(flagged->sampleSlots[0]->missing && !poolChanged->sampleSlots[0]->missing,"pool missing flag changes sharing even while the locator exists");
    db.exec("UPDATE media_files SET missing=0");
    db.exec("UPDATE plugin_refs SET uid='org.adidaw.builtin.utility'");auto wrongUid=SnapshotBuilder::fromStore(*store,*poolChanged);
    check(!wrongUid->sampleSlots[0]->problem.empty() && wrongUid->sampleSlots[0]!=poolChanged->sampleSlots[0],"cached state metadata still validates changed device UID");
    db.exec("UPDATE plugin_refs SET uid='org.adidaw.builtin.color-cab'");
    const auto clear=prepareSampleClear(*store,insert.deviceId,1);check(clear.ok && commitSampleAssignment(*store,clear).ok,"clear slot is an ordinary undoable state op");
    for(const auto& request:clear.requests)glue.appliedState(*store,request.payload,3);
    check(std::abs(render(*instance)-0.25f)<1e-6f && SnapshotBuilder::fromStore(*store)->sampleSlots[0]->empty,"clear restores identity and snapshot empty state");
    const auto stale=commitSampleAssignment(*store,again);check(!stale.ok && stale.stale,"stale prepared assignment refuses after another edit");
    check(!prepareSampleAssignment(*store,insert.deviceId,2,file,48000,9).ok && !prepareSampleAssignment(*store,999,1,file,48000,9).ok,
        "undeclared slot and missing device refuse before import");
    check(callbackAllocations==0,"assigned, missing-file, cleared and restored sample processing allocate nothing");
    {
        audio::CallbackScope callback;
        const auto denied=prepareSampleClear(*store,insert.deviceId,1);const auto deniedCommit=commitSampleAssignment(*store,clear);
        check(!denied.ok && !deniedCommit.ok,"sample command preparation and commit refuse on callback before SQLite");
    }
    check(audio::callbackFileIo.load(std::memory_order_relaxed)==0,"all live processing and undo/redo perform no callback file IO");
}
#endif
void callbackRefusal() {
    bool refused=false;audio::CallbackScope callback;
    try{(void)restoreBuiltinSample(emptyBuiltinSample("org.adidaw.builtin.color-cab",1),48000);}catch(const std::exception&){refused=true;}
    check(refused,"sample preparation refuses on audio thread before IO");
}
}
int main(){try{
#if ADI_WITH_PD
assignmentAndPlayback();
#endif
callbackRefusal();}catch(const std::exception& exception){countAllocations=false;check(false,exception.what());}std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
