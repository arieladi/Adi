// Standalone host ABI, exact-delay and raw-context verification. Original code, MIT.
#define NOMINMAX
#include "../probe_shared.h"
#include "time_shared.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include "../../platform_selftest.h"
#include <vector>
#include <algorithm>
using namespace Steinberg;using namespace Steinberg::Vst;
static int checks=0;void check(bool x,const char*s){++checks;if(!x){printf("FAIL %s\n",s);throw std::runtime_error("diagnostic assertion failed");}}
#define UNKNOWN tresult PLUGIN_API queryInterface(const TUID,void**p)override{if(p)*p=nullptr;return kNoInterface;} uint32 PLUGIN_API addRef()override{return 1;}uint32 PLUGIN_API release()override{return 1;}
struct Queue:IParamValueQueue{UNKNOWN ParamID PLUGIN_API getParameterId()override{return 1;}int32 PLUGIN_API getPointCount()override{return 1;}tresult PLUGIN_API getPoint(int32 i,int32&o,ParamValue&v)override{if(i)return kInvalidArgument;o=3;v=.25;return kResultOk;}tresult PLUGIN_API addPoint(int32,ParamValue,int32&)override{return kNotImplemented;}};
struct Changes:IParameterChanges{UNKNOWN Queue q;int32 PLUGIN_API getParameterCount()override{return 1;}IParamValueQueue*PLUGIN_API getParameterData(int32 i)override{return i?nullptr:&q;}IParamValueQueue*PLUGIN_API addParameterData(const ParamID&,int32&)override{return nullptr;}};
double sample(int i){return i>=0&&i%137==17?.1+.001*((i/137)%97):0.;}
template<class T>void run(IComponent*c,IAudioProcessor*a,IEditController*e,int delay,int format,int mode){
 ProcessSetup setup{mode,format,8192,48000};check(a->setupProcessing(setup)==kResultOk,"setup");check(c->setActive(1)==kResultOk,"active");a->setProcessing(1);e->setParamNormalized(1,1);
 std::vector<T>storageL(8192),storageR(8192),storageOL(8192),storageOR(8192);T*l=storageL.data(),*r=storageR.data(),*ol=storageOL.data(),*orr=storageOR.data();T*ip[2]{l,r},*op[2]{ol,orr};
 AudioBusBuffers in{},out{};in.numChannels=out.numChannels=2;if constexpr(sizeof(T)==4){in.channelBuffers32=ip;out.channelBuffers32=op;}else{in.channelBuffers64=ip;out.channelBuffers64=op;}
 ProcessContext ctx{};ctx.state=ProcessContext::kPlaying|ProcessContext::kProjectTimeMusicValid|ProcessContext::kTempoValid|ProcessContext::kTimeSigValid|ProcessContext::kBarPositionValid|ProcessContext::kCycleValid|ProcessContext::kContTimeValid;
 ctx.sampleRate=48000;ctx.tempo=120;ctx.timeSigNumerator=7;ctx.timeSigDenominator=8;ctx.projectTimeMusic=-.25;ctx.barPositionMusic=-3.5;ctx.cycleStartMusic=2;ctx.cycleEndMusic=6;ctx.continousTimeSamples=123456;
 ProcessData d{};d.processMode=mode;d.symbolicSampleSize=format;d.numInputs=d.numOutputs=1;d.inputs=&in;d.outputs=&out;d.processContext=&ctx;
 int at=0;for(int pass=0;pass<4;++pass)for(int n:{31,2048,7,1024,4800}){
  d.numSamples=n;ctx.projectTimeSamples=at-99;
  for(int i=0;i<n;++i){l[i]=(T)sample(at+i);r[i]=(T)-sample(at+i);}
  check(a->process(d)==kResultOk,"process");
  for(int i=0;i<n;++i){check(ol[i]==(T)sample(at+i-delay),"exact L delay");check(orr[i]==(T)-sample(at+i-delay),"exact R delay");}
  at+=n;
 }
 a->setProcessing(0);c->setActive(0);
 if(!delay){
  c->setActive(1);a->setProcessing(1);e->setParamNormalized(1,1);d.numSamples=16;std::fill_n(l,16,T(1));std::fill_n(r,16,T(1));Changes changes;d.inputParameterChanges=&changes;check(a->process(d)==kResultOk,"automation");
  for(int i=0;i<16;++i)check(ol[i]==T(i<3?1:.25),"exact parameter offset");
  a->setProcessing(0);c->setActive(0);
 }
}

