// SPDX-License-Identifier: AGPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/audio/warp_map.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/clip_commands.hpp"
#include "adi/edit_plan.hpp"
#include "adi/engine/session.hpp"
#include "adi/history.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace adi;
using namespace adi::engine;
namespace placement = adi::edit_plan;
int checks=0,failures=0;
void check(bool ok,const char* label) {
    ++checks;if(!ok){++failures;std::printf("FAIL %s\n",label);}
}
std::int64_t nanos(std::int64_t frames) {
    return static_cast<std::int64_t>(std::llround(static_cast<double>(frames)*1e9/48000.0));
}
// This fixture's only junction is frame 100: 120 BPM becomes 60 BPM.
// Chosen frame positions are multiples of 100, so both musical segments are
// integral without deriving the oracle from production's TimeMap.
std::int64_t ticks(std::int64_t frames,bool junction) {
    return junction && frames>100?24024+(frames-100)*12012/100:frames*24024/100;
}
struct Fixture {
    test::TempDirectory directory{"edit_plan_playback","project"};
    std::unique_ptr<Store> store;
    Session session;
    Precision width;
    bool musical,junction;
    explicit Fixture(std::uint32_t sourceRate,Precision precision,bool tickClock,
                     bool tempoJunction,bool constant=false)
        :width(precision),musical(tickClock),junction(tempoJunction) {
        StoreError error{};store=Store::createEmpty(directory.path()/"session.adi",error);
        if(!store)throw std::runtime_error("create edit placement playback project");
        store->db().exec("INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(1,'audio','Material',0),(9,'master','Main',1);");
        if(junction)store->db().exec("DELETE FROM tempo_map; INSERT INTO tempo_map(pos_ticks,bpm,curve) VALUES(0,120,0),(24024,60,0)");
        constexpr std::uint32_t count=8000;
        std::vector<float> media(static_cast<std::size_t>(count)*2);
        for(std::uint32_t frame=0;frame<count;++frame)for(std::size_t channel=0;channel<2;++channel)
            media[static_cast<std::size_t>(frame)*2+channel]=constant?0.5f:
                static_cast<float>((frame%997)+1)/2048.0f*(channel==0?1.0f:0.5f);
        {audio::WavWriter writer(directory.path()/"source.wav",sourceRate,2,audio::WavFormat::Float32);
            writer.write(media.data(),count);writer.close();}
        store->db().exec("INSERT INTO media_files(id,hash_blake3,rel_path,format) VALUES(1,'placement-source','source.wav','wav')");
    }
    std::int64_t coordinate(std::int64_t frame) const {return musical?ticks(frame,junction):nanos(frame);}
    clip_edit::TimePoint point(std::int64_t frame) const {return {musical?0:1,coordinate(frame)};}
    placement::EditScope scope(std::int64_t end=3200) const {
        const std::array<std::int64_t,1> tracks={1};
        return placement::materialScope(tracks,musical?placement::PlacementClock::Ticks:
            placement::PlacementClock::Nanoseconds,{0,coordinate(end)});
    }
    void add(std::int64_t id,std::int64_t begin,std::int64_t end,std::int64_t source=11) {
        SQLite::Statement root(store->db(),musical?
            "INSERT INTO clips(id,track_id,kind,time_base,pos_ticks,length_ticks) VALUES(?,1,'audio',0,?,?)":
            "INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns) VALUES(?,1,'audio',1,?,?)");
        root.bind(1,id);root.bind(2,coordinate(begin));root.bind(3,coordinate(end)-coordinate(begin));root.exec();
        SQLite::Statement audioRow(store->db(),"INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(?,1,?,6000)");
        audioRow.bind(1,id);audioRow.bind(2,source);audioRow.exec();
    }
    void warp(std::int64_t id) {
        // Re-pitch's 1:1 source map at 120 BPM is deterministic; a tempo
        // junction still projects a different rate in the latter segment.
        const auto markers=audio::writeWarpMap(std::array<WarpMarker,2>{WarpMarker{0,0},WarpMarker{6000,1441440}},6000);
        if(!markers)throw std::runtime_error("encode placement Warp fixture");
        SQLite::Statement put(store->db(),"UPDATE audio_clips SET warp_enabled=1,warp_mode='re-pitch',warp_markers=? WHERE clip_id=?");
        put.bind(1,markers.bytes.data(),static_cast<int>(markers.bytes.size()));put.bind(2,id);put.exec();
    }
    bool load() {
        SessionSpec spec{2,48000,257};spec.precision=width;session.graph().setFadeFrames(0);
        const bool ok=session.load(*store,{},spec);check(ok,"load actual placement Session at its native precision");
        if(!ok)std::printf("Session: %s\n",session.error().c_str());
        return ok;
    }
    std::vector<double> render(std::size_t frames) {
        session.transport().locate(0);session.transport().play();
        std::vector<double> result(frames*2);
        std::array<float,514> narrow{};std::array<double,514> wide{};
        float* out[]={narrow.data(),narrow.data()+257};double* out64[]={wide.data(),wide.data()+257};
        constexpr std::array<std::int32_t,4> blocks={64,137,257,101};std::size_t turn=0;
        for(std::size_t first=0;first<frames;) {
            const auto count=static_cast<std::int32_t>(std::min<std::size_t>(static_cast<std::size_t>(blocks[turn++%blocks.size()]),frames-first));
            if(!session.clips()->prime(std::chrono::seconds(5)))throw std::runtime_error("prime placement PCM");
            AudioIo io;io.numOut=2;io.frames=count;if(width==Precision::Float64)io.out64=out64;else io.out=out;
            {audio::CallbackScope callback;session.process(io);}
            for(std::int32_t frame=0;frame<count;++frame)for(std::size_t channel=0;channel<2;++channel) {
                const auto offset=channel*257+static_cast<std::size_t>(frame);
                result[(first+static_cast<std::size_t>(frame))*2+channel]=width==Precision::Float64?wide[offset]:static_cast<double>(narrow[offset]);
            }
            first+=static_cast<std::size_t>(count);
        }
        return result;
    }
    bool apply(const clip_edit::PreparedEdit& edit,const char* label) {
        check(edit.ok,label);if(!edit.ok){std::printf("prepare: %s\n",edit.error.c_str());return false;}
        const auto committed=clip_edit::commitPrepared(*store,edit);check(committed.ok,"commit one captured placement transaction");
        if(!committed.ok){std::printf("commit: %s\n",committed.error.c_str());return false;}
        const bool refreshed=session.refresh(*store);check(refreshed,"refresh actual Session after placement");return refreshed;
    }
    void undoRedo(const std::vector<double>& before,const std::vector<double>& after) {
        const bool undone=History(*store).undo().ok && session.refresh(*store);
        check(undone,"one placement Undo restores the prior project and publishes its graph");
        if(undone)check(render(before.size()/2)==before,"one placement Undo restores every native PCM sample");
        const bool redone=History(*store).redo().ok && session.refresh(*store);
        check(redone,"one placement Redo restores its captured rows and graph");
        if(redone)check(render(after.size()/2)==after,"one placement Redo restores every native PCM sample");
        check(session.problems().empty() && session.clips()->underrunSamples()==0,"prepared placement plays without problems or source underruns");
    }
};
void groupMovePcm(std::uint32_t sourceRate,Precision width,bool musical) {
    Fixture f(sourceRate,width,musical,true);f.add(1,0,800);f.add(2,900,1100,79);f.add(3,1200,2900,157);
    auto& db=f.store->db();
    db.exec("UPDATE clips SET content_offset_ticks=12012,loop_enabled=1,loop_start_ticks=24024,loop_len_ticks=48048,"
        "fade_in_ticks=48048,fade_out_ticks=12012,fade_in_curve=2,fade_out_curve=5 WHERE id=1;"
        "UPDATE clips SET content_offset_ticks=12012 WHERE id=2;"
        "UPDATE clips SET content_offset_ticks=24024,fade_in_ticks=24024,fade_out_ticks=24024,fade_in_curve=2,fade_out_curve=4 WHERE id=3;"
        "INSERT INTO clip_playback_details(clip_id,in_tension,out_tension) VALUES(1,0.4,-0.3),(3,-0.2,0.25)");
    if(!f.load())return;
    const auto before=f.render(3100);
    check(before[200*2]!=0 && before[950*2]!=0 && before[2450*2]!=0,"group move oracle contains audible original sources and the destination gap");
    const std::array<placement::Move,2> moves={placement::Move{1,{1,std::nullopt},f.point(1500)},placement::Move{2,{1,std::nullopt},f.point(2500)}};
    const auto edit=placement::prepareMove(*f.store,f.scope(),moves,{});
    if(!f.apply(edit,"prepare a two-member group with disjoint destination masks"))return;
    auto expected=before;
    for(std::size_t frame=0;frame<3100;++frame)for(std::size_t channel=0;channel<2;++channel) {
        double value=before[frame*2+channel];
        if(frame<800 || (frame>=900 && frame<1100))value=0;
        if(frame>=1500 && frame<2300)value=before[(frame-1500)*2+channel];
        // The tick source's visible duration grows by 100 physical samples
        // after relocation. Its captured original fade already reached zero.
        if(musical && frame>=2300 && frame<2400)value=0;
        if(frame>=2500 && frame<2700)value=before[(frame-1600)*2+channel];
        expected[frame*2+channel]=value;
    }
    const auto after=f.render(3100);
    check(after==expected,"group move preserves exact source phase, loops and fades; all victim survivors and the sparse gap retain their original PCM");
    check(db.execAndGet("SELECT count(*) FROM clips WHERE id IN (1,2,3)").getInt()==3 &&
        db.execAndGet("SELECT count(*) FROM audio_clips WHERE clip_id IN (1,2,3)").getInt()==3,
        "moved sources and the leftmost destination survivor retain root and source identities");
    f.undoRedo(before,expected);
}
void warpDestinationTrim(Precision width,bool musical) {
    Fixture f(48000,width,musical,true);f.add(1,100,900);f.warp(1);f.add(2,1800,2000);
    auto& db=f.store->db();
    db.exec("UPDATE clips SET content_offset_ticks=12012,loop_enabled=1,loop_start_ticks=24024,loop_len_ticks=48048,"
        "fade_in_ticks=96096,fade_out_ticks=48048,fade_in_curve=2,fade_out_curve=4 WHERE id=1;"
        "UPDATE clips SET muted=1 WHERE id=2");
    if(!f.load())return;
    const auto before=f.render(2200);
    check(before[250*2]!=0 && before[550*2]!=0,"Warp destination baseline is audible on both sides of the planned mask");
    const std::array<placement::Move,1> moves={placement::Move{2,{1,std::nullopt},f.point(300)}};
    if(!f.apply(placement::prepareMove(*f.store,f.scope(),moves,{}),"place a muted source through the middle of a warped loop"))return;
    auto expected=before;for(std::size_t frame=300;frame<500;++frame)for(std::size_t channel=0;channel<2;++channel)expected[frame*2+channel]=0;
    check(f.render(2200)==expected,"destination Warp fragments preserve every sample, inherited fade and loop phase outside the actual mask");
    f.undoRedo(before,expected);
}
void inheritedResizeFade(Precision width,bool musical,bool warped,bool incomingFade) {
    Fixture f(48000,width,musical,false,true);f.add(1,0,800,0);if(warped)f.warp(1);
    f.store->db().exec(incomingFade?"UPDATE clips SET fade_in_ticks=168168,fade_in_curve=2":
        "UPDATE clips SET fade_out_ticks=168168,fade_out_curve=4");
    if(!f.load())return;
    const auto original=f.render(1000);
    if(!f.apply(clip_edit::prepareSplit(*f.store,1,2,f.point(400),48000),"split a long fade to create an inherited zero-handle source domain"))return;
    const auto split=f.render(1000);check(split==original,"split prerequisite preserves the native long-fade oracle exactly");
    const auto edit=placement::prepareResizeEnd(*f.store,f.scope(),{incomingFade?2:1,f.point(incomingFade?700:300)},{});
    if(!f.apply(edit,"resize the anchored end of a fragment with an inherited fade"))return;
    auto expected=original;
    const std::size_t begin=incomingFade?700u:300u,end=incomingFade?800u:400u;
    for(std::size_t frame=begin;frame<end;++frame)for(std::size_t channel=0;channel<2;++channel)expected[frame*2+channel]=0;
    check(f.render(1000)==expected,"zero-handle inherited fade retains its original source domain when only the visible end moves");
    f.undoRedo(split,expected);
}
void activeResizeFade(Precision width,bool musical,bool warped) {
    Fixture f(48000,width,musical,false,true);f.add(1,0,800,0);if(warped)f.warp(1);
    f.store->db().exec("UPDATE clips SET fade_out_ticks=24024,fade_out_curve=1");
    if(!f.load())return;
    const auto before=f.render(1000);
    if(!f.apply(placement::prepareResizeEnd(*f.store,f.scope(),{1,f.point(600)},{}),"shorten a clip whose authored outgoing fade follows its end"))return;
    const auto after=f.render(1000);bool exact=true;
    for(std::size_t frame=0;frame<1000;++frame)for(std::size_t channel=0;channel<2;++channel) {
        // The baseline has no fade before frame 700. In particular, Warp's
        // sinc pre-render has its own boundary ringing even for a constant
        // source. Preserve that independently rendered PCM, and apply only
        // the analytically specified new fade rather than assuming 0.5 PCM.
        const double gain=frame<500?1.0:frame<600?static_cast<double>(599-frame)/100.0:0.0;
        const double want=before[frame*2+channel]*gain;
        const bool matches=std::abs(after[frame*2+channel]-want)<(width==Precision::Float64?1e-12:1e-7);
        if(exact && !matches) {
            std::printf("active fade mismatch: width=%s clock=%s warp=%d frame=%zu channel=%zu actual=%.17g expected=%.17g before=%.17g\n",
                width==Precision::Float64?"double":"float",musical?"ticks":"ns",warped?1:0,frame,channel,
                after[frame*2+channel],want,before[frame*2+channel]);
            SQLite::Statement domain(f.store->db(),warped?
                "SELECT in_begin_ticks,in_end_ticks,out_begin_ticks,out_end_ticks FROM clip_warp_fade_domains WHERE clip_id=1":
                "SELECT in_begin_ns,in_end_ns,out_begin_ns,out_end_ns FROM clip_playback_details WHERE clip_id=1");
            if(domain.executeStep())for(int field=0;field<4;++field)
                std::printf("  captured domain[%d]=%s\n",field,domain.getColumn(field).isNull()?"NULL":domain.getColumn(field).getString().c_str());
        }
        exact=exact && matches;
    }
    check(exact,"authored fade-out reanchors its captured 100-sample duration at the new end, with an exact zero last sample");
    f.undoRedo(before,after);
}
void setAppData(const std::filesystem::path& path) {
#if defined(_WIN32)
    if(_wputenv_s(L"ADI_HOME",path.wstring().c_str())!=0)throw std::runtime_error("set placement app data");
#else
    if(setenv("ADI_HOME",path.string().c_str(),1)!=0)throw std::runtime_error("set placement app data");
#endif
}
}
int main() {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    test::TempDirectory appData{"edit_plan_playback","appdata"};
    try {
        setAppData(appData.path());
        for(const auto sourceRate:{44100u,48000u})for(const auto width:{Precision::Float32,Precision::Float64})for(const bool musical:{false,true})
            groupMovePcm(sourceRate,width,musical);
        for(const auto width:{Precision::Float32,Precision::Float64})for(const bool musical:{false,true}) {
            warpDestinationTrim(width,musical);
            for(const bool warped:{false,true}) {
                for(const bool incomingFade:{false,true})inheritedResizeFade(width,musical,warped,incomingFade);
                activeResizeFade(width,musical,warped);
            }
        }
    } catch(const std::exception& error) {check(false,error.what());}
    check(audio::callbackFileIo.load()==0,"placement audio callbacks perform no file I/O");
    std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;
}
