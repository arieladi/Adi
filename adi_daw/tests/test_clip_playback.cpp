// SPDX-License-Identifier: GPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/engine/session.hpp"
#include "adi/engine/clip_fade.hpp"
#include "adi/clip_commands.hpp"
#include "adi/history.hpp"
#include "adi/audio/decode.hpp"
#include "adi/audio/progressive_decode.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/audio/warp_map.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/store.hpp"
#include "adi/ops.hpp"
#include "adi/textproj.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <numbers>
#include <thread>
#include <cstdlib>
#include <filesystem>
#include <new>
using namespace adi;
using namespace adi::engine;
namespace fs = std::filesystem;
namespace { std::atomic<std::uint32_t> callbackAllocations{0}; }
void* operator new(std::size_t n) {
    if(audio::onAudioThread) callbackAllocations.fetch_add(1,std::memory_order_relaxed);
    if(void* p=std::malloc(n?n:1))return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) {return operator new(n);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
// Nothrow allocations use the same counted malloc/free pair under sanitizers.
void* operator new(std::size_t n,const std::nothrow_t&) noexcept {
    try {return ::operator new(n);} catch (...) {return nullptr;}
}
void* operator new[](std::size_t n,const std::nothrow_t&) noexcept {
    try {return ::operator new[](n);} catch (...) {return nullptr;}
}
void operator delete(void* p,const std::nothrow_t&) noexcept {::operator delete(p);}
void operator delete[](void* p,const std::nothrow_t&) noexcept {::operator delete[](p);}
namespace {
int checks=0, failures=0;
void check(bool ok,const char* name) { ++checks;if(!ok){++failures;std::printf("FAIL %s\n",name);} }
std::int64_t ns(std::int64_t frames) { return static_cast<std::int64_t>(std::llround(static_cast<double>(frames)*1e9/48000)); }
struct Fixture {
    test::TempDirectory dir{"clip_playback","project"};
    std::unique_ptr<Store> store;
    Session session;
    std::vector<float> media;
    explicit Fixture(std::uint32_t rate=48000,std::uint16_t channels=2,std::uint32_t frames=100000,double hz=0) {
        StoreError error{};store=Store::createEmpty(dir.path()/"session.adi",error);
        if(!store)throw std::runtime_error("create project");
        store->db().exec("INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(1,'audio','Audio',0),(9,'master','Master',1)");
        media.resize(static_cast<std::size_t>(frames)*channels);
        for(std::uint32_t i=0;i<frames;++i)for(std::uint16_t c=0;c<channels;++c)
            media[static_cast<std::size_t>(i)*channels+c]=hz>0?static_cast<float>(0.5*std::sin(2*std::numbers::pi*hz*i/rate)):(hz<0?0.5f:static_cast<float>((i%997)+1)/1024.0f*(c==0?1.0f:0.5f));
        audio::WavWriter w(dir.path()/"audio.wav",rate,channels,audio::WavFormat::Float32);w.write(media.data(),frames);w.close();
        store->db().exec("INSERT INTO media_files(id,hash_blake3,rel_path,format) VALUES(1,'test','audio.wav','wav')");
    }
    void add(int id,std::int64_t start,std::int64_t length,std::int64_t source=0,std::int64_t sourceLength=80000) {
        SQLite::Statement q(store->db(),"INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns) VALUES(?,1,'audio',1,?,?)");
        q.bind(1,id);q.bind(2,ns(start));q.bind(3,ns(length));q.exec();
        SQLite::Statement a(store->db(),"INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(?,1,?,?)");
        a.bind(1,id);a.bind(2,source);a.bind(3,sourceLength);a.exec();
    }
    void load(int block, double rate = 48000.0) { session.graph().setFadeFrames(0);if(!session.load(*store,{}, {2,rate,block}))throw std::runtime_error(session.error());session.transport().play(); }
    std::vector<float> render(int frames,bool prime=true,std::chrono::milliseconds timeout=std::chrono::seconds(300)) {
        if(prime && !session.clips()->prime(timeout))throw std::runtime_error("prime timeout");
        std::vector<float> result(static_cast<std::size_t>(frames)*2);
        float* out[2]={result.data(),result.data()+frames};
        session.process({out,nullptr,2,0,frames,0});return result;
    }
};
void placement() {
    for(const int block:{64,128,256,512,1024,2048,4096,257}) {
        Fixture f;f.add(1,73,21001,11,25000);f.load(block);
        bool exact=true;int at=0;
        while(at<24000) {
            const int n=std::min(block,24000-at);const auto out=f.render(n);
            for(int c=0;c<2;++c)for(int i=0;i<n;++i) {
                const int t=at+i;const float want=t>=73 && t<21074?f.media[static_cast<std::size_t>(t-73+11)*2+static_cast<std::size_t>(c)]:0;
                exact=exact && out[static_cast<std::size_t>(c*n+i)]==want;
            }
            at+=n;
        }
        check(exact,"sample-exact placement and source window across block sizes/pages");
        check(f.session.transport().position()==24000,"transport advances once per callback");
        check(f.session.clips()->underrunSamples()==0,"primed rendering has no underruns");
    }
    Fixture f;f.add(1,0,2000,0,3000);
    f.store->db().exec("UPDATE clips SET time_base=0,pos_ns=NULL,length_ns=NULL,pos_ticks=576576,length_ticks=1153152; DELETE FROM tempo_map; INSERT INTO tempo_map(pos_ticks,bpm,curve) VALUES(0,120,0),(576576,60,0)");
    f.load(4096);f.session.transport().locate(2300);const auto out=f.render(4096);
    check(out[99]==0 && out[100]==f.media[0] && out[101]==f.media[2],"tick placement integrates tempo map");
}
void properties() {
    Fixture f;f.add(1,0,500,10,1000);f.add(2,0,500,10,1000);f.add(3,0,500,10,1000);
    f.store->db().exec("UPDATE clips SET gain_db=-6.020599913279624 WHERE id=1; UPDATE clips SET muted=1 WHERE id=3; UPDATE audio_clips SET channel_mode=2 WHERE clip_id=2");
    f.load(512);auto o=f.render(512);
    check(std::abs(o[30]-(f.media[80]*0.5f+f.media[81]))<1e-6f,"overlapping clips gain mute right mode");
    check(std::abs(o[512+30]-(f.media[81]*0.5f+f.media[81]))<1e-6f,"stereo channel routing");
    check(o[500]==0 && o[499]!=0,"clip length ends exactly without default fade");
    Fixture fades;fades.add(1,0,800,0,1000);
    // At 120 BPM 240.24 ticks per sample: 100 samples = 24024 ticks.
    fades.store->db().exec("UPDATE clips SET fade_in_ticks=24024,fade_out_ticks=24024");fades.load(1024);o=fades.render(1024);
    check(o[0]==0 && std::abs(o[50]-fades.media[100]*0.5f)<1e-6f,"linear fade in is per sample");
    check(o[799]==0 && std::abs(o[749]-fades.media[1498]*0.5f)<1e-6f,"linear fade out reaches zero on final sample");
    Fixture loop;loop.add(1,0,2000,7,1000);
    loop.store->db().exec("UPDATE clips SET loop_enabled=1,loop_start_ticks=24024,loop_len_ticks=48048,content_offset_ticks=12012");loop.load(2048);o=loop.render(2048);
    bool exact=true;
    for(int i=0;i<2000;++i){auto s=i+50;if(s>=100)s=100+(s-100)%200;exact=exact && o[static_cast<std::size_t>(i)]==loop.media[static_cast<std::size_t>(s+7)*2];}
    check(exact,"clip loop and content offset use source window");
    for(int mode:{1,3}) {
        Fixture m;m.add(1,0,100,0,100);m.store->db().exec("UPDATE audio_clips SET channel_mode="+std::to_string(mode));m.load(128);o=m.render(128);
        const float want=mode==1?m.media[20]:(m.media[20]+m.media[21])*0.5f;
        check(o[10]==want && o[138]==want,"left and mono-sum channel modes");
    }
}
void editedFades() {
    for(const auto width:{Precision::Float32,Precision::Float64}) {
        Fixture f(48000,2,4000,-1);f.add(1,0,800,0,1000);
        f.store->db().exec("UPDATE clips SET fade_in_ticks=24024,fade_in_curve=2");
        OpRequest edit;edit.opType="clip.setFades";
        edit.payload={{"id",1},{"in",{{"tension",0.0}}},{"out",{{"ticks",24024},{"curve",5},{"tension",0.3}}}};
        check(OpJournal(*f.store).commit(edit).ok,"fade gesture commits both sides as one step");
        SessionSpec spec;spec.channels=2;spec.sampleRate=48000;spec.maxFrames=1024;spec.precision=width;
        f.session.graph().setFadeFrames(0);
        check(f.session.load(*f.store,{},spec),"load edited fade at native width");
        f.session.transport().play();
        std::array<std::array<float,1024>,2> narrow{};std::array<std::array<double,1024>,2> wide{};
        float* output[]={narrow[0].data(),narrow[1].data()};double* output64[]={wide[0].data(),wide[1].data()};
        auto render=[&] {
            f.session.transport().locate(0);
            if(!f.session.clips()->prime(std::chrono::seconds(2)))throw std::runtime_error("edited fade prime");
            AudioIo io;io.numOut=2;io.frames=1024;
            if(width==Precision::Float64)io.out64=output64;else io.out=output;
            audio::CallbackScope callback;f.session.process(io);
        };
        auto sample=[&](std::size_t i){return width==Precision::Float64?wide[0][i]:static_cast<double>(narrow[0][i]);};
        render();
        check(std::abs(sample(50)-0.25)<1e-7,"explicit zero tension changes old square to continuous linear exponent");
        const double shaped=0.5*clipFadeShape(0.5,5,0.3);
        check(std::abs(sample(749)-shaped)<(width==Precision::Float64?1e-12:1e-7),"rendered fade-out matches the window's shared continuous evaluator");
        check(sample(0)==0 && sample(799)==0 && sample(800)==0,"edited fade endpoints and placement are exact");
        check(History(*f.store).undo().ok && f.session.refresh(*f.store),"undo rebuilds legacy envelope and absent shape row");
        render();
        check(std::abs(sample(50)-0.125)<1e-7 && sample(799)==0.5,"undo restores old square arithmetic and no automatic end fade");
    }
}
void splitSourceClocks() {
    for(const auto sourceRate:{44100u,48000u})for(const bool loop:{false,true}) {
        Fixture f(sourceRate,2,4000);f.add(1,0,800,0,1000);
        f.store->db().exec("DELETE FROM tempo_map; INSERT INTO tempo_map(pos_ticks,bpm,curve) VALUES(0,120,0),(24024,60,0)");
        f.store->db().exec("UPDATE clips SET content_offset_ticks=24024");
        if(loop)f.store->db().exec("UPDATE clips SET loop_enabled=1,loop_start_ticks=24024,loop_len_ticks=48048");
        f.load(1024);const auto original=f.render(1024);
        const auto prepared=clip_edit::prepareSplit(*f.store,1,2,{1,ns(300)},48000);
        check(prepared.ok,"split captures unwarped source clocks across a tempo change");
        if(!prepared.ok){std::printf("split prepare: %s\\n",prepared.error.c_str());continue;}
        const auto committed=clip_edit::commitPrepared(*f.store,prepared);
        check(committed.ok,"prepared split commits as one journal transaction");
        if(!committed.ok){std::printf("split commit: %s\\n",committed.error.c_str());continue;}
        check(f.session.refresh(*f.store),"split refreshes playing session");
        f.session.transport().locate(0);const auto split=f.render(1024);
        check(split==original,"split preserves exact resampled PCM and loop phase on both sides");
        check(History(*f.store).undo().ok && f.session.refresh(*f.store),"split undo rejoins original clip and removes source-clock rows");
        f.session.transport().locate(0);const auto restored=f.render(1024);
        check(restored==original,"split undo restores original rendered bytes");
        check(History(*f.store).redo().ok && f.session.refresh(*f.store),"split redo restores captured rows and clocks");
        f.session.transport().locate(0);const auto redone=f.render(1024);
        check(redone==original,"split redo remains sample exact");
    }
}
void splitInsideFades() {
    for(const auto width:{Precision::Float32,Precision::Float64})for(const std::int64_t cut:{50,750}) {
        Fixture f(48000,2,4000,-1);f.add(1,0,800,0,1000);
        f.store->db().exec("UPDATE clips SET fade_in_ticks=24024,fade_out_ticks=24024,fade_in_curve=2,fade_out_curve=5");
        f.store->db().exec("INSERT INTO clip_playback_details(clip_id,in_tension,out_tension) VALUES(1,0.4,-0.3)");
        SessionSpec spec{2,48000,1024};spec.precision=width;f.session.graph().setFadeFrames(0);
        check(f.session.load(*f.store,{},spec),"load original continuous fades at native width");
        std::vector<double> before(2048),after(2048);
        auto render=[&](std::vector<double>& result) {
            f.session.transport().locate(0);f.session.transport().play();
            if(!f.session.clips()->prime(std::chrono::seconds(2)))throw std::runtime_error("split fade prime");
            std::array<std::array<float,1024>,2> narrow{};std::array<std::array<double,1024>,2> wide{};
            float* out[]={narrow[0].data(),narrow[1].data()};double* out64[]={wide[0].data(),wide[1].data()};
            AudioIo io;io.numOut=2;io.frames=1024;
            if(width==Precision::Float64)io.out64=out64;else io.out=out;
            {audio::CallbackScope callback;f.session.process(io);}
            for(std::size_t c=0;c<2;++c)for(std::size_t i=0;i<1024;++i)
                result[c*1024+i]=width==Precision::Float64?wide[c][i]:static_cast<double>(narrow[c][i]);
        };
        render(before);
        const auto prepared=clip_edit::prepareSplit(*f.store,1,2,{1,ns(cut)},48000);
        check(prepared.ok && clip_edit::commitPrepared(*f.store,prepared).ok && f.session.refresh(*f.store),"split inside an active fade commits and rebuilds");
        render(after);check(before==after,"split preserves exact original fade gain on both sides of its cut");
        OpRequest reset;reset.opType="clip.setFades";reset.payload={{"id",2},{"in",{{"ticks",0}}},{"out",{{"ticks",0}}}};
        check(OpJournal(*f.store).commit(reset).ok && History(*f.store).undo().ok && f.session.refresh(*f.store),"fade handle edit and undo restore retained envelope clocks");
        render(after);check(before==after,"undo of fade handle restores the split envelope exactly");
    }
}

std::vector<double> renderSplitPcm(Fixture& f,Precision width,std::int32_t frames) {
    if(frames<=0)return {};
    f.session.transport().locate(0);f.session.transport().play();
    const auto total=static_cast<std::size_t>(frames);
    std::vector<double> result(total*2);
    std::array<float,2048> narrow{};std::array<double,2048> wide{};
    float* out[]={narrow.data(),narrow.data()+1024};double* out64[]={wide.data(),wide.data()+1024};
    for(std::int32_t first=0;first<frames;) {
        const auto count=std::min<std::int32_t>(1024,frames-first);
        if(!f.session.clips() || !f.session.clips()->prime(std::chrono::seconds(5)))return {};
        AudioIo io;io.numOut=2;io.frames=count;
        if(width==Precision::Float64)io.out64=out64;else io.out=out;
        {audio::CallbackScope callback;f.session.process(io);}
        for(std::size_t c=0;c<2;++c)for(std::int32_t i=0;i<count;++i) {
            const auto offset=static_cast<std::size_t>(i);
            result[c*total+static_cast<std::size_t>(first)+offset]=width==Precision::Float64?
                wide[c*1024+offset]:static_cast<double>(narrow[c*1024+offset]);
        }
        first+=count;
    }
    return result;
}
bool twoSplitAudioRoundTrip(Fixture& f,Precision width,std::int32_t frames,
                           std::int32_t clipBegin,std::int32_t clipEnd,
                           std::int32_t firstCut,std::int32_t lastCut,
                           clip_edit::TimePoint begin,clip_edit::TimePoint end) {
    SessionSpec spec{2,48000,1024};spec.precision=width;f.session.graph().setFadeFrames(0);
    const bool loaded=f.session.load(*f.store,{},spec);
    check(loaded,"load native audio for one two-point split");if(!loaded)return false;
    const auto original=renderSplitPcm(f,width,frames);
    const auto total=static_cast<std::size_t>(frames);
    bool baseline=original.size()==total*2;
    if(baseline)for(std::size_t c=0;c<2;++c) {
        for(const auto cut:{firstCut,lastCut})for(const auto offset:{-16,0}) {
            const auto at=static_cast<std::size_t>(cut+offset);
            baseline=baseline && std::any_of(original.begin()+static_cast<std::ptrdiff_t>(c*total+at),
                original.begin()+static_cast<std::ptrdiff_t>(c*total+at+16),[](double value){return std::abs(value)>1e-9;});
        }
        for(std::int32_t i=0;i<frames;++i)
            if(i<clipBegin || i>=clipEnd)baseline=baseline && original[c*total+static_cast<std::size_t>(i)]==0;
    }
    check(baseline && f.session.clips()->underrunSamples()==0,
          "two-point baseline is audible on both sides of both cuts and silent outside its placement");
    if(!baseline)return false;
    const auto prepared=clip_edit::prepareSplit(*f.store,1,2,3,begin,end,48000);
    check(prepared.ok,"prepare both audio cuts from one original image");if(!prepared.ok)return false;
    const bool committed=clip_edit::commitPrepared(*f.store,prepared).ok && f.session.refresh(*f.store);
    check(committed,"commit and refresh both audio cuts as one edit");if(!committed)return false;
    check(f.store->db().execAndGet("SELECT count(*) FROM clips WHERE id IN (1,2,3)").getInt()==3,
          "two-point audio split creates both caller-identified pieces");
    const auto split=renderSplitPcm(f,width,frames);
    check(split.size()==original.size() && split==original,
          "two cuts preserve every native PCM sample, source phase and original fade envelope");
    const bool undone=History(*f.store).undo().ok && f.session.refresh(*f.store);
    check(undone && f.store->db().execAndGet("SELECT count(*) FROM clips").getInt()==1,
          "one audio undo removes both created pieces");if(!undone)return false;
    const auto restored=renderSplitPcm(f,width,frames);
    check(restored.size()==original.size() && restored==original,"one audio undo restores all original PCM exactly");
    const bool redone=History(*f.store).redo().ok && f.session.refresh(*f.store);
    check(redone && f.store->db().execAndGet("SELECT count(*) FROM clips WHERE id IN (1,2,3)").getInt()==3,
          "one audio redo restores both captured pieces");if(!redone)return false;
    const auto replayed=renderSplitPcm(f,width,frames);
    check(replayed.size()==original.size() && replayed==original,"one audio redo restores all split PCM exactly");
    return true;
}
void twoPointSplitSourceClocks() {
    for(const auto sourceRate:{44100u,48000u})for(const auto width:{Precision::Float32,Precision::Float64})for(const bool loop:{false,true}) {
        Fixture f(sourceRate,2,4000);f.add(1,73,800,11,2000);
        f.store->db().exec("DELETE FROM tempo_map; INSERT INTO tempo_map(pos_ticks,bpm,curve) VALUES(0,120,0),(72072,60,0);"
            "UPDATE clips SET content_offset_ticks=12012,fade_in_ticks=96096,fade_out_ticks=24024,fade_in_curve=2,fade_out_curve=5;"
            "INSERT INTO clip_playback_details(clip_id,in_tension,out_tension) VALUES(1,0.4,-0.3)");
        if(loop)f.store->db().exec("UPDATE clips SET loop_enabled=1,loop_start_ticks=24024,loop_len_ticks=48048");
        (void)twoSplitAudioRoundTrip(f,width,1024,73,873,130,816,{1,ns(130)},{1,ns(816)});
    }
}
void twoPointSplitWarpFades() {
    constexpr auto quarter=textproj::kPPQ;
    for(const auto width:{Precision::Float32,Precision::Float64}) {
        Fixture f(48000,2,12000,440);f.add(1,0,48000,0,12000);
        auto& db=f.store->db();
        db.exec("DELETE FROM tempo_map; INSERT INTO tempo_map(pos_ticks,bpm,curve) VALUES(0,120,0);"
            "UPDATE clips SET time_base=0,pos_ticks=0,length_ticks=11531520,pos_ns=NULL,length_ns=NULL,"
            "loop_enabled=1,loop_start_ticks=0,loop_len_ticks=5765760,fade_in_ticks=8648640,fade_out_ticks=8648640,fade_in_curve=2,fade_out_curve=4;"
            "UPDATE audio_clips SET warp_enabled=1,warp_mode='complex';"
            "INSERT INTO clip_playback_details(clip_id,in_tension,out_tension) VALUES(1,0.35,-0.4)");
        const std::array<WarpMarker,2> markers{{{0,0},{12000,quarter}}};
        const auto map=audio::writeWarpMap(markers,12000);
        check(static_cast<bool>(map),"prepare the original two-point Warp marker map");if(!map)continue;
        SQLite::Statement setMap(db,"UPDATE audio_clips SET warp_markers=? WHERE clip_id=1");
        setMap.bind(1,map.bytes.data(),static_cast<int>(map.bytes.size()));setMap.exec();
        const auto originalMap=db.execAndGet("SELECT hex(warp_markers) FROM audio_clips WHERE clip_id=1").getString();
        if(!twoSplitAudioRoundTrip(f,width,50000,0,48000,12000,18000,{0,quarter/2},{0,3*quarter/4}))continue;
        check(db.execAndGet("SELECT count(*) FROM clip_warp_fade_domains WHERE clip_id IN (1,2,3) AND "
            "in_begin_ticks=0 AND in_end_ticks=8648640 AND out_begin_ticks=2882880 AND out_end_ticks=11531520").getInt()==3,
            "both Warp cuts retain the first image's complete original fade domains");
        bool sameMap=true;
        SQLite::Statement maps(db,"SELECT hex(warp_markers) FROM audio_clips ORDER BY clip_id");
        while(maps.executeStep())sameMap=sameMap && maps.getColumn(0).getString()==originalMap;
        check(sameMap,"two-point Warp split and one redo retain the original marker bytes");
    }
}

void rangeSurvivorPcm(std::uint32_t sourceRate,Precision width,bool loop,bool wholeTime) {
    Fixture f(sourceRate,2,4000);f.add(1,100,800,11,2000);
    auto& db=f.store->db();
    db.exec("DELETE FROM tempo_map; INSERT INTO tempo_map(pos_ticks,bpm,curve) VALUES(0,120,0),(24024,60,0);"
        "UPDATE clips SET content_offset_ticks=24024,fade_in_ticks=96096,fade_out_ticks=24024,fade_in_curve=2,fade_out_curve=5;"
        "INSERT INTO clip_playback_details(clip_id,in_tension,out_tension) VALUES(1,0.4,-0.3)");
    if(loop)db.exec("UPDATE clips SET time_base=0,pos_ticks=24024,length_ticks=96096,pos_ns=NULL,length_ns=NULL,"
        "loop_enabled=1,loop_start_ticks=24024,loop_len_ticks=48048");
    SessionSpec spec{2,48000,2048};spec.precision=width;f.session.graph().setFadeFrames(0);
    const bool loaded=f.session.load(*f.store,{},spec);
    check(loaded,"load a faded source whose only range survivor will be its tail");if(!loaded)return;
    auto render=[&](std::int32_t frames) {
        f.session.transport().locate(0);f.session.transport().play();
        if(!f.session.clips()->prime(std::chrono::seconds(2)))return std::vector<double>{};
        const auto count=static_cast<std::size_t>(frames);
        std::vector<double> result(count*2);std::vector<float> narrow(count*2);
        float* out[]={narrow.data(),narrow.data()+frames};double* out64[]={result.data(),result.data()+frames};
        AudioIo io;io.numOut=2;io.frames=frames;if(width==Precision::Float64)io.out64=out64;else io.out=out;
        {audio::CallbackScope callback;f.session.process(io);}
        if(width==Precision::Float32)std::copy(narrow.begin(),narrow.end(),result.begin());
        return result;
    };
    const auto original=render(1200);
    bool audible=original.size()==2400;
    if(audible)for(std::size_t channel=0;channel<2;++channel) {
        const auto base=channel*1200;
        audible=audible && original[base+299]!=0 && original[base+300]!=0 && original[base+750]!=0;
        for(std::size_t frame=0;frame<1200;++frame)
            if(frame<100 || frame>=900)audible=audible && original[base+frame]==0;
    }
    check(audible && f.session.clips()->underrunSamples()==0,
          "sole-tail baseline has real audio at the retained edge and original fade-out, with exact placement silence");
    if(!audible)return;
    const clip_edit::RangeSelection selection{{1},{1,ns(50)},{1,ns(300)}};
    clip_edit::PreparedEdit edit;
    if(wholeTime)edit=clip_edit::prepareDeleteTime(*f.store,12012,48048,48000);
    else if(loop) {
        auto cut=clip_edit::prepareCutRange(*f.store,selection,48000);
        check(cut.copy.ok && cut.copy.images.size()==1,"Cut captures only the removed material before preserving its original-ID tail");
        if(!cut.copy.ok || cut.copy.images.size()!=1)return;
        edit=std::move(cut.edit);
    } else edit=clip_edit::prepareDeleteRange(*f.store,selection,48000);
    check(edit.ok,"prepare a sole-tail material or whole-Time removal");if(!edit.ok)return;
    const bool committed=clip_edit::commitPrepared(*f.store,edit).ok && f.session.refresh(*f.store);
    check(committed,"commit and refresh one sole-tail removal");if(!committed)return;
    auto sameIdentity=[&] {
        return db.execAndGet("SELECT count(*) FROM clips").getInt()==1 &&
            db.execAndGet("SELECT count(*) FROM clips WHERE id=1").getInt()==1 &&
            db.execAndGet("SELECT count(*) FROM audio_clips WHERE clip_id=1 AND media_id=1 AND src_start_frames=11 AND src_len_frames=2000").getInt()==1;
    };
    check(sameIdentity(),"the sole audible tail keeps its original clip and audio-source row identity");
    const std::int32_t frames=wholeTime?950:1200;
    std::vector<double> expected(static_cast<std::size_t>(frames)*2);
    for(std::size_t channel=0;channel<2;++channel)for(std::int32_t frame=0;frame<frames;++frame) {
        const auto source=wholeTime && frame>=50?frame+250:frame;
        expected[channel*static_cast<std::size_t>(frames)+static_cast<std::size_t>(frame)]=
            !wholeTime && frame>=50 && frame<300?0:original[channel*1200+static_cast<std::size_t>(source)];
    }
    const auto after=render(frames);
    check(after.size()==expected.size() && after==expected,wholeTime?
        "Delete Time shifts the original suffix by exactly the removed duration, preserving source phase, loop and fades":
        "material Delete/Cut silences only the selected interval and keeps the original suffix at its exact timeline samples");
    const bool undone=History(*f.store).undo().ok && f.session.refresh(*f.store);
    check(undone && sameIdentity(),"one sole-tail undo restores the original clip and source identities");if(!undone)return;
    const auto restored=render(1200);
    check(restored.size()==original.size() && restored==original,"one sole-tail undo restores every original native PCM sample");
    const bool redone=History(*f.store).redo().ok && f.session.refresh(*f.store);
    check(redone && sameIdentity(),"one sole-tail redo retains the captured original identities");if(!redone)return;
    const auto replayed=render(frames);
    check(replayed.size()==expected.size() && replayed==expected,"one sole-tail redo restores the exact retained PCM and original envelope");
}
void rangeSurvivorSourceClocks() {
    for(const auto sourceRate:{44100u,48000u})for(const auto width:{Precision::Float32,Precision::Float64}) {
        // Delete a linear prefix; Cut a musical, looped prefix. Their shared
        // trim receives both clocks without duplicating the full audio matrix.
        for(const bool loop:{false,true})rangeSurvivorPcm(sourceRate,width,loop,false);
        rangeSurvivorPcm(sourceRate,width,true,true); // whole-Time removal also relocates the surviving tail
    }
}
void silenceSourceClocks() {
    for(const auto sourceRate:{44100u,48000u}) {
        Fixture f(sourceRate,2,4000);f.add(1,0,800,0,1000);
        f.store->db().exec("DELETE FROM tempo_map; INSERT INTO tempo_map(pos_ticks,bpm) VALUES(0,120),(24024,60)");
        f.store->db().exec("UPDATE clips SET content_offset_ticks=24024,fade_in_ticks=96096,fade_out_ticks=24024");
        f.load(2048);const auto original=f.render(1200);
        const auto prepared=clip_edit::prepareInsertSilence(*f.store,48048,12012,48000);
        check(prepared.ok && clip_edit::commitPrepared(*f.store,prepared).ok && f.session.refresh(*f.store),"Insert Silence splits and moves a faded resampled source");
        f.session.transport().locate(0);const auto shifted=f.render(1300);
        bool exact=true;
        for(std::size_t c=0;c<2;++c)for(std::size_t i=0;i<1300;++i) {
            const float expected=i>=300 && i<400?0:original[c*1200+(i<300?i:i-100)];
            exact=exact && shifted[c*1300+i]==expected;
        }
        check(exact,"Insert Silence preserves source samples and fade envelopes exactly around the silent gap");
        check(History(*f.store).undo().ok && f.session.refresh(*f.store),"undo Insert Silence refreshes original source");
        f.session.transport().locate(0);check(f.render(1200)==original,"Insert Silence undo restores original PCM bytes");
    }
}

void deleteTimeSourceClocks() {
    for(const auto sourceRate:{44100u,48000u})for(const auto width:{Precision::Float32,Precision::Float64})for(const bool loop:{false,true}) {
        Fixture f(sourceRate,2,4000);f.add(1,0,800,0,2000);
        f.store->db().exec("DELETE FROM tempo_map; INSERT INTO tempo_map(pos_ticks,bpm) VALUES(0,120),(24024,60)");
        f.store->db().exec("UPDATE clips SET content_offset_ticks=24024,fade_in_ticks=96096,fade_out_ticks=24024");
        if(loop)f.store->db().exec("UPDATE clips SET loop_enabled=1,loop_start_ticks=24024,loop_len_ticks=48048");
        SessionSpec spec{2,48000,2048};spec.precision=width;f.session.graph().setFadeFrames(0);
        check(f.session.load(*f.store,{},spec),"load Delete Time source at native precision");
        auto render=[&](int frames) {
            f.session.transport().locate(0);f.session.transport().play();
            if(!f.session.clips()->prime(std::chrono::seconds(2)))throw std::runtime_error("Delete Time prime");
            std::vector<double> result(static_cast<std::size_t>(frames)*2);
            std::vector<float> narrow(static_cast<std::size_t>(frames)*2);
            float* out[]={narrow.data(),narrow.data()+frames};double* out64[]={result.data(),result.data()+frames};
            AudioIo io;io.numOut=2;io.frames=frames;
            if(width==Precision::Float64)io.out64=out64;else io.out=out;
            {audio::CallbackScope callback;f.session.process(io);}
            if(width==Precision::Float32)std::copy(narrow.begin(),narrow.end(),result.begin());
            return result;
        };
        const auto original=render(1200);
        const auto prepared=clip_edit::prepareDeleteTime(*f.store,48048,72072,48000);
        check(prepared.ok && clip_edit::commitPrepared(*f.store,prepared).ok && f.session.refresh(*f.store),"Delete Time trims a resampled source and retained fade domains");
        const auto after=render(1000);bool exact=true;
        for(std::size_t channel=0;channel<2;++channel)for(std::size_t position=0;position<1000;++position)
            exact=exact && after[channel*1000+position]==original[channel*1200+(position<300?position:position+200)];
        check(exact,"heard samples equal the original with only the selected time removed, both precisions and looping sources");
        check(History(*f.store).undo().ok && f.session.refresh(*f.store) && render(1200)==original,"Delete Time undo restores the exact rendered PCM");
        check(History(*f.store).redo().ok && f.session.refresh(*f.store) && render(1000)==after,"Delete Time redo restores the exact captured source clocks");
    }
}

void timeClipboardSourceClocks() {
    for(const auto sourceRate:{44100u,48000u})for(const auto width:{Precision::Float32,Precision::Float64})for(const bool loop:{false,true}) {
        Fixture f(sourceRate,2,4000);f.add(1,0,800,0,2000);f.add(2,1100,200,2000,500);
        f.store->db().exec("DELETE FROM tempo_map; INSERT INTO tempo_map(pos_ticks,bpm) VALUES(0,120),(24024,60),(96096,240)");
        f.store->db().exec("UPDATE clips SET fade_in_ticks=48048,fade_out_ticks=12012 WHERE id=1");
        if(loop)f.store->db().exec("UPDATE clips SET loop_enabled=1,loop_start_ticks=24024,loop_len_ticks=48048 WHERE id=1");
        SessionSpec spec{2,48000,2048};spec.precision=width;f.session.graph().setFadeFrames(0);
        check(f.session.load(*f.store,{},spec),"load whole-Time clipboard sources at native precision");
        auto render=[&](int frames) {
            f.session.transport().locate(0);f.session.transport().play();
            if(!f.session.clips()->prime(std::chrono::seconds(2)))throw std::runtime_error("Time clipboard prime");
            std::vector<double> result(static_cast<std::size_t>(frames)*2);std::vector<float> narrow(static_cast<std::size_t>(frames)*2);
            float* out[]={narrow.data(),narrow.data()+frames};double* out64[]={result.data(),result.data()+frames};
            AudioIo io;io.numOut=2;io.frames=frames;if(width==Precision::Float64)io.out64=out64;else io.out=out;
            {audio::CallbackScope callback;f.session.process(io);}
            if(width==Precision::Float32)std::copy(narrow.begin(),narrow.end(),result.begin());
            return result;
        };
        const auto original=render(1600);const auto copy=clip_edit::copyTime(*f.store,24024,72072,48000);
        check(copy.ok && copy.nanos==8333334,"copy physical duration at source tempo");
        const auto paste=clip_edit::preparePasteTime(*f.store,copy,144144,48000);
        const auto committed=clip_edit::commitPrepared(*f.store,paste);
        const bool refreshed=committed.ok && f.session.refresh(*f.store);
        if(!paste.ok)std::printf("Time clipboard prepare: %s\n",paste.error.c_str());
        if(!committed.ok)std::printf("Time clipboard commit: %s\n",committed.error.c_str());
        if(committed.ok && !refreshed)std::printf("Time clipboard refresh: %s\n",f.session.error().c_str());
        check(paste.ok && committed.ok && refreshed,"paste source tempo, faded material and global context as one step");
        const auto after=render(2000);bool exact=true;
        for(std::size_t channel=0;channel<2;++channel)for(std::size_t position=0;position<2000;++position) {
            const auto source=position<800?position:position<1200?position-800+100:position-400;
            exact=exact && after[channel*2000+position]==original[channel*1600+source];
        }
        check(exact,"heard Paste Time uses 400 copied frames rather than 100 target-tempo frames, with exact faded/resampled/looped PCM");
        check(History(*f.store).undo().ok && f.session.refresh(*f.store) && render(1600)==original,"Time paste undo restores original heard bytes");
        check(History(*f.store).redo().ok && f.session.refresh(*f.store) && render(2000)==after,"Time paste redo restores captured source clocks and map context");
    }
}

void sourceMarkers() {
    auto set = [](Store& store, const char* name, Payload payload) {
        OpRequest op; op.opType=name;op.payload=std::move(payload);
        if (!OpJournal(store).commit(op).ok) throw std::runtime_error("clip property op failed");
    };
    Fixture plain;plain.add(1,0,500,7,1000);
    set(*plain.store,"clip.setStartMarker",{{"id",1},{"ticks",12012}}); // source sample 50
    set(*plain.store,"clip.setEndMarker",{{"id",1},{"ticks",48048}}); // source sample 200
    plain.load(512);const auto out=plain.render(512);
    check(out[0]==plain.media[114] && out[149]==plain.media[412] && out[150]==0 && out[499]==0,
          "clip Start/End ops gate audio at exact source samples without moving placement");
    check(plain.session.setPrecision(Precision::Float64),"source markers switch to native double graph");
    plain.session.transport().locate(0);const auto wide=plain.render(512);
    check(wide[0]==plain.media[114] && wide[149]==plain.media[412] && wide[150]==0,
          "source End also gates a native double graph at the exact sample");
    Fixture loop;loop.add(1,0,500,7,1000);
    set(*loop.store,"clip.setStartMarker",{{"id",1},{"ticks",12012}});
    set(*loop.store,"clip.setEndMarker",{{"id",1},{"ticks",48048}});
    set(*loop.store,"clip.setLoop",{{"id",1},{"on",true},{"start",24024},{"length",48048}});
    loop.load(512);const auto repeated=loop.render(512);
    check(repeated[150]==loop.media[414] && repeated[250]==loop.media[214] && repeated[500]==0,
          "Loop uses its own source window while the End marker is inactive");
}
void seeksAndStalls() {
    Fixture f;f.add(1,0,90000,0,90000);f.load(4096);(void)f.render(4096);
    f.session.transport().locate(70003);
    const bool refilled=f.session.clips()->prime(std::chrono::seconds(2));
    check(refilled,"locate requests a refill before rendering");
    if(!refilled)return;
    auto o=f.render(4096);
    check(o[0]==f.media[140006] && o[4095]==f.media[148196],"locate refills distant uncached page");
    f.session.transport().loop(17003,72003);f.session.transport().locate(71900);o=f.render(4096);
    check(o[102]==f.media[144004] && o[103]==f.media[34006] && f.session.transport().position()==20996,"transport loop wraps sample-exactly mid block");
    f.session.transport().play(false);const auto pos=f.session.transport().position();o=f.render(64);
    check(std::all_of(o.begin(),o.end(),[](float x){return x==0;}) && f.session.transport().position()==pos,"stopped transport is silent and stationary");
    f.session.clips()->stallForTest(true);std::this_thread::sleep_for(std::chrono::milliseconds(20));
    f.session.transport().loop(0,0,false);f.session.transport().locate(55000);f.session.transport().play();
    const auto before=f.session.clips()->underrunSamples();const auto start=std::chrono::steady_clock::now();o=f.render(64,false);
    const auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    check(std::all_of(o.begin(),o.end(),[](float x){return x==0;}) && f.session.clips()->underrunSamples()==before+64,"stalled stream emits silence and counts missing samples");
    check(ms<50,"underrun callback never waits for worker");
    f.session.clips()->stallForTest(false);o=f.render(64);
    check(o[0]==f.media[110128],"stream resumes after stall at current playhead");
}
void conversion() {
    for(const auto rate:{44100u,96000u}) {
        Fixture f(rate,1,rate*2,997);f.add(1,0,96000,0,rate*2);f.load(4096);
        double squared=0,peak=0;std::size_t count=0;
        for(int at=0;at<90000;at+=4096) {
            auto o=f.render(4096);
            for(int i=0;i<4096;++i) {
                if(at+i<512)continue;
                const double want=0.5*std::sin(2*std::numbers::pi*997*(at+i)/48000);
                const double error=static_cast<double>(o[static_cast<std::size_t>(i)])-want;
                squared+=error*error;peak=std::max(peak,std::abs(error));++count;
            }
        }
        const double rms=std::sqrt(squared/static_cast<double>(count));
        std::printf("SRC %u->48000 997Hz RMS %.9g peak %.9g SNR %.3f dB\n",rate,rms,peak,20*std::log10((0.5/std::sqrt(2.0))/rms));
        check(rms<1e-5 && peak<1e-4,"sine conversion accurate including page seams");
        f.session.transport().locate(37001);const auto o=f.render(64);
        check(std::abs(o[0]-0.5*std::sin(2*std::numbers::pi*997*37001/48000))<1e-4,"resampler seek preserves absolute phase");
    }
    Fixture alias(96000,1,192000,30000);alias.add(1,0,96000,0,192000);alias.load(4096);alias.session.transport().locate(10000);auto o=alias.render(4096);
    double energy=0;for(int i=0;i<4096;++i)energy+=static_cast<double>(o[static_cast<std::size_t>(i)])*o[static_cast<std::size_t>(i)];
    const double rms=std::sqrt(energy/4096);std::printf("SRC 96000->48000 30kHz rejection %.3f dB\n",20*std::log10((0.5/std::sqrt(2.0))/rms));
    check(rms<1e-5,"downsampling rejects above-Nyquist sine");
}
void refreshAndVariableBlocks() {
    Fixture f;f.add(1,31,70000,0,70000);f.load(4096);
    bool exact=true;int at=0;
    for(int round=0;round<5;++round)for(const int n:{64,137,1024,4096}) {
        const auto out=f.render(n);
        for(int i=0;i<n;++i) {
            const float want=at+i>=31?f.media[static_cast<std::size_t>(at+i-31)*2]:0;
            exact=exact && out[static_cast<std::size_t>(i)]==want;
        }
        at+=n;
    }
    check(exact,"variable callback sizes preserve exact positions");
    f.store->db().exec("UPDATE clips SET gain_db=-6.020599913279624");
    check(f.session.refresh(*f.store),"refresh publishes new clip sources");
    auto out=f.render(64);
    check(std::abs(out[0]-f.media[static_cast<std::size_t>(at-31)*2]*0.5f)<1e-6f,"refreshed clip gain reaches published graph");
    f.session.tick(1000); // reclaim old graph and its worker on this thread
    f.session.prepare(48000,128);out=f.render(128);
    check(std::abs(out[0]-f.media[static_cast<std::size_t>(at+64-31)*2]*0.5f)<1e-6f,"block reprepare preserves playhead and clip source");
    // Producer (message thread) publishes while driver consumes. Do not touch
    // Transport or prime from this thread while that driver is running.
    std::atomic<bool> stop{false};
    std::thread driver([&]{
        std::array<float,256> samples{};float* ptrs[]={samples.data(),samples.data()+128};
        while(!stop.load()) f.session.process({ptrs,nullptr,2,0,128,0});
    });
    bool refreshed=true;
    for(int i=0;i<4;++i){refreshed=f.session.refresh(*f.store)&&refreshed;f.session.tick(2000+i);}
    stop.store(true);driver.join();f.session.tick(3000);
    check(refreshed,"concurrent publication and graph reclamation keep sources alive");
}
void fractionalEndpoint() {
    Fixture f;f.add(1,0,1000,0,1000);
    f.store->db().exec("UPDATE clips SET pos_ns=10000,length_ns=10000");
    f.load(64);const auto out=f.render(64);
    check(out[0]==f.media[0] && out[1]==0,"absolute clip endpoints round once");
}
void relativeProject() {
    Fixture f;f.add(1,0,1000,0,1000);f.store.reset();
    const auto previous=std::filesystem::current_path();
    std::filesystem::current_path(f.dir.path());
    StoreError error{};f.store=Store::open("session.adi",error);
    std::filesystem::current_path(previous);
    if(!f.store)throw std::runtime_error("reopen relative project");
    f.load(64);const auto out=f.render(64);
    check(out[0]==f.media[0],"media stays relative to opened project after working directory changes");
}
void readAhead() {
    Fixture f;f.add(1,12000,4000,0,4000);f.load(64);
    if(!f.session.clips()->prime())throw std::runtime_error("ahead prime");
    f.session.clips()->stallForTest(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    f.session.transport().locate(12000);
    const auto out=f.render(64,false);
    check(out[0]==f.media[0],"fixed horizon prefetches future clip at 64 samples");
}
void segmentsAndCrop() {
    Fixture f;f.add(1,63,12000,9,20000);f.load(4096);
    if(!f.session.clips()->prime())throw std::runtime_error("segment prime");
    auto sources=f.session.clips()->sourcesFor(1);
    std::array<float,8192> output{};float* ptrs[]={output.data(),output.data()+4096};
    for(int offset=0;offset<4096;offset+=128) {
        NodeIo io;io.out=ptrs;io.channels=2;io.frames=128;io.blockOffset=offset;io.sampleRate=48000;
        audio::CallbackScope callback;sources.at(0)->process(io);
    }
    bool exact=true;
    for(int i=0;i<4096;++i)exact=exact && output[static_cast<std::size_t>(i)]==(i>=63?f.media[static_cast<std::size_t>(i-63+9)*2]:0);
    check(exact,"sub-block offsets address the original block buffers and clock");
    Fixture crop(44100,1,88200,997);crop.add(1,0,48000,123,44100);crop.load(4096);crop.session.transport().locate(8100);
    const auto out=crop.render(4096);double peak=0;
    for(int i=0;i<4096;++i)peak=std::max(peak,std::abs(static_cast<double>(out[static_cast<std::size_t>(i)])-0.5*std::sin(2*std::numbers::pi*997*(123.0/44100+(8100.0+i)/48000))));
    check(peak<1e-4,"converted cropped window keeps phase across page boundary");
    Transport t;t.loop(10,110);t.locate(100);t.play();
    check(t.sampleAt(250)==50,"multiple loop wraps inside one callback");
    t.locate(std::numeric_limits<std::int64_t>::max());
    check(t.sampleAt(std::numeric_limits<std::int64_t>::max())>=10 && t.sampleAt(std::numeric_limits<std::int64_t>::max())<110,"large transport offsets avoid signed overflow");
}
// Tests set ADI_HOME, so a decoded clip's cache is a temporary folder and not
// the user's (ADR-0149). The one place this suite writes an environment variable.
void setHome(const fs::path& p) {
#if defined(_WIN32)
    _wputenv_s(L"ADI_HOME", p.wstring().c_str());
#else
    setenv("ADI_HOME", p.string().c_str(), 1);
#endif
}
void decodedPins() {
    // ADR-0156: a decoded clip pins its cache copy while a generation reads
    // it, and the pin goes back with the generation. Every edit rebuilds, and
    // each rebuild used to pin the copy again, for good.
    auto pins=[]{int n=0;for(const auto& e:audio::defaultDecodeCache().entries())n+=e.pins;return n;};
    {
        Fixture f;
        fs::copy_file(fs::path(ADI_DECODE_FIXTURES)/"tone16.flac",f.dir.path()/"tone.flac");
        f.store->db().exec("INSERT INTO media_files(id,hash_blake3,rel_path,format) VALUES(2,'flac','tone.flac','flac');"
                           "INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns) VALUES(1,1,'audio',1,0,100000000);"
                           "INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(1,2,0,4800)");
        f.load(512);
        // Loading no longer waits for complete compressed conversion. The
        // private prefix has no LRU entry; assert this pin after publication.
        auto decoded=audio::defaultProgressiveDecoder().open(f.dir.path()/"tone.flac");
        const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(10);
        while(decoded->snapshot().status==audio::ProgressiveStatus::Pending && std::chrono::steady_clock::now()<until)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        check(decoded->snapshot().status==audio::ProgressiveStatus::Complete && pins()==1,"a completed decoded clip pins its cache copy while it plays");
        for(int i=0;i<5;++i) {
            f.store->db().exec("UPDATE clips SET gain_db=gain_db-1");
            if(!f.session.refresh(*f.store))throw std::runtime_error(f.session.error());
            f.render(512);f.session.graph().collect();
        }
        check(pins()==1,"five rebuilds later it is still one pin, not six");
    }
    check(pins()==0,"closing the session gives every pin back");
}
void unsupported() {
    for(int kind=0;kind<3;++kind) {
        Fixture f;f.add(1,0,500,0,1000);
        if(kind==0)f.store->db().exec("UPDATE audio_clips SET warp_enabled=1");
        if(kind==1)f.store->db().exec("UPDATE audio_clips SET reverse=1");
        if(kind==2){std::ofstream file(f.dir.path()/"audio.wav",std::ios::binary|std::ios::trunc);file<<"not WAV";}
        f.load(512);const auto o=f.render(512);
        check(!f.session.problems().empty() && std::all_of(o.begin(),o.end(),[](float x){return x==0;}),"unsupported clip is silent with named session problem");
    }
}

void highRates() {
    auto nsHigh = [](std::int64_t frames, double rate) {
        return static_cast<std::int64_t>(std::llround(static_cast<double>(frames)*1e9/rate));
    };

    try { Fixture f; f.load(1024, 44099.0); check(false, "session below 44.1kHz must reject"); } catch(const std::exception& e) { check(std::string(e.what()).find("44.1-768")!=std::string::npos, "session floor rejection names the valid range"); }
    try { Fixture f; f.load(1024, 768000.0); check(true, "768k boundary acceptance"); } catch(const std::exception&) { check(false, "768k boundary acceptance failed"); }

        // Cascaded huge upsample: 1Hz source constant 4096 frames, away from the zero-extended edges
        {
            Fixture f(1, 1, 4096, -1.0); // constant 0.5
            SQLite::Statement q(f.store->db(),"INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns) VALUES(1,1,'audio',1,0,?)");
            q.bind(1, nsHigh(768000LL * 4096, 768000.0)); q.exec(); // length = 4096 seconds
            SQLite::Statement a(f.store->db(),"INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(1,1,0,4096)");
            a.exec();

            f.load(4096, 768000.0);
            f.session.transport().locate(2000LL * 768000); // interior, beyond the sinc support
            const auto out = f.render(4096,true,std::chrono::seconds(300));
            double peak = 0;
            for(int i = 0; i < 4096; ++i) {
                peak = std::max(peak, std::abs(static_cast<double>(out[static_cast<std::size_t>(i)]) - 0.5));
            }
            std::printf("SRC cascaded 1->768000 DC peak %.9g\n",peak);
            check(peak < 1e-4, "cascaded huge upsample 1Hz->768kHz constant");
        }

        // Cascaded large upsample: 5Hz sine 100Hz converted to 48k quality interior
        {
            Fixture f(100, 1, 100 * 60, 5.0); // 100Hz rate, 60s length, 5Hz sine
            SQLite::Statement q(f.store->db(),"INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns) VALUES(1,1,'audio',1,0,?)");
            q.bind(1, nsHigh(48000LL * 60, 48000.0)); q.exec();
            SQLite::Statement a(f.store->db(),"INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(1,1,0,6000)");
            a.exec();

            f.load(4096, 48000.0);

            // interior seek, e.g. 10 seconds
            f.session.transport().locate(10 * 48000);
            const auto out = f.render(4096,true,std::chrono::seconds(300));
            double peak = 0, squared = 0;
            for(int i = 0; i < 4096; ++i) {
                const double t = (10 * 48000.0 + i) / 48000.0;
                const double want = 0.5 * std::sin(2 * std::numbers::pi * 5.0 * t);
                const double error = static_cast<double>(out[static_cast<std::size_t>(i)]) - want;
                squared += error * error;
                peak = std::max(peak, std::abs(error));
            }
            const double rms = std::sqrt(squared / 4096);
            std::printf("SRC cascaded 100->48000 5Hz RMS %.9g peak %.9g\n", rms, peak);
            check(rms < 1e-3 && peak < 1e-2, "cascaded 100Hz->48kHz sine quality interior");
        }

    for(const auto rate : {192000u, 384000u}) {
        // exact placement
        {
            Fixture f(rate, 2, rate);
            SQLite::Statement q(f.store->db(),"INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns) VALUES(1,1,'audio',1,?,?)");
            q.bind(1, nsHigh(73, rate)); q.bind(2, nsHigh(21001, rate)); q.exec();
            SQLite::Statement a(f.store->db(),"INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(1,1,11,25000)");
            a.exec();

            f.load(256, rate);

            bool exact = true; int at = 0;
            while(at < 24000) {
                const int n = std::min(256, 24000 - at);
                const auto out = f.render(n);
                for(int c=0; c<2; ++c) for(int i=0; i<n; ++i) {
                    const int t = at + i;
                    const float want = t >= 73 && t < 21074 ? f.media[static_cast<std::size_t>(t-73+11)*2+static_cast<std::size_t>(c)] : 0;
                    exact = exact && out[static_cast<std::size_t>(c*n+i)] == want;
                }
                at += n;
            }
            check(exact, "high rate exact placement");
        }

        // conversion 22.05/44.1/96k sine accuracy
        for (const auto sourceRate : {22050u, 44100u, 96000u}) {
            Fixture f(sourceRate, 1, sourceRate * 2, 997);
            SQLite::Statement q(f.store->db(),"INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns) VALUES(1,1,'audio',1,0,?)");
            q.bind(1, nsHigh(rate * 2, rate)); q.exec();
            SQLite::Statement a(f.store->db(),"INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(1,1,0,?)");
            a.bind(1, sourceRate * 2); a.exec();

            f.load(4096, rate);

            double squared = 0, peak = 0; std::size_t count = 0;
            for(int at = 0; at < 90000; at += 4096) {
                auto out = f.render(4096);
                for(int i = 0; i < 4096; ++i) {
                    if(at + i < 512) continue;
                    const double want = 0.5 * std::sin(2 * std::numbers::pi * 997 * (at + i) / static_cast<double>(rate));
                    const double error = static_cast<double>(out[static_cast<std::size_t>(i)]) - want;
                    squared += error * error; peak = std::max(peak, std::abs(error)); ++count;
                }
            }
            const double rms = std::sqrt(squared / static_cast<double>(count));
            std::printf("SRC %u->%u 997Hz RMS %.9g peak %.9g SNR %.3f dB\n", sourceRate, rate, rms, peak, 20 * std::log10((0.5 / std::sqrt(2.0)) / rms));
            check(rms < 1e-4 && peak < 1e-3, "high rate sine conversion accurate");
        }

        // duration-based readahead stalled worker (locate)
        {
            Fixture f(rate, 2, rate * 2);
            SQLite::Statement q(f.store->db(),"INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns) VALUES(1,1,'audio',1,0,?)");
            q.bind(1, nsHigh(rate * 2, rate)); q.exec();
            SQLite::Statement a(f.store->db(),"INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(1,1,0,?)");
            a.bind(1, rate * 2); a.exec();

            f.load(64, rate);

            std::int64_t horizonSamples = 16384LL * rate / 48000LL;
            f.session.transport().locate(0);
            f.session.transport().play();
            if(!f.session.clips()->prime()) throw std::runtime_error("prime timeout");
            f.session.clips()->stallForTest(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));

            // Primed at zero, then no worker progress. The last promised
            // sample must already be cached, not newly requested by prime().
            f.session.transport().locate(horizonSamples - 64);
            const auto out = f.render(64, false);
            bool exact=true;for(std::size_t i=0;i<64;++i)
                exact=exact && out[i]==f.media[(static_cast<std::size_t>(horizonSamples-64)+i)*2];
            check(exact && f.session.clips()->underrunSamples()==0, "high rate duration-based readahead prefetch");
            f.session.clips()->stallForTest(false);
            f.session.transport().locate(static_cast<std::int64_t>(rate));
            const auto located=f.render(64);
            check(located[0]==f.media[static_cast<std::size_t>(rate)*2],"high rate cold locate refills");
            f.session.transport().loop(static_cast<std::int64_t>(rate),static_cast<std::int64_t>(rate)+100);
            f.session.transport().locate(static_cast<std::int64_t>(rate)+80);
            const auto wrapped=f.render(64);
            bool wrap=true;for(std::size_t i=0;i<64;++i)
                wrap=wrap && wrapped[i]==f.media[(static_cast<std::size_t>(rate)+(80+i)%100)*2];
            check(wrap,"high rate transport loop wraps mid-block");
        }

        // loop
        {
            Fixture f(rate, 2, rate * 2);
            SQLite::Statement q(f.store->db(),"INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns,loop_enabled,loop_start_ticks,loop_len_ticks,content_offset_ticks) VALUES(1,1,'audio',1,0,?,1,24024,48048,12012)");
            q.bind(1, nsHigh(rate * 2, rate)); q.exec();
            SQLite::Statement a(f.store->db(),"INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(1,1,0,?)");
            a.bind(1, rate * 2); a.exec();

            f.load(2048, rate);

            auto out = f.render(2048);
            bool exact = true;
            std::int64_t loopBegin = 100LL * rate / 48000LL;
            std::int64_t loopLen = 200LL * rate / 48000LL;
            std::int64_t offset = 50LL * rate / 48000LL;
            for(int i = 0; i < 2000; ++i) {
                auto s = i + offset;
                if(s >= loopBegin) s = loopBegin + (s - loopBegin) % loopLen;
                exact = exact && out[static_cast<std::size_t>(i)] == f.media[static_cast<std::size_t>(s) * 2];
            }
            check(exact, "high rate clip loop and content offset");
        }
    }
}


}
int main(int argc,char** argv) {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    const std::string only=argc>1?argv[1]:"all";
    test::TempDirectory home{"clip_playback","home"};setHome(home.path());
    try {
        if(only=="all" || only=="placement")placement();
        if(only=="all" || only=="properties")properties();
        if(only=="all" || only=="fades")editedFades();
        if(only=="all" || only=="split"){splitSourceClocks();splitInsideFades();twoPointSplitSourceClocks();twoPointSplitWarpFades();rangeSurvivorSourceClocks();silenceSourceClocks();deleteTimeSourceClocks();timeClipboardSourceClocks();}
        if(only=="all" || only=="seeks")seeksAndStalls();
        if(only=="all" || only=="conversion")conversion();
        if(only=="all" || only=="markers")sourceMarkers();
        if(only=="all" || only=="refresh")refreshAndVariableBlocks();
        if(only=="all" || only=="unsupported")unsupported();
        if(only=="all" || only=="segments")segmentsAndCrop();
        if(only=="all" || only=="ahead")readAhead();
        if(only=="all" || only=="relative")relativeProject();
        if(only=="all" || only=="endpoint")fractionalEndpoint();
        if(only=="all" || only=="highRates")highRates();
        if(only=="all" || only=="pins")decodedPins();
    }
    catch(const std::exception& e){check(false,e.what());}
    check(callbackAllocations.load()==0,"callback allocates nothing");
    check(audio::callbackFileIo.load()==0,"no file I/O on the audio thread");
    std::printf("%s -- %d checks, %d failure(s)\n",failures?"FAIL":"PASS",checks,failures);
    return failures?1:0;
}