// Exercise the real module: marker filtering must not alter audio or silence flags.
template<class T>void markerTest(IComponent*c,IAudioProcessor*a,IEditController*e,int delay,int format,TimeHeader*h){
 ProcessSetup setup{kRealtime,format,8192,48000};check(a->setupProcessing(setup)==kResultOk,"marker setup");
 check(c->setActive(1)==kResultOk,"marker active");check(a->setProcessing(1)==kResultOk,"marker processing");e->setParamNormalized(1,1);
 std::vector<T>storageL(8192),storageR(8192),storageOL(8192),storageOR(8192);T*l=storageL.data(),*r=storageR.data(),*ol=storageOL.data(),*orr=storageOR.data();T*ip[2]{l,r},*op[2]{ol,orr};
 AudioBusBuffers in{},out{};in.numChannels=out.numChannels=2;
 if constexpr(sizeof(T)==4){in.channelBuffers32=ip;out.channelBuffers32=op;}else{in.channelBuffers64=ip;out.channelBuffers64=op;}
 ProcessData d{};d.processMode=kRealtime;d.symbolicSampleSize=format;d.numInputs=d.numOutputs=1;d.inputs=&in;d.outputs=&out;d.numSamples=128;
 auto*records=adi_platform::mappedAt<TimeRecord,TIME_HEADER_BYTES>(h);auto before=h->next;
 check(a->process(d)==kResultOk,"silence process");check(h->next==before+1,"silence record");auto&z=records[before];
 check(z.committed==before+1&&z.input_count==0&&z.input_total==0&&z.output_count==0&&z.output_total==0,"silence no sparse markers");
 check(out.silenceFlags==3,"silence flags exact zero");
 for(int i=0;i<128;++i)check(ol[i]==0&&orr[i]==0,"zero audio unchanged");
 // More than 32 tiny nonzeros precede the marker; these formerly filled the sparse array.
 for(int i=0;i<8192;++i)l[i]=T(i%2?1e-10:-1e-10);
 l[70]=T(.125);l[80]=T(7e-8);l[90]=T(1e-7);l[91]=-T(1e-7);
 l[92]=std::nextafter(T(1e-7),T(1));l[93]=std::nextafter(T(1e-7),T(0));
 for(int i=0;i<8192;++i)r[i]=-l[i];d.numSamples=8192;before=h->next;
 check(a->process(d)==kResultOk,"tiny signal process");check(h->next==before+1,"tiny signal record");auto&t=records[before];
 check(t.committed==before+1,"tiny signal commit");
 std::vector<int>expectedIn,expectedOut;
 for(int i=0;i<8192;++i){T y=i>=delay?l[i-delay]:T(0);
  check(ol[i]==y&&orr[i]==-y,"tiny signal audio and exact delay unchanged");
  if(std::abs(double(l[i]))>1e-7)expectedIn.push_back(i);
  if(std::abs(double(y))>1e-7)expectedOut.push_back(i);
 }
 check(out.silenceFlags==0,"tiny nonzero audio still nonsilent");
 check(expectedIn.size()<32&&expectedOut.size()<32,"bounded sparse expectations");
 check(t.input_total==expectedIn.size()&&t.input_count==expectedIn.size(),"input threshold count");
 check(t.output_total==expectedOut.size()&&t.output_count==expectedOut.size(),"output threshold count");
 for(size_t j=0;j<expectedIn.size();++j)check(t.input[j].offset==expectedIn[j]&&t.input[j].value==double(l[expectedIn[j]]),"input threshold marker");
 for(size_t j=0;j<expectedOut.size();++j)check(t.output[j].offset==expectedOut[j]&&t.output[j].value==double(l[expectedOut[j]-delay]),"output threshold marker");
 check(t.input[0].offset==70&&t.output[0].offset==70+delay,"late impulse survives tiny prefix");
 a->setProcessing(0);c->setActive(0);
}

