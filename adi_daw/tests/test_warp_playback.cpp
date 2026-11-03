// SPDX-License-Identifier: AGPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/audio/warp_map.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/engine/session.hpp"
#include "adi/engine/warp_projection.hpp"
#include "adi/store.hpp"
#include "adi/store_rows.hpp"
#include "adi/textproj.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <numbers>
namespace {std::atomic<unsigned> callbackAllocations{0};}
void* operator new(std::size_t n){if(adi::audio::onAudioThread)++callbackAllocations;if(void* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return operator new(n);}
void operator delete(void* p)noexcept{std::free(p);}
void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}
void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
namespace {
using namespace adi;using namespace adi::engine;constexpr std::int64_t q=textproj::kPPQ;
int checks=0,failures=0;
void check(bool ok,const char* text){++checks;if(!ok){++failures;std::printf("FAIL %s\n",text);}}
struct Fixture {
    test::TempDirectory temporary{"warp_playback","project"};std::unique_ptr<Store> store;Session session;
    explicit Fixture(std::uint32_t sourceRate=48000) {
        StoreError error{};store=Store::createEmpty(temporary.path()/"song.adi",error);if(!store)throw std::runtime_error(toString(error));
        const auto frames=sourceRate/4;std::vector<float> input(static_cast<std::size_t>(frames)*2);
        for(std::uint32_t frame=0;frame<frames;++frame)for(std::size_t channel=0;channel<2;++channel)
            input[static_cast<std::size_t>(frame)*2+channel]=static_cast<float>(0.25*std::sin(2*std::numbers::pi*(channel==0?440:660)*frame/sourceRate));
        {audio::WavWriter writer(temporary.path()/"source.wav",sourceRate,2,audio::WavFormat::Float32);writer.write(input.data(),frames);writer.close();}
        auto& db=store->db();
        db.exec("INSERT INTO tracks(id,kind,name) VALUES(1,'audio','Warp'),(9,'master','Main');"
            "INSERT INTO media_files(id,hash_blake3,rel_path,format) VALUES(1,'warp-source','source.wav','wav');"
            "INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks) VALUES(7,1,'audio',0,5765760)");
        {SQLite::Statement row(db,"INSERT INTO audio_clips(clip_id,media_id,src_len_frames,warp_enabled,warp_mode) VALUES(7,1,?,1,'complex')");row.bind(1,frames);row.exec();}
        const auto markers=audio::writeWarpMap(std::vector<WarpMarker>{{0,0},{frames,q}},frames);
        {SQLite::Statement row(db,"UPDATE audio_clips SET warp_markers=? WHERE clip_id=7");row.bind(1,markers.bytes.data(),static_cast<int>(markers.bytes.size()));row.exec();}
        db.exec("INSERT INTO clip_playback_details(clip_id,content_offset_ns,source_end_ns,loop_begin_ns,loop_end_ns) VALUES(7,0,250000000,0,250000000)");
    }
    void load(Precision width) {
        SessionSpec spec{2,48000,1024};spec.precision=width;session.graph().setFadeFrames(0);
        if(!session.load(*store,{},spec))throw std::runtime_error(session.error());
        session.transport().locate(0);session.transport().play();
    }
    std::vector<double> render(std::size_t frames,Precision width) {
        std::vector<double> result(frames*2);std::array<float,2048> narrow{};std::array<double,2048> wide{};
        float* out[]={narrow.data(),narrow.data()+1024};double* out64[]={wide.data(),wide.data()+1024};
        for(std::size_t first=0;first<frames;) {
            const auto count=static_cast<std::int32_t>(std::min<std::size_t>(1024,frames-first));
            if(!session.clips()->prime(std::chrono::seconds(5)))throw std::runtime_error("prime warped project");
            AudioIo io;io.frames=count;io.numOut=2;if(width==Precision::Float64)io.out64=out64;else io.out=out;
            {audio::CallbackScope scope;session.process(io);}
            for(std::size_t channel=0;channel<2;++channel)for(std::int32_t frame=0;frame<count;++frame)
                result[(first+static_cast<std::size_t>(frame))*2+channel]=width==Precision::Float64?wide[channel*1024+static_cast<std::size_t>(frame)]:narrow[channel*1024+static_cast<std::size_t>(frame)];
            first+=static_cast<std::size_t>(count);
        }
        return result;
    }
};
void modesAndLoops() {
    for(const auto width:{Precision::Float32,Precision::Float64})for(const auto* mode:{"beats","tones","texture","re-pitch","complex","complexPro"}) {
        Fixture f;
        {SQLite::Statement row(f.store->db(),"UPDATE audio_clips SET warp_mode=?");row.bind(1,mode);row.exec();}
        f.load(width);const auto output=f.render(26000,width);
        if(!f.session.problems().empty())std::printf("mode %s: %s\n",mode,f.session.problems().front().c_str());
        check(f.session.problems().empty(),"actual Session loads every stored warp mode at either native precision");
        double power=0;bool finite=true;
        for(std::size_t frame=18000;frame<22000;++frame){power+=output[frame*2]*output[frame*2];finite=finite && std::isfinite(output[frame*2]);}
        check(finite && power>1,"heard warped PCM continues beyond the old unwarped source-end pin");
        check(std::all_of(output.begin()+48000,output.end(),[](double value){return value==0;}) &&
              f.session.clips()->underrunSamples()==0,"warped clip ends at its projected timeline boundary without underruns");
        f.store->db().exec("UPDATE clips SET loop_enabled=1,loop_start_ticks=0,loop_len_ticks=5765760,length_ticks=11531520");
        check(f.session.refresh(*f.store),"refresh a source musical loop on a warped clip");
        f.session.transport().locate(0);const auto looped=f.render(48000,width);
        check(std::equal(looped.begin(),looped.begin()+48000,looped.begin()+48000),"warped loop uses musical bounds rather than old physical loop pins");
    }
}
void sourceRatesAndTempoBoundary() {
    for(const auto width:{Precision::Float32,Precision::Float64}) {
        Fixture f(44100);f.load(width);const auto output=f.render(24000,width);
        double power=0;for(const auto value:output)power+=value*value;
        check(f.session.problems().empty() && power>1 && f.session.clips()->underrunSamples()==0,"actual project warp converts source-rate audio before native output playback");
        // Tempo changes halfway through a bar must split a warp interval.
        f.store->db().exec("DELETE FROM tempo_map;INSERT INTO tempo_map(pos_ticks,bpm) VALUES(0,120),(2882880,60)");
        check(f.session.refresh(*f.store),"tempo edit rebuilds a prepared warp generation");
        f.session.transport().locate(0);const auto changed=f.render(38000,width);power=0;
        for(std::size_t frame=29000;frame<33000;++frame)power+=changed[frame*2]*changed[frame*2];
        check(power>1 && std::all_of(changed.begin()+72000,changed.end(),[](double value){return value==0;}),"project tempo inside a warp interval changes heard duration from 24000 to 36000 frames");
    }
    TempoMap tempo;tempo.events={{0,120,0},{q/2,60,0}};
    const std::vector<WarpMarker> map={{0,0},{12000,q}};
    const auto anchors=projectWarpAnchors(map,tempo,0,48000);
    check(anchors==std::vector<audio::WarpAnchor>{{0,0},{6000,12000},{12000,36000}},"tempo projection inserts its exact interior anchor instead of flattening a bar");
    check(warpSourceSamples(tempo,-q,q,48000)==24000 && warpSourceSamples(tempo,-q,2*q,48000)==60000,"trimmed source clock extends the initial tempo before the project origin");
}
void modelAndRefusals() {
    Fixture f;const auto model=rows::readModel(*f.store);
    check(model.audioClips.size()==1 && model.audioClips[0].warpMarkers &&
          audio::readWarpMap(*model.audioClips[0].warpMarkers,12000),"coherent native rows carry the stored marker bytes into playback");
    f.store->db().exec("UPDATE audio_clips SET warp_markers=X''");auto empty=rows::readModel(*f.store);
    check(empty.audioClips[0].warpMarkers && empty.audioClips[0].warpMarkers->empty(),"empty warp BLOB stays distinct from SQL NULL");
    f.load(Precision::Float32);
    const auto silent=f.render(1024,Precision::Float32);
    check(!f.session.problems().empty() && std::all_of(silent.begin(),silent.end(),[](double value){return value==0;}),"malformed stored warp data reports a named problem");
}
void trimmedMusicalOffset() {
    for(const auto width:{Precision::Float32,Precision::Float64}) {
        Fixture f;f.load(width);const auto complete=f.render(24000,width);
        // A prior unwarped physical pin must not override the new source
        // musical offset. Both clocks intentionally disagree in this row.
        f.store->db().exec("UPDATE clips SET content_offset_ticks=1441440,length_ticks=4324320");
        check(f.session.refresh(*f.store),"refresh a trimmed warped source with an old physical offset pin");
        f.session.transport().locate(0);const auto trimmed=f.render(20000,width);
        check(f.session.problems().empty() && std::equal(trimmed.begin(),trimmed.begin()+36000,complete.begin()+12000) &&
              std::all_of(trimmed.begin()+36000,trimmed.end(),[](double value){return value==0;}),
              "heard trimmed Warp audio uses the musical source offset and ignores its old nanosecond pin");
    }
}
}
int main(){try{modesAndLoops();sourceRatesAndTempoBoundary();modelAndRefusals();trimmedMusicalOffset();}catch(const std::exception& error){check(false,error.what());}check(callbackAllocations.load()==0 && audio::callbackFileIo.load()==0,"actual warped audio callback allocates nothing and performs no file I/O");std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
