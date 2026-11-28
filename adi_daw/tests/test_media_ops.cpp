// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/media/media_ops.hpp"
#include "adi/media/blake3.hpp"
#include "adi/media/peaks.hpp"
#include "adi/store.hpp"
#include "adi/history.hpp"
#include "adi/digest.hpp"
#include "adi/engine/session.hpp"
#include "adi/material_commands.hpp"
#include "adi/render/freeze_capture.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/audio/decode.hpp"
#include "adi/audio/io_audit.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <cstdio>
#include <fstream>
#include <array>
#include <algorithm>
namespace {
using namespace adi;
namespace fs = std::filesystem;
int checks = 0, failures = 0;
void check(bool ok, const char* name) { ++checks; if (!ok) { ++failures; std::printf("FAIL %s\n", name); } }
void write(const fs::path& p, const std::string& bytes) { std::ofstream out(p, std::ios::binary); out << bytes; if (!out) throw std::runtime_error("fixture write"); }
struct Fixture {
    adi::test::TempDirectory temp{"media_ops", "project"};
    std::unique_ptr<Store> store;
    Fixture() {
        fs::create_directory(temp.path() / "project");
        StoreError error; store = Store::createEmpty(temp.path() / "project" / "song.adi", error);
        if (!store) throw std::runtime_error("fixture create");
        store->db().exec("INSERT INTO project(id,name) VALUES(1,'Media')");
    }
    fs::path folder() const { return temp.path() / "project"; }
};
CommitResult op(Store& store, const char* type, Payload payload) {
    OpRequest r; r.opType = type; r.payload = std::move(payload); return OpJournal(store).commit(r);
}
void registry() {
    for (const char* name : {"media.import", "media.unlink", "media.relink"}) {
        const auto* d = OpRegistry::instance().find(name);
        check(d && d->apply && d->buildInverse && d->scope == Scope::Edit && !d->ephemeral, "media op registered with inverse");
    }
    check(!OpRegistry::instance().find("media.embed") && !OpRegistry::instance().find("media.extract"), "retired embedding ops never implemented");
}
void operations() {
    Fixture f; auto& s = *f.store;
    write(f.folder() / "source.bin", "first media content");
    const auto cwd = fs::current_path();
    struct Restore { fs::path p; ~Restore() { fs::current_path(p); } } restore{cwd};
    fs::current_path(f.temp.path());
    check(media::importMedia(s, "source.bin", 10, 123456).ok, "import hashes a project-relative input path");
    const auto original = media::mediaRow(s.db(), 10);
    check(original.at("rel_path") == "source.bin" && original.at("abs_path_hint").is_null(), "relative path is computed against adi folder, not cwd");
    check(original.at("hash_blake3") == media::blake3File(f.folder() / "source.bin").hex, "import stores actual BLAKE3");
    check(original.at("imported_utc") == 123456 && original.at("size_bytes") == 19, "import captures supplied time and measured size");
    check(s.db().execAndGet("SELECT embedded FROM media_files WHERE id=10").getInt() == 0, "import always referenced");
    const auto duplicate = media::importMedia(s, f.folder() / "source.bin", 11, 123457);
    check(duplicate.ok && duplicate.existing && duplicate.mediaId == 10, "same hash deduplicates to existing id");
    check(s.db().execAndGet("SELECT COUNT(*) FROM ops").getInt() == 1, "dedup creates no destructive undo entry");
    check(!media::importMedia(s, "absent", 12, 0).ok, "missing import reported");
    check(s.db().execAndGet("SELECT COUNT(*) FROM media_files").getInt() == 1, "failed import leaves no row");
    fs::rename(f.folder() / "source.bin", f.folder() / "moved.bin");
    check(media::relinkMedia(s, 10, "moved.bin").ok, "relink accepts moved identical content");
    check(media::mediaRow(s.db(), 10).at("rel_path") == "moved.bin", "relink updates location with same id");
    History history(s);
    check(history.undo().ok && media::mediaRow(s.db(), 10) == original, "relink inverse restores previous paths despite absent old file");
    check(history.redo().ok && media::mediaRow(s.db(), 10).at("rel_path") == "moved.bin", "relink redo applies captured paths");
    write(f.folder() / "wrong.bin", "different content");
    check(!media::relinkMedia(s, 10, "wrong.bin").ok, "relink refuses hash mismatch");
    check(media::mediaRow(s.db(), 10).at("rel_path") == "moved.bin", "failed relink leaves paths intact");
    s.db().exec("UPDATE media_files SET sample_rate=48000,channels=2,frames=99,duration_ns=42,format='test',bit_depth=24,peaks=X'0001FF',missing=1 WHERE id=10");
    const auto complete = media::mediaRow(s.db(), 10);
    check(op(s, "media.unlink", {{"id", 10}}).ok, "unlink removes pool row");
    check(fs::exists(f.folder() / "moved.bin"), "unlink never deletes media file");
    fs::remove(f.folder() / "moved.bin");
    check(history.undo().ok && media::mediaRow(s.db(), 10) == complete, "unlink inverse restores every metadata column and peaks without disk IO");
    check(history.redo().ok && s.db().execAndGet("SELECT COUNT(*) FROM media_files").getInt() == 0, "unlink redo");
    check(history.undo().ok, "restore for refusals");
    s.db().exec("INSERT INTO tracks(id,kind,name,freeze_media_id) VALUES(1,'audio','Frozen',10)");
    check(!op(s, "media.unlink", {{"id", 10}}).ok, "unlink refuses freeze references instead of losing inverse state");
    check(s.db().execAndGet("SELECT freeze_media_id FROM tracks WHERE id=1").getInt() == 10, "refused unlink preserves reference");
    check(!op(s, "media.relink", {{"id", 10}, {"hash", std::string(64,'0')}, {"paths", {{"rel_path", "wrong"}, {"abs_path_hint", nullptr}, {"missing", 0}}}}).ok, "raw relink cannot change content identity");
    check(!op(s, "media.import", {{"id", 99}, {"row", {{"hash_blake3", "bad"}}}}).ok, "malformed captured row rejected");
    check(!op(s, "media.unlink", {{"id", 999}}).ok, "unlink absent row fails transactionally");
}
void replay() {
    Fixture a, b;
    write(a.folder() / "a", "replay contents");
    check(media::importMedia(*a.store, "a", 20, 77).ok, "replay setup import");
    write(a.folder() / "b", "replay contents");
    check(media::relinkMedia(*a.store, 20, "b").ok, "replay setup relink");
    check(op(*a.store, "media.unlink", {{"id",20}}).ok, "replay setup unlink");
    fs::remove(a.folder() / "a"); fs::remove(a.folder() / "b");
    SQLite::Statement rows(a.store->db(), "SELECT op_type,payload FROM ops ORDER BY seq");
    bool applied = true;
    while (rows.executeStep()) {
        const auto data = rows.getColumn(1); std::string error;
        auto payload = decodeFromLog({static_cast<const std::byte*>(data.getBlob()), static_cast<std::size_t>(data.getBytes())}, error);
        if (!payload) { applied = false; break; }
        OpRequest request; request.opType = rows.getColumn(0).getString(); request.payload = *payload; request.actor = Actor::Script;
        applied &= OpJournal(*b.store).commit(request).ok;
    }
    check(applied, "persisted CBOR replays all three ops with source files removed");
    check(digestProject(*a.store).text == digestProject(*b.store).text, "replayed project digest matches");
    History ha(*a.store), hb(*b.store);
    check(ha.undo().ok && hb.undo().ok, "both replay histories undo unlink");
    check(media::mediaRow(a.store->db(),20) == media::mediaRow(b.store->db(),20), "replay captures identical full inverse row");
    check(ha.undo().ok && hb.undo().ok && ha.undo().ok && hb.undo().ok, "both histories undo relink and import without files");
}
OpRequest request(const char* type,Payload payload) {
    OpRequest r;r.opType=type;r.payload=std::move(payload);return r;
}
void batchRelink() {
    Fixture f;auto& s=*f.store;
    s.db().exec("INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(1,'audio','Audio',0),(9,'master','Main',1)");
    for(int i=0;i<2;++i) {
        const auto name=std::string(i==0?"first.wav":"second.wav");
        {audio::WavWriter wave(f.folder()/name,48000,1,audio::WavFormat::Float32);
            std::array<float,320> samples{};samples.fill(i==0?0.25f:0.125f);wave.write(samples.data(),320);}
        OpRequest imported;
        const auto captured=media::prepareImport(s,name,10+i,123,imported);
        if(!captured.ok)throw std::runtime_error(captured.error);
        const std::vector<OpRequest> initial{imported,
            request("clip.create",{{"id",5+i},{"track",1},{"kind","audio"},{"pos",0},{"length",5765760}}),
            request("clip.attachAudio",{{"id",5+i},{"media",10+i},{"frames",320}})};
        if(!OpJournal(s).commit(initial).ok)throw std::runtime_error("batch relink audio fixture");
    }
    s.db().exec("UPDATE media_files SET missing=1 WHERE id=11");
    const std::array<Payload,2> beforeRows{media::mediaRow(s.db(),10),media::mediaRow(s.db(),11)};
    const auto before=digestProject(s).text;
    const auto opCount=s.db().execAndGet("SELECT count(*) FROM ops").getInt64();
    fs::create_directory(f.folder()/"found");
    fs::rename(f.folder()/"first.wav",f.folder()/"found/renamed.wav");
    fs::rename(f.folder()/"second.wav",f.folder()/"found/second.wav");
    fs::create_directory(f.folder()/"wrong");
    fs::copy_file(f.folder()/"found/renamed.wav",f.folder()/"wrong/second.wav");
    check(fs::file_size(f.folder()/"wrong/second.wav")==fs::file_size(f.folder()/"found/second.wav"),
          "wrong-content candidate has the same basename and byte size as the missing source");
    const std::array<media::RelinkTarget,2> targets{{{10,"found/renamed.wav"},{11,"found/second.wav"}}};
    const auto unchanged=[&] {return digestProject(s).text==before &&
        s.db().execAndGet("SELECT count(*) FROM ops").getInt64()==opCount;};
    auto invalid=targets;invalid[1].path="wrong/second.wav";
    check(!media::relinkMediaBatch(s,invalid).ok && unchanged(),
          "a later wrong-content match refuses all earlier verified paths and writes no journal rows");
    invalid=targets;invalid[1].path="absent.wav";
    check(!media::relinkMediaBatch(s,invalid).ok && unchanged(),"one unreadable candidate refuses the entire relink batch");
    invalid=targets;invalid[1].mediaId=999;
    check(!media::relinkMediaBatch(s,invalid).ok && unchanged(),"one missing media id refuses the entire relink batch");
    invalid=targets;invalid[1]=invalid[0];
    check(!media::relinkMediaBatch(s,invalid).ok && unchanged(),"duplicate media ids refuse rather than relink one identity twice");
    check(!media::relinkMediaBatch(s,{}).ok && unchanged(),"empty selection writes no undo step");
    {StoreError error{};auto readOnly=Store::open(f.folder()/"song.adi",error,true);
        check(readOnly && !media::relinkMediaBatch(*readOnly,targets).ok && unchanged(),"read-only project refuses batch relink");}
    {audio::CallbackScope callback;
        check(!media::relinkMediaBatch(s,targets).ok,"batch relink refuses the audio callback");
        check(!media::relinkMedia(s,10,targets[0].path).ok,"single relink refuses before copying a path on the audio callback");}
    check(unchanged(),"callback refusal preserves rows and journal");
    s.db().exec("CREATE TEMP TRIGGER refuse_second_relink BEFORE UPDATE OF rel_path ON media_files "
                "WHEN NEW.id=11 BEGIN SELECT RAISE(ABORT,'intentional second relink failure'); END");
    const auto refused=media::relinkMediaBatch(s,targets);
    check(!refused.ok && refused.error.find("intentional second relink failure")!=std::string::npos && unchanged(),
          "second journal apply failure rolls back the first relink and its log");
    s.db().exec("DROP TRIGGER refuse_second_relink");
    const auto result=media::relinkMediaBatch(s,targets,Actor::Script);
    check(result.ok && result.mediaId==0,"two exact content matches commit through the public batch API");
    if(!result.ok){std::printf("batch relink error: %s\n",result.error.c_str());return;}
    History history(s);
    check(history.nextUndo() && history.nextUndo()->opCount==2,"all matched media paths form one Undo step");
    check(s.db().execAndGet("SELECT count(DISTINCT txn_id) FROM ops WHERE op_type='media.relink'").getInt()==1 &&
          s.db().execAndGet("SELECT count(*) FROM ops WHERE op_type='media.relink' AND actor='script'").getInt()==2,
          "batch stores ordinary media.relink payloads under one transaction and the supplied actor");
    for(std::size_t i=0;i<targets.size();++i) {
        auto expected=beforeRows[i];expected["rel_path"]=media::pathUtf8(targets[i].path);
        expected["abs_path_hint"]=nullptr;expected["missing"]=0;
        check(media::mediaRow(s.db(),targets[i].mediaId)==expected,"relink preserves identity, audio metadata, peaks and import time");
    }
    check(s.db().execAndGet("SELECT count(*) FROM audio_clips WHERE (clip_id=5 AND media_id=10) OR (clip_id=6 AND media_id=11)").getInt()==2,
          "the two clips retain their media identities and attachments");
    const auto plays=[&](Store& store) {
        engine::Session session;engine::SessionSpec spec;spec.sampleRate=48000;spec.maxFrames=64;
        if(!session.load(store,{},spec))return false;
        session.graph().setFadeFrames(0);session.transport().play();
        if(!session.clips() || !session.clips()->problems().empty() || !session.clips()->prime())return false;
        std::array<float,64> left{},right{};float* out[]{left.data(),right.data()};
        engine::AudioIo io;io.out=out;io.numOut=2;io.frames=64;session.process(io);
        return left==right && std::all_of(left.begin(),left.end(),[](float value){return value==0.375f;});
    };
    check(plays(s),"both relinked WAVs produce their exact summed PCM through Session");
    const auto after=digestProject(s).text;
    check(history.undo().ok && digestProject(s).text==before,"one Undo restores every previous media location despite absent old files");
    fs::rename(f.folder()/"found",f.folder()/"temporarily-unavailable");
    check(history.redo().ok && digestProject(s).text==after,"one Redo replays all captured paths without reading the files");
    fs::rename(f.folder()/"temporarily-unavailable",f.folder()/"found");
    f.store.reset();StoreError error{};f.store=Store::open(f.folder()/"song.adi",error);
    check(f.store && digestProject(*f.store).text==after && plays(*f.store),"batch relink and exact PCM survive project close/reopen");
    if(f.store) {
        History reopened(*f.store);
        check(reopened.undo().ok && digestProject(*f.store).text==before && reopened.redo().ok && digestProject(*f.store).text==after,
              "the reopened project retains the same atomic relink Undo/Redo");
    }
}
void importClip() {
    Fixture f;auto& s=*f.store;
    s.db().exec("INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(1,'audio','Audio',0),(9,'master','Main',1)");
    const auto path=f.folder()/"sound.bin";
    std::array<float,320> signal{};
    for(std::size_t i=0;i<signal.size();++i)signal[i]=static_cast<float>(i%31)/64;
    {audio::WavWriter w(path,48000,1,audio::WavFormat::Pcm24);w.write(signal.data(),320);}
    OpRequest captured;
    const auto prepared=media::prepareImport(s,path,10,123,captured);
    check(prepared.ok && prepared.audio && prepared.audio->sampleRate==48000 &&
          prepared.audio->channels==1 && prepared.audio->frames==320,"capture probes actual audio despite its extension");
    check(captured.opType=="media.import" && s.db().execAndGet("SELECT count(*) FROM media_files").getInt()==0 &&
          s.db().execAndGet("SELECT count(*) FROM ops").getInt()==0,"prepareImport writes no project rows or undo entry");
    if(!prepared.ok)return;
    const auto& row=captured.payload.at("row");
    check(row.at("sample_rate")==48000 && row.at("channels")==1 && row.at("frames")==320 &&
          row.at("duration_ns")==6666666 && row.at("format")=="wav" && row.at("bit_depth")==24,
          "captured metadata uses source frames and rate with exact integer duration");
    std::vector<OpRequest> batch{captured,
        request("clip.create",{{"id",5},{"track",1},{"kind","audio"},{"name","Sound"},{"pos",0},{"length",5765760}}),
        request("clip.attachAudio",{{"id",5},{"media",10},{"frames",320}})};
    fs::rename(path,f.folder()/"hidden");
    const auto committed=OpJournal(s).commit(batch);
    check(committed.ok,"media, clip and attachment commit together without reading the captured file");
    fs::rename(f.folder()/"hidden",path);
    if(!committed.ok)return;
    History h(s);
    check(h.nextUndo() && h.nextUndo()->opCount==3,"the imported playing clip is one undo step");
    check(s.db().execAndGet("SELECT count(*) FROM audio_clips WHERE clip_id=5 AND src_start_frames=0 "
          "AND src_len_frames=320 AND warp_enabled=0 AND warp_mode='none'").getInt()==1,"import is unwarped with an exact source window");
    {
        engine::Session session;engine::SessionSpec spec;spec.sampleRate=48000;spec.maxFrames=64;
        check(session.load(s,{},spec),"a clip imported only through the public APIs loads for playback");
        session.transport().play();check(session.clips()->prime(),"imported audio primes its worker");
        std::array<float,64> left{},right{};float* out[]{left.data(),right.data()};
        engine::AudioIo io;io.out=out;io.numOut=2;io.frames=64;session.process(io);
        check(std::equal(left.begin(),left.end(),signal.begin()) && left==right,"the imported mono audio actually plays, in both output channels");
    }
    const auto before=digestProject(s).text;
    check(op(s,"clip.delete",{{"id",5}}).ok && h.undo().ok && digestProject(s).text==before,
          "Delete then Undo restores a playing audio attachment");
    s.db().exec("UPDATE audio_clips SET src_start_frames=3,src_len_frames=200,warp_enabled=1,warp_mode='beats',"
                "warp_markers=X'0102',transpose_semis=2.5,formant_shift=-1.25,reverse=1,channel_mode=3 WHERE clip_id=5");
    const auto edited=digestProject(s).text;
    check(!op(s,"clip.detachAudio",{{"id",5}}).ok && digestProject(s).text==edited,"detach refuses lossy removal of edited audio");
    check(op(s,"clip.delete",{{"id",5}}).ok && h.undo().ok && digestProject(s).text==edited,
          "Delete's inverse captures every audio property, including warp-marker bytes");
    s.db().exec("UPDATE audio_clips SET warp_markers=X'' WHERE clip_id=5");
    check(op(s,"clip.delete",{{"id",5}}).ok && h.undo().ok &&
          s.db().execAndGet("SELECT typeof(warp_markers)='blob' AND length(warp_markers)=0 FROM audio_clips WHERE clip_id=5").getInt()==1,
          "empty warp bytes remain an empty blob, distinct from NULL");
    // The preceding delete/undo leaves the imported batch at the head.
    check(h.undo().ok && s.db().execAndGet("SELECT count(*) FROM clips").getInt()==0 &&
          s.db().execAndGet("SELECT count(*) FROM audio_clips").getInt()==0 &&
          s.db().execAndGet("SELECT count(*) FROM media_files").getInt()==0,"one Undo removes the imported clip, attachment and new pool row");
    fs::remove(path);
    check(h.redo().ok && s.db().execAndGet("SELECT count(*) FROM audio_clips").getInt()==1,
          "Redo restores captured audio without a source file");
}
void movedProjectKeepsExternalSample() {
    Fixture f;
    const auto sample = f.temp.path() / "Library" / "external.wav";
    fs::create_directory(sample.parent_path());
    const auto makeWave = [](const fs::path& path, float value) {
        audio::WavWriter writer(path, 48000, 1, audio::WavFormat::Float32);
        std::array<float, 320> samples{};
        samples.fill(value);
        writer.write(samples.data(), 320);
    };
    makeWave(sample, 0.25f);

    f.store->db().exec("INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(1,'audio','Audio',0),(9,'master','Main',1)");
    OpRequest imported;
    const auto prepared = media::prepareImport(*f.store, sample, 10, 123, imported);
    if (!prepared.ok) throw std::runtime_error(prepared.error);
    const std::vector<OpRequest> ops{imported,
        request("clip.create", {{"id",5},{"track",1},{"kind","audio"},{"pos",0},{"length",5765760}}),
        request("clip.attachAudio", {{"id",5},{"media",10},{"frames",320}})};
    if (!OpJournal(*f.store).commit(ops).ok) throw std::runtime_error("external sample fixture");
    const auto captured = media::mediaRow(f.store->db(), 10);
    check(captured.at("rel_path") == "../Library/external.wav" &&
          captured.at("abs_path_hint") == media::pathUtf8(fs::canonical(sample)),
          "same-drive external sample captures both its relative and original location");

    const auto relocated = f.temp.path() / "Moved" / "Set" / "song.adi";
    fs::create_directories(relocated.parent_path());
    f.store.reset();
    fs::rename(f.folder() / "song.adi", relocated);
    StoreError storeError{};
    auto reopened = Store::open(relocated, storeError);
    if (!reopened) throw std::runtime_error("open relocated project");
    const auto plays = [&] {
        engine::Session session;
        engine::SessionSpec spec;
        spec.sampleRate = 48000;
        spec.maxFrames = 64;
        if (!session.load(*reopened, {}, spec)) return false;
        session.graph().setFadeFrames(0);
        session.transport().play();
        if (!session.clips() || !session.clips()->problems().empty() || !session.clips()->prime()) return false;
        std::array<float, 64> left{}, right{};
        float* output[]{left.data(), right.data()};
        engine::AudioIo io;
        io.out = output;
        io.numOut = 2;
        io.frames = 64;
        session.process(io);
        return left == right && std::all_of(left.begin(), left.end(), [](float value) { return value == 0.25f; });
    };
    check(plays(), "moving only the project file still plays the original external sample");

    const auto displaced = relocated.parent_path().parent_path() / "Library" / "external.wav";
    fs::create_directory(displaced.parent_path());
    makeWave(displaced, 0.5f);
    check(plays(), "a different file at the relocated relative path cannot shadow the original sample");
    std::string error;
    const auto resolved = media::resolveMedia(media::projectFolder(*reopened), media::mediaRow(reopened->db(), 10), error);
    check(!resolved.empty() && fs::equivalent(resolved, sample), "verified media lookup selects the original contents");
    const auto peaks = media::buildMediaPeaks(*reopened, 10, 16, {}, nullptr, nullptr);
    check(peaks && !peaks.data->bins.empty() && peaks.data->bins.front().maximum == 0.25f,
          "waveform lookup reads the same sample as playback");
    {
        clip_edit::BounceOptions bounce;
        bounce.job.source = {1, engine::OfflineTap::PostFader};
        bounce.job.endSample = 64;
        bounce.job.session = {2, 48000, 64};
        bounce.destination.track = 1;
        bounce.destination.clipId = 6;
        bounce.destination.mediaId = 11;
        check(clip_edit::prepareBounce(*reopened, bounce).edit.ok,
              "bounce's private snapshot retains the original media fallback");
    }
    {
        render::FreezeCaptureSpec freeze;
        freeze.tracks = {1};
        freeze.endSample = 64;
        freeze.session = {2, 48000, 64};
        const auto capture = render::captureFreezeAudio(*reopened, freeze);
        check(capture.ready(), "freeze verifies and pins the media that playback resolved");
    }

    fs::remove(displaced);
    fs::rename(sample, displaced);
    check(plays(), "the relative path works when the project and its sample are moved together");
    fs::rename(displaced, sample);

    const auto copied = relocated.parent_path().parent_path() / "copy.adi";
    if (!reopened->saveCopy(copied, StoreFileCollision::Fail, StoreCopyPolicy::shareWithUndo).ok) throw std::runtime_error("copy relocated project");
    auto copy = Store::open(copied, storeError);
    if (!copy) throw std::runtime_error("open copied project");
    History copyHistory(*copy);
    check(copyHistory.undo().ok && copyHistory.redo().ok &&
          media::mediaRow(copy->db(), 10).at("rel_path") == "../Library/external.wav",
          "Save a Copy rebases to the available source after a manual move");

    const auto newSource = sample.parent_path() / "renamed.wav";
    fs::rename(sample, newSource);
    check(media::relinkMedia(*reopened, 10, newSource).ok &&
          media::mediaRow(reopened->db(), 10).at("abs_path_hint") == media::pathUtf8(fs::canonical(newSource)),
          "relink captures a new original location");
}
void captureDecodeAndDedup() {
    Fixture f;audio::DecodeCache cache(f.temp.path()/"decoded",{1024*1024,0});
    OpRequest captured;
    const auto prepared=media::prepareImport(*f.store,fs::path(ADI_DECODE_FIXTURES)/"tone24.flac",10,1,captured,&cache);
    audio::WavReader reference(fs::path(ADI_DECODE_FIXTURES)/"src24.wav");
    check(prepared.ok && prepared.audio && prepared.audio->sampleRate==reference.sampleRate() &&
          prepared.audio->channels==reference.channels() && prepared.audio->frames==static_cast<std::int64_t>(reference.frames()),
          "compressed capture reports decoded frames, rate and channels");
    check(prepared.ok && captured.payload.at("row").at("format")=="flac" &&
          captured.payload.at("row").at("bit_depth").is_null(),"compressed source format stays FLAC without inventing its original bit depth");
    const auto entries=cache.entries();
    check(entries.size()==1 && entries[0].pins==0,"metadata capture releases its decode-cache pin");
    OpRequest corrupt;
    check(!media::prepareImport(*f.store,fs::path(ADI_DECODE_FIXTURES)/"truncated.flac",11,1,corrupt,&cache).ok && corrupt.opType.empty(),
          "corrupt known audio fails without a prepared op");
    const auto source=f.folder()/"sample.wav";
    {audio::WavWriter w(source,48000,1,audio::WavFormat::Float32);float v=0.5f;w.write(&v,1);}
    check(media::importMedia(*f.store,source,20,1).ok,"dedup setup");
    OpRequest duplicate;const auto reused=media::prepareImport(*f.store,source,21,2,duplicate);
    check(reused.ok && reused.existing && reused.mediaId==20 && reused.audio && reused.audio->frames==1 && duplicate.opType.empty(),
          "reusing healthy media still returns audio dimensions and needs no import op");
    fs::rename(source,f.folder()/"moved.wav");
    const auto relocated=media::prepareImport(*f.store,f.folder()/"moved.wav",22,3,duplicate);
    check(relocated.ok && relocated.existing && duplicate.opType=="media.relink" &&
          media::mediaRow(f.store->db(),20).at("rel_path")=="sample.wav","reusing moved media captures a relink without applying it");
    check(OpJournal(*f.store).commit(duplicate).ok && media::mediaRow(f.store->db(),20).at("rel_path")=="moved.wav",
          "the captured relink repairs playback paths to matching imported content");
    const auto preserved=duplicate.payload;bool callbackRefused=false;
    {audio::CallbackScope scope;callbackRefused=!media::prepareImport(*f.store,source,23,4,duplicate).ok;}
    check(callbackRefused && duplicate.opType=="media.relink" && duplicate.payload==preserved,
          "capture refuses the callback before file access or clearing caller-owned requests");
}
void atomicAttachmentFailure() {
    Fixture f;const auto source=f.folder()/"sample.wav";
    {audio::WavWriter w(source,48000,1,audio::WavFormat::Pcm16);float v=0.5f;w.write(&v,1);}
    f.store->db().exec("INSERT INTO tracks(id,kind,name) VALUES(1,'audio','Audio')");
    OpRequest captured;
    check(media::prepareImport(*f.store,source,10,1,captured).ok,"prepare failing-batch fixture");
    const std::vector<OpRequest> batch{captured,
        request("clip.create",{{"id",5},{"track",1},{"kind","audio"},{"pos",0},{"length",1}}),
        request("clip.attachAudio",{{"id",5},{"media",10},{"frames",2}})};
    check(!OpJournal(*f.store).commit(batch).ok && !History(*f.store).canUndo() &&
          f.store->db().execAndGet("SELECT count(*) FROM media_files").getInt()==0 &&
          f.store->db().execAndGet("SELECT count(*) FROM clips").getInt()==0,"a failed attachment rolls back the entire import and history");
    check(OpJournal(*f.store).commit(captured).ok &&
          op(*f.store,"clip.create",{{"id",5},{"track",1},{"kind","audio"},{"pos",0},{"length",1}}).ok &&
          op(*f.store,"clip.create",{{"id",6},{"track",1},{"kind","midi"},{"pos",0},{"length",1}}).ok,"attachment refusal fixtures");
    const auto before=digestProject(*f.store).text;
    for(const auto& bad:std::array<Payload,4>{
        Payload{{"id",999},{"media",10},{"frames",1}},Payload{{"id",6},{"media",10},{"frames",1}},
        Payload{{"id",5},{"media",999},{"frames",1}},Payload{{"id",5},{"media",10},{"frames",0}}})
        check(!op(*f.store,"clip.attachAudio",bad).ok && digestProject(*f.store).text==before,
              "missing/non-audio clips, missing media and empty windows refuse without changing the project");
    check(op(*f.store,"clip.attachAudio",{{"id",5},{"media",10},{"frames",1}}).ok,"a valid source window attaches");
    const auto attached=digestProject(*f.store).text;
    check(!op(*f.store,"clip.attachAudio",{{"id",5},{"media",10},{"frames",1}}).ok && digestProject(*f.store).text==attached,
          "a second attachment cannot replace the first");
    check(op(*f.store,"clip.detachAudio",{{"id",5}}).ok && History(*f.store).undo().ok && digestProject(*f.store).text==attached,
          "an unedited attachment detaches and undoes exactly");
}
}
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    try { registry(); operations(); replay(); batchRelink(); importClip(); movedProjectKeepsExternalSample(); captureDecodeAndDedup(); atomicAttachmentFailure(); } catch (const std::exception& e) { check(false, e.what()); }
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
