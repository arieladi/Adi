// SPDX-License-Identifier: GPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/engine/session.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <numbers>
#include <thread>
#include <cstdlib>
#include <new>
using namespace adi;
using namespace adi::engine;
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
        StoreError error{};store=Store::create(dir.path()/"session.adi",error);
        if(!store)throw std::runtime_error("create project");
        store->db().exec("INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(1,'audio','Audio',0),(9,'master','Master',1)");
        media.resize(static_cast<std::size_t>(frames)*channels);
        for(std::uint32_t i=0;i<frames;++i)for(std::uint16_t c=0;c<channels;++c)
            media[static_cast<std::size_t>(i)*channels+c]=hz>0?static_cast<float>(0.5*std::sin(2*std::numbers::pi*hz*i/rate)):static_cast<float>((i%997)+1)/1024.0f*(c==0?1.0f:0.5f);
        audio::WavWriter w(dir.path()/"audio.wav",rate,channels,audio::WavFormat::Float32);w.write(media.data(),frames);w.close();
        store->db().exec("INSERT INTO media_files(id,hash_blake3,rel_path,format) VALUES(1,'test','audio.wav','wav')");
    }
    void add(int id,std::int64_t start,std::int64_t length,std::int64_t source=0,std::int64_t sourceLength=80000) {
        SQLite::Statement q(store->db(),"INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns) VALUES(?,1,'audio',1,?,?)");
        q.bind(1,id);q.bind(2,ns(start));q.bind(3,ns(length));q.exec();
        SQLite::Statement a(store->db(),"INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(?,1,?,?)");
        a.bind(1,id);a.bind(2,source);a.bind(3,sourceLength);a.exec();
    }
    void load(int block) { session.graph().setFadeFrames(0);if(!session.load(*store,{}, {2,48000.0,block}))throw std::runtime_error(session.error());session.transport().play(); }
    std::vector<float> render(int frames,bool prime=true) {
        if(prime && !session.clips()->prime())throw std::runtime_error("prime timeout");
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
}
int main(int argc,char** argv) {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    const std::string only=argc>1?argv[1]:"all";
    try {
        if(only=="all" || only=="placement")placement();
        if(only=="all" || only=="properties")properties();
        if(only=="all" || only=="seeks")seeksAndStalls();
        if(only=="all" || only=="conversion")conversion();
        if(only=="all" || only=="refresh")refreshAndVariableBlocks();
        if(only=="all" || only=="unsupported")unsupported();
        if(only=="all" || only=="segments")segmentsAndCrop();
    }
    catch(const std::exception& e){check(false,e.what());}
    check(callbackAllocations.load()==0,"callback allocates nothing");
    check(audio::callbackFileIo.load()==0,"no file I/O on the audio thread");
    std::printf("%s -- %d checks, %d failure(s)\n",failures?"FAIL":"PASS",checks,failures);
    return failures?1:0;
}
