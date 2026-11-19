// Standalone ABI/process tests for ADI diagnostic VST3. Does not launch a DAW.
#define NOMINMAX
#include "../probe_shared.h"
#include "pcm_shared.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/ivstprefetchablesupport.h"
#include <cstdio>
#include <cstdlib>
#include <algorithm>
using namespace Steinberg;using namespace Steinberg::Vst;
static int checks=0;
static void check(bool b,const char *why){++checks;if(!b){printf("FAIL: %s\n",why);exit(1);}}
#define TEST_UNKNOWN tresult PLUGIN_API queryInterface(const TUID,void **p) override{if(p)*p=nullptr;return kNoInterface;} uint32 PLUGIN_API addRef()override{return 1;} uint32 PLUGIN_API release()override{return 1;}
struct Stream:IBStream{
 TEST_UNKNOWN char data[128]{};int pos=0,used=0;
 tresult PLUGIN_API read(void *p,int32 n,int32 *got)override{int k=std::max(0,std::min(n,used-pos));memcpy(p,data+pos,k);pos+=k;if(got)*got=k;return k==n?kResultOk:kResultFalse;}
 tresult PLUGIN_API write(void *p,int32 n,int32 *got)override{int k=std::max(0,std::min(n,128-pos));memcpy(data+pos,p,k);pos+=k;used=std::max(used,pos);if(got)*got=k;return k==n?kResultOk:kResultFalse;}
 tresult PLUGIN_API seek(int64 p,int32 mode,int64 *res)override{int64 np=mode==kIBSeekSet?p:mode==kIBSeekCur?pos+p:used+p;if(np<0||np>used)return kInvalidArgument;pos=(int)np;if(res)*res=pos;return kResultOk;}
 tresult PLUGIN_API tell(int64 *p)override{if(!p)return kInvalidArgument;*p=pos;return kResultOk;}
};
struct Queue:IParamValueQueue{TEST_UNKNOWN
 ParamID id=1;int32 count=2,offs[2]{0,32};double vals[2]{1.0,0.5};
 ParamID PLUGIN_API getParameterId()override{return id;}
 int32 PLUGIN_API getPointCount()override{return count;}
 tresult PLUGIN_API getPoint(int32 ix,int32 &off,ParamValue &v)override{if(ix<0||ix>=count)return kInvalidArgument;off=offs[ix];v=vals[ix];return kResultOk;}
 tresult PLUGIN_API addPoint(int32,ParamValue,int32&)override{return kNotImplemented;}
};
struct Changes:IParameterChanges{TEST_UNKNOWN Queue q;
 int32 PLUGIN_API getParameterCount()override{return 1;}
 IParamValueQueue *PLUGIN_API getParameterData(int32 i)override{return i==0?&q:nullptr;}
 IParamValueQueue *PLUGIN_API addParameterData(const ParamID&,int32&)override{return nullptr;}
};
int main(int argc,char **argv){
 wchar_t mapname[128];swprintf_s(mapname,L"Local\\ADI_VST3_SELFTEST_%lu",GetCurrentProcessId());SetEnvironmentVariableW(L"ADI_ENGINE_MAP",mapname);
 HANDLE hm=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,(DWORD)(ADI_MAP_BYTES>>32),(DWORD)ADI_MAP_BYTES,mapname);check(hm!=nullptr,"shared mapping");
 auto *h=(ADIHeader*)MapViewOfFile(hm,FILE_MAP_ALL_ACCESS,0,0,ADI_MAP_BYTES);check(h!=nullptr,"mapping view");memcpy(h->magic,"ADIENG01",8);h->record_size=sizeof(ADIRecord);h->capacity=ADI_CAPACITY;h->enabled=2;
 wchar_t pcmname[128];swprintf_s(pcmname,L"%ls_PCM",mapname);HANDLE pm=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,(DWORD)ADI_PCM_MAP_BYTES,pcmname);check(pm!=nullptr,"PCM mapping");auto *ph=(ADIPCMHeader*)MapViewOfFile(pm,FILE_MAP_ALL_ACCESS,0,0,ADI_PCM_MAP_BYTES);check(ph!=nullptr,"PCM mapping view");memcpy(ph->magic,"ADIPCM01",8);ph->version=1;ph->chunk_size=sizeof(ADIPCMChunk);ph->capacity_frames=ADI_PCM_FRAMES;ph->capacity_chunks=ADI_PCM_CHUNKS;ph->enabled=1;
 HMODULE dll=LoadLibraryA(argc>1?argv[1]:"ADIEngineProbe.vst3");check(dll!=nullptr,"LoadLibrary VST3 binary");
 auto get=(IPluginFactory*(PLUGIN_API*)())GetProcAddress(dll,"GetPluginFactory");check(get!=nullptr,"factory export");auto *f=get();check(f->countClasses()==3,"three variants");
 for(int variant=0;variant<3;++variant){PClassInfo ci{};check(f->getClassInfo(variant,&ci)==kResultOk,"class enumeration");IComponent *c=nullptr;check(f->createInstance(ci.cid,IComponent_iid,(void**)&c)==kResultOk&&c,"component construction");check(c->initialize(nullptr)==kResultOk,"component initialize");
  IAudioProcessor *a=nullptr;IEditController *e=nullptr;check(c->queryInterface(IAudioProcessor_iid,(void**)&a)==kResultOk,"audio interface");check(c->queryInterface(IEditController_iid,(void**)&e)==kResultOk,"single-component controller");
  IPrefetchableSupport *pre=nullptr;auto pr=c->queryInterface(IPrefetchableSupport_iid,(void**)&pre);if(variant==2)check(pr==kNoInterface&&pre==nullptr,"absent prefetch interface");else{check(pr==kResultOk&&pre,"present prefetch interface");PrefetchableSupport support=99;check(pre->getPrefetchableSupport(support)==kResultOk&&support==static_cast<PrefetchableSupport>(variant==1?kIsNeverPrefetchable:kIsYetPrefetchable),"prefetch result");pre->release();}
  e->setParamNormalized(0,double(20+variant)/4095.0);e->setParamNormalized(2,2.0/20000.0);
  check(c->getBusCount(kAudio,kInput)==2&&c->getBusCount(kAudio,kOutput)==1,"two input buses one output");BusInfo bus{};check(c->getBusInfo(kAudio,kInput,1,bus)==kResultOk&&bus.busType==kAux&&bus.channelCount==2,"auxiliary bus metadata");SpeakerArrangement arrangements[2]{3,3},arrout[1]{3};check(a->setBusArrangements(arrangements,2,arrout,1)==kResultOk&&c->activateBus(kAudio,kInput,1,true)==kResultOk,"sidechain activation");
  ProcessSetup s{kRealtime,kSample32,1024,48000};check(a->setupProcessing(s)==kResultOk,"setup realtime");check(c->setActive(true)==kResultOk&&a->setProcessing(true)==kResultOk,"activation");
  float il[64],ir[64],ol[64]{},orr[64]{};std::fill_n(il,64,0.2f);std::fill_n(ir,64,-0.1f);float *inp[2]{il,ir},*outp[2]{ol,orr};AudioBusBuffers in,out;in.numChannels=out.numChannels=2;in.channelBuffers32=inp;out.channelBuffers32=outp;
  ProcessContext context{};context.sampleRate=48000;context.projectTimeSamples=48000;context.continousTimeSamples=64000;context.state=ProcessContext::kPlaying|ProcessContext::kContTimeValid;
  AudioBusBuffers buses[2]{in,in};Changes changes;ProcessData d;d.processMode=kPrefetch;d.symbolicSampleSize=kSample32;d.numSamples=64;d.numInputs=2;d.numOutputs=1;d.inputs=buses;d.outputs=&out;d.processContext=&context;d.inputParameterChanges=&changes;
  check(a->process(d)==kResultOk,"prefetch without setup transition");for(int n=0;n<64;++n){check(std::abs(ol[n]-(n<32?0.2f:0.1f))<1e-7,"sample-stamped gain L");check(std::abs(orr[n]-(n<32?-0.1f:-0.05f))<1e-7,"sample-stamped gain R");}check(out.silenceFlags==0,"non-silent flags");
  if(variant==0){check(ph->next_chunk==0,"non-observer ID does not capture");h->enabled=0;e->setParamNormalized(0,100.0/4095);e->setParamNormalized(1,0.25);d.inputParameterChanges=nullptr;check(a->process(d)==kResultOk&&ph->next_chunk==1,"PCM float32 capture without event tracing");e->setParamNormalized(0,20.0/4095);e->setParamNormalized(1,0.5);d.inputParameterChanges=&changes;h->enabled=2;}
  Stream stream;check(c->getState(&stream)==kResultOk,"state serialize");e->setParamNormalized(1,0);stream.pos=0;check(c->setState(&stream)==kResultOk&&std::abs(e->getParamNormalized(1)-0.5)<1e-12,"state round trip");
  d.numSamples=0;changes.q.count=1;changes.q.vals[0]=0.25;check(a->process(d)==kResultOk&&e->getParamNormalized(1)==0.25,"zero-sample parameter flush");
  d.numSamples=64;d.inputParameterChanges=nullptr;std::fill_n(il,64,0.0f);std::fill_n(ir,64,0.0f);d.processMode=kRealtime;check(a->process(d)==kResultOk&&out.silenceFlags==3,"silence flags");
  e->setParamNormalized(3,1);check(a->process(d)==kResultOk&&out.silenceFlags==0&&std::abs(ol[3])>0,"optional generator");e->setParamNormalized(3,0);
  check(a->setProcessing(false)==kResultOk&&c->setActive(false)==kResultOk,"deactivation for offline");s.processMode=kOffline;s.symbolicSampleSize=kSample64;check(a->setupProcessing(s)==kResultOk,"offline setup");c->setActive(true);a->setProcessing(true);
  double dl[64],dr[64],dol[64]{},dor[64]{};std::fill_n(dl,64,0.4);std::fill_n(dr,64,-0.2);double *dip[2]{dl,dr},*dop[2]{dol,dor};in.channelBuffers64=dip;buses[0]=buses[1]=in;out.channelBuffers64=dop;d.symbolicSampleSize=kSample64;d.processMode=kOffline;check(a->process(d)==kResultOk&&std::abs(dol[12]-0.1)<1e-12,"64-bit offline output");
  if(variant==0){h->enabled=0;e->setParamNormalized(0,100.0/4095);check(a->process(d)==kResultOk&&ph->next_chunk==2,"PCM float64 capture");ph->enabled=0;check(a->process(d)==kResultOk&&ph->next_chunk==2,"PCM disabled no writes");ph->enabled=1;e->setParamNormalized(0,20.0/4095);h->enabled=2;}
  a->setProcessing(false);c->setActive(false);c->terminate();e->release();a->release();c->release();
 }
 auto *records=(ADIRecord*)((char*)h+ADI_HEADER_BYTES);int process=0,lifecycle=0,prefetchqueries=0;for(LONG64 n=0;n<h->next;++n){auto &r=records[n];check(r.committed==n+1,"record committed");check(r.pid==GetCurrentProcessId()&&r.tid==GetCurrentThreadId(),"record thread/process");if(r.kind==4){++process;check(r.rt==-1&&r.heard==-1&&r.decode==-1,"unknown REAPER APIs sentinel");check(r.rate==48000&&r.transport==1.0&&r.steady==64000,"context fields");check(r.qpc1>=r.qpc0&&r.id>=20&&r.id<=22,"timing and identity");}else if(r.kind==7){++lifecycle;if(r.events==6)++prefetchqueries;}}
 check(process==15,"five processing calls per variant");check(prefetchqueries==2,"two prefetch support queries");check(h->lost==0,"no trace overflow");
 check(ph->next_chunk==2&&ph->next_frame==128&&ph->lost_frames==0&&ph->lost_chunks==0,"PCM counts");auto *pc=(ADIPCMChunk*)((char*)ph+ADI_PCM_HEADER_BYTES);auto *pd=(double*)((char*)ph+ADI_PCM_DATA_OFFSET);for(int i=0;i<128;++i)check(std::abs(pd[i]-(i<64?0.05:0.1))<1e-8,"PCM sample data");check(pc[0].committed==1&&pc[1].committed==2&&pc[0].start_frame==0&&pc[1].start_frame==64,"PCM commit/offset");check(pc[0].mode==kPrefetch&&pc[1].mode==kOffline&&pc[0].transport==1&&pc[1].frames==64&&pc[0].qpc1>=pc[0].qpc0,"PCM metadata");
 ADIPCMTrace overflow;check(overflow.open(),"PCM extra reader/writer mapping");ph->next_frame=ADI_PCM_FRAMES-10;pd[ADI_PCM_FRAMES-1]=123.0;float excess[64]{};ADIPCMChunk meta{};overflow.append(excess,64,meta);check(ph->lost_frames==64&&ph->lost_chunks==1&&pd[ADI_PCM_FRAMES-1]==123.0,"PCM overflow whole-block drop");overflow.close();
 f->release();FreeLibrary(dll);printf("PASS: %d checks; process_records=%d lifecycle_records=%d prefetch_queries=%d lost=%lld; PCM2chunks128frames and bounded-overflow verified\n",checks,process,lifecycle,prefetchqueries,h->lost);UnmapViewOfFile(ph);CloseHandle(pm);UnmapViewOfFile(h);CloseHandle(hm);return 0;
}
