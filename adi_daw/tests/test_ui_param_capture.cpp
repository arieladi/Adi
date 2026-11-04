// SPDX-License-Identifier: GPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/engine/param_ops.hpp"
#include "adi/engine/session.hpp"
#include "adi/history.hpp"
#include "adi/media/blake3.hpp"
#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <thread>

namespace {
using namespace adi;
using namespace adi::engine;
int checks=0,failures=0;
void check(bool ok,const std::string& what){++checks;if(!ok){++failures;std::printf("FAIL %s\n",what.c_str());}}
bool near(double a,double b){return std::abs(a-b)<1e-9;}
class Knobs final:public device::DeviceInstance {
public:
    Knobs() {
        identity_.name="UI knobs";identity_.format="test";
        params_[0].id="cutoff";params_[0].name="Cutoff";params_[0].hasRealRange=true;params_[0].minReal=20;params_[0].maxReal=20000;
        params_[1].id="gain";params_[1].name="Gain";
    }
    const device::DeviceIdentity& identity()const noexcept override{return identity_;}
    bool loaded()const noexcept override{return true;}
    void process(const NodeIo& io)noexcept override {
        for(const auto& event:io.events)if(event.type==EventType::ParamValue && event.paramId==17)values_[0].store(event.value);
        device::passThrough(io);
    }
    std::int32_t paramCount()const noexcept override{return 2;}
    const device::ParamDescriptor* paramAt(std::int32_t index)const noexcept override{return index>=0 && index<2?&params_[static_cast<std::size_t>(index)]:nullptr;}
    device::ParamValue getParam(const std::string& id)const noexcept override {
        for(std::size_t i=0;i<2;++i)if(params_[i].id==id) {
            const auto value=values_[i].load(std::memory_order_relaxed);
            return i==0?device::ParamValue::withReal(value,20+value*19980):device::ParamValue::fromNormalized(value);
        }
        return {};
    }
    bool setParam(const std::string& id,const device::ParamValue& value)override {
        if(refuse)return false;
        for(std::size_t i=0;i<2;++i)if(params_[i].id==id) {
            ++sets;
            if(deferred)pending_.push_back({i,value.normalized});
            else {
                values_[i].store(value.normalized,std::memory_order_relaxed);
                if(echo)broadcastParam(static_cast<std::int32_t>(i),ParamEventKind::Value,value.normalized);
            }
            return true;
        }
        return false;
    }
    bool hasPendingParamChanges()const noexcept override{return !pending_.empty();}
    void consume() {
        for(const auto& [index,value]:pending_) {
            values_[index].store(value,std::memory_order_relaxed);
            if(echo)broadcastParam(static_cast<std::int32_t>(index),ParamEventKind::Value,value);
        }
        pending_.clear();
    }
    void consumeOne() {
        if(pending_.empty())return;
        const auto [index,value]=pending_.front();pending_.erase(pending_.begin());
        values_[index].store(value,std::memory_order_relaxed);
        if(echo)broadcastParam(static_cast<std::int32_t>(index),ParamEventKind::Value,value);
    }
    void begin(std::int32_t index){broadcastParam(index,ParamEventKind::Begin,0);}
    void value(std::int32_t index,double value) {
        values_[static_cast<std::size_t>(index)].store(value,std::memory_order_relaxed);
        broadcastParam(index,ParamEventKind::Value,value);
    }
    void end(std::int32_t index){broadcastParam(index,ParamEventKind::End,0);}
    std::vector<std::string> stateRoles()const override{return stateful?std::vector<std::string>{"chunk"}:std::vector<std::string>{};}
    std::vector<std::uint8_t> saveState(const std::string&)const override {
        if(!stateful)return {};
        const std::array<double,2> values={values_[0].load(),values_[1].load()};
        std::vector<std::uint8_t> bytes(sizeof values);std::memcpy(bytes.data(),values.data(),sizeof values);return bytes;
    }
    bool loadState(const std::string&,const std::vector<std::uint8_t>& bytes)override {
        if(!stateful || bytes.size()!=sizeof(double)*2)return false;
        std::array<double,2> values{};std::memcpy(values.data(),bytes.data(),bytes.size());
        for(std::size_t i=0;i<2;++i)values_[i].store(values[i]);
        pending_.clear();++loads;return true;
    }
    void flags(bool missing,bool automatable){params_[0].missing=missing;params_[0].automatable=automatable;}
    void renameCutoff(const std::string& name){params_[0].id=name;identity_.format="clap";}
    bool echo=false,deferred=false,refuse=false,stateful=false;
    int sets=0,loads=0;
private:
    device::DeviceIdentity identity_;
    std::array<device::ParamDescriptor,2> params_;
    std::array<std::atomic<double>,2> values_{{0.5,0.25}};
    std::vector<std::pair<std::size_t,double>> pending_;
};
void event(ParamOps& ops,ParamEventKind kind,double value,std::int64_t now,std::vector<OpRequest>& out,const char* param="cutoff") {
    if(!ops.uiEvent(7,param,kind,value,now,out))throw std::runtime_error("UI event refused");
}
struct Project {
    test::TempDirectory temp{"ui_param_capture","project"};
    std::unique_ptr<Store> store;
    Project() {
        StoreError error=StoreError::Ok;store=Store::create(temp.path()/"song.adi",error);
        if(!store)throw std::runtime_error(toString(error));
        store->db().exec("INSERT INTO tracks(id,kind,name) VALUES(1,'audio','Audio')");
        store->db().exec("INSERT INTO plugin_refs(id,format,uid,name) VALUES(1,'clap','test.ui','UI knobs')");
        store->db().exec("INSERT INTO device_chains(id,track_id) VALUES(1,1)");
        store->db().exec("INSERT INTO devices(id,chain_id,plugin_ref_id) VALUES(7,1,1)");
    }
};

void explicitGesture() {
    Knobs device;device.echo=true;ParamOps ops(512);check(ops.attach(7,device),"attach separate captures");
    std::vector<OpRequest> out;
    event(ops,ParamEventKind::Begin,0.5,0,out);
    for(int i=1;i<=200;++i)event(ops,ParamEventKind::Value,0.5+0.3*i/200.0,i,out);
    check(out.empty() && near(device.getParam("cutoff").normalized,0.8),"drag sounds immediately and stays uncommitted until End");
    event(ops,ParamEventKind::End,0.8,201,out);
    check(out.size()==2 && out[0].opType=="device.setParam" && out[0].payload.at("norm")==0.5 &&
              out[1].payload.at("norm")==0.8 && near(out[1].payload.at("real").get<double>(),16004),"one gesture emits first-touch opener and final normalized/real values");
    check(ops.uiCaptureStats(7)->pushed==202 && ops.uiCaptureStats(7)->dropped==0 &&
              ops.captureStats(7)->pushed==200 && ops.captureStats(7)->echoesSwallowed==200,
          "one-slot UI capture stays separate from plug-in echoes across a long drag");
    out.clear();check(ops.drain(1000,out)==0 && out.empty(),"no echo produces a second edit");
    event(ops,ParamEventKind::Begin,0.8,1010,out);event(ops,ParamEventKind::Value,1.5,1011,out);event(ops,ParamEventKind::End,1.0,1012,out);
    check(out.size()==1 && out[0].payload.at("norm")==1.0,"later gestures need no opener and clamp normalized UI input");
}
void implicitAndRefusal() {
    Knobs device;ParamOps ops;ops.attach(7,device);std::vector<OpRequest> out;
    event(ops,ParamEventKind::Value,0.6,0,out);event(ops,ParamEventKind::Value,0.7,100,out);
    check(out.empty() && ops.drain(249,out)==0,"keyboard values wait the existing quiet interval");
    check(ops.drain(250,out)==2 && out.front().payload.at("norm")==0.5 && out.back().payload.at("norm")==0.7,
          "timer drain closes an unbracketed keyboard gesture with its opener");
    out.clear();const auto sets=device.sets;
    check(!ops.uiEvent(7,"missing",ParamEventKind::Value,0.3,260,out) &&
              !ops.uiEvent(99,"cutoff",ParamEventKind::Value,0.3,260,out) &&
              !ops.uiEvent(7,"cutoff",ParamEventKind::Value,std::numeric_limits<double>::quiet_NaN(),260,out) &&
              !ops.uiEvent(7,"cutoff",static_cast<ParamEventKind>(99),0.3,260,out),"invalid UI target/value/kind refused");
    device.flags(true,true);check(!ops.uiEvent(7,"cutoff",ParamEventKind::Begin,0.7,260,out),"missing parameter refused");
    device.flags(false,false);check(!ops.uiEvent(7,"cutoff",ParamEventKind::Begin,0.7,260,out),"non-automatable parameter refused");
    check(out.empty() && device.sets==sets,"refused inputs neither set nor record the device");
    device.flags(false,true);device.refuse=true;
    event(ops,ParamEventKind::Begin,0.7,270,out);
    check(!ops.uiEvent(7,"cutoff",ParamEventKind::Value,0.9,271,out) && near(device.getParam("cutoff").normalized,0.7),"backend refusal is visible and does not create an edit");
    event(ops,ParamEventKind::End,0.9,272,out);
    check(out.empty() && ops.stats().appliedRefused==1,"refused set leaves no UI value pending");
    device.value(0,0.9);ops.drain(280,out);ops.drain(430,out);
    check(out.size()==1 && out[0].payload.at("norm")==0.9,"failed-set echo guard cannot swallow a later real plug-in edit");
}
void journalAndDeferred() {
    Project project;Knobs device;ParamOps ops;ops.attach(7,device);std::vector<OpRequest> out;
    event(ops,ParamEventKind::Begin,0.5,0,out);event(ops,ParamEventKind::Value,0.8,1,out);event(ops,ParamEventKind::End,0.8,2,out);
    OpJournal journal(*project.store);check(journal.commit(out).ok,"commit entire UI gesture as one transaction");
    const auto logged=journal.recent();check(logged.size()==2 && logged[0].txnId==logged[1].txnId,"opener and edit share one undo step");
    check(History(*project.store).undo().ok,"undo gesture");
    for(const auto& op:logged)if(op.inverse)ops.applied(*op.inverse,10);
    check(near(device.getParam("cutoff").normalized,0.5) && project.store->db().execAndGet("SELECT count(*) FROM plugin_params").getInt()==0,
          "undo reaches actual pre-gesture value and removes first row");
    check(History(*project.store).redo().ok,"redo gesture");
    for(auto it=logged.rbegin();it!=logged.rend();++it)ops.applied(it->payload,20);
    check(near(device.getParam("cutoff").normalized,0.8),"redo reaches final value");
    Knobs delayed;delayed.deferred=true;ParamOps queue;queue.attach(7,delayed);out.clear();
    event(queue,ParamEventKind::Begin,0.5,0,out);event(queue,ParamEventKind::Value,0.8,1,out);event(queue,ParamEventKind::End,0.8,2,out);
    check(near(delayed.getParam("cutoff").normalized,0.5),"asynchronous backend can still report old audible value");
    queue.applied({{"dev",7},{"param","cutoff"},{"norm",0.5}},3);
    delayed.consume();check(near(delayed.getParam("cutoff").normalized,0.5) && delayed.sets==2,"immediate undo supersedes accepted intent even while getter is old");
}
void asynchronousEchoes() {
    Knobs device;device.deferred=true;device.echo=true;ParamOps ops;ops.attach(7,device);std::vector<OpRequest> out;
    event(ops,ParamEventKind::Begin,0.5,0,out);event(ops,ParamEventKind::Value,0.6,1,out);
    event(ops,ParamEventKind::Value,0.8,2,out);event(ops,ParamEventKind::End,0.8,3,out);
    check(out.size()==2,"asynchronous drag records only its final value");out.clear();
    device.consumeOne();ops.drain(4,out);ops.drain(200,out);
    check(out.empty(),"earlier accepted echo cannot become an edit while the final value is still pending");
    device.consume();ops.drain(201,out);ops.drain(400,out);
    check(out.empty() && ops.captureStats(7)->echoesSwallowed==2 && near(device.getParam("cutoff").normalized,0.8),
          "all delayed UI echoes are swallowed in their original TTL without duplicate undo entries");
}
void cancellationAndState() {
    Knobs old,replacement;ParamOps ops;ops.attach(7,old);std::vector<OpRequest> out;
    event(ops,ParamEventKind::Begin,0.5,0,out);event(ops,ParamEventKind::Value,0.8,1,out);event(ops,ParamEventKind::End,0.8,2,out);
    out.clear();event(ops,ParamEventKind::Begin,0.8,3,out);event(ops,ParamEventKind::Value,0.9,4,out);
    ops.detach(7);check(ops.drain(1000,out)==0 && !ops.uiCaptureStats(7),"detach cancels UI state without draining into a removed device");
    ops.attach(7,replacement);event(ops,ParamEventKind::End,0.9,1001,out);
    check(out.empty(),"old End cannot finish a gesture on a replacement instance");
    event(ops,ParamEventKind::Begin,0.5,1002,out);event(ops,ParamEventKind::Value,0.7,1003,out);event(ops,ParamEventKind::End,0.7,1004,out);
    check(out.size()==2 && out[0].payload.at("norm")==0.5,"reattach seeds a fresh opener");
    out.clear();event(ops,ParamEventKind::Begin,0.7,1005,out);event(ops,ParamEventKind::Value,0.8,1006,out);
    ops.applied({{"dev",7},{"param","cutoff"},{"norm",0.3}},1007);
    event(ops,ParamEventKind::End,0.8,1008,out);
    check(out.empty() && near(replacement.getParam("cutoff").normalized,0.3),"external edit cancels stale UI completion");
    for(bool defer:{false,true}) {
        Project project;Knobs state;state.stateful=true;state.deferred=defer;ParamOps capture;capture.attach(7,state);
        const auto baseline=state.saveState("chunk");const auto hash=media::blake3Bytes(std::as_bytes(std::span(baseline))).hex;
        SQLite::Statement blob(project.store->db(),"INSERT INTO state_blobs(hash_blake3,data,size_bytes) VALUES(?,?,?)");
        blob.bind(1,hash);blob.bind(2,baseline.data(),static_cast<int>(baseline.size()));blob.bind(3,static_cast<std::int64_t>(baseline.size()));blob.exec();
        out.clear();event(capture,ParamEventKind::Begin,0.5,0,out);event(capture,ParamEventKind::Value,0.8,1,out);
        check(capture.appliedState(*project.store,{{"dev",7},{"role","chunk"},{"hash",hash}},2),"state recall supersedes manual values despite cached-equal hash");
        event(capture,ParamEventKind::End,0.8,3,out);state.consume();
        check(out.empty() && near(state.getParam("cutoff").normalized,0.5) && state.loads==1,"state recall cancels accepted pending value and stale End");
    }
}
void twoProducers() {
    Knobs device;ParamOps ops(8192);ops.attach(7,device);std::vector<OpRequest> out;
    constexpr int count=3000;
    std::thread producer([&]{device.begin(1);for(int i=1;i<=count;++i)device.value(1,static_cast<double>(i%97)/96.0);device.end(1);});
    event(ops,ParamEventKind::Begin,0.5,0,out);
    for(int i=1;i<=200;++i)event(ops,ParamEventKind::Value,0.5+0.3*i/200.0,i,out);
    event(ops,ParamEventKind::End,0.8,201,out);producer.join();ops.drain(500,out);
    std::vector<double> cutoff,gain;
    for(const auto& op:out)(op.payload.at("param")=="cutoff"?cutoff:gain).push_back(op.payload.at("norm").get<double>());
    check(cutoff.size()==2 && cutoff[0]==0.5 && near(cutoff[1],0.8) && gain.size()==2 && gain[0]==0.25 && near(gain[1],static_cast<double>(count%97)/96.0),
          "simultaneous plug-in and window producers preserve both independent gestures");
    check(ops.captureStats(7)->pushed==count+2 && ops.captureStats(7)->dropped==0 && ops.uiCaptureStats(7)->pushed==202 &&
              ops.uiCaptureStats(7)->dropped==0,"each producer owns its ring with no loss");
}
void immediateAutomationOverride() {
    Project project;
    project.store->db().exec("INSERT INTO tracks(id,kind,name) VALUES(9,'master','Main')");
    project.store->db().exec("INSERT INTO mixer_strip(track_id) VALUES(1),(9)");
    project.store->db().exec("INSERT INTO routing(id,src_kind,src_id,dst_kind,dst_id,kind) VALUES(1,'track',1,'track',9,'main')");
    project.store->db().exec("INSERT INTO automation_lanes(id,owner_kind,owner_id,param_ref,value_domain,default_value) VALUES(90,'device',7,'17','normalized',0.2)");
    Session session;
    session.graph().setFadeFrames(0);
    Knobs* device=nullptr;
    check(session.load(*project.store,[&](const DeviceRequest&,std::string&)->std::unique_ptr<device::DeviceInstance> {
        auto instance=std::make_unique<Knobs>();instance->renameCutoff("00000011");device=instance.get();return instance;
    },{2,48000,64}),"session with an automated device loads");
    if(!device)return;
    ParamOps ops;check(ops.attachSession(session)==1,"UI capture knows its session owner");
    std::array<float,64> left{},right{};float* output[]={left.data(),right.data()};
    session.transport().play();session.process({output,nullptr,2,0,64,0});
    check(near(device->getParam("00000011").normalized,0.2) && !session.automationOverridden(),"lane plays before the manual gesture");
    std::vector<OpRequest> out;
    event(ops,ParamEventKind::Begin,0.2,0,out,"00000011");event(ops,ParamEventKind::Value,0.8,1,out,"00000011");
    session.process({output,nullptr,2,0,64,0});
    check(out.empty() && session.overriddenLanes().contains(90) && near(device->getParam("00000011").normalized,0.8),
          "the live UI value overrides automation before End creates its undo step");
    event(ops,ParamEventKind::End,0.8,2,out,"00000011");
    check(out.size()==2 && near(out.front().payload.at("norm").get<double>(),0.2),"automated first-touch opener remembers the actual starting value");
    check(OpJournal(*project.store).commit(out).ok && session.refresh(*project.store),"UI gesture commits and refreshes as one edit");
    check(session.reenableAutomation(90),"Re-Enable Automation remains the resume path");
    session.process({output,nullptr,2,0,64,0});
    check(near(device->getParam("00000011").normalized,0.2),"re-enabled lane follows the playhead again");
    OpRequest agent;agent.opType="device.setParam";agent.actor=Actor::Agent;
    agent.payload={{"dev",7},{"param","00000011"},{"norm",0.6}};
    const bool committed=OpJournal(*project.store).commit(agent).ok;
    ops.applied(agent.payload,3);
    check(committed && session.refresh(*project.store) && session.overriddenLanes().contains(90),
          "stored CLAP hex parameter edit overrides its decimal lane reference on refresh");
    out.clear();event(ops,ParamEventKind::Begin,0.2,3,out,"00000011");event(ops,ParamEventKind::Value,0.7,4,out,"00000011");
    OpRequest remove;remove.opType="device.remove";remove.payload={{"id",7}};
    check(OpJournal(*project.store).commit(remove).ok && session.refresh(*project.store),"remove device during an unfinished UI gesture");
    ops.attachSession(session);
    check(!ops.isAttached(7) && !ops.uiEvent(7,"00000011",ParamEventKind::End,0.7,5,out) && ops.drain(1000,out)==0 && out.empty(),
          "session synchronization cancels retired-device UI state and refuses late frames");
}
} // namespace
int main() {
    try{explicitGesture();implicitAndRefusal();journalAndDeferred();asynchronousEchoes();cancellationAndState();twoProducers();immediateAutomationOverride();}
    catch(const std::exception& error){check(false,std::string("exception: ")+error.what());}
    std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;
}