int main(int argc,char**argv){
 try{
 adi_platform_selftest(check);
 adi_platform::Environment saved("ADI_TIME_MAP");
 const auto name=std::string("Local\\ADI_TIME_SELFTEST_")+std::to_string(adi_platform::pid());
 check(adi_platform::setEnvironment("ADI_TIME_MAP",name),"mapping environment");
 adi_platform::Mapping map;check(map.open(name,TIME_MAP_BYTES,true),"exclusive mapping");
 auto*h=static_cast<TimeHeader*>(map.data());memcpy(h->magic,"ADITIME1",8);h->version=1;h->record_size=sizeof(TimeRecord);h->capacity=TIME_CAP;h->frequency=adi_platform::frequency();h->enabled=1;
 adi_platform::Module dll;check(dll.open(argc>1?argv[1]:"ADITimeProbe.vst3",true),"load VST3 module and platform entry");
 auto get=reinterpret_cast<IPluginFactory*(PLUGIN_API*)()>(dll.symbol("GetPluginFactory"));check(get!=nullptr,"factory export");auto*f=get();check(f&&f->countClasses()==3,"classes");
 for(int v=0;v<3;++v){
  PClassInfo info{};f->getClassInfo(v,&info);IComponent*c=nullptr;check(f->createInstance(info.cid,IComponent_iid,(void**)&c)==kResultOk,"create");c->initialize(nullptr);
  IAudioProcessor*a=nullptr;IEditController*e=nullptr;c->queryInterface(IAudioProcessor_iid,(void**)&a);c->queryInterface(IEditController_iid,(void**)&e);check(a&&e,"interfaces");
  const int delay=v==1?1024:v==2?4800:0;check(a->getLatencySamples()==(uint32)delay&&a->getTailSamples()==(uint32)delay,"reported latency/tail");e->setParamNormalized(0,double(10+v)/4095);
  run<float>(c,a,e,delay,kSample32,kRealtime);run<double>(c,a,e,delay,kSample64,kOffline);
  e->release();a->release();c->terminate();c->release();
 }
 auto*records=adi_platform::mappedAt<TimeRecord,TIME_HEADER_BYTES>(h);check(h->next==122,"process count");check(h->lost==0,"no loss");
 bool parameters=false,negative=false;
 for(adi_platform::Atomic64 i=0;i<h->next;++i){auto&r=records[i];check(adi_platform::acquire(&r.committed)==i+1,"commit");check(r.context_present&&r.context_bytes==sizeof(ProcessContext),"context presence/size");check(r.tempo==120&&r.numerator==7&&r.denominator==8&&r.project_music==-.25&&r.continuous_samples==123456,"raw musical context");
 if(r.project_samples==-99)negative=true;if(r.param_count){check(r.parameters[0].id==1&&r.parameters[0].offset==3&&r.parameters[0].value==.25,"parameter record");parameters=true;}}
 check(parameters&&negative,"parameter and negative clock evidence");

 for(int v=0;v<3;++v){
  PClassInfo info{};f->getClassInfo(v,&info);IComponent*c=nullptr;check(f->createInstance(info.cid,IComponent_iid,(void**)&c)==kResultOk,"marker create");c->initialize(nullptr);
  IAudioProcessor*a=nullptr;IEditController*e=nullptr;c->queryInterface(IAudioProcessor_iid,(void**)&a);c->queryInterface(IEditController_iid,(void**)&e);check(a&&e,"marker interfaces");
  const int delay=v==1?1024:v==2?4800:0;
  markerTest<float>(c,a,e,delay,kSample32,h);markerTest<double>(c,a,e,delay,kSample64,h);
  e->release();a->release();c->terminate();c->release();
 }
 check(h->next==134&&h->lost==0,"all marker cases committed without loss");

 printf("PASS checks=%d records=%lld record_bytes=%zu context_bytes=%zu header_bytes=%zu map_bytes=%zu\n",checks,static_cast<long long>(h->next),sizeof(TimeRecord),sizeof(ProcessContext),sizeof(TimeHeader),TIME_MAP_BYTES);
 f->release();dll.close();map.close();return 0;
 }catch(const std::exception& error){std::fprintf(stderr,"FAIL: %s\n",error.what());return 1;}
}

