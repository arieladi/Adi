// ADI engine research diagnostic probe. Original code MIT, 2026-11-17.
// Interface definitions: Steinberg vst3_pluginterfaces 4f547e8 (MIT).
// No allocation, locks, file I/O or host UI calls in process().
#define NOMINMAX
#include "../probe_shared.h"
#include "pcm_shared.h"
#include "time_shared.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/ivstprefetchablesupport.h"
#include "pluginterfaces/vst/ivstevents.h"
#include <atomic>
#include <algorithm>
#include <cwchar>

using namespace Steinberg;
using namespace Steinberg::Vst;
// Ignore tiny host-supplied anti-denormal signals in the sparse trace only.
static constexpr double markerFloor = 1e-7;
static_assert(std::atomic<double>::is_always_lock_free,"parameter access must be lock-free");
static const TUID CLASS_IDS[3]={
  INLINE_UID(0x41444954,0x494D4550,0x524F4245,0x00000001),
  INLINE_UID(0x41444954,0x494D4550,0x524F4245,0x00000002),
  INLINE_UID(0x41444954,0x494D4550,0x524F4245,0x00000003)};
static const char *CLASS_NAMES[3]={"ADI Time Observer","ADI Pure Delay 1024","ADI Pure Delay 4800"};
static double norm(double x){return std::isfinite(x)?std::clamp(x,0.0,1.0):0.0;}
static bool eq(const char *a,const char *b){return a&&b&&!memcmp(a,b,16);}
static double maxplain(ParamID id){return id==0?4095.0:id==2?20000.0:1.0;}
static void text16(TChar *dst,const wchar_t *src,size_t count){wcsncpy_s(reinterpret_cast<wchar_t*>(dst),count,src,_TRUNCATE);}

