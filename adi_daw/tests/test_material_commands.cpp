// SPDX-License-Identifier: AGPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/material_commands.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/history.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <cstdio>
#include <fstream>
#include <stdexcept>
namespace {
using namespace adi;
int checks=0,failures=0;
void check(bool ok,const char* what){++checks;if(!ok){++failures;std::printf("FAIL %s\n",what);}}
struct Project {
    test::TempDirectory temporary{"material_commands","actions"};
    std::unique_ptr<Store> store;
    Project() {
        StoreError error{};store=Store::create(temporary.path()/"song.adi",error);
        if(!store)throw std::runtime_error(toString(error));
        store->db().exec("INSERT INTO tracks(id,kind,name) VALUES(21,'audio','Audio'),(29,'master','Main')");
        std::vector<float> samples(2048);
        for(std::size_t i=0;i<samples.size();++i)samples[i]=static_cast<float>(i%31)/64;
        audio::WavWriter writer(temporary.path()/"source.wav",48000,2,audio::WavFormat::Float32);
        writer.write(samples.data(),1024);writer.close();
    }
    clip_edit::ImportOptions options()const {
        clip_edit::ImportOptions result;result.source=temporary.path()/"source.wav";result.track=21;
        result.clipId=7;result.mediaId=8;result.at={1,0};result.copyIn=true;return result;
    }
};
void imports() {
    Project p;auto& db=p.store->db();auto options=p.options();
    auto prepared=clip_edit::prepareImportAudio(*p.store,options,48000);
    check(prepared.edit.ok && db.execAndGet("SELECT count(*) FROM media_files").getInt()==0 &&
          db.execAndGet("SELECT count(*) FROM clips").getInt()==0,"import capture hashes and stages without project writes");
    const auto committed=clip_edit::commitMaterial(*p.store,prepared);
    if(!committed.ok)std::printf("import: %s\n",committed.error.c_str());
    check(committed.ok && committed.seqs.size()==2,"media and complete playing clip share one native transaction");
    check(db.execAndGet("SELECT frames FROM media_files WHERE id=8").getInt64()==1024 &&
          db.execAndGet("SELECT sample_rate FROM media_files WHERE id=8").getInt()==48000 &&
          db.execAndGet("SELECT channels FROM media_files WHERE id=8").getInt()==2 &&
          db.execAndGet("SELECT src_len_frames FROM audio_clips").getInt64()==1024,"audio attachment has complete immutable source dimensions");
    const auto path=media::projectFolder(*p.store)/media::pathFromUtf8(db.execAndGet("SELECT rel_path FROM media_files").getString());
    check(std::filesystem::exists(path) && path.parent_path().filename()=="audio","copy-in publishes into the project media folder");
    check(!clip_edit::commitMaterial(*p.store,prepared).ok && std::filesystem::exists(path),"reusing a completed material result cannot delete its committed media");
    check(db.execAndGet("SELECT fade_in_ticks+fade_out_ticks FROM clips").getInt64()==0 &&
          db.execAndGet("SELECT warp_enabled FROM audio_clips").getInt()==0,"clean import defaults to no automatic fades and no unsupported Warp");
    check(History(*p.store).undo().ok && db.execAndGet("SELECT count(*) FROM clips").getInt()==0 &&
          db.execAndGet("SELECT count(*) FROM media_files").getInt()==0 && std::filesystem::exists(path),"undo removes media and clip together while retaining the owned source file");
    check(History(*p.store).redo().ok && db.execAndGet("SELECT count(*) FROM audio_clips").getInt()==1,"redo needs no external source probe");
    options.clipId=9;options.mediaId=10;options.at={0,5765760};options.edgeFades=true;
    auto duplicate=clip_edit::prepareImportAudio(*p.store,options,48000);
    check(duplicate.edit.ok && clip_edit::commitMaterial(*p.store,duplicate).ok &&
          db.execAndGet("SELECT count(*) FROM media_files").getInt()==1 &&
          db.execAndGet("SELECT fade_in_ticks FROM clips WHERE id=9").getInt64()>0,"reimport deduplicates media and explicit edge-fade choice acts");
    options.warp=true;auto unsupported=clip_edit::prepareImportAudio(*p.store,options,48000);
    check(!unsupported.edit.ok && unsupported.edit.error.find("warp renderer")!=std::string::npos,"Warp refuses with the missing backend named");
}
void refusalAndRollback() {
    Project p;auto options=p.options();auto& db=p.store->db();
    auto prepared=clip_edit::prepareImportAudio(*p.store,options,48000);
    OpRequest rename;rename.opType="track.rename";rename.payload={{"id",21},{"name","Newer"}};
    check(OpJournal(*p.store).commit(rename).ok,"intervening project edit");
    const auto stale=clip_edit::commitMaterial(*p.store,prepared);
    check(!stale.ok && stale.stale && db.execAndGet("SELECT count(*) FROM media_files").getInt()==0,"stale import refuses before publication or any media row");
    prepared=clip_edit::prepareImportAudio(*p.store,options,48000);
    {
        auto current=prepared.edit.options.beforeCommit;
        (void)current;
        // Fail after publication, during SQL COMMIT, through a deferred FK.
        db.exec("CREATE TABLE future_deferred(id INTEGER PRIMARY KEY,parent INTEGER REFERENCES tracks(id) DEFERRABLE INITIALLY DEFERRED) STRICT");
        db.exec("CREATE TRIGGER fail_import AFTER INSERT ON clips BEGIN INSERT INTO future_deferred VALUES(new.id,999);END");
        const auto result=clip_edit::commitMaterial(*p.store,prepared);
        check(!result.ok && db.execAndGet("SELECT count(*) FROM media_files").getInt()==0 &&
              db.execAndGet("SELECT count(*) FROM clips").getInt()==0,"late SQL commit failure rolls back every project row");
        bool empty=true;for(const auto& entry:std::filesystem::directory_iterator(p.temporary.path()/"audio")){(void)entry;empty=false;}
        check(empty,"late SQL failure removes only the newly published media");
    }
}
void bounce() {
    Project p;auto& db=p.store->db();auto imported=clip_edit::prepareImportAudio(*p.store,p.options(),48000);
    check(clip_edit::commitMaterial(*p.store,imported).ok,"bounce fixture imports source");
    clip_edit::BounceOptions options;options.job.source={21,engine::OfflineTap::PostFader};
    options.job.beginSample=127;options.job.endSample=639;options.job.session={2,48000,64};
    options.job.encoding.format=audio::WavFormat::Float32;
    options.destination=p.options();options.destination.clipId=10;options.destination.mediaId=11;options.destination.at={1,1000000000};
    auto material=clip_edit::prepareBounce(*p.store,options);
    if(!material.edit.ok)std::printf("bounce: %s\n",material.edit.error.c_str());
    check(material.edit.ok && db.execAndGet("SELECT count(*) FROM clips").getInt()==1,"bounce renders private WAL snapshot without changing the live project");
    const auto result=clip_edit::commitMaterial(*p.store,material);
    if(!result.ok)std::printf("bounce commit: %s\n",result.error.c_str());
    check(result.ok && db.execAndGet("SELECT src_len_frames FROM audio_clips WHERE clip_id=10").getInt64()==512,"bounce prompt publishes and attaches exact render range");
    const auto path=media::projectFolder(*p.store)/media::pathFromUtf8(db.execAndGet("SELECT rel_path FROM media_files WHERE id=11").getString());
    audio::WavReader wave(path);std::vector<float> samples(1024);const auto read=wave.read(samples.data(),512);
    bool exact=read==512;
    for(std::size_t i=0;i<samples.size();++i)exact=exact && samples[i]==static_cast<float>((i+254)%31)/64;
    check(exact,"bounce PCM matches the source range at its requested offset");
    check(History(*p.store).undo().ok && db.execAndGet("SELECT count(*) FROM clips").getInt()==1,"bounce undo restores project before rendered clip");
    std::stop_source stop;stop.request_stop();render::Control control;control.stop=stop.get_token();
    auto cancelled=clip_edit::prepareBounce(*p.store,options,{},control);
    check(!cancelled.edit.ok && db.execAndGet("SELECT count(*) FROM clips").getInt()==1,"cancelled bounce never commits a clip");
}
void bounceInPlaceAndStale() {
    Project p;auto imported=clip_edit::prepareImportAudio(*p.store,p.options(),48000);
    check(clip_edit::commitMaterial(*p.store,imported).ok,"Bounce in Place fixture source");
    clip_edit::BounceOptions options;options.job.source={21,engine::OfflineTap::PostFader};
    options.job.beginSample=127;options.job.endSample=639;options.job.session={2,48000,64};
    options.job.encoding.format=audio::WavFormat::Float32;
    options.destination=p.options();options.destination.clipId=8;options.destination.mediaId=11;options.destination.at={1,2645833};
    options.replace=clip_edit::RangeSelection{{21},{1,2645833},{1,13312500}};
    auto material=clip_edit::prepareBounce(*p.store,options);
    const auto result=clip_edit::commitMaterial(*p.store,material);
    if(!result.ok)std::printf("bounce in place: %s\n",result.error.c_str());
    check(material.edit.ok && result.ok && p.store->db().execAndGet("SELECT count(*) FROM clips").getInt()==3,"Bounce in Place trims crossing material and reserves the caller's next clip ID");
    check(History(*p.store).undo().ok && p.store->db().execAndGet("SELECT count(*) FROM clips").getInt()==1,"Bounce in Place undo restores the source whole");
    options.replace.reset();options.destination.clipId=20;options.destination.mediaId=21;
    bool edited=false;render::Control control;
    control.progress=[&](render::JobId,std::uint64_t,std::uint64_t) {
        if(edited)return;
        edited=true;
        OpRequest rename;rename.opType="track.rename";rename.payload={{"id",21},{"name","Changed during bounce"}};
        if(!OpJournal(*p.store).commit(rename).ok)throw std::runtime_error("concurrent bounce fixture edit");
    };
    auto stale=clip_edit::prepareBounce(*p.store,options,{},control);
    const auto refused=clip_edit::commitMaterial(*p.store,stale);
    check(edited && stale.edit.ok && !refused.ok && refused.stale &&
          p.store->db().execAndGet("SELECT count(*) FROM clips").getInt()==1,"a project edit during render refuses its captured result before publication");
}

}
int main(){try{imports();refusalAndRollback();bounce();bounceInPlaceAndStale();}catch(const std::exception& e){check(false,e.what());}std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
