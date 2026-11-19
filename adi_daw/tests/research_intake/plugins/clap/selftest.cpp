// Standalone CLAP factory, DSP, state, logging and capability tests; no DAW.
#define NOMINMAX
#include "../probe_shared.h"
#include <clap/clap.h>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
static int checks=0,rescans=0;
static void check(bool b,const char *why){++checks;if(!b){printf("FAIL: %s\n",why);exit(1);}}
static bool CLAP_ABI threadYes(const clap_host_t*){return true;}
static void CLAP_ABI rescan(const clap_host_t*,clap_param_rescan_flags){++rescans;}
static void CLAP_ABI clear(const clap_host_t*,clap_id,clap_param_clear_flags){}
static void CLAP_ABI hostVoid(const clap_host_t*){}
static const void *CLAP_ABI getHost(const clap_host_t*,const char *id){static const clap_host_thread_check_t tc{threadYes,threadYes};static const clap_host_params_t hp{rescan,clear,hostVoid};if(!strcmp(id,CLAP_EXT_THREAD_CHECK))return &tc;if(!strcmp(id,CLAP_EXT_PARAMS))return &hp;return nullptr;}
struct Events {clap_input_events_t iface{this,size,get};clap_event_param_value_t ev[4]{};uint32_t n=0;
 static uint32_t CLAP_ABI size(const clap_input_events_t *p){return ((Events*)p->ctx)->n;}
 static const clap_event_header_t *CLAP_ABI get(const clap_input_events_t *p,uint32_t i){auto *s=(Events*)p->ctx;return i<s->n?&s->ev[i].header:nullptr;}
 void set(uint32_t index,clap_id id,double value,uint32_t time){auto &e=ev[index];e={};e.header={sizeof(e),time,CLAP_CORE_EVENT_SPACE_ID,CLAP_EVENT_PARAM_VALUE,0};e.param_id=id;e.note_id=-1;e.port_index=e.channel=e.key=-1;e.value=value;n=std::max(n,index+1);}
};
struct Stream {uint8_t bytes[128]{};uint64_t pos=0,used=0;clap_istream_t in{this,read};clap_ostream_t out{this,write};
 static int64_t CLAP_ABI write(const clap_ostream_t *p,const void *data,uint64_t n){auto *s=(Stream*)p->ctx;n=std::min({n,uint64_t(7),128-s->pos});memcpy(s->bytes+s->pos,data,n);s->pos+=n;s->used=std::max(s->used,s->pos);return n;}
 static int64_t CLAP_ABI read(const clap_istream_t *p,void *data,uint64_t n){auto *s=(Stream*)p->ctx;n=std::min({n,uint64_t(7),s->used-s->pos});memcpy(data,s->bytes+s->pos,n);s->pos+=n;return n;}
};
int main(int argc,char **argv){
 wchar_t mapname[128];swprintf_s(mapname,L"Local\\ADI_CLAP_SELFTEST_%lu",GetCurrentProcessId());SetEnvironmentVariableW(L"ADI_ENGINE_MAP",mapname);HANDLE hm=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,(DWORD)(ADI_MAP_BYTES>>32),(DWORD)ADI_MAP_BYTES,mapname);check(hm!=nullptr,"mapping");auto *h=(ADIHeader*)MapViewOfFile(hm,FILE_MAP_ALL_ACCESS,0,0,ADI_MAP_BYTES);check(h!=nullptr,"mapping view");memcpy(h->magic,"ADIENG01",8);h->record_size=sizeof(ADIRecord);h->capacity=ADI_CAPACITY;h->enabled=2;
 HMODULE dll=LoadLibraryA(argc>1?argv[1]:"ADIEngineProbe.clap");check(dll!=nullptr,"LoadLibrary CLAP");auto *entry=(const clap_plugin_entry_t*)GetProcAddress(dll,"clap_entry");check(entry&&entry->init("ADIEngineProbe.clap"),"entry init");auto *factory=(const clap_plugin_factory_t*)entry->get_factory(CLAP_PLUGIN_FACTORY_ID);check(factory&&factory->get_plugin_count(factory)==2,"two variants");clap_host_t host{CLAP_VERSION,nullptr,"ADI standalone ABI test","ADI Research","","1.0",getHost,hostVoid,hostVoid,hostVoid};
 for(uint32_t variant=0;variant<2;++variant){auto *description=factory->get_plugin_descriptor(factory,variant);check(description!=nullptr,"descriptor");auto *p=factory->create_plugin(factory,&host,description->id);check(p&&p->init(p),"create/init");
  auto *params=(const clap_plugin_params_t*)p->get_extension(p,CLAP_EXT_PARAMS);auto *ports=(const clap_plugin_audio_ports_t*)p->get_extension(p,CLAP_EXT_AUDIO_PORTS);auto *render=(const clap_plugin_render_t*)p->get_extension(p,CLAP_EXT_RENDER);auto *state=(const clap_plugin_state_t*)p->get_extension(p,CLAP_EXT_STATE);check(params&&ports&&render&&state,"extensions");check(render->has_hard_realtime_requirement(p)==(variant==1),"hard realtime variant");check(ports->count(p,true)==2&&ports->count(p,false)==1,"main/sidechain ports");clap_audio_port_info_t pi{};check(ports->get(p,1,true,&pi)&&pi.channel_count==2&&!(pi.flags&CLAP_AUDIO_PORT_IS_MAIN),"aux stereo port");
  Events init;init.set(0,0,30+variant,0);init.set(1,2,2,0);params->flush(p,&init.iface,nullptr);double val=0;check(params->get_value(p,0,&val)&&val==30+variant,"plain ID flush");check(render->set(p,CLAP_RENDER_REALTIME),"realtime render mode");check(p->activate(p,48000,1,1024)&&p->start_processing(p),"activate/start");
  float il[64],ir[64],ol[64]{},orr[64]{},side[64]{};std::fill_n(il,64,0.2f);std::fill_n(ir,64,-0.1f);float *ip[2]{il,ir},*op[2]{ol,orr},*sp[2]{side,side};clap_audio_buffer_t ins[2]{};clap_audio_buffer_t out{};ins[0].data32=ip;ins[1].data32=sp;ins[0].channel_count=ins[1].channel_count=out.channel_count=2;out.data32=op;
  clap_event_transport_t transport{};transport.flags=CLAP_TRANSPORT_HAS_SECONDS_TIMELINE|CLAP_TRANSPORT_IS_PLAYING;transport.song_pos_seconds=CLAP_SECTIME_FACTOR;
  Events changes;changes.set(0,1,1,0);changes.set(1,1,0.5,32);clap_process_t d{};d.steady_time=64000;d.frames_count=64;d.audio_inputs=ins;d.audio_inputs_count=2;d.audio_outputs=&out;d.audio_outputs_count=1;d.transport=&transport;d.in_events=&changes.iface;
  check(p->process(p,&d)==CLAP_PROCESS_CONTINUE,"process32");for(int n=0;n<64;++n){check(std::abs(ol[n]-(n<32?0.2f:0.1f))<1e-7,"sample-stamped gain L");check(std::abs(orr[n]-(n<32?-0.1f:-0.05f))<1e-7,"sample-stamped gain R");}
  p->stop_processing(p);p->deactivate(p);Stream stream;check(state->save(p,&stream.out)&&stream.used==36,"partial-write state save");Events zero;zero.set(0,1,0,0);params->flush(p,&zero.iface,nullptr);stream.pos=0;check(state->load(p,&stream.in)&&params->get_value(p,1,&val)&&val==0.5,"partial-read state load");
  check(p->activate(p,48000,1,1024)&&p->start_processing(p),"restart");std::fill_n(il,64,0.0f);std::fill_n(ir,64,0.0f);d.in_events=nullptr;d.steady_time+=64;check(p->process(p,&d)==CLAP_PROCESS_CONTINUE&&ol[10]==0&&orr[10]==0,"silence output");
  Events gen;gen.set(0,3,1,0);params->flush(p,&gen.iface,nullptr);d.steady_time+=64;check(p->process(p,&d)==CLAP_PROCESS_CONTINUE&&std::abs(ol[3])>0,"generator");gen.set(0,3,0,0);params->flush(p,&gen.iface,nullptr);
  p->stop_processing(p);p->deactivate(p);check(render->set(p,CLAP_RENDER_OFFLINE),"offline render mode");check(p->activate(p,48000,1,1024)&&p->start_processing(p),"offline start");double dl[64],dr[64],dol[64]{},dor[64]{},ds[64]{};std::fill_n(dl,64,0.4);std::fill_n(dr,64,-0.2);double *dip[2]{dl,dr},*dop[2]{dol,dor},*dsp[2]{ds,ds};ins[0].data32=ins[1].data32=out.data32=nullptr;ins[0].data64=dip;ins[1].data64=dsp;out.data64=dop;d.steady_time+=64;check(p->process(p,&d)==CLAP_PROCESS_CONTINUE&&std::abs(dol[12]-0.2)<1e-12,"64-bit offline");
  LONG64 before=h->next;h->enabled=0;check(p->process(p,&d)==CLAP_PROCESS_CONTINUE&&h->next==before,"tracing-disabled no records");h->enabled=2;p->reset(p);p->stop_processing(p);p->deactivate(p);p->destroy(p);
 }
 int process=0,hardqueries=0,offline=0;auto *records=(ADIRecord*)((char*)h+ADI_HEADER_BYTES);for(LONG64 n=0;n<h->next;++n){auto &r=records[n];check(r.committed==n+1,"commit sequence");if(r.kind==3){++process;check(r.rt==-1&&r.heard==-1&&r.decode==-1,"API absence sentinels");check(r.transport==1&&r.rate==48000&&r.steady>=64000,"transport");check((r.flags&0xC0000000)==0xC0000000,"logical audio thread check");check(r.id>=30&&r.id<=31&&r.qpc1>=r.qpc0&&r.frames==64,"identity/timing");if(r.mode==CLAP_RENDER_OFFLINE)++offline;}else if(r.kind==7&&r.events==111)++hardqueries;}
 check(process==8&&offline==2&&hardqueries==2,"record counts");check(rescans==2,"state rescan notification");check(h->lost==0,"no trace overflow");entry->deinit();FreeLibrary(dll);printf("PASS: %d checks; process_records=%d offline_records=%d hard_requirement_queries=%d lost=%lld\n",checks,process,offline,hardqueries,h->lost);UnmapViewOfFile(h);CloseHandle(hm);return 0;
}