class Probe final: public IComponent,public IAudioProcessor,public IEditController,
                   public IPrefetchableSupport,public IProcessContextRequirements {
  std::atomic<uint32> refs{1};
  std::atomic<double> params[4]{{0.0},{1.0},{0.0},{0.0}};
  int variant;
  double rate=48000.0,phase=0.0,workstate=0.2;
  int setupMode=kRealtime;
  SpeakerArrangement auxArrangement=3;
  ADITrace trace;
  TimeTrace timetrace;
  double delayline[2][4800]{};uint32 delayhead=0;
  uint32 latency()const{return variant==1?1024:variant==2?4800:0;}
  ADIPCMTrace pcm;
  bool (*rtfn)()=nullptr;
  double (*heardfn)()=nullptr;
  double (*decodefn)()=nullptr;
  IComponentHandler *handler=nullptr;
  void event(uint32 code,int mode=0,int frames=0){
    if(!trace.enabled(2))return;
    ADIRecord r{};r.qpc0=r.qpc1=adi_qpc();r.kind=7;r.id=(uint32)std::llround(params[0].load()*4095.0);
    r.events=code;r.mode=mode;r.frames=frames;r.flags=(uint32)variant;r.rate=rate;r.rt=-1;
    r.heard=r.decode=r.transport=-1;r.steady=-1;trace.record(r);
  }
  tresult loadState(IBStream *s){
    if(!s)return kInvalidArgument;uint32 magic=0;int32 n=0;
    if(s->read(&magic,4,&n)!=kResultOk||n!=4||magic!=0x31494441)return kResultFalse;
    double p[4];if(s->read(p,sizeof(p),&n)!=kResultOk||n!=sizeof(p))return kResultFalse;
    for(int i=0;i<4;++i)params[i].store(norm(p[i]));return kResultOk;
  }
  template<class T> void samples(ProcessData &d,double p[4],bool logging,double &inRms,double &outRms){
    IParamValueQueue *queues[4]{};int32 qi[4]{},qn[4]{},qo[4]{};double qv[4]{};
    if(d.inputParameterChanges)for(int32 j=0;j<d.inputParameterChanges->getParameterCount();++j){
      auto *q=d.inputParameterChanges->getParameterData(j);if(!q||q->getParameterId()>=4)continue;
      int id=q->getParameterId();queues[id]=q;qn[id]=q->getPointCount();
      if(qn[id]&&q->getPoint(0,qo[id],qv[id])!=kResultOk)qn[id]=0;
    }
    T **ins=nullptr,**outs=nullptr;int ni=0,no=0;
    if(d.numInputs&&d.inputs){ni=d.inputs[0].numChannels;if constexpr(sizeof(T)==4)ins=d.inputs[0].channelBuffers32;else ins=d.inputs[0].channelBuffers64;}
    if(d.numOutputs&&d.outputs){no=d.outputs[0].numChannels;if constexpr(sizeof(T)==4)outs=d.outputs[0].channelBuffers32;else outs=d.outputs[0].channelBuffers64;}
    uint64 silentMask=no>=64?~uint64(0):((uint64(1)<<no)-1);double sumIn=0,sumOut=0;
    for(int32 i=0;i<d.numSamples;++i){
      for(int k=0;k<4;++k)while(qi[k]<qn[k]&&qo[k]<=i){
        p[k]=norm(qv[k]);if(++qi[k]<qn[k]&&queues[k]->getPoint(qi[k],qo[k],qv[k])!=kResultOk)qn[k]=qi[k];
      }
      double synth=0;if(p[3]>=0.5){synth=0.05*std::sin(phase);phase+=6.2831853071795864769*997.0/rate;if(phase>=6.2831853071795864769)phase-=6.2831853071795864769;}
      for(int c=0;c<no;++c){double x=(c<ni&&ins&&ins[c])?ins[c][i]:0.0;double delayed=x;if(latency()&&c<2){delayed=delayline[c][delayhead];delayline[c][delayhead]=x;}double y=(delayed+synth)*p[1];
        if(outs&&outs[c])outs[c][i]=(T)y;if(logging){sumIn+=x*x;sumOut+=y*y;}if(y!=0.0&&c<64)silentMask&=~(uint64(1)<<c);}
      if(latency())delayhead=(delayhead+1)%latency();
    }
    // Zero-sample parameter flush, and last queued value persistence.
    for(int k=0;k<4;++k){if(queues[k]&&qn[k]&&d.numSamples==0){int32 off;double v;if(queues[k]->getPoint(qn[k]-1,off,v)==kResultOk)p[k]=norm(v);}params[k].store(p[k]);}
    if(d.numOutputs&&d.outputs)d.outputs[0].silenceFlags=silentMask;
    double count=double(d.numSamples)*no;inRms=logging&&count>0?std::sqrt(sumIn/count):0;outRms=logging&&count>0?std::sqrt(sumOut/count):0;
  }
public:
  explicit Probe(int v):variant(v){trace.open();pcm.open();timetrace.open();if(auto get=adi_api()){rtfn=(bool(*)())get("IsInRealTimeAudio");heardfn=(double(*)())get("GetPlayPosition");decodefn=(double(*)())get("GetPlayPosition2");}event(1);}
  ~Probe(){if(handler)handler->release();trace.close();pcm.close();timetrace.close();}
  tresult PLUGIN_API queryInterface(const TUID requestedIid,void **out) override {
    if(!out)return kInvalidArgument;*out=nullptr;
    if(eq(requestedIid,FUnknown_iid)||eq(requestedIid,IPluginBase_iid)||eq(requestedIid,IComponent_iid))*out=(IComponent*)this;
    else if(eq(requestedIid,IAudioProcessor_iid))*out=(IAudioProcessor*)this;
    else if(eq(requestedIid,IEditController_iid))*out=(IEditController*)this;
    else if(eq(requestedIid,IProcessContextRequirements_iid))*out=(IProcessContextRequirements*)this;
    else if(eq(requestedIid,IPrefetchableSupport_iid)){event(7,variant);*out=(IPrefetchableSupport*)this;}
    if(!*out)return kNoInterface;addRef();return kResultOk;
  }
  uint32 PLUGIN_API addRef() override{return ++refs;}
  uint32 PLUGIN_API release() override{uint32 n=--refs;if(!n)delete this;return n;}
  tresult PLUGIN_API initialize(FUnknown*) override{event(2);return kResultOk;}
  tresult PLUGIN_API terminate() override{event(8);return kResultOk;}
  tresult PLUGIN_API getControllerClassId(TUID) override{return kNotImplemented;}
  tresult PLUGIN_API setIoMode(IoMode) override{return kResultOk;}
  int32 PLUGIN_API getBusCount(MediaType t,BusDirection dir) override{return t==kAudio?(dir==kInput?2:1):0;}
  tresult PLUGIN_API getBusInfo(MediaType t,BusDirection dir,int32 ix,BusInfo &b) override{
    if(t!=kAudio||ix<0||ix>=getBusCount(t,dir))return kInvalidArgument;b={};b.mediaType=kAudio;b.direction=dir;b.channelCount=ix==1&&auxArrangement==0?0:2;b.busType=ix==0?kMain:kAux;b.flags=ix==0?BusInfo::kDefaultActive:0;text16(b.name,ix==1?L"Sidechain":dir==kInput?L"Stereo In":L"Stereo Out",128);return kResultOk;}
  tresult PLUGIN_API getRoutingInfo(RoutingInfo&,RoutingInfo&) override{return kNotImplemented;}
  tresult PLUGIN_API activateBus(MediaType t,BusDirection dir,int32 ix,TBool) override{return t==kAudio&&ix>=0&&ix<getBusCount(t,dir)?kResultOk:kInvalidArgument;}
  tresult PLUGIN_API setActive(TBool b) override{if(b){memset(delayline,0,sizeof(delayline));delayhead=0;}event(3,b);return kResultOk;}
  tresult PLUGIN_API setState(IBStream *s) override{return loadState(s);}
  tresult PLUGIN_API setComponentState(IBStream *s) override{return loadState(s);}
  tresult PLUGIN_API getState(IBStream *s) override{
    if(!s)return kInvalidArgument;uint32 magic=0x31494441;int32 n=0;double p[4];for(int i=0;i<4;++i)p[i]=params[i].load();
    if(s->write(&magic,4,&n)!=kResultOk||n!=4)return kResultFalse;return s->write(p,sizeof(p),&n)==kResultOk&&n==sizeof(p)?kResultOk:kResultFalse;}
  tresult PLUGIN_API setBusArrangements(SpeakerArrangement *in,int32 ni,SpeakerArrangement *out,int32 no) override{if((ni!=1&&ni!=2)||no!=1||!in||!out||in[0]!=3||out[0]!=3||(ni==2&&in[1]!=0&&in[1]!=3))return kResultFalse;auxArrangement=ni==2?in[1]:0;return kResultOk;}
  tresult PLUGIN_API getBusArrangement(BusDirection dir,int32 ix,SpeakerArrangement &a) override{if(ix<0||ix>=getBusCount(kAudio,dir))return kInvalidArgument;a=ix==1?auxArrangement:3;return kResultOk;}
  tresult PLUGIN_API canProcessSampleSize(int32 s) override{return s==kSample32||s==kSample64?kResultOk:kResultFalse;}
  uint32 PLUGIN_API getLatencySamples() override{return latency();}
  uint32 PLUGIN_API getTailSamples() override{return latency();}
  tresult PLUGIN_API setupProcessing(ProcessSetup &s) override{if(s.sampleRate<=0||canProcessSampleSize(s.symbolicSampleSize)!=kResultOk)return kInvalidArgument;rate=s.sampleRate;setupMode=s.processMode;event(5,s.processMode,s.maxSamplesPerBlock);return kResultOk;}
  tresult PLUGIN_API setProcessing(TBool b) override{event(4,b);return kResultOk;}
  tresult PLUGIN_API process(ProcessData &d) override {
    if(d.numSamples<0||canProcessSampleSize(d.symbolicSampleSize)!=kResultOk)return kInvalidArgument;
    TimeRecord tr{};const bool timeLogging=timetrace.enabled();
    if(timeLogging){
      tr.qpc=adi_qpc();tr.variant=variant;tr.latency=latency();tr.frames=d.numSamples;tr.sample_size=d.symbolicSampleSize;tr.mode=d.processMode;
      tr.id=(uint32)std::llround(params[0].load()*4095.0);tr.gain_before=params[1].load();tr.sample_rate=rate;
      if(d.processContext){auto&c=*d.processContext;tr.context_present=1;tr.context_state=c.state;tr.project_samples=c.projectTimeSamples;tr.continuous_samples=c.continousTimeSamples;tr.system_ns=c.systemTime;tr.sample_rate=c.sampleRate;tr.project_music=c.projectTimeMusic;tr.bar_music=c.barPositionMusic;tr.cycle_start_music=c.cycleStartMusic;tr.cycle_end_music=c.cycleEndMusic;tr.tempo=c.tempo;tr.numerator=c.timeSigNumerator;tr.denominator=c.timeSigDenominator;
        static_assert(sizeof(ProcessContext)<=256,"raw context capacity");tr.context_bytes=sizeof(ProcessContext);memcpy(tr.raw_context,&c,sizeof(c));}
      if(d.inputParameterChanges)for(int j=0;j<d.inputParameterChanges->getParameterCount();++j){auto*q=d.inputParameterChanges->getParameterData(j);if(!q)continue;for(int k=0;k<q->getPointCount();++k){int off;double v;if(q->getPoint(k,off,v)==kResultOk){++tr.param_total;if(tr.param_count<64)tr.parameters[tr.param_count++]={q->getParameterId(),off,v};}}}
      if(d.numInputs&&d.inputs&&d.inputs[0].numChannels>0)for(int i=0;i<d.numSamples;++i){double x=0;auto&b=d.inputs[0];if(d.symbolicSampleSize==kSample32&&b.channelBuffers32&&b.channelBuffers32[0])x=b.channelBuffers32[0][i];if(d.symbolicSampleSize==kSample64&&b.channelBuffers64&&b.channelBuffers64[0])x=b.channelBuffers64[0][i];if(std::abs(x)>markerFloor){++tr.input_total;if(tr.input_count<32)tr.input[tr.input_count++]={i,0,x};}}
    }
    bool logging=trace.enabled(2),capture=pcm.enabled()&&std::llround(params[0].load()*4095.0)==100;ADIRecord r{};if(logging||capture){r.qpc0=adi_qpc();r.kind=4;r.rt=rtfn?(rtfn()?1:0):-1;r.heard=heardfn?heardfn():-1;r.decode=decodefn?decodefn():-1;r.transport=-1;r.steady=-1;r.mode=d.processMode;r.rate=rate;r.frames=d.numSamples;
      if(d.processContext){r.transport=double(d.processContext->projectTimeSamples)/(d.processContext->sampleRate>0?d.processContext->sampleRate:rate);r.flags=d.processContext->state;if(r.flags&ProcessContext::kContTimeValid)r.steady=d.processContext->continousTimeSamples;}
      r.events=d.inputEvents?d.inputEvents->getEventCount():0;}
    double p[4];for(int i=0;i<4;++i)p[i]=params[i].load();
    if(d.symbolicSampleSize==kSample32)samples<float>(d,p,logging,r.input_rms,r.output_rms);else samples<double>(d,p,logging,r.input_rms,r.output_rms);
    int iterations=(int)std::llround(p[2]*20000.0);adi_work(d.numSamples,iterations,workstate);
    if(logging||capture){r.id=(uint32)std::llround(p[0]*4095.0);r.parameter=p[1];r.work=iterations;r.qpc1=adi_qpc();}
    if(capture&&d.numOutputs>0&&d.outputs&&d.outputs[0].numChannels>0){ADIPCMChunk chunk{};chunk.qpc0=r.qpc0;chunk.qpc1=r.qpc1;chunk.transport=r.transport;chunk.mode=r.mode;chunk.rt=r.rt;chunk.flags=r.flags;
      if(d.symbolicSampleSize==kSample32&&d.outputs[0].channelBuffers32)pcm.append(d.outputs[0].channelBuffers32[0],d.numSamples,chunk);
      else if(d.symbolicSampleSize==kSample64&&d.outputs[0].channelBuffers64)pcm.append(d.outputs[0].channelBuffers64[0],d.numSamples,chunk);}
    if(timeLogging){
      tr.gain_after=params[1].load();
      if(d.numOutputs&&d.outputs&&d.outputs[0].numChannels>0)for(int i=0;i<d.numSamples;++i){double x=0;auto&b=d.outputs[0];if(d.symbolicSampleSize==kSample32&&b.channelBuffers32&&b.channelBuffers32[0])x=b.channelBuffers32[0][i];if(d.symbolicSampleSize==kSample64&&b.channelBuffers64&&b.channelBuffers64[0])x=b.channelBuffers64[0][i];if(std::abs(x)>markerFloor){++tr.output_total;if(tr.output_count<32)tr.output[tr.output_count++]={i,0,x};}}
      timetrace.record(tr);
    }
    if(logging)trace.record(r);return kResultOk;
  }
  tresult PLUGIN_API getPrefetchableSupport(PrefetchableSupport &s) override{s=kIsYetPrefetchable;event(6,s);return kResultOk;}
  uint32 PLUGIN_API getProcessContextRequirements() override{return kNeedSystemTime|kNeedContinousTimeSamples|kNeedTransportState|kNeedProjectTimeMusic|kNeedBarPositionMusic|kNeedCycleMusic|kNeedTempo|kNeedTimeSignature|kNeedSamplesToNextClock|kNeedFrameRate;}
  int32 PLUGIN_API getParameterCount() override{return 4;}
  tresult PLUGIN_API getParameterInfo(int32 ix,ParameterInfo &p) override{
    if(ix<0||ix>=4)return kInvalidArgument;static const wchar_t *names[4]={L"Probe ID",L"Gain",L"Work iterations/sample",L"Generate 997Hz"};p={};p.id=ix;p.flags=ParameterInfo::kCanAutomate;p.stepCount=ix==0?4095:ix==2?20000:ix==3?1:0;p.defaultNormalizedValue=ix==1?1:0;p.unitId=0;text16(p.title,names[ix],128);text16(p.shortTitle,names[ix],128);return kResultOk;}
  tresult PLUGIN_API getParamStringByValue(ParamID id,ParamValue v,String128 out) override{if(id>=4)return kInvalidArgument;swprintf_s((wchar_t*)out,128,L"%.6g",v*maxplain(id));return kResultOk;}
  tresult PLUGIN_API getParamValueByString(ParamID id,TChar *s,ParamValue &v) override{if(id>=4||!s)return kInvalidArgument;wchar_t *end=nullptr;double x=wcstod((wchar_t*)s,&end);if(end==(wchar_t*)s)return kResultFalse;v=norm(x/maxplain(id));return kResultOk;}
  ParamValue PLUGIN_API normalizedParamToPlain(ParamID id,ParamValue v) override{return v*maxplain(id);}
  ParamValue PLUGIN_API plainParamToNormalized(ParamID id,ParamValue v) override{return norm(v/maxplain(id));}
  ParamValue PLUGIN_API getParamNormalized(ParamID id) override{return id<4?params[id].load():0;}
  tresult PLUGIN_API setParamNormalized(ParamID id,ParamValue v) override{if(id>=4)return kInvalidArgument;params[id].store(norm(v));return kResultOk;}
  tresult PLUGIN_API setComponentHandler(IComponentHandler *h) override{if(h)h->addRef();if(handler)handler->release();handler=h;return kResultOk;}
  IPlugView *PLUGIN_API createView(FIDString) override{return nullptr;}
};

