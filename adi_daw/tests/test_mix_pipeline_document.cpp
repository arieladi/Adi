// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/ui/project_document.hpp"
#include "adi/mix/clip/Values.hpp"
#include "juce/builtin_host.hpp"
#include "juce/mix_pipeline_host.hpp"
#include "temp_directory.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <thread>
#include <chrono>

using namespace adi;
namespace {
int checks=0,failures=0;
void check(bool ok,const char* label){++checks;if(!ok){++failures;std::printf("FAIL %s\n",label);}}
struct Tone final:engine::Node {
    std::int64_t cursor=0;
    void process(const engine::NodeIo& io)noexcept override {
        for(int i=0;i<io.frames;++i){const auto value=static_cast<float>(.9*std::sin(static_cast<double>(cursor++)*.071));
            for(int c=0;c<io.channels;++c)io.out[c][io.blockOffset+i]=value;}
    }
};
bool edit(ui::ProjectDocument& doc,const ui::DevicePanelData& data,const std::string& id,double value){
    const auto* instance=doc.session().instanceFor(data.id);if(!instance)return false;
    const auto before=instance->getParam(id).normalized;using K=engine::ParamEventKind;
    const bool accepted=doc.parameterGestureExpected(data.id,id,K::Begin,before,data.runtime,data.instance)&&
        doc.parameterGestureExpected(data.id,id,K::Value,value,data.runtime,data.instance)&&
        doc.parameterGestureExpected(data.id,id,K::End,value,data.runtime,data.instance);
    if(!accepted)std::printf("EDIT diagnostic: id=%s reason=%s\n",id.c_str(),doc.error().c_str());
    return accepted;
}
}
int main(){
    test::TempDirectory temp("mix_pipeline_document","serial-pages");std::string error;
    auto doc=ui::ProjectDocument::open(temp.path()/"song.adi",true,
        device::withFullMixFunctions(device::withBuiltinDevices()),error);
    check(doc!=nullptr,"full native function document opens");if(!doc)return 2;
    check(doc->addMixFunction(mix::Function::Limiter,2),"add one L function through common prepared batch");
    doc->tick(0);const auto publication=doc->parameterFeed().current();
    const auto found=std::find_if(publication->begin(),publication->end(),[](const auto& p){return p.uid=="org.adidaw.mix.limiter";});
    check(found!=publication->end()&&found->available,"real full processing provider resolves");if(found==publication->end())return 2;
    auto shown=*found;auto* instance=doc->session().instanceFor(shown.id);
    check(instance&&instance->paramCount()==67,"full processing controls exist before first processing");if(!instance)return 2;
    check(instance->getParam("19").real==1&&instance->getParam("20").real==0,"Limiter starts on and Clip off");
    check(shown.parameterGroups.at("19").groupId=="limiter"&&shown.parameterGroups.at("19").groupEnableControl&&
          shown.parameterGroups.at("20").groupId=="clip"&&shown.parameterGroups.at("20").groupEnableControl,
          "real independent stage enables drive group metadata");
    Tone tone;doc->session().setSourcesFor([&](std::int64_t track){return track==2?std::vector<engine::Node*>{&tone}:std::vector<engine::Node*>{};});
    doc->prepare(48000,256);doc->tick(1);doc->session().transport().play();
    // Preparation/source replacement changes the presentation token. A real retained
    // editor receives the fresh publication before it can start another gesture.
    const auto prepared=doc->parameterFeed().current();
    const auto fresh=std::find_if(prepared->begin(),prepared->end(),[&](const auto& p){return p.id==shown.id;});
    check(fresh!=prepared->end()&&fresh->available,"prepared function publishes a fresh editable identity");
    if(fresh==prepared->end())return 2;
    shown=*fresh;instance=doc->session().instanceFor(shown.id);
    const auto limiterLatency=instance->latencySamples();
    check(edit(*doc,shown,std::to_string(21+mix::clip::threshold),.875),"Clip threshold uses shared gesture path");
    check(edit(*doc,shown,"20",1),"Clip can turn on while Limiter stays on");
    std::array<float,256> left{},right{};std::array<float*,2> pointers{left.data(),right.data()};
    engine::AudioIo io;io.out=pointers.data();io.numOut=2;io.frames=256;
    bool finite=true;
    for(int block=0;block<100;++block){io.streamTimeSamples=block*256;doc->process(io);doc->tick(block+1);
        finite=finite&&std::all_of(left.begin(),left.end(),[](float v){return std::isfinite(v);});}
    check(finite&&instance->getParam("19").real==1&&instance->getParam("20").real==1,"both enabled stages run through the real session");
    check(instance->latencySamples()>limiterLatency,"host observes summed Clip and Limiter latency");
    const auto bothState=instance->saveState("state");
    check(doc->ops().undo().ok&&doc->synchronise(),"one undo reverses Clip enable");doc->tick(101);
    check(instance->getParam("19").real==1&&instance->getParam("20").real==0,"undo changes only the chosen page enable");
    for(int block=0;block<20;++block){doc->process(io);doc->tick(102+block);}
    check(instance->latencySamples()==limiterLatency,"Clip off returns to exact Limiter-only latency");
    rows::PluginRef ref;ref.format="internal";ref.uid="org.adidaw.mix.limiter";ref.name="L";
    engine::DeviceRequest request;request.ref=&ref;request.exporting=true;
    auto offline=device::makeFullMixFunction(request,error);
    check(offline&&offline->loadState("state",bothState),"fresh export instance restores full stages and continuation");
    if(offline){
        check(offline->setParam("73",device::ParamValue::fromNormalized(1)),"explicit export quality flag sets");
        const auto online=offline->getParam("7").normalized;offline->prepare(48000,256);
        if(offline->latencySamples()<=limiterLatency)std::printf("EXPORT diagnostic: latency=%d baseline=%d error=%s\n",offline->latencySamples(),limiterLatency,error.c_str());
        check(offline->latencySamples()>limiterLatency&&offline->getParam("7").normalized==online,
              "export quality is prepared before audio and preserves online control");offline->release();
    }
    doc->release();std::printf("pipeline document: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
