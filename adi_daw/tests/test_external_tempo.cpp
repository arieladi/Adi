// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/engine/session.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/audio/warp_map.hpp"
#include "adi/media/blake3.hpp"
#include "adi/freeze_commands.hpp"
#include "adi/digest.hpp"
#include "adi/ops.hpp"
#include "adi/store.hpp"
#include "adi/textproj.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <numbers>
namespace {std::atomic<unsigned> allocations{0},frees{0};}
void* operator new(std::size_t n){if(adi::audio::onAudioThread)++allocations;if(void* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return operator new(n);}
void operator delete(void* p)noexcept{if(p && adi::audio::onAudioThread)++frees;std::free(p);}
void operator delete[](void* p)noexcept{operator delete(p);}
void operator delete(void* p,std::size_t)noexcept{operator delete(p);}
void operator delete[](void* p,std::size_t)noexcept{operator delete(p);}
// Nothrow allocations use the same counted malloc/free pair under sanitizers.
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    try { return ::operator new(n); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
    try { return ::operator new[](n); } catch (...) { return nullptr; }
}
void operator delete(void* p, const std::nothrow_t&) noexcept { ::operator delete(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { ::operator delete[](p); }
namespace {
using namespace adi;using namespace adi::engine;constexpr auto q=textproj::kPPQ;
int checks=0,failures=0;void check(bool good,const char* label){++checks;if(!good){++failures;std::printf("FAIL: %s\n",label);}}
struct Heard{EventType type{};std::int64_t sample=0;};
struct Recorder final:device::DeviceInstance {
    device::DeviceIdentity id;TransportInfo last{};std::array<Heard,64> heard{};std::size_t count=0;std::int64_t clock=0;
    const device::DeviceIdentity& identity()const noexcept override{return id;}
    bool loaded()const noexcept override{return true;}
    bool processesDouble()const noexcept override{return true;}
    EventFlow eventFlow()const noexcept override{return EventFlow::Consume;}
    std::int64_t tailSamples()const noexcept override{return kInfiniteTail;}
    void process(const NodeIo& io)noexcept override{
        if(io.transport)last=*io.transport;
        for(const auto& event:io.events)if((event.type==EventType::NoteOn || event.type==EventType::NoteOff) && count<heard.size())heard[count++]={event.type,clock+event.frame};
        for(int c=0;c<io.channels;++c)for(int f=0;f<io.frames;++f){const auto at=io.blockOffset+f;if(io.wide())io.out64[c][at]=0;else io.out[c][at]=0;}
    }
};
struct Project {
    test::TempDirectory temp{"external_tempo","project"};std::unique_ptr<Store> store;Recorder* recorder=nullptr;
    Project(){StoreError error{};store=Store::createEmpty(temp.path()/"song.adi",error);if(!store)throw std::runtime_error("project");
        std::vector<float> samples(96000);for(std::size_t i=0;i<48000;++i){samples[i*2]=static_cast<float>(.3*std::sin(2*std::numbers::pi*440*static_cast<double>(i)/48000));samples[i*2+1]=static_cast<float>(.2*std::sin(2*std::numbers::pi*660*static_cast<double>(i)/48000));}
        {audio::WavWriter writer(temp.path()/"source.wav",48000,2,audio::WavFormat::Float32);writer.write(samples.data(),48000);writer.close();}
        auto& db=store->db();db.exec("INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(1,'audio','Warp',0),(2,'midi','Notes',1),(9,'master','Main',2); INSERT INTO plugin_refs(id,format,uid,name,subtype) VALUES(1,'vst3','test.external-tempo','Recorder','instrument'); INSERT INTO device_chains(id,track_id) VALUES(1,2); INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name) VALUES(1,1,0,1,'Recorder'); DELETE FROM tempo_map; INSERT INTO tempo_map(pos_ticks,bpm,curve) VALUES(0,120,0),(5765760,240,0); INSERT INTO media_files(id,hash_blake3,rel_path,format,sample_rate,channels,frames) VALUES(1,'pending-fixture-hash','source.wav','wav',48000,2,48000); INSERT INTO clips(id,track_id,kind,time_base,pos_ticks,length_ticks) VALUES(7,1,'audio',0,5765760,11531520),(8,2,'midi',0,5765760,11531520); INSERT INTO audio_clips(clip_id,media_id,src_len_frames,warp_enabled,warp_mode) VALUES(7,1,48000,1,'re-pitch'); INSERT INTO clip_playback_details(clip_id,content_offset_ns,source_end_ns) VALUES(7,0,1000000000); INSERT INTO automation_lanes(id,owner_kind,owner_id,param_ref,time_base,value_domain,default_value,enabled) VALUES(1,'track',1,'volume',0,'real',0,1)");
        {const auto identity=media::blake3File(temp.path()/"source.wav");if(!identity)throw std::runtime_error("source hash");SQLite::Statement hash(db,"UPDATE media_files SET hash_blake3=? WHERE id=1");hash.bind(1,identity.hex);hash.exec();}
        const auto map=audio::writeWarpMap(std::array<WarpMarker,2>{WarpMarker{0,0},WarpMarker{48000,2*q}},48000);{SQLite::Statement put(db,"UPDATE audio_clips SET warp_markers=? WHERE clip_id=7");put.bind(1,map.bytes.data(),static_cast<int>(map.bytes.size()));put.exec();}
        NoteRecord note{};note.start_ticks=q/4;note.dur_ticks=q/2;note.note_id=42;note.key=60;note.channel=1;note.vel_on=100;note.vel_off=40;note.probability=10000;
        if(!store->putEventStream(8,"notes",writeStream<NoteRecord>(FourCC::Notes,std::array<NoteRecord,1>{note})))throw std::runtime_error("ANOT");
        std::array<AutomationPoint,2> points{};points[0].point_id=1;points[0].time=0;points[0].value=-18;points[0].curve=1;points[1].point_id=2;points[1].time=4*q;points[1].value=0;points[1].curve=1;
        if(!store->putAutomationData(1,std::nullopt,writeStream<AutomationPoint>(FourCC::Automation,points)))throw std::runtime_error("AAUT");
    }
    DeviceLoader loader(){return [this](const DeviceRequest&,std::string&)->std::unique_ptr<device::DeviceInstance>{auto value=std::make_unique<Recorder>();recorder=value.get();return value;};}
};
void load(Project& project,Session& session,SessionSpec spec){session.graph().setFadeFrames(0);if(!session.load(*project.store,project.loader(),spec))throw std::runtime_error(session.error());if(!session.problems().empty())throw std::runtime_error(session.problems().front());session.transport().play();}
std::vector<double> render(Project& project,Session& session,double rate,Precision width,std::size_t frames) {
    (void)rate;std::vector<double> pcm(frames*2);std::array<float,512> narrow{};std::array<double,512> wide{};float* channels[]{narrow.data(),narrow.data()+256};double* channels64[]{wide.data(),wide.data()+256};
    const auto a=allocations.load(),f=frees.load(),io=audio::callbackFileIo.load();for(std::size_t first=0;first<frames;first+=256){const auto count=static_cast<std::int32_t>(std::min<std::size_t>(256,frames-first));if(!session.clips()->prime(std::chrono::seconds(10)))throw std::runtime_error("prime");project.recorder->clock=static_cast<std::int64_t>(first);AudioIo block;block.frames=count;block.numOut=2;if(width==Precision::Float64)block.out64=channels64;else block.out=channels;{audio::CallbackScope scope;session.process(block);}for(std::size_t i=0;i<static_cast<std::size_t>(count);++i){pcm[(first+i)*2]=width==Precision::Float64?wide[i]:narrow[i];pcm[(first+i)*2+1]=width==Precision::Float64?wide[256+i]:narrow[256+i];}}
    check(a==allocations.load() && f==frees.load() && io==audio::callbackFileIo.load() && session.clips()->readErrors()==0 && session.clips()->underrunSamples()==0,"coherent external/reference Session callbacks have zero allocation/free/file I/O and ready PCM");return pcm;
}
void coherent() {
    for(double rate:{44100.,48000.})for(auto width:{Precision::Float32,Precision::Float64}){
        Project project,reference;reference.store->db().exec("DELETE FROM tempo_map;INSERT INTO tempo_map(pos_ticks,bpm,curve) VALUES(0,60,0)");const auto original=digestProject(*project.store).text;const auto changes=sqlite3_total_changes(project.store->db().getHandle());
        Session session,expected;SessionSpec spec{2,rate,256};spec.precision=width;spec.externalTempoBpm=60;load(project,session,spec);spec.externalTempoBpm.reset();load(reference,expected,spec);
        check(session.model().tempo.size()==2 && session.model().tempo[0].bpm==120 && session.model().tempo[1].bpm==240,"external performance generation preserves the raw authored model tempo events");
        const auto frames=static_cast<std::size_t>(rate*3.1);const auto heard=render(project,session,rate,width,frames),wanted=render(reference,expected,rate,width,frames);check(heard==wanted && std::any_of(heard.begin(),heard.end(),[](double value){return value!=0;}),"actual Warp PCM and track automation match a native constant-tempo project at both widths/rates");
        const auto begin=static_cast<std::size_t>(rate);check(std::all_of(heard.begin(),heard.begin()+static_cast<std::ptrdiff_t>(begin*2),[](double value){return value==0;}) && std::all_of(heard.begin()+static_cast<std::ptrdiff_t>(static_cast<std::size_t>(rate*3)*2),heard.end(),[](double value){return value==0;}),"external Warp placement begins at one second and its two musical beats end at three seconds");
        const auto& recorder=*project.recorder;check(recorder.count==2 && recorder.heard[0].type==EventType::NoteOn && recorder.heard[0].sample==std::llround(rate*1.25) && recorder.heard[1].type==EventType::NoteOff && recorder.heard[1].sample==std::llround(rate*1.75),"actual native MIDI note boundaries follow the same externally prepared tempo");
        check(recorder.last.bpm==60 && recorder.last.timelineSample>static_cast<std::int64_t>(rate*3),"actual instrument TransportInfo follows external tempo even past the authored tempo change");
        check(digestProject(*project.store).text==original && sqlite3_total_changes(project.store->db().getHandle())==changes && project.store->db().execAndGet("SELECT count(*) FROM ops").getInt()==0,"external preparation and playback change no authored rows or journal entries");
        project.store->db().exec("UPDATE tempo_map SET bpm=333");check(session.refresh(*project.store),"project refresh retains the selected external performance clock");session.prepare(rate,256);session.transport().locate(0);expected.transport().locate(0);project.recorder->count=reference.recorder->count=0;
        check(render(project,session,rate,width,frames)==render(reference,expected,rate,width,frames),"refresh ignores authored tempo automation for every external playback consumer");
        const auto grantedRate=rate==44100?48000.:44100.;session.prepare(grantedRate,256);expected.prepare(grantedRate,256);session.transport().locate(0);expected.transport().locate(0);project.recorder->count=reference.recorder->count=0;
        check(render(project,session,grantedRate,width,static_cast<std::size_t>(grantedRate*3.1))==render(reference,expected,grantedRate,width,static_cast<std::size_t>(grantedRate*3.1)) && project.recorder->last.bpm==60,"actual driver rate re-preparation preserves coherent external audio and clock timing");
        check(project.recorder->count==2 && project.recorder->heard[0].sample==std::llround(grantedRate*1.25) && project.recorder->heard[1].sample==std::llround(grantedRate*1.75),"MIDI re-preparation uses the actual newly granted rate and the retained external tempo");rate=grantedRate;
        OpRequest loop;loop.opType="transport.setLoop";loop.payload={{"on",true},{"start",q},{"end",2*q}};check(OpJournal(*project.store).commit(loop).ok && OpJournal(*reference.store).commit(loop).ok && session.refresh(*project.store) && expected.refresh(*reference.store),"native saved musical loop publishes under the external performance tempo");session.transport().locate(0);expected.transport().locate(0);project.recorder->count=reference.recorder->count=0;const auto loopFrames=static_cast<std::size_t>(rate*2.25);
        check(render(project,session,rate,width,loopFrames)==render(reference,expected,rate,width,loopFrames) && session.transport().position()==std::llround(rate*1.25),"musical loop boundaries and repeated native playback use the coherent external clock");
    }
}
void refusal() {
    Project project;const auto original=digestProject(*project.store).text;const auto nan=std::numeric_limits<double>::quiet_NaN();for(double bpm:std::array<double,5>{0,19,1000,nan,std::numeric_limits<double>::infinity()}){Session candidate;SessionSpec spec;spec.externalTempoBpm=bpm;check(!candidate.load(*project.store,{},spec),"invalid external tempo refuses a native Session generation");spec.externalTempoBpm.reset();check(candidate.load(*project.store,project.loader(),spec),"invalid external choice does not poison later valid native loading");}
    Session offline;SessionSpec invalid;invalid.performanceState=false;invalid.externalTempoBpm=60;check(!offline.load(*project.store,{},invalid),"external performance tempo cannot accidentally alter an offline render generation");const auto prior=offline.error();invalid.performanceState=true;
    unsigned sql=0;sqlite3_trace_v2(project.store->db().getHandle(),SQLITE_TRACE_STMT,[](unsigned,void* context,void*,void*){if(audio::onAudioThread)++*static_cast<unsigned*>(context);return 0;},&sql);const auto a=allocations.load(),f=frees.load(),io=audio::callbackFileIo.load();bool accepted=false;{audio::CallbackScope scope;accepted=offline.load(*project.store,{},invalid);}sqlite3_trace_v2(project.store->db().getHandle(),0,nullptr,nullptr);
    check(!accepted && offline.error()==prior && sql==0 && a==allocations.load() && f==frees.load() && io==audio::callbackFileIo.load(),"native load refuses callback use before SQL/allocation/free/I/O or diagnostic mutation");check(digestProject(*project.store).text==original,"all rejected native generation choices preserve authored project state");
}
void frozen() {
    Project project;render::FreezeCaptureSpec spec;spec.tracks={1};spec.endSample=96000;spec.session={2,48000,256};auto capture=render::captureFreezeAudio(*project.store,spec,project.loader());check(capture.ready(),"actual native freeze capture prepares an absolute-time cache");if(!capture.ready())return;
    auto prepared=freeze::prepareFreeze(*project.store,capture,0,100);const bool committed=prepared.ready() && freeze::commitFreeze(*project.store,prepared,0).ok;check(committed,"actual native freeze installs its fingerprinted audio cache");if(!committed)return;
    const auto original=digestProject(*project.store).text;Session candidate;SessionSpec external;external.externalTempoBpm=60;check(!candidate.load(*project.store,project.loader(),external) && candidate.error().find("unfreeze")!=std::string::npos && digestProject(*project.store).text==original,"external tempo refuses existing absolute-time frozen material without silently changing its timing or source");
}
}
int main(){try{coherent();refusal();frozen();}catch(const std::exception& e){++failures;std::printf("EXCEPTION: %s\n",e.what());}std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