class Factory final:public IPluginFactory2 {
  std::atomic<uint32> refs{1};
public:
  tresult PLUGIN_API queryInterface(const TUID requestedIid,void **out) override{if(!out)return kInvalidArgument;*out=nullptr;if(eq(requestedIid,FUnknown_iid)||eq(requestedIid,IPluginFactory_iid)||eq(requestedIid,IPluginFactory2_iid))*out=(IPluginFactory2*)this;if(!*out)return kNoInterface;addRef();return kResultOk;}
  uint32 PLUGIN_API addRef() override{return ++refs;}
  uint32 PLUGIN_API release() override{return --refs;}
  tresult PLUGIN_API getFactoryInfo(PFactoryInfo *p) override{if(!p)return kInvalidArgument;*p={};strcpy_s(p->vendor,"ADI Research");strcpy_s(p->url,"https://github.com/arieladi/Adi");strcpy_s(p->email,"research@localhost.invalid");p->flags=PFactoryInfo::kUnicode;return kResultOk;}
  int32 PLUGIN_API countClasses() override{return 3;}
  tresult PLUGIN_API getClassInfo(int32 ix,PClassInfo *p) override{if(ix<0||ix>=3||!p)return kInvalidArgument;*p={};memcpy(p->cid,CLASS_IDS[ix],16);p->cardinality=PClassInfo::kManyInstances;strcpy_s(p->category,kVstAudioEffectClass);strcpy_s(p->name,CLASS_NAMES[ix]);return kResultOk;}
  tresult PLUGIN_API getClassInfo2(int32 ix,PClassInfo2 *p) override{if(ix<0||ix>=3||!p)return kInvalidArgument;*p={};memcpy(p->cid,CLASS_IDS[ix],16);p->cardinality=PClassInfo::kManyInstances;strcpy_s(p->category,kVstAudioEffectClass);strcpy_s(p->name,CLASS_NAMES[ix]);strcpy_s(p->subCategories,"Fx|Tools");strcpy_s(p->vendor,"ADI Research");strcpy_s(p->version,"1.0.0");strcpy_s(p->sdkVersion,"VST 3.8.0");return kResultOk;}
  tresult PLUGIN_API createInstance(FIDString cid,FIDString requestedIid,void **out) override{if(!out)return kInvalidArgument;*out=nullptr;for(int i=0;i<3;++i)if(eq(cid,CLASS_IDS[i])){auto *p=new Probe(i);tresult r=p->queryInterface(requestedIid,out);p->release();return r;}return kNoInterface;}
};
extern "C" __declspec(dllexport) IPluginFactory *PLUGIN_API GetPluginFactory(){static Factory f;f.addRef();return &f;}
extern "C" __declspec(dllexport) bool InitDll(){return true;}
extern "C" __declspec(dllexport) bool ExitDll(){return true;}
