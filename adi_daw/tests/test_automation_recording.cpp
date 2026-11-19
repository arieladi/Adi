// SPDX-License-Identifier: AGPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/engine/automation_recording.hpp"
#include "adi/history.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <stdexcept>
#include <thread>
namespace {
thread_local bool watch=false;
std::atomic<int> allocations{0};
}
void* operator new(std::size_t n) {if(watch)++allocations;if(void* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void* operator new[](std::size_t n) {return ::operator new(n);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
namespace {
using namespace adi;
using namespace adi::engine;
int checks=0,failures=0;
void check(bool ok,const char* label){++checks;if(!ok){++failures;std::printf("FAIL: %s\n",label);}}
std::unique_ptr<Store> project(const std::filesystem::path& path) {
    StoreError e=StoreError::Ok;auto s=Store::createEmpty(path,e);if(!s)throw std::runtime_error("store");
    s->db().exec("INSERT INTO project(id,name) VALUES(1,'Record');INSERT INTO tracks(id,kind,name) VALUES(1,'audio','Track');"
        "INSERT INTO automation_lanes(id,owner_kind,owner_id,param_ref,time_base) VALUES(2,'track',1,'volume',1)");return s;
}
std::vector<std::byte> bytes(Store& s) {
    SQLite::Statement q(s.db(),"SELECT data FROM automation_data WHERE id=3");if(!q.executeStep())return {};
    const auto c=q.getColumn(0);const auto* b=static_cast<const std::byte*>(c.getBlob());return {b,b+c.getBytes()};
}
void run() {
    const AutomationTarget target{3,2,std::nullopt};AutomationRecorder recorder(target,16);
    std::vector<RecordedAutomation> out;
    check(recorder.armed() && !recorder.recording(),"Automation Arm defaults on and transport record off");
    check(!recorder.push(AutomationGesture::Begin,0,0.1) && recorder.drain(out)==0,"transport record gate");
    check(recorder.setRecording(true,0) && recorder.setArmed(false,0),"producer controls");
    check(!recorder.push(AutomationGesture::Value,1,0.2) && recorder.drain(out)==0,"disarmed capture ignores manual values");
    check(recorder.setArmed(true,1),"rearm while recording");
    constexpr double precise=0.12345678901234567;constexpr std::int64_t wide=(std::int64_t{1}<<54)+1;
    watch=true;
    const bool begin=recorder.push(AutomationGesture::Begin,wide,precise);
    const bool value=recorder.push(AutomationGesture::Value,wide+10,0.5);
    const bool end=recorder.push(AutomationGesture::End,wide+20,0.8);
    watch=false;
    check(begin && value && end && allocations==0,"producer capture allocates nothing");
    check(recorder.drain(out)==1 && out.size()==1,"completed gesture emitted once");
    if(out.empty())return;
    check(out[0].target.data==3 && out[0].target.lane==2 && out[0].points.size()==3,"explicit target and all points retained");
    check(out[0].points.size()>=2 && out[0].points[0].time==wide && out[0].points[1].time==wide+10 && out[0].points[0].value==precise,"timestamps and values are never narrowed or drain-timed");
    check(recorder.drain(out)==0,"second drain does not duplicate a gesture");
    check(recorder.push(AutomationGesture::Value,100,0.2),"implicit manual gesture starts");
    check(recorder.push(AutomationGesture::Value,100,0.3),"coincident update accepted");
    check(recorder.setArmed(false,120) && recorder.drain(out)==1,"disarm closes the prior gesture at supplied time");
    check(out.back().points.size()==2 && out.back().points[0].value==0.3 && out.back().points[1].time==120 && out.back().points[1].value==0.3,"coincident values coalesce and held endpoint is recorded");
    check(recorder.setArmed(true,130) && recorder.push(AutomationGesture::Value,140,0.4),"new recording after arm transition");
    check(recorder.setRecording(false,150) && recorder.drain(out)==1 && out.back().points.back().time==150,"stop closes capture at producer transport time");
    check(!recorder.setRecording(true,-1) && !recorder.recording(),"invalid gate time leaves control unchanged");
    recorder.setRecording(true,200);recorder.push(AutomationGesture::Begin,200,0.2);
    check(!recorder.push(AutomationGesture::Value,190,0.3),"unannounced backward seek invalidates gesture");
    recorder.push(AutomationGesture::End,210,0.3);
    check(recorder.drain(out)==0,"invalid timeline never yields a partial recording");
    recorder.push(AutomationGesture::Begin,200,0.2);recorder.push(AutomationGesture::Value,210,std::numeric_limits<double>::infinity());
    recorder.push(AutomationGesture::End,220,0.2);
    check(recorder.drain(out)==0,"nonfinite value invalidates whole gesture");
    recorder.push(AutomationGesture::Begin,300,0.1);recorder.push(AutomationGesture::Begin,400,0.2);recorder.push(AutomationGesture::End,450,0.3);
    check(recorder.drain(out)==2,"new begin closes old gesture without crossing a seek");
    AutomationRecorder small(target,1);std::vector<RecordedAutomation> recovered;small.setRecording(true,0);
    check(small.push(AutomationGesture::Begin,0,0.1) && !small.push(AutomationGesture::Value,1,0.2),"bounded ring reports overflow");
    check(small.drain(recovered)==0,"overflow discards a pending prefix");
    small.push(AutomationGesture::End,2,0.3);check(small.drain(recovered)==0,"overflow-tainted end cannot publish partial data");
    small.push(AutomationGesture::Begin,3,0.4);small.drain(recovered);small.push(AutomationGesture::End,4,0.5);small.drain(recovered);
    check(recovered.size()==1 && recovered[0].points.front().time==3 && small.stats().dropped>0,"next healthy gesture recovers after overflow");
    AutomationRecorder bounded(target,8,2);bounded.setRecording(true,0);bounded.push(AutomationGesture::Begin,0,0.1);
    bounded.push(AutomationGesture::Value,1,0.2);bounded.push(AutomationGesture::End,2,0.3);
    std::vector<RecordedAutomation> none;
    check(bounded.drain(none)==0 && bounded.stats().discardedGestures==1,"owner point budget rejects whole gesture");
    AutomationRecorder zero(target,0);zero.setRecording(true,0);
    check(!zero.push(AutomationGesture::Begin,0,0.1) && zero.stats().dropped==1,"zero capacity rejects safely");

    test::TempDirectory temp("automation","recording");auto store=project(temp.path()/"a.adi");std::string error;
    const auto prepared=prepareRecordedAutomation(*store,out.front(),error);
    check(prepared && automation::commitShapeInsertion(*store,*prepared).ok,"captured gesture commits through native atomic replacement");
    const auto persisted=bytes(*store);StreamReader<AutomationPoint> reader(persisted,FourCC::Automation);
    check(reader.ok() && reader.count()==3 && reader.at(0)->value==precise && reader.at(0)->time==wide &&
        (reader.header().flags&StreamFlags::TimeIsNanos)!=0,"recorded data preserves lane units and precision");
    History history(*store);
    check(history.undo().ok && bytes(*store).empty(),"one undo removes the whole recorded gesture");
    check(history.redo().ok && bytes(*store)==persisted,"recording redo exact bytes");
    auto invalid=out.front();invalid.points[0].point_id=8;
    check(!prepareRecordedAutomation(*store,invalid,error),"recorded IDs are assigned only by owner preparation");
    invalid=out.front();invalid.points[0].curve=99;
    check(!prepareRecordedAutomation(*store,invalid,error),"unknown recording curve rejected");
    invalid=out.front();invalid.gesture=0;
    check(!prepareRecordedAutomation(*store,invalid,error),"missing gesture identity rejected");

    // Exactly one producer and one consumer exchange 1000 complete gestures.
    AutomationRecorder concurrent(target,4);std::vector<RecordedAutomation> transferred;std::atomic<std::size_t> ack{0};
    std::atomic<bool> producerOk{true},abort{false};
    std::thread producer([&]{
        concurrent.setRecording(true,0);
        for(std::size_t i=0;i<1000;++i) {
            while(ack.load(std::memory_order_acquire)<i && !abort.load(std::memory_order_relaxed))std::this_thread::yield();
            if(abort.load(std::memory_order_relaxed))return;
            watch=true;
            const bool a=concurrent.push(AutomationGesture::Begin,static_cast<std::int64_t>(i*2),0.1);
            const bool b=concurrent.push(AutomationGesture::End,static_cast<std::int64_t>(i*2+1),0.2);
            watch=false;if(!a || !b)producerOk=false;
        }
    });
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    while(transferred.size()<1000 && std::chrono::steady_clock::now()<deadline) {concurrent.drain(transferred);ack.store(transferred.size(),std::memory_order_release);std::this_thread::yield();}
    abort.store(true,std::memory_order_relaxed);producer.join();
    bool ordered=true;for(std::size_t i=0;i<transferred.size();++i)ordered=ordered && transferred[i].gesture==i+1 &&
        transferred[i].points.size()==2 && transferred[i].points[0].time==static_cast<std::int64_t>(i*2);
    check(producerOk && ordered && transferred.size()==1000 && concurrent.stats().dropped==0,"SPSC preserves every timestamped gesture across actual threads");
    check(allocations==0,"all guarded producer operations remain allocation-free");
}
} // namespace
int main(){try{run();}catch(const std::exception& e){++failures;std::printf("EXCEPTION: %s\n",e.what());}std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
