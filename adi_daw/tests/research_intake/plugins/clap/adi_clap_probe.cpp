// ADI engine diagnostic CLAP probe, original code MIT, 2026-11-17.
#define NOMINMAX
#include "../probe_shared.h"
#include <clap/clap.h>
#include <atomic>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <new>
static_assert(std::atomic<double>::is_always_lock_free,"lock-free parameters required");
static const char *features[]={CLAP_PLUGIN_FEATURE_AUDIO_EFFECT,CLAP_PLUGIN_FEATURE_UTILITY,CLAP_PLUGIN_FEATURE_STEREO,nullptr};
static const clap_plugin_descriptor_t desc[2]={
 {CLAP_VERSION,"org.adi.research.engine-probe","ADI Engine Probe","ADI Research","","","","1.0.0","Engine scheduling diagnostic",features},
 {CLAP_VERSION,"org.adi.research.engine-probe-hard-rt","ADI Engine Probe Hard Realtime","ADI Research","","","","1.0.0","Reports hard realtime requirement",features}};
static double maximum(clap_id id){return id==0?4095.0:id==2?20000.0:1.0;}
static double clampval(clap_id id,double v){return std::isfinite(v)?std::clamp(v,0.0,maximum(id)):0.0;}
struct Probe {
 clap_plugin_t plugin{};const clap_host_t *host;int variant;
 ADITrace trace;std::atomic<double> params[4]{{0},{1},{0},{0}};
 double rate=48000,phase=0,workstate=0.2;std::atomic<int> mode{CLAP_RENDER_REALTIME};
 bool (*rtfn)()=nullptr;double (*heardfn)()=nullptr;double (*decodefn)()=nullptr;
 const clap_host_thread_check_t *threadcheck=nullptr;
 const clap_host_params_t *hostparams=nullptr;
 static Probe *self(const clap_plugin_t *p){return (Probe*)p->plugin_data;}
 void event(uint32_t code,int value=0,uint32_t frames=0){if(!trace.enabled(2))return;ADIRecord r{};r.qpc0=r.qpc1=adi_qpc();r.kind=7;r.id=(uint32_t)std::llround(params[0].load());r.events=code;r.mode=value;r.flags=variant;r.rate=rate;r.frames=frames;r.rt=-1;r.transport=r.heard=r.decode=-1;r.steady=-1;trace.record(r);}
 explicit Probe(const clap_host_t *h,int v):host(h),variant(v){plugin={&desc[v],this,init,destroy,activate,deactivate,start,stop,reset,process,extension,onmain};}
 static bool CLAP_ABI init(const clap_plugin_t *p){auto *s=self(p);s->trace.open();if(auto get=adi_api()){s->rtfn=(bool(*)())get("IsInRealTimeAudio");s->heardfn=(double(*)())get("GetPlayPosition");s->decodefn=(double(*)())get("GetPlayPosition2");}if(s->host->get_extension){s->threadcheck=(const clap_host_thread_check_t*)s->host->get_extension(s->host,CLAP_EXT_THREAD_CHECK);s->hostparams=(const clap_host_params_t*)s->host->get_extension(s->host,CLAP_EXT_PARAMS);}s->event(101);return true;}
 static void CLAP_ABI destroy(const clap_plugin_t *p){auto *s=self(p);s->event(108);s->trace.close();delete s;}
 static bool CLAP_ABI activate(const clap_plugin_t *p,double sr,uint32_t minf,uint32_t maxf){auto *s=self(p);if(sr<=0||minf<1||maxf<minf)return false;s->rate=sr;s->event(102,s->mode.load(),maxf);return true;}
 static void CLAP_ABI deactivate(const clap_plugin_t *p){self(p)->event(103);}
 static bool CLAP_ABI start(const clap_plugin_t *p){self(p)->event(104);return true;}
 static void CLAP_ABI stop(const clap_plugin_t *p){self(p)->event(105);}
 static void CLAP_ABI reset(const clap_plugin_t *p){auto *s=self(p);s->phase=0;s->workstate=0.2;s->event(106);}
 static void CLAP_ABI onmain(const clap_plugin_t*){}
 void apply(const clap_event_header_t *h,double v[4]){if(!h||h->space_id!=CLAP_CORE_EVENT_SPACE_ID||h->type!=CLAP_EVENT_PARAM_VALUE||h->size<sizeof(clap_event_param_value_t))return;auto *ev=(const clap_event_param_value_t*)h;if(ev->param_id>=4||ev->note_id!=-1||ev->port_index!=-1||ev->channel!=-1||ev->key!=-1)return;v[ev->param_id]=clampval(ev->param_id,ev->value);}
 template<class T> void audio(const clap_process_t *d,double v[4],bool logging,double &rin,double &rout){
  uint32_t count=d->in_events?d->in_events->size(d->in_events):0,ei=0;
  auto getEvent=[&](){return ei<count?d->in_events->get(d->in_events,ei):nullptr;};
  const clap_event_header_t *ev=getEvent();
  const clap_audio_buffer_t *in=d->audio_inputs_count?&d->audio_inputs[0]:nullptr;
  clap_audio_buffer_t *out=d->audio_outputs_count?&d->audio_outputs[0]:nullptr;
  T **ip=nullptr,**op=nullptr;
  if constexpr(sizeof(T)==4){if(in)ip=in->data32;if(out)op=out->data32;}else{if(in)ip=in->data64;if(out)op=out->data64;}
  uint32_t nc=out?out->channel_count:0;double si=0,so=0;
  for(uint32_t n=0;n<d->frames_count;++n){while(ei<count&&(!ev||ev->time<=n)){if(ev)apply(ev,v);++ei;ev=getEvent();}
   double synth=0;if(v[3]>=0.5){synth=.05*std::sin(phase);phase+=6.2831853071795864769*997.0/rate;if(phase>=6.2831853071795864769)phase-=6.2831853071795864769;}
   for(uint32_t c=0;c<nc;++c){double x=(in&&c<in->channel_count&&ip&&ip[c])?ip[c][n]:0;double y=(x+synth)*v[1];if(op&&op[c])op[c][n]=(T)y;if(logging){si+=x*x;so+=y*y;}}
  }
  if(out)out->constant_mask=0; // Conservatively no constant-buffer optimization claim.
  double n=double(d->frames_count)*nc;rin=logging&&n?std::sqrt(si/n):0;rout=logging&&n?std::sqrt(so/n):0;
 }
 static clap_process_status CLAP_ABI process(const clap_plugin_t *p,const clap_process_t *d){auto *s=self(p);if(!d)return CLAP_PROCESS_ERROR;bool logging=s->trace.enabled(2);ADIRecord r{};
  if(logging){r.qpc0=adi_qpc();r.kind=3;r.rate=s->rate;r.frames=d->frames_count;r.mode=s->mode.load();r.rt=s->rtfn?(s->rtfn()?1:0):-1;r.heard=s->heardfn?s->heardfn():-1;r.decode=s->decodefn?s->decodefn():-1;r.transport=-1;r.steady=d->steady_time;r.events=d->in_events?d->in_events->size(d->in_events):0;
   if(d->transport){r.flags=d->transport->flags;if(r.flags&CLAP_TRANSPORT_HAS_SECONDS_TIMELINE)r.transport=double(d->transport->song_pos_seconds)/double(CLAP_SECTIME_FACTOR);}
   // High bits are diagnostic thread-check availability/value, separate from CLAP transport bits.
   if(s->threadcheck){r.flags|=1u<<30;if(s->threadcheck->is_audio_thread(s->host))r.flags|=1u<<31;}}
  double v[4];for(int i=0;i<4;++i)v[i]=s->params[i].load();bool use64=d->audio_outputs_count&&d->audio_outputs[0].data64;
  if(use64)s->audio<double>(d,v,logging,r.input_rms,r.output_rms);else s->audio<float>(d,v,logging,r.input_rms,r.output_rms);
  for(int i=0;i<4;++i)s->params[i].store(v[i]);int iterations=(int)std::llround(v[2]);adi_work(d->frames_count,iterations,s->workstate);
  if(logging){r.id=(uint32_t)std::llround(v[0]);r.parameter=v[1];r.work=iterations;r.qpc1=adi_qpc();s->trace.record(r);}return CLAP_PROCESS_CONTINUE;
 }
 static uint32_t CLAP_ABI portCount(const clap_plugin_t*,bool input){return input?2:1;}
 static bool CLAP_ABI portGet(const clap_plugin_t*,uint32_t ix,bool input,clap_audio_port_info_t *o){if(!o||ix>=(input?2u:1u))return false;*o={};o->id=ix;strcpy_s(o->name,ix==1?"Sidechain":input?"Stereo In":"Stereo Out");o->flags=CLAP_AUDIO_PORT_SUPPORTS_64BITS|CLAP_AUDIO_PORT_REQUIRES_COMMON_SAMPLE_SIZE|(ix==0?CLAP_AUDIO_PORT_IS_MAIN:0);o->channel_count=2;o->port_type=CLAP_PORT_STEREO;o->in_place_pair=ix==0?0:CLAP_INVALID_ID;return true;}
 static uint32_t CLAP_ABI paramCount(const clap_plugin_t*){return 4;}
 static bool CLAP_ABI paramInfo(const clap_plugin_t*,uint32_t ix,clap_param_info_t *o){if(ix>=4||!o)return false;static const char *names[4]={"Probe ID","Gain","Work iterations/sample","Generate 997Hz"};*o={};o->id=ix;o->flags=CLAP_PARAM_IS_AUTOMATABLE|(ix!=1?CLAP_PARAM_IS_STEPPED:0);strcpy_s(o->name,names[ix]);o->min_value=0;o->max_value=maximum(ix);o->default_value=ix==1?1:0;return true;}
 static bool CLAP_ABI paramGet(const clap_plugin_t *p,clap_id id,double *v){if(id>=4||!v)return false;*v=self(p)->params[id].load();return true;}
 static bool CLAP_ABI paramText(const clap_plugin_t*,clap_id id,double v,char *out,uint32_t cap){if(id>=4||!out||!cap)return false;return snprintf(out,cap,"%.6g",v)>=0;}
 static bool CLAP_ABI paramParse(const clap_plugin_t*,clap_id id,const char *str,double *v){if(id>=4||!str||!v)return false;char *end=nullptr;double x=strtod(str,&end);if(end==str||!std::isfinite(x))return false;*v=clampval(id,x);return true;}
 static void CLAP_ABI flush(const clap_plugin_t *p,const clap_input_events_t *in,const clap_output_events_t*){auto *s=self(p);double v[4];for(int i=0;i<4;++i)v[i]=s->params[i].load();if(in)for(uint32_t i=0;i<in->size(in);++i)s->apply(in->get(in,i),v);for(int i=0;i<4;++i)s->params[i].store(v[i]);}
 static bool CLAP_ABI hard(const clap_plugin_t *p){auto *s=self(p);s->event(111,s->variant);return s->variant==1;}
 static bool CLAP_ABI render(const clap_plugin_t *p,clap_plugin_render_mode mode){if(mode!=CLAP_RENDER_REALTIME&&mode!=CLAP_RENDER_OFFLINE)return false;auto *s=self(p);s->mode.store(mode);s->event(112,mode);return true;}
 static bool CLAP_ABI save(const clap_plugin_t *p,const clap_ostream_t *out){if(!out)return false;uint8_t bytes[36];uint32_t magic=0x31494441;memcpy(bytes,&magic,4);for(int i=0;i<4;++i){double v=self(p)->params[i].load();memcpy(bytes+4+8*i,&v,8);}uint64_t pos=0;while(pos<sizeof(bytes)){int64_t n=out->write(out,bytes+pos,sizeof(bytes)-pos);if(n<=0||uint64_t(n)>sizeof(bytes)-pos)return false;pos+=n;}return true;}
 static bool CLAP_ABI load(const clap_plugin_t *p,const clap_istream_t *in){if(!in)return false;uint8_t bytes[36];uint64_t pos=0;while(pos<sizeof(bytes)){int64_t n=in->read(in,bytes+pos,sizeof(bytes)-pos);if(n<=0||uint64_t(n)>sizeof(bytes)-pos)return false;pos+=n;}uint32_t magic;memcpy(&magic,bytes,4);if(magic!=0x31494441)return false;auto *s=self(p);for(int i=0;i<4;++i){double v;memcpy(&v,bytes+4+8*i,8);s->params[i].store(clampval(i,v));}if(s->hostparams)s->hostparams->rescan(s->host,CLAP_PARAM_RESCAN_VALUES);return true;}
 static uint32_t CLAP_ABI latency(const clap_plugin_t*){return 0;}
 static const void *CLAP_ABI extension(const clap_plugin_t *p,const char *id){
  static const clap_plugin_audio_ports_t ports{portCount,portGet};static const clap_plugin_params_t pars{paramCount,paramInfo,paramGet,paramText,paramParse,flush};static const clap_plugin_render_t rend{hard,render};static const clap_plugin_state_t state{save,load};static const clap_plugin_latency_t lat{latency};
  if(!strcmp(id,CLAP_EXT_AUDIO_PORTS))return &ports;if(!strcmp(id,CLAP_EXT_PARAMS))return &pars;if(!strcmp(id,CLAP_EXT_RENDER)){self(p)->event(110);return &rend;}if(!strcmp(id,CLAP_EXT_STATE))return &state;if(!strcmp(id,CLAP_EXT_LATENCY))return &lat;return nullptr;}
};
static uint32_t CLAP_ABI count(const clap_plugin_factory_t*){return 2;}
static const clap_plugin_descriptor_t *CLAP_ABI descriptor(const clap_plugin_factory_t*,uint32_t i){return i<2?&desc[i]:nullptr;}
static const clap_plugin_t *CLAP_ABI create(const clap_plugin_factory_t*,const clap_host_t *host,const char *id){if(!host||!id||!clap_version_is_compatible(host->clap_version))return nullptr;for(int i=0;i<2;++i)if(!strcmp(id,desc[i].id)){auto *p=new(std::nothrow) Probe(host,i);return p?&p->plugin:nullptr;}return nullptr;}
static bool CLAP_ABI entryInit(const char*){return true;}
static void CLAP_ABI entryDeinit(){}
static const void *CLAP_ABI factory(const char *id){static const clap_plugin_factory_t f{count,descriptor,create};return !strcmp(id,CLAP_PLUGIN_FACTORY_ID)?&f:nullptr;}
extern "C" __declspec(dllexport) const clap_plugin_entry_t clap_entry={CLAP_VERSION,entryInit,entryDeinit,factory};
