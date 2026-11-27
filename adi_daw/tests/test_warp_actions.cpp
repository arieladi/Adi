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
#include "adi/row_capture.hpp"
#include "adi/store.hpp"
#include "adi/textproj.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
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
int main(){try{actionAndHistory();replaceClipSource();editedSourceWindow();explicitSourceTempo();correctWarpedSourceTempo();disableWarpOnLoop();changePlacedWarpMode();modesAndGuards();futureMapRefusesRewrite();cancellationAndCaptureGuards();}catch(const std::exception& e){check(false,e.what());}std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
