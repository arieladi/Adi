// SPDX-License-Identifier: AGPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/warp_commands.hpp"
#include "adi/material_commands.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/digest.hpp"
#include "adi/engine/session.hpp"
#include "adi/history.hpp"
#include "adi/store.hpp"
#include "adi/textproj.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
namespace {
using namespace adi;using namespace adi::engine;constexpr std::int64_t q=textproj::kPPQ;
int checks=0,failures=0;
void check(bool ok,const char* what){++checks;if(!ok){++failures;std::printf("FAIL %s\n",what);}}
struct Fixture {
    test::TempDirectory temporary{"warp_actions","project"};std::unique_ptr<Store> store;
    Fixture() {
        StoreError error{};store=Store::createEmpty(temporary.path()/"song.adi",error);if(!store)throw std::runtime_error(toString(error));
        store->db().exec("INSERT INTO tracks(id,kind,name) VALUES(1,'audio','Warp'),(9,'master','Main')");
        std::vector<float> input(44100+1234,0);for(std::size_t beat=0;beat<4;++beat)input[1234+11025*beat]=1;
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
void modesAndGuards() {
    Fixture f;auto& db=f.store->db();
    for(const auto mode:{audio::WarpMode::Beats,audio::WarpMode::Tones,audio::WarpMode::Texture,audio::WarpMode::RePitch,audio::WarpMode::Complex,audio::WarpMode::ComplexPro}) {
        auto options=f.options();options.mode=mode;
        if(!apply(f,clip_edit::prepareSetWarp(*f.store,7,options,48000),"capture any of the six Warp mode selections"))return;
        check(db.execAndGet("SELECT warp_mode FROM audio_clips").getString()==audio::warpModeName(mode) && History(*f.store).undo().ok,"selected mode is captured and its change undoes");
    }
    check(!clip_edit::prepareSetWarp(*f.store,999,f.options(),48000).ok,"missing clip refuses");
    db.exec("UPDATE clips SET content_offset_ticks=100");check(!clip_edit::prepareSetWarp(*f.store,7,f.options(),48000).ok,"edited source offset refuses with no silent remapping");db.exec("UPDATE clips SET content_offset_ticks=0");
    db.exec("UPDATE clips SET loop_enabled=1,loop_len_ticks=5765760");check(!clip_edit::prepareSetWarp(*f.store,7,f.options(),48000).ok,"source loop refuses the clean-import producer");db.exec("UPDATE clips SET loop_enabled=0");
    db.exec("UPDATE clips SET length_ticks=1000");check(!clip_edit::prepareSetWarp(*f.store,7,f.options(),48000).ok,"trimmed visible extent refuses the clean-import producer");
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
int main(){try{actionAndHistory();modesAndGuards();futureMapRefusesRewrite();cancellationAndCaptureGuards();}catch(const std::exception& e){check(false,e.what());}std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
