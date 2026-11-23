// SPDX-License-Identifier: AGPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/audio/warp_map.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/engine/session.hpp"
#include "adi/engine/warp_projection.hpp"
#include "adi/store.hpp"
#include "adi/store_rows.hpp"
#include "adi/clip_commands.hpp"
#include "adi/digest.hpp"
#include "adi/history.hpp"
#include "adi/edit_plan.hpp"
#include "adi/textproj_store.hpp"
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
bool applyEdit(Fixture& fixture,const clip_edit::PreparedEdit& edit,const char* label) {
    if(!edit.ok){std::printf("%s: %s\n",label,edit.error.c_str());check(false,label);return false;}
    const auto committed=clip_edit::commitPrepared(*fixture.store,edit);
    if(!committed.ok)std::printf("%s: %s\n",label,committed.error.c_str());
    check(committed.ok,label);return committed.ok;
}
void trimAtWarpMarker() {
    for(const auto width:{Precision::Float32,Precision::Float64}) {
        Fixture f;const auto markers=audio::writeWarpMap(std::vector<WarpMarker>{{0,0},{6000,q/2},{12000,q}},12000);
        SQLite::Statement marker(f.store->db(),"UPDATE audio_clips SET warp_markers=? WHERE clip_id=7");
        marker.bind(1,markers.bytes.data(),static_cast<int>(markers.bytes.size()));marker.exec();
        const auto before=digestProject(*f.store).text;f.load(width);const auto original=f.render(26000,width);
        const std::array<std::int64_t,1> tracks{1};edit_plan::Options options;options.policy.automationFollowsClips=false;
        const auto trim=edit_plan::prepareTrim(*f.store,
            edit_plan::materialScope(tracks,edit_plan::PlacementClock::Ticks,{0,2*q}),{7,{0,q/2}},options);
        if(!applyEdit(f,trim,"left trim exactly at a stored warp marker"))return;
        check(f.session.refresh(*f.store),"refresh a marker-aligned anchored Warp trim");
        f.session.transport().locate(0);const auto actual=f.render(26000,width);
        bool exact=actual.size()==52000 && original.size()==52000;
        if(exact)for(std::size_t i=0;i<actual.size();++i)exact=exact && actual[i]==(i<24000?0:original[i]);
        check(f.session.problems().empty() && exact &&
                  f.store->db().execAndGet("SELECT content_offset_ticks FROM clips WHERE id=7").getInt64()==q/2,
              "Warp trim retains original anchored marker timing and native-width samples");
        check(History(*f.store).undo().ok && digestProject(*f.store).text==before,
              "Warp trim undo restores exact original rows and optional-domain absence");
    }
}
void extendWarpWithStalePhysicalPin() {
    for(const auto width:{Precision::Float32,Precision::Float64}) {
        Fixture f;f.load(width);const auto original=f.render(26000,width);
        // Stored old ns pins are legal but ignored by Warp playback. This is
        // the same source after an earlier musical trim/move.
        f.store->db().exec("UPDATE clips SET pos_ticks=2882880,length_ticks=2882880,content_offset_ticks=2882880");
        const auto before=digestProject(*f.store).text;const std::array<std::int64_t,1> tracks{1};
        edit_plan::Options options;options.policy.automationFollowsClips=false;
        const auto trim=edit_plan::prepareTrim(*f.store,
            edit_plan::materialScope(tracks,edit_plan::PlacementClock::Ticks,{0,2*q}),{7,{0,0}},options);
        if(!applyEdit(f,trim,"extend Warp to source zero despite a stale physical pin"))return;
        check(f.store->db().execAndGet("SELECT count(*) FROM clips WHERE pos_ticks=0 AND length_ticks=5765760 AND content_offset_ticks=0").getInt()==1 &&
                  f.store->db().execAndGet("SELECT content_offset_ns FROM clip_playback_details WHERE clip_id=7").getInt64()==0,
              "Warp extension restores full musical bounds without rewriting ignored physical metadata");
        check(f.session.refresh(*f.store),"refresh an extended Warp clip with a stale physical pin");
        f.session.transport().locate(0);const auto actual=f.render(26000,width);
        check(f.session.problems().empty() && actual==original,"extended Warp renders the complete original native-width PCM");
        check(History(*f.store).undo().ok && digestProject(*f.store).text==before,
              "stale-pin Warp extension undoes exact source clocks and geometry");
    }
}
void warpedEditsKeepHeardFades() {
    for(const auto width:{Precision::Float32,Precision::Float64}) {
        Fixture f;auto& db=f.store->db();
        db.exec("UPDATE clips SET loop_enabled=1,loop_start_ticks=0,loop_len_ticks=5765760,length_ticks=11531520,fade_in_ticks=8648640,fade_out_ticks=8648640,fade_in_curve=2,fade_out_curve=4");
        db.exec("UPDATE clip_playback_details SET in_tension=0.35,out_tension=-0.4");
        db.exec("ALTER TABLE clip_warp_fade_domains ADD COLUMN future_blob BLOB NOT NULL DEFAULT X'00FF01'");
        f.load(width);const auto original=f.render(50000,width);const auto before=digestProject(*f.store).text;
        if(!applyEdit(f,clip_edit::prepareSplit(*f.store,7,17,{0,q/2},48000),"split a warped loop inside overlapping fades"))return;
        check(db.execAndGet("SELECT count(*) FROM clip_warp_fade_domains WHERE in_begin_ticks=0 AND in_end_ticks=8648640 AND out_begin_ticks=2882880 AND out_end_ticks=11531520 AND hex(future_blob)='00FF01'").getInt()==2,
              "both split pieces retain the original source musical fade domains and future bytes");
        check(f.session.refresh(*f.store),"refresh after Warp split");f.session.transport().locate(0);
        check(f.session.problems().empty() && f.render(50000,width)==original,"Warp split preserves every heard sample through overlapping fades and source-loop wrapping");
        if(!applyEdit(f,clip_edit::prepareSplit(*f.store,17,18,{0,3*q/4},48000),"split an already retained Warp fade again"))return;
        check(f.session.refresh(*f.store),"refresh after repeated Warp split");f.session.transport().locate(0);
        check(f.session.problems().empty() && f.render(50000,width)==original,"repeated split retains the first fade's shape and unrolled clock exactly");
        const auto after=digestProject(*f.store).text;History history(*f.store);
        check(history.undo().ok && history.undo().ok && digestProject(*f.store).text==before,"undo both Warp splits restores exact rows and absent domain metadata");
        check(history.redo().ok && history.redo().ok && digestProject(*f.store).text==after,"redo restores literal Warp maps, retained fades and source offsets");
        const auto copy=clip_edit::copyClip(*f.store,18,48000);
        check(copy.ok,"copy a trimmed warped loop with retained fades");
        if(!applyEdit(f,clip_edit::preparePaste(*f.store,copy,27,1,{0,4*q},48000),"paste warped material at a new musical position"))return;
        check(db.execAndGet("SELECT hex(warp_markers) FROM audio_clips WHERE clip_id=27").getString()==db.execAndGet("SELECT hex(warp_markers) FROM audio_clips WHERE clip_id=18").getString() &&
              db.execAndGet("SELECT out_end_ticks FROM clip_warp_fade_domains WHERE clip_id=27").getInt64()==2*q,
              "paste preserves the map byte-for-byte and remaps the full typed fade child to the new clip");
        const auto beforeHandle=digestProject(*f.store).text;
        OpRequest handle;handle.opType="clip.setFades";handle.payload={{"id",18},{"out",{{"ticks",q/8},{"tension",0.2}}}};
        check(OpJournal(*f.store).commit(handle).ok && db.execAndGet("SELECT out_begin_ticks IS NULL AND in_begin_ticks=0 FROM clip_warp_fade_domains WHERE clip_id=18").getInt()!=0,
              "a Warp fade handle replaces only its chosen side's retained domain");
        check(history.undo().ok && digestProject(*f.store).text==beforeHandle,"handle undo restores all retained domains and future bytes exactly");
    }
}
void warpFadeProjectionAndSharing() {
    Fixture f;auto& db=f.store->db();
    db.exec("INSERT INTO clip_warp_fade_domains VALUES(7,0,100,200,300)");
    auto old=SnapshotBuilder::fromStore(*f.store);
    constexpr std::array<const char*,4> columns={"in_begin_ticks","in_end_ticks","out_begin_ticks","out_end_ticks"};
    for(std::size_t field=0;field<4;++field) {
        db.exec(std::string("UPDATE clip_warp_fade_domains SET ")+columns[field]+"="+columns[field]+"+1");
        auto next=SnapshotBuilder::fromStore(*f.store,*old);const auto model=rows::readModel(*f.store);
        check(next->tracks[0]->clips[0]!=old->tracks[0]->clips[0] && next->tracks[0]->clips[0]->fadeClockTicks[field]==model.clips[0].fadeClockTicks[field] &&
              next->tracks[0]->clips[0]->fadeClockTicks[field]!=old->tracks[0]->clips[0]->fadeClockTicks[field],"each musical Warp fade coordinate participates in coherent projection and structural sharing");
        old=std::move(next);
    }
    check(SnapshotBuilder::fromStore(*f.store,*old)->tracks[0]==old->tracks[0],"unchanged Warp fade metadata shares its complete track");
    const auto text=textproj::projectStore(*f.store).text;
    check(text.find("fadeInBeginTicks 1")!=std::string::npos && text.find("fadeOutEndTicks 301")!=std::string::npos,"text projection retains source musical domains including explicit zero/absence distinctions");
    bool incomplete=false,backwards=false;
    try{db.exec("UPDATE clip_warp_fade_domains SET in_end_ticks=NULL");}catch(const std::exception&){incomplete=true;}
    try{db.exec("UPDATE clip_warp_fade_domains SET out_end_ticks=out_begin_ticks");}catch(const std::exception&){backwards=true;}
    check(incomplete && backwards,"schema refuses partial or nonincreasing musical fade pairs");
}
void warpFadesAcrossTempoAndLinearSplit() {
    for(const auto width:{Precision::Float32,Precision::Float64}) {
        Fixture f;auto& db=f.store->db();
        db.exec("DELETE FROM tempo_map;INSERT INTO tempo_map(pos_ticks,bpm) VALUES(0,120),(2882880,60)");
        db.exec("UPDATE clips SET time_base=1,pos_ticks=NULL,length_ticks=NULL,pos_ns=0,length_ns=750000000,fade_in_ticks=4324320,fade_out_ticks=4324320");
        f.load(width);const auto original=f.render(38000,width);const auto before=digestProject(*f.store).text;
        if(!applyEdit(f,clip_edit::prepareSplit(*f.store,7,17,{1,500000000},48000),"split linear placement inside a warped fade after a tempo junction"))return;
        check(f.session.refresh(*f.store),"refresh a split linear Warp placement");f.session.transport().locate(0);
        check(f.session.problems().empty() && f.render(38000,width)==original,"linear Warp split preserves heard fades across the project's tempo junction");
        check(History(*f.store).undo().ok && digestProject(*f.store).text==before,"undo linear Warp split restores exact source clocks and placement domain");
        const auto range=clip_edit::copyRange(*f.store,{{1},{0,q/4},{0,3*q/4}},48000);
        check(range.ok && range.images.size()==1,"copy a partial Warp range through both fades");
        const auto deletion=clip_edit::prepareDeleteRange(*f.store,{{1},{0,q/4},{0,3*q/4}},48000);
        if(!applyEdit(f,deletion,"delete a partial Warp range with retained edges"))return;
        check(db.execAndGet("SELECT count(*) FROM clip_warp_fade_domains").getInt()==2 && History(*f.store).undo().ok && digestProject(*f.store).text==before,
              "range deletion captures both edge domains and undoes without losing the original fade");
    }
}
}
int main(){try{modesAndLoops();sourceRatesAndTempoBoundary();modelAndRefusals();trimmedMusicalOffset();trimAtWarpMarker();extendWarpWithStalePhysicalPin();warpedEditsKeepHeardFades();warpFadeProjectionAndSharing();warpFadesAcrossTempoAndLinearSplit();}catch(const std::exception& error){check(false,error.what());}check(callbackAllocations.load()==0 && audio::callbackFileIo.load()==0,"actual warped audio callback allocates nothing and performs no file I/O");std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
