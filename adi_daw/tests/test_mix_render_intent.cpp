// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/session.hpp"
#include "adi/store.hpp"
#include "temp_directory.hpp"
#include <array>
#include <cstdio>
#include <vector>

using namespace adi;
namespace {
int checks=0,failures=0;
void check(bool ok,const char* label){++checks;if(!ok){++failures;std::printf("FAIL %s\n",label);}}
struct Probe final:device::DeviceInstance {
    device::DeviceIdentity id;
    int calls=0;bool exporting=false;
    bool finiteTail = false;
    explicit Probe(const rows::PluginRef& ref):id{ref.format,ref.uid,ref.name,ref.vendor,ref.version}{}
    std::int64_t tailSamples() const noexcept override {
        return finiteTail ? 0 : engine::kInfiniteTail;
    }
    const device::DeviceIdentity& identity()const noexcept override{return id;}
    bool loaded()const noexcept override{return true;}
    void process(const engine::NodeIo& io)noexcept override{++calls;exporting=io.exporting;device::passThrough(io);}
    bool setParam(const std::string&,const device::ParamValue&)override{return true;}
    bool loadState(const std::string&,const std::vector<std::uint8_t>&)override{return true;}
};
void run(Store& store,bool exporting,bool performanceState,bool finiteTail=false) {
    std::vector<Probe*> probes;std::vector<bool> requests;
    engine::DeviceLoader loader=[&](const engine::DeviceRequest& rq,std::string& error)->std::unique_ptr<device::DeviceInstance>{
        if(!rq.ref){error="fixture requires a reference";return {};}
        requests.push_back(rq.exporting);
        auto probe=std::make_unique<Probe>(*rq.ref);
        probe->finiteTail = finiteTail;
        probes.push_back(probe.get());
        return probe;
    };
    engine::SessionSpec spec;spec.sampleRate=48000;spec.maxFrames=64;
    spec.exporting=exporting;spec.performanceState=performanceState;
    engine::Session session;check(session.load(store,loader,spec),"explicit render session loads");
    check(!requests.empty(),"loader receives prepared requests");
    bool preparedCorrectly=true;for(const auto value:requests)preparedCorrectly=preparedCorrectly&&value==exporting;
    check(preparedCorrectly,"quality intent reaches loader before prepare");
    std::array<float,64> left{},right{};std::array<float*,2> ptr{left.data(),right.data()};
    engine::AudioIo io;io.out=ptr.data();io.numOut=2;io.frames=64;
    // Session's pinned spec is authoritative; callers cannot guess export intent from stop/play.
    io.exporting=!exporting;session.process(io);
    bool called=false,correct=true;
    for(const auto* probe:probes)if(probe->calls){called=true;correct=correct&&probe->exporting==exporting;}
    if (finiteTail && !exporting)
        check(!called && session.graph().currentGraph()->stats().nodesSuspended > 0,
              "a zero-tail device still suspends in a silent live callback");
    else
        check(called && correct, finiteTail
              ? "the real export session invokes a zero-tail device on a silent block"
              : "pinned explicit quality intent reaches device processing while stopped");
}
}
int main(){
    test::TempDirectory temp("mix_render_intent","session");StoreError error{};
    auto store=Store::createNewProject(temp.path()/"song.adi",error);if(!store)return 2;
    run(*store,false,true);run(*store,false,false);run(*store,true,false);
    run(*store,false,true,true);
    run(*store,true,false,true);
    std::printf("render intent: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
