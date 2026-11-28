// SPDX-License-Identifier: AGPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/warp_commands.hpp"
#include "adi/clip_image.hpp"
#include "adi/material_commands.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/digest.hpp"
#include "adi/edit_rows.hpp"
#include "adi/engine/session.hpp"
#include "adi/history.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/row_capture.hpp"
#include "adi/store.hpp"
#include "adi/textproj.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
namespace {
using namespace adi;using namespace adi::engine;constexpr std::int64_t q=textproj::kPPQ;
int checks=0,failures=0;
void check(bool ok,const char* what){++checks;if(!ok){++failures;std::printf("FAIL %s\n",what);}}
struct Fixture {
    test::TempDirectory temporary{"warp_actions","project"};std::unique_ptr<Store> store;
    explicit Fixture(bool beats=true) {
        StoreError error{};store=Store::createEmpty(temporary.path()/"song.adi",error);if(!store)throw std::runtime_error(toString(error));
        store->db().exec("INSERT INTO tracks(id,kind,name) VALUES(1,'audio','Warp'),(9,'master','Main')");
        std::vector<float> input(beats ? 44100+1234 : 44100,0);
        if(beats)for(std::size_t beat=0;beat<4;++beat)input[1234+11025*beat]=1;
        {audio::WavWriter writer(temporary.path()/"source.wav",44100,1,audio::WavFormat::Float32);writer.write(input.data(),static_cast<std::uint32_t>(input.size()));writer.close();}
        clip_edit::ImportOptions options;options.source=temporary.path()/"source.wav";options.track=1;options.clipId=7;options.mediaId=8;options.at={0,q};
        auto imported=clip_edit::prepareImportAudio(*store,options,48000);const auto committed=clip_edit::commitMaterial(*store,imported);
        if(!committed.ok)throw std::runtime_error(committed.error);
    }
    clip_edit::WarpOptions options()const {clip_edit::WarpOptions value;value.analysis.tempoHint=240;return value;}
};
bool apply(Fixture& f,const clip_edit::PreparedEdit& edit,const char* label) {
    if(!edit.ok){std::printf("%s: %s\n",label,edit.error.c_str());check(false,label);return false;}
    const auto committed=clip_edit::commitPrepared(*f.store,edit);if(!committed.ok)std::printf("%s: %s\n",label,committed.error.c_str());check(committed.ok,label);return committed.ok;
}
void replaceClipSource() {
    Fixture fixture;
    auto& db=fixture.store->db();
    db.exec("INSERT INTO media_files(id,hash_blake3,frames,sample_rate,channels,format) VALUES(18,'replacement',45334,44100,1,'wav')");
    const auto before=digestProject(*fixture.store).text;
    auto source=adi::detail::captureRow(db,"audio_clips","clip_id",7);
    if(!source)throw std::runtime_error("capture original audio clip source");
    (*source)["media_id"]=18;
    edit::RowEdits rows(db);
    rows.set("audio_clips",7,*source);
    OpRequest replacement;
    replacement.opType="audioClip.setSource";
    replacement.payload={{"id",7},{"changes",rows.changes()}};
    const auto result=OpJournal(*fixture.store).commit(replacement);
    check(result.ok && db.execAndGet("SELECT media_id FROM audio_clips WHERE clip_id=7").getInt64()==18,
          "a captured replacement changes only the chosen audio clip's media identity");
    if(result.ok) {
        History history(*fixture.store);
        check(history.undo().ok && digestProject(*fixture.store).text==before,
              "source replacement undo restores the original project exactly");
    }
}
void replaceWarpedSource() {
    Fixture fixture;
    auto& store=*fixture.store;
    auto& db=store.db();
    constexpr std::int64_t frames=45334;
    const auto map=audio::writeWarpMap(std::vector<WarpMarker>{{0,0},{22000,q},{frames,3*q}},frames);
    SQLite::Statement warp(db,"UPDATE audio_clips SET warp_enabled=1,warp_mode='complex',warp_markers=?,"
                              "transpose_semis=2.5 WHERE clip_id=7");
    warp.bind(1,map.bytes.data(),static_cast<int>(map.bytes.size()));warp.exec();
    db.exec("UPDATE clips SET gain_db=-7 WHERE id=7;"
            "INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks) VALUES(12,1,'audio',0,10);"
            "INSERT INTO audio_clips(clip_id,media_id,src_len_frames) VALUES(12,8,45334);"
            "INSERT INTO media_files(id,hash_blake3,frames,sample_rate,channels,format) VALUES(18,'equal',45334,44100,1,'wav')");
    const auto before=digestProject(store).text;
    const auto originalMap=db.execAndGet("SELECT hex(warp_markers) FROM audio_clips WHERE clip_id=7").getString();
    if(!apply(fixture,clip_edit::prepareSetSource(store,7,18,frames,44100),"swap in a different file of equal duration"))return;
    check(db.execAndGet("SELECT media_id=18 AND warp_enabled=1 AND warp_mode='complex' AND transpose_semis=2.5 FROM audio_clips WHERE clip_id=7").getInt()==1 &&
          db.execAndGet("SELECT hex(warp_markers) FROM audio_clips WHERE clip_id=7").getString()==originalMap &&
          db.execAndGet("SELECT media_id FROM audio_clips WHERE clip_id=12").getInt64()==8 &&
          std::abs(db.execAndGet("SELECT gain_db FROM clips WHERE id=7").getDouble() + 7.0) < 1e-6,
          "equal-duration replacement retains exact marker bytes and settings and leaves the other clip alone");
    History history(store);
    check(history.undo().ok && digestProject(store).text==before,"undo restores the original source and every clip property");

    const auto file=fixture.temporary.path()/"shorter.wav";
    std::vector<float> samples(22050,.25f);
    {audio::WavWriter writer(file,44100,1,audio::WavFormat::Float32);writer.write(samples.data(),22050);writer.close();}
    OpRequest imported;
    const auto captured=media::prepareImport(store,file,19,1,imported);
    check(captured.ok && captured.audio,"read a new replacement file without writing its pool row");
    if(!captured.ok || !captured.audio)return;
    if(!apply(fixture,clip_edit::prepareSetSource(store,7,captured.mediaId,captured.audio->frames,captured.audio->sampleRate,
                                                  std::move(imported)),"commit import and unequal-duration replacement as one edit"))return;
    SQLite::Statement current(db,"SELECT warp_markers FROM audio_clips WHERE clip_id=7");
    current.executeStep();
    const auto bytes=current.getColumn(0);
    const auto newMap=audio::readWarpMap(std::span(static_cast<const std::byte*>(bytes.getBlob()),
                                                   static_cast<std::size_t>(bytes.getBytes())),22050);
    check(newMap && newMap.markers==std::vector<WarpMarker>{{0,0},{22050,3*q}},
          "a new duration resets hand-aligned markers and keeps the clip's Warp span");
    current.reset();
    check(history.undo().ok && digestProject(store).text==before &&
          db.execAndGet("SELECT count(*) FROM media_files WHERE id=19").getInt()==0,
          "one undo restores the sample and removes the newly imported media row");
}
void replaceUnwarpedSourcePastStart() {
    for(const bool pinned:{false,true}) {
        Fixture fixture(false);
        auto& store=*fixture.store;
        auto& db=store.db();
        db.exec("INSERT INTO tempo_map(pos_ticks,bpm) VALUES(0,120),("+std::to_string(q)+",60);"
                "UPDATE clips SET time_base=1,pos_ns=500000000,length_ns=500000000,pos_ticks=NULL,length_ticks=NULL,"
                "content_offset_ticks="+std::to_string(pinned?q/4:3*q/8)+" WHERE id=7;"
                "UPDATE audio_clips SET transpose_semis=12 WHERE clip_id=7;");
        SQLite::Statement clock(db,"INSERT INTO clip_playback_details(clip_id,content_offset_ns) VALUES(7,?)"
                                   " ON CONFLICT(clip_id) DO UPDATE SET content_offset_ns=excluded.content_offset_ns");
        if(pinned)clock.bind(1,375000000);
        else clock.bind(1);
        clock.exec();
        OpContext context{store,db};
        const auto before=clip_edit::captureImage(context,7);
        const auto head=OpJournal(store).headSeq();
        const auto replacement=[&](const char* name,std::uint32_t frames) {
            const auto file=fixture.temporary.path()/name;
            const std::vector<float> samples(frames,.25f);
            {audio::WavWriter writer(file,48000,1,audio::WavFormat::Float32);writer.write(samples.data(),frames);writer.close();}
            OpRequest imported;
            const auto captured=media::prepareImport(store,file,19,1,imported);
            if(!captured.ok || !captured.audio)throw std::runtime_error("could not stage the replacement WAV");
            return clip_edit::prepareSetSource(store,7,captured.mediaId,captured.audio->frames,captured.audio->sampleRate,
                                               std::move(imported));
        };
        // At +12 semitones the replacement plays in half its native time.
        const auto tooShort=replacement("too-short.wav",pinned?36000U:32000U);
        const auto refused=clip_edit::commitPrepared(store,tooShort);
        check(!tooShort.ok && tooShort.requests.empty() && !refused.ok && tooShort.error.find("Start")!=std::string::npos,
              pinned?"a pitched replacement ending exactly at the pinned Start is refused"
                    :"a pitched replacement ending before the legacy Start at the clip's tempo is refused");
        check(OpJournal(store).headSeq()==head && db.execAndGet("SELECT count(*) FROM media_files WHERE id=19").getInt()==0 &&
              clip_edit::captureImage(context,7)==before,
              "refusing the replacement leaves no new media row, history entry or changed clip");
        if(refused.ok)continue;

        const auto shortTail=replacement("short-tail.wav",42000U);
        if(!apply(fixture,shortTail,"accept a replacement that plays some audio after Start"))continue;
        const auto after=clip_edit::captureImage(context,7);
        const auto& source=after.at("children").at("audio_clips").front();
        check(source.at("media_id")==19 && source.at("src_len_frames")==42000 && source.at("transpose_semis")==12 &&
              after.at("row")==before.at("row") &&
              after.at("children").at("clip_playback_details")==before.at("children").at("clip_playback_details"),
              "the shorter audible replacement keeps Start and Pitch and leaves the Arrangement extent intact");
    }
}
std::vector<float> render(Fixture& f) {
    Session session;SessionSpec spec{2,48000,1024};session.graph().setFadeFrames(0);
    if(!session.load(*f.store,{},spec) || !session.problems().empty())throw std::runtime_error("load Warp action audio");
    session.transport().play();std::vector<float> result(128000);std::array<float,2048> output{};float* channels[]={output.data(),output.data()+1024};
    for(std::size_t first=0;first<result.size();) {
        if(!session.clips()->prime(std::chrono::seconds(5)))throw std::runtime_error("prime Warp action audio");
        const auto count=static_cast<std::int32_t>(std::min<std::size_t>(1024,result.size()-first));AudioIo io;io.frames=count;io.numOut=2;io.out=channels;session.process(io);
        std::copy_n(output.begin(),count,result.begin()+static_cast<std::ptrdiff_t>(first));first+=static_cast<std::size_t>(count);
    }
    return result;
}
void actionAndHistory() {
    Fixture f;auto& db=f.store->db();const auto before=digestProject(*f.store).text;
    const auto pending=clip_edit::prepareSetWarp(*f.store,7,f.options(),48000);
    check(pending.ok && pending.options.expectHead && digestProject(*f.store).text==before,"Warp button analyzes a clean existing import without a project write");
    if(!apply(f,pending,"commit existing-clip Auto-Warp as one captured edit"))return;
    check(db.execAndGet("SELECT warp_enabled FROM audio_clips").getInt()==1 && db.execAndGet("SELECT src_start_frames FROM audio_clips").getInt64()==1234 &&
          db.execAndGet("SELECT src_len_frames FROM audio_clips").getInt64()==44100 && db.execAndGet("SELECT length_ticks FROM clips").getInt64()==4*q,
          "Auto-Warp captures first-beat trim, map and musical source length on the original identity");
    const auto enabled=digestProject(*f.store).text;const auto heard=render(f);bool beatGrid=true;
    for(std::ptrdiff_t beat=0;beat<4;++beat)beatGrid=beatGrid && *std::max_element(heard.begin()+24000+beat*24000-2,heard.begin()+24000+beat*24000+3)>0.5f;
    check(beatGrid,"the existing clip's analyzed attacks are heard on the project beat grid");
    History history(*f.store);check(history.undo().ok && digestProject(*f.store).text==before,"Warp enable undo restores exact original source and physical pins");
    check(history.redo().ok && digestProject(*f.store).text==enabled && render(f)==heard,"Warp redo restores captured bytes and heard audio without analysis");
    auto off=f.options();off.enabled=false;
    if(!apply(f,clip_edit::prepareSetWarp(*f.store,7,off,48000),"disable Warp with a captured physical source clock"))return;
    check(db.execAndGet("SELECT warp_enabled FROM audio_clips").getInt()==0 && db.execAndGet("SELECT source_end_ns FROM clip_playback_details").getInt64()==1000000000 &&
          db.execAndGet("SELECT length_ticks FROM clips").getInt64()==2*q,"Warp off restores the actual one-second source duration instead of retaining the stretched extent");
    const auto unwarped=render(f);bool physicalGrid=true;
    for(std::ptrdiff_t beat=0;beat<4;++beat)physicalGrid=physicalGrid && *std::max_element(unwarped.begin()+24000+beat*12000-2,unwarped.begin()+24000+beat*12000+3)>0.5f;
    check(physicalGrid && std::all_of(unwarped.begin()+72000,unwarped.end(),[](float x){return x==0;}),"disabled Warp is heard at physical source speed and ends at its new boundary");
    check(history.undo().ok && digestProject(*f.store).text==enabled && render(f)==heard,"undo Warp off restores the complete stretched clip and its PCM");
}
void editedSourceWindow() {
    Fixture f;
    auto& db=f.store->db();
    OpRequest resize;
    resize.opType="clip.resize";
    resize.payload={{"id",7},{"pos",q},{"length",q}};
    if(!OpJournal(*f.store).commit(resize).ok) {
        check(false,"trim the unwarped clip's right edge before switching Warp");
        return;
    }
    if(!apply(f,clip_edit::prepareSplit(*f.store,7,11,{0,q+q/2},48000),
              "split the trimmed audio clip"))return;
    auto on=f.options();
    on.analysis.sourceBpm=240;
    on.analysis.fromFirstBeat=false;
    for(const auto id:{7,11}) {
        if(!apply(f,clip_edit::prepareSetWarp(*f.store,id,on,48000),
                  "enable Warp on a piece of a split and trimmed clip"))return;
    }
    check(db.execAndGet("SELECT length_ticks FROM clips WHERE id=7").getInt64()==q &&
              db.execAndGet("SELECT length_ticks FROM clips WHERE id=11").getInt64()==q &&
              db.execAndGet("SELECT content_offset_ticks FROM clips WHERE id=11").getInt64()==q,
          "both trimmed windows become one beat at 240 source BPM, the second at the next source beat");
    OpRequest start;
    start.opType="clip.setStartMarker";
    start.payload={{"id",11},{"ticks",q+q/2}};
    if(!OpJournal(*f.store).commit(start).ok) {
        check(false,"move the warped clip's Start marker");
        return;
    }
    auto off=on;
    off.enabled=false;
    if(!apply(f,clip_edit::prepareSetWarp(*f.store,11,off,48000),
              "disable Warp after moving the split piece's Start"))return;
    check(db.execAndGet("SELECT warp_enabled FROM audio_clips WHERE clip_id=11").getInt()==0 &&
              std::llabs(db.execAndGet("SELECT content_offset_ns FROM clip_playback_details WHERE clip_id=11").getInt64()-375'000'000)<22'676 &&
              db.execAndGet("SELECT pos_ticks FROM clips WHERE id=11").getInt64()==q+q/2,
          "Warp off keeps the sample position and the clip's Arrangement start");
}
void explicitSourceTempo() {
    Fixture f(false);
    check(!clip_edit::prepareSetWarp(*f.store,7,f.options(),48000).ok,
          "silence contains no beat for Auto-Warp");
    auto manual=f.options();
    manual.analysis.sourceBpm=120;
    if(!apply(f,clip_edit::prepareSetWarp(*f.store,7,manual,48000),
              "an explicit source BPM enables Warp without a detected beat"))return;
    auto& db=f.store->db();
    check(db.execAndGet("SELECT src_start_frames FROM audio_clips WHERE clip_id=7").getInt64()==0 &&
          db.execAndGet("SELECT src_len_frames FROM audio_clips WHERE clip_id=7").getInt64()==44100 &&
          db.execAndGet("SELECT length_ticks FROM clips WHERE id=7").getInt64()==2*q,
          "a manual source tempo covers the complete sample from frame zero");
}
void correctWarpedSourceTempo() {
    Fixture f;
    auto& db=f.store->db();
    if(!apply(f,clip_edit::prepareSetWarp(*f.store,7,f.options(),48000),
              "prepare a warped one-second source with a detected 240 BPM"))return;

    const auto request=[](const char* type,Payload payload){
        OpRequest op;
        op.opType=type;
        op.payload=std::move(payload);
        return op;
    };
    const std::array edits{
        request("clip.resize",{{"id",7},{"pos",q},{"length",2*q}}),
        request("clip.setStartMarker",{{"id",7},{"ticks",q}}),
        request("clip.setEndMarker",{{"id",7},{"ticks",3*q}}),
        request("clip.setLoop",{{"id",7},{"on",true},{"start",q},{"length",q}})
    };
    if(!OpJournal(*f.store).commit(edits).ok){
        check(false,"prepare a trimmed and looped warped clip");
        return;
    }

    const auto original=digestProject(*f.store).text;
    auto manual=f.options();
    manual.analysis.sourceBpm=120;
    if(!apply(f,clip_edit::prepareSetWarp(*f.store,7,manual,48000),
              "halve the source tempo of an existing warped clip"))return;
    const auto get=[&](const char* sql){return db.execAndGet(sql).getInt64();};
    check(get("SELECT pos_ticks FROM clips WHERE id=7")==q &&
          get("SELECT src_start_frames FROM audio_clips WHERE clip_id=7")==1234 &&
          get("SELECT src_len_frames FROM audio_clips WHERE clip_id=7")==44100,
          "a tempo correction keeps the song start and immutable source window");
    check(get("SELECT content_offset_ticks FROM clips WHERE id=7")==q/2 &&
          get("SELECT length_ticks FROM clips WHERE id=7")==q &&
          get("SELECT end_marker_ticks FROM clip_properties WHERE clip_id=7")==3*q/2 &&
          get("SELECT loop_start_ticks FROM clips WHERE id=7")==q/2 &&
          get("SELECT loop_len_ticks FROM clips WHERE id=7")==q/2,
          "the new tempo projects the same physical Start, End, trim and loop boundaries");
    const auto half=digestProject(*f.store).text;
    History history(*f.store);
    check(history.undo().ok && digestProject(*f.store).text==original &&
          history.redo().ok && digestProject(*f.store).text==half,
          "tempo correction Undo and Redo restore the captured map and clip bounds");
    manual.analysis.sourceBpm=240;
    if(!apply(f,clip_edit::prepareSetWarp(*f.store,7,manual,48000),
              "double the source tempo of an existing warped clip"))return;
    check(digestProject(*f.store).text==original,
          "halving then doubling an exact clock restores all clip and marker data");
}
void disableWarpOnLoop() {
    Fixture f;
    auto on = f.options();
    if (!apply(f, clip_edit::prepareSetWarp(*f.store, 7, on, 48000), "warp audio before enabling its loop"))
        return;
    OpRequest loop;
    loop.opType = "clip.setLoop";
    loop.payload = {{"id", 7}, {"on", true}, {"start", q}, {"length", 2 * q}};
    if (!OpJournal(*f.store).commit(loop).ok) {
        check(false, "loop the middle two beats of the warped source");
        return;
    }
    auto& db = f.store->db();
    // An Arrangement piece split during a repeat can start beyond the file's first pass.
    db.exec("UPDATE clips SET content_offset_ticks=" + std::to_string(6 * q) + " WHERE id=7");
    const auto before = digestProject(*f.store).text;
    auto off = on;
    off.enabled = false;
    if (!apply(f, clip_edit::prepareSetWarp(*f.store, 7, off, 48000), "disable Warp on a looped repeat"))
        return;
    check(db.execAndGet("SELECT loop_enabled FROM clips WHERE id=7").getInt() == 0 &&
              db.execAndGet("SELECT warp_enabled FROM audio_clips WHERE clip_id=7").getInt() == 0 &&
              db.execAndGet("SELECT content_offset_ns FROM clip_playback_details WHERE clip_id=7").getInt64() == 500'000'000,
          "Warp off disables Loop and keeps the repeat's first audible source sample");
    History history(*f.store);
    check(history.undo().ok && digestProject(*f.store).text == before,
          "one Undo restores Warp, the enabled loop and its unrolled playback offset");
    check(history.redo().ok, "redo stops the loop with Warp again");
    if (!apply(f, clip_edit::prepareSetWarp(*f.store, 7, on, 48000), "re-enable Warp with its saved source map"))
        return;
    check(db.execAndGet("SELECT loop_enabled FROM clips WHERE id=7").getInt() == 0 &&
              db.execAndGet("SELECT loop_start_ticks FROM clips WHERE id=7").getInt64() == q &&
              db.execAndGet("SELECT loop_len_ticks FROM clips WHERE id=7").getInt64() == 2 * q,
          "re-enabling Warp leaves the saved loop brace valid and the switch off");
}
void changePlacedWarpMode() {
    Fixture f;
    if(!apply(f,clip_edit::prepareSetWarp(*f.store,7,f.options(),48000),"warp the placed clip before changing mode"))return;
    OpRequest loop;
    loop.opType="clip.setLoop";
    loop.payload={{"id",7},{"on",true},{"start",q},{"length",q}};
    if(!OpJournal(*f.store).commit(loop).ok ||
       !apply(f,clip_edit::prepareSplit(*f.store,7,11,{0,2*q},48000),"split the looped clip before changing mode"))return;
    f.store->db().exec("UPDATE audio_clips SET transpose_semis=3 WHERE clip_id=11");
    OpContext context{*f.store,f.store->db()};
    const auto before=clip_edit::captureImage(context,11);
    const auto project=digestProject(*f.store).text;
    // A different rendering algorithm has no need to decode or reanalyse the sample.
    std::filesystem::remove(f.temporary.path()/"source.wav");
    auto options=f.options();
    options.mode=audio::WarpMode::RePitch;
    const auto mode=clip_edit::prepareSetWarp(*f.store,11,options,48000);
    check(mode.ok && mode.requests.size()==1 && digestProject(*f.store).text==project,
          "a placed warped clip can prepare a mode-only edit with the source offline");
    if(!apply(f,mode,"change the Warp mode in one captured edit"))return;
    auto expected=before;
    expected["children"]["audio_clips"][0]["warp_mode"]="re-pitch";
    check(clip_edit::captureImage(context,11)==expected,
          "changing the algorithm preserves the marker bytes, placement, trim, loop and stored pitch");
    History history(*f.store);
    check(history.undo().ok && digestProject(*f.store).text==project && history.redo().ok &&
          clip_edit::captureImage(context,11)==expected,"Undo and Redo restore the two algorithms without reanalysis");
}
void turnTransientsIntoMarkers() {
    Fixture f(false);
    auto options=f.options();
    options.analysis.sourceBpm=120;
    if(!apply(f,clip_edit::prepareSetWarp(*f.store,7,options,48000),"warp the source before promoting transients"))return;
    OpContext context{*f.store,f.store->db()};
    const auto map=[&] {
        const auto image=clip_edit::captureImage(context,7);
        const auto& bytes=image.at("children").at("audio_clips").front().at("warp_markers").get_binary();
        return audio::readWarpMap(std::span(reinterpret_cast<const std::byte*>(bytes.data()),bytes.size()),44100).markers;
    };
    const auto before=map();
    const std::array<std::uint64_t,7> onsets{0,11025,11025,22050,44099,44100,99999};
    const auto promotion=clip_edit::prepareWarpMarkersAtTransients(*f.store,7,onsets,48000);
    check(promotion.ok && promotion.requests.size()==1,"one captured edit promotes all interior transients");
    if(!apply(f,promotion,"promote cached file transients without reading PCM"))return;
    const auto after=map();
    check(after.size()==5 && after.front()==before.front() && after.back()==before.back() &&
          after[1]==WarpMarker{11025,q/2} && after[2]==WarpMarker{22050,q} && after[3].sourceFrame==44099,
          "transients become positions on the existing clock, with endpoints and duplicate frames deduplicated");
    History history(*f.store);
    check(history.undo().ok && map()==before && history.redo().ok && map()==after,
          "promoting transients restores captured Warp map bytes on Undo and Redo");
}
void warpFromHere() {
    Fixture f(false);
    auto warp=f.options();
    warp.analysis.sourceBpm=120;
    if(!apply(f,clip_edit::prepareSetWarp(*f.store,7,warp,48000),"seed a Warp From Here source"))return;
    for(const auto point:{WarpMarker{8820,q/2},WarpMarker{17640,3*q/2}})
        if(!apply(f,clip_edit::prepareWarpMarkerEdit(*f.store,7,clip_edit::WarpMarkerChange::Add,point,48000),
                  "seed an interior Warp marker"))return;
    OpContext context{*f.store,f.store->db()};
    const auto map=[&] {
        const auto image=clip_edit::captureImage(context,7);
        const auto& bytes=image.at("children").at("audio_clips").front().at("warp_markers").get_binary();
        return audio::readWarpMap(std::span(reinterpret_cast<const std::byte*>(bytes.data()),bytes.size()),44100).markers;
    };
    const auto before=map();
    if(before.size()!=4){check(false,"Warp From Here fixture has both interior anchors");return;}
    // The already-detected file onsets remain usable when the media goes offline.
    f.store->db().exec("UPDATE media_files SET rel_path='unavailable.wav',abs_path_hint=NULL,missing=1 WHERE id=8");
    const std::array<std::uint64_t,10> onsets{100,4410,14553,18963,23373,27783,32193,36603,41013,44100};
    audio::AutoWarpOptions analysis;
    const auto suffix=clip_edit::prepareWarpFromHere(*f.store,7,q,onsets,analysis,48000);
    check(suffix.ok && suffix.requests.size()==1,"cached transients stage one Warp From Here edit without media access");
    if(!apply(f,suffix,"commit the inferred right-side map"))return;
    const auto after=map();
    check(after.size()>=4 && after[0]==before[0] && after[1]==before[1] && after[2]==WarpMarker{13230,q} &&
          after.back().sourceFrame==before.back().sourceFrame && after.back().tick!=before.back().tick &&
          std::ranges::find(after,before[2].sourceFrame,&WarpMarker::sourceFrame)==after.end(),
          "the left anchors and the new boundary survive, with the previous right anchor removed");
    History history(*f.store);
    check(history.undo().ok && map()==before && history.redo().ok && map()==after,
          "Undo and Redo restore exact captured Warp From Here marker bytes");
    if(!history.undo().ok){check(false,"restore the original map for the Set BPM case");return;}
    check(!clip_edit::prepareWarpFromHere(*f.store,7,q,{},analysis,48000).ok && map()==before,
          "an inferred Warp From Here without cached transients leaves the map intact");
    analysis.sourceBpm=60;
    if(!apply(f,clip_edit::prepareWarpFromHere(*f.store,7,q,{},analysis,48000),
              "use the Set BPM with no detected transients"))return;
    const std::vector<WarpMarker> expected{before[0],before[1],{13230,q},{44100,17*q/10}};
    check(map()==expected,"the explicit BPM variant makes a straight suffix through the source endpoint");
}
void warpWholeSampleAsLoop() {
    Fixture f(false);
    auto warp=f.options();
    warp.analysis.sourceBpm=120;
    if(!apply(f,clip_edit::prepareSetWarp(*f.store,7,warp,48000),"seed a whole-sample loop source"))return;
    OpContext context{*f.store,f.store->db()};
    const auto before=clip_edit::captureImage(context,7);
    f.store->db().exec("UPDATE media_files SET rel_path='unavailable.wav',abs_path_hint=NULL,missing=1 WHERE id=8");
    constexpr auto length=6*q;
    check(!clip_edit::prepareWarpAsLoop(*f.store,7,clip_edit::WarpLoopSelection{q,q},length,48000).ok &&
          clip_edit::captureImage(context,7)==before,"an empty selection refuses without changing the clip");
    const auto loop=clip_edit::prepareWarpAsLoop(*f.store,7,std::nullopt,length,48000);
    check(loop.ok && loop.requests.size()==1,"the captured map alone prepares a two-bar loop in three-quarter time");
    if(!apply(f,loop,"commit Warp Sample as Loop"))return;
    const auto after=clip_edit::captureImage(context,7);
    const auto& row=after.at("row");
    const auto& bytes=after.at("children").at("audio_clips").front().at("warp_markers").get_binary();
    const auto map=audio::readWarpMap(std::span(reinterpret_cast<const std::byte*>(bytes.data()),bytes.size()),44100);
    check(map && map.markers==std::vector<WarpMarker>{{0,0},{44100,length}} && row.at("loop_enabled")==1 &&
          row.at("loop_start_ticks")==0 && row.at("loop_len_ticks")==length && row.at("content_offset_ticks")==0 &&
          row.at("pos_ticks")==before.at("row").at("pos_ticks") && row.at("length_ticks")==before.at("row").at("length_ticks"),
          "the full sample fits the requested loop while its Arrangement position and length are retained");
    History history(*f.store);
    check(history.undo().ok && clip_edit::captureImage(context,7)==before &&
          history.redo().ok && clip_edit::captureImage(context,7)==after,
          "Undo and Redo restore both captured map and loop state");
}
void modesAndGuards() {
    Fixture f;auto& db=f.store->db();
    for(const auto mode:{audio::WarpMode::Beats,audio::WarpMode::Tones,audio::WarpMode::Texture,audio::WarpMode::RePitch,audio::WarpMode::Complex,audio::WarpMode::ComplexPro}) {
        auto options=f.options();options.mode=mode;
        if(!apply(f,clip_edit::prepareSetWarp(*f.store,7,options,48000),"capture any of the six Warp mode selections"))return;
        check(db.execAndGet("SELECT warp_mode FROM audio_clips").getString()==audio::warpModeName(mode) && History(*f.store).undo().ok,"selected mode is captured and its change undoes");
    }
    check(!clip_edit::prepareSetWarp(*f.store,999,f.options(),48000).ok,"missing clip refuses");
    db.exec("UPDATE clips SET loop_enabled=1,loop_len_ticks=5765760");check(!clip_edit::prepareSetWarp(*f.store,7,f.options(),48000).ok,"source loop refuses the clean-import producer");db.exec("UPDATE clips SET loop_enabled=0");
}
void futureMapRefusesRewrite() {
    Fixture f;auto markers=audio::writeWarpMap(std::vector<WarpMarker>{{0,0},{45334,4*q}},45334);
    StreamHeader header{};std::memcpy(&header,markers.bytes.data(),sizeof header);header.version=2;std::memcpy(markers.bytes.data(),&header,sizeof header);
    {SQLite::Statement write(f.store->db(),"UPDATE audio_clips SET warp_markers=?");write.bind(1,markers.bytes.data(),static_cast<int>(markers.bytes.size()));write.exec();}
    const auto before=digestProject(*f.store).text;
    check(!clip_edit::prepareSetWarp(*f.store,7,f.options(),48000).ok && digestProject(*f.store).text==before,"Auto-Warp refuses to rewrite unknown future marker fields instead of discarding them");
}
void cancellationAndCaptureGuards() {
    Fixture f;const auto original=digestProject(*f.store).text;std::stop_source stop;auto options=f.options();options.stop=stop.get_token();
    auto pending=clip_edit::prepareSetWarp(*f.store,7,options,48000);stop.request_stop();
    check(pending.ok && !clip_edit::commitPrepared(*f.store,pending).ok && digestProject(*f.store).text==original,"cancel after analysis rejects all rows in the captured commit");
    check(!clip_edit::prepareSetWarp(*f.store,7,options,48000).ok,"pre-cancelled Warp edit does no analysis");
    pending=clip_edit::prepareSetWarp(*f.store,7,f.options(),48000);OpRequest rename;rename.opType="track.rename";rename.payload={{"id",1},{"name","New edit"}};
    check(OpJournal(*f.store).commit(rename).ok && !clip_edit::commitPrepared(*f.store,pending).ok,"a journal edit after Warp analysis rejects the stale result");
    pending=clip_edit::prepareSetWarp(*f.store,7,f.options(),48000);const auto before=digestProject(*f.store).text;
    {std::ofstream changed(f.temporary.path()/"source.wav",std::ios::binary|std::ios::app);changed<<"changed";}
    check(pending.ok && !clip_edit::commitPrepared(*f.store,pending).ok && digestProject(*f.store).text==before,"a changed source file after analysis rolls back the entire edit");
}
}
int main() {
    try {
        actionAndHistory();
        replaceClipSource();
        replaceWarpedSource();
        replaceUnwarpedSourcePastStart();
        editedSourceWindow();
        explicitSourceTempo();
        correctWarpedSourceTempo();
        disableWarpOnLoop();
        changePlacedWarpMode();
        turnTransientsIntoMarkers();
        warpFromHere();
        warpWholeSampleAsLoop();
        modesAndGuards();
        futureMapRefusesRewrite();
        cancellationAndCaptureGuards();
    } catch (const std::exception& e) {
        check(false, e.what());
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
