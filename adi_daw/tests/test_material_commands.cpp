// SPDX-License-Identifier: AGPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/material_commands.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/blob.hpp"
#include "adi/digest.hpp"
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
          db.execAndGet("SELECT warp_enabled FROM audio_clips").getInt()==0,"clean import defaults to no automatic fades and Warp off");
    check(History(*p.store).undo().ok && db.execAndGet("SELECT count(*) FROM clips").getInt()==0 &&
          db.execAndGet("SELECT count(*) FROM media_files").getInt()==0 && std::filesystem::exists(path),"undo removes media and clip together while retaining the owned source file");
    check(History(*p.store).redo().ok && db.execAndGet("SELECT count(*) FROM audio_clips").getInt()==1,"redo needs no external source probe");
    options.clipId=9;options.mediaId=10;options.at={0,5765760};options.edgeFades=true;
    auto duplicate=clip_edit::prepareImportAudio(*p.store,options,48000);
    check(duplicate.edit.ok && clip_edit::commitMaterial(*p.store,duplicate).ok &&
          db.execAndGet("SELECT count(*) FROM media_files").getInt()==1 &&
          db.execAndGet("SELECT fade_in_ticks FROM clips WHERE id=9").getInt64()>0,"reimport deduplicates media and explicit edge-fade choice acts");
    options.warp=true;auto unsupported=clip_edit::prepareImportAudio(*p.store,options,48000);
    check(!unsupported.edit.ok && unsupported.edit.error.find("source tempo")!=std::string::npos,"a short sample without a measured beat needs an explicit source tempo");
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
        db.exec("DROP TRIGGER fail_import;DROP TABLE future_deferred");
        const auto retry=clip_edit::commitMaterial(*p.store,prepared);
        check(retry.ok && db.execAndGet("SELECT count(*) FROM clips").getInt()==1,
              "same prepared material survives publication and late SQL rollback for retry");
    }
}
void externalCopyOwnership() {
    Project p;auto options=p.options();options.copyIn=false;
    auto external=clip_edit::prepareImportAudio(*p.store,options,48000);
    check(clip_edit::commitMaterial(*p.store,external).ok,"first import references external source explicitly");
    options.copyIn=true;options.clipId=10;options.mediaId=11;
    auto owned=clip_edit::prepareImportAudio(*p.store,options,48000);
    const auto copied=clip_edit::commitMaterial(*p.store,owned);
    const auto row=media::mediaRow(p.store->db(),8);std::string error;
    const auto playable=media::resolveMedia(media::projectFolder(*p.store),row,error);
    check(copied.ok && playable.parent_path().filename()=="audio" &&
          p.store->db().execAndGet("SELECT count(*) FROM media_files").getInt()==1,
          "copy-in rehomes an existing valid external media identity into owned audio");
    check(History(*p.store).undo().ok && p.store->db().execAndGet("SELECT rel_path FROM media_files WHERE id=8").getString()=="source.wav" && std::filesystem::exists(options.source),
          "undo of rehoming restores the still-existing external source path");
    Project changed;auto notCopied=changed.options();notCopied.copyIn=false;
    auto captured=clip_edit::prepareImportAudio(*changed.store,notCopied,48000);
    {std::ofstream writer(notCopied.source,std::ios::binary|std::ios::trunc);writer<<"changed after prepare";}
    const auto refused=clip_edit::commitMaterial(*changed.store,captured);
    check(!refused.ok && changed.store->db().execAndGet("SELECT count(*) FROM media_files").getInt()==0 && std::filesystem::exists(notCopied.source),
          "external material changes refuse native commit without deleting that source");
}
void batchImports() {
    Project p;
    auto &db = p.store->db();
    auto first = p.options();
    first.at = {0, 0};
    first.track = 22;
    first.edgeFades = true;
    auto second = first;
    second.source = p.temporary.path() / "same-bytes.wav";
    std::filesystem::copy_file(first.source, second.source);
    second.clipId = 9;
    second.mediaId = 10;
    auto third = first;
    third.source = p.temporary.path() / "another.wav";
    third.clipId = 11;
    third.mediaId = 12;
    clip_edit::ImportBatch batch;
    batch.expectedHead = OpJournal(*p.store).headSeq();
    batch.consecutive = true;
    batch.imports = {first, second, third};
    OpRequest create;
    create.opType = "track.create";
    create.payload = {{"id", 22}, {"kind", "audio"}, {"name", "Imported"}, {"index", 1}};
    batch.createTracks.push_back(create);
    auto bad = clip_edit::prepareImportAudioBatch(*p.store, batch, 48000);
    check(!bad.edit.ok && bad.refusedImport == 2 && !clip_edit::commitMaterial(*p.store, bad).ok &&
          db.execAndGet("SELECT count(*) FROM tracks WHERE id=22").getInt() == 0 && !History(*p.store).head(),
          "SC-158: a missing last file refuses the complete batch before editing the project");
    {
        std::vector<float> samples(2048, 0.1f);
        audio::WavWriter writer(third.source, 48000, 2, audio::WavFormat::Float32);
        writer.write(samples.data(), 1024);
        writer.close();
    }
    auto ready = clip_edit::prepareImportAudioBatch(*p.store, batch, 48000);
    check(ready.edit.ok, "SC-158: prepare consecutive files, fades, copy and a new track");
    if (!ready.edit.ok) return;
    db.exec("CREATE TABLE batch_deferred(id INTEGER PRIMARY KEY,parent INTEGER REFERENCES tracks(id) DEFERRABLE INITIALLY DEFERRED) STRICT");
    db.exec("CREATE TRIGGER fail_batch AFTER INSERT ON clips WHEN new.id=11 BEGIN INSERT INTO batch_deferred VALUES(new.id,999);END");
    const auto refused = clip_edit::commitMaterial(*p.store, ready);
    check(!refused.ok && std::filesystem::is_empty(p.temporary.path() / "audio") &&
          db.execAndGet("SELECT count(*) FROM tracks WHERE id=22").getInt() == 0 && !History(*p.store).head(),
          "SC-158: deferred SQL refusal retracts both new copied assets and all project rows");
    db.exec("DROP TRIGGER fail_batch;DROP TABLE batch_deferred");
    const auto before = digestProject(*p.store).text;
    const auto landed = clip_edit::commitMaterial(*p.store, ready);
    const auto after = digestProject(*p.store).text;
    check(landed.ok && db.execAndGet("SELECT count(*) FROM media_files").getInt() == 2 &&
          db.execAndGet("SELECT count(*) FROM audio_clips WHERE media_id=8").getInt() == 2 &&
          db.execAndGet("SELECT count(*) FROM clips WHERE track_id=22 AND fade_in_ticks>0 AND fade_out_ticks>0").getInt() == 3 &&
          db.execAndGet("SELECT b.pos_ticks=a.pos_ticks+a.length_ticks AND c.pos_ticks=b.pos_ticks+b.length_ticks "
                        "FROM clips a,clips b,clips c WHERE a.id=7 AND b.id=9 AND c.id=11").getInt() == 1,
          "SC-158: one commit preserves order and fades and deduplicates different files of the same content");
    check(History(*p.store).undo().ok && db.execAndGet("SELECT count(*) FROM clips").getInt() == 0 &&
          db.execAndGet("SELECT count(*) FROM tracks WHERE id=22").getInt() == 0 && !History(*p.store).head() &&
          History(*p.store).redo().ok && db.execAndGet("SELECT count(*) FROM clips WHERE track_id=22").getInt() == 3,
          "SC-158: one Undo and one Redo cover the complete drop");
    // Every stored row, the media pool included, not only the clips and the track.
    check(History(*p.store).undo().ok && digestProject(*p.store).text == before,
          "SC-158: Undo of the drop restores every project row, the media pool included");
    check(History(*p.store).redo().ok && digestProject(*p.store).text == after,
          "SC-158: Redo of the drop repeats every project row");
}
void batchReplacement() {
    Project project;
    auto& db = project.store->db();
    db.exec("INSERT INTO clips(id,track_id,kind,name,pos_ticks,length_ticks) "
            "VALUES(6,21,'midi','Original',0,1000000)");
    auto first = project.options();
    first.at = {0, 200000};
    auto second = first;
    second.clipId = 9;
    second.mediaId = 10;
    clip_edit::ImportBatch batch;
    batch.expectedHead = OpJournal(*project.store).headSeq();
    batch.consecutive = true;
    batch.placementPolicy.emplace();
    batch.imports = {first, second};
    auto prepared = clip_edit::prepareImportAudioBatch(*project.store, batch, 48000);
    const auto applied = clip_edit::commitMaterial(*project.store, prepared);
    if (!applied.ok)
        std::printf("batch replacement: %s\n", applied.error.c_str());
    check(prepared.edit.ok && applied.ok &&
              db.execAndGet("SELECT count(*) FROM clips WHERE track_id=21").getInt() == 4 &&
              db.execAndGet("SELECT length_ticks FROM clips WHERE id=6").getInt64() == 200000 &&
              db.execAndGet("SELECT count(*) FROM clips a,clips b,clips tail WHERE a.id=7 AND b.id=9 "
                            "AND tail.name='Original' AND b.pos_ticks=a.pos_ticks+a.length_ticks "
                            "AND tail.pos_ticks=b.pos_ticks+b.length_ticks "
                            "AND tail.pos_ticks+tail.length_ticks=1000000").getInt() == 1,
          "SC-35/158: consecutive files in one drop clear the same original together and preserve both faces");
    check(applied.ok && History(*project.store).undo().ok &&
              db.execAndGet("SELECT count(*) FROM clips WHERE track_id=21").getInt() == 1 &&
              db.execAndGet("SELECT length_ticks FROM clips WHERE id=6").getInt64() == 1000000,
          "SC-35/158: one Undo restores the whole original after a consecutive replacement");
}
void bounce() {
    Project p;auto& db=p.store->db();auto imported=clip_edit::prepareImportAudio(*p.store,p.options(),48000);
    check(clip_edit::commitMaterial(*p.store,imported).ok,"bounce fixture imports source");
    p.store->db().exec("INSERT INTO automation_lanes(id,owner_kind,owner_id,param_ref,value_domain)"
                       " VALUES(91,'track',21,'volume','real')");
    AutomationPoint point{};point.time=50000;point.value=-6;point.point_id=1;
    const auto automation=writeStream<AutomationPoint>(FourCC::Automation,{&point,1});
    check(p.store->putAutomationData(91,{},automation),"Bounce in Place fixture has track Volume automation");
    clip_edit::BounceOptions options;options.job.source={21,engine::OfflineTap::PreFader};
    options.job.beginSample=127;options.job.endSample=639;options.job.session={2,48000,64};
    options.job.encoding.format=audio::WavFormat::Float32;
    options.destination=p.options();options.destination.clipId=10;options.destination.mediaId=11;options.destination.at={1,1000000000};options.destination.warp=true;
    auto material=clip_edit::prepareBounce(*p.store,options);
    if(!material.edit.ok)std::printf("bounce: %s\n",material.edit.error.c_str());
    check(material.edit.ok && db.execAndGet("SELECT count(*) FROM clips").getInt()==1,"bounce renders private WAL snapshot without changing the live project");
    const auto result=clip_edit::commitMaterial(*p.store,material);
    if(!result.ok)std::printf("bounce commit: %s\n",result.error.c_str());
    check(result.ok && db.execAndGet("SELECT src_len_frames FROM audio_clips WHERE clip_id=10").getInt64()==512 &&
          db.execAndGet("SELECT warp_enabled FROM audio_clips WHERE clip_id=10").getInt()==0,"bounce publishes the exact render range with Warp off despite an import preference");
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
    p.store->db().exec("INSERT INTO automation_lanes(id,owner_kind,owner_id,param_ref,value_domain)"
                       " VALUES(91,'track',21,'volume','real')");
    AutomationPoint point{};point.time=50000;point.value=-6;point.point_id=1;
    const auto automation=writeStream<AutomationPoint>(FourCC::Automation,{&point,1});
    check(p.store->putAutomationData(91,{},automation),"Bounce in Place fixture has track Volume automation");
    clip_edit::BounceOptions options;options.job.source={21,engine::OfflineTap::PreFader};
    options.job.beginSample=127;options.job.endSample=639;options.job.session={2,48000,64};
    options.job.encoding.format=audio::WavFormat::Float32;
    options.destination=p.options();options.destination.clipId=8;options.destination.mediaId=11;options.destination.at={1,2645833};
    options.replace=clip_edit::RangeSelection{{21},{1,2645833},{1,13312500}};
    auto material=clip_edit::prepareBounce(*p.store,options);
    const auto result=clip_edit::commitMaterial(*p.store,material);
    if(!result.ok)std::printf("bounce in place: %s\n",result.error.c_str());
    check(material.edit.ok && result.ok && p.store->db().execAndGet("SELECT count(*) FROM clips").getInt()==3,"Bounce in Place trims crossing material and reserves the caller's next clip ID");
    auto& db = p.store->db();
    check(result.ok &&
              db.execAndGet("SELECT src_len_frames FROM audio_clips WHERE clip_id=8").getInt64()==512 &&
              db.execAndGet("SELECT min(clip_id) FROM audio_clips WHERE clip_id NOT IN (7,8)").getInt64()==9 &&
              db.execAndGet("SELECT count(*) FROM clips WHERE id=7").getInt()==1,
          "the supplied next ID belongs to the bounce and the crossing source uses two other IDs");
    check(p.store->getAutomationData(91,{})==automation,"Bounce in Place leaves the live Volume envelope byte-exact");
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
int main(){try{imports();refusalAndRollback();externalCopyOwnership();batchImports();batchReplacement();bounce();bounceInPlaceAndStale();}catch(const std::exception& e){check(false,e.what());}std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
