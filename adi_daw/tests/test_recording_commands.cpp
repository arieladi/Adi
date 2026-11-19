// SPDX-License-Identifier: AGPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/recording_commands.hpp"
#include "adi/engine/session.hpp"
#include "adi/engine/audio_recording.hpp"
#include "adi/digest.hpp"
#include "adi/history.hpp"
#include "adi/store.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/media/blake3.hpp"
#include "adi/media/publish_file.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <exception>
#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <thread>
namespace {
using namespace adi;using namespace adi::engine;
int checks=0,failures=0;
void check(bool ok,const char* what){++checks;if(!ok){++failures;std::printf("FAIL %s\n",what);}}
AudioRecordingResult finish(AudioInputRecorder& recorder) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(std::chrono::steady_clock::now()<deadline){if(auto result=recorder.poll())return *result;std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    throw std::runtime_error("recording did not finish");
}
struct Fixture {
    test::TempDirectory temporary{"recording_commands","project"};std::unique_ptr<Store> store;
    Fixture(){StoreError error{};store=Store::createEmpty(temporary.path()/"song.adi",error);if(!store)throw std::runtime_error(toString(error));
        store->db().exec("INSERT INTO tracks(id,kind,name,record_armed) VALUES(21,'audio','One',1),(22,'audio','Two',1),(9,'master','Main',0)");}
    static float sample(std::size_t frame){return static_cast<float>(frame%17)/32-0.25f;}
    AudioRecordingResult capture(bool two=false,Precision precision=Precision::Float32,bool loop=false,int totalFrames=300,
        const std::filesystem::path& workFolder={},std::int64_t startSample=512) {
        Session session;SessionSpec format{2,48000,128};format.precision=precision;session.graph().setFadeFrames(0);
        if(!session.load(*store,{},format))throw std::runtime_error(session.error());
        AudioRecordingSpec request;request.workFolder=workFolder.empty()?temporary.path():workFolder;request.sampleRate=48000;request.maxFrames=128;request.ringFrames=4096;request.targets={{21,{0}}};if(two)request.targets.push_back({22,{0}});
        if(loop)request.loop=AudioRecordingLoop{512,640,8};
        std::string error;if(!session.startRecording(request,error))throw std::runtime_error(error);
        if(loop)session.transport().loop(512,640);
        session.transport().locate(startSample);session.transport().play();std::array<float,128> source{};const float* inputs[]={source.data()};std::array<float,256> out{};float* outputs[]={out.data(),out.data()+128};
        std::int64_t stream=0;
        for(int consumed=0;consumed<totalFrames;) {
            const auto count=std::min(128,totalFrames-consumed);consumed+=count;
            for(int frame=0;frame<count;++frame)source[static_cast<std::size_t>(frame)]=sample(static_cast<std::size_t>(stream+frame));
            AudioIo io;io.in=inputs;io.numIn=1;io.out=outputs;io.numOut=2;io.frames=count;io.streamTimeSamples=stream;session.process(io);stream+=count;
        }
        session.recording().stop();AudioIo idle;idle.frames=1;idle.out=outputs;idle.numOut=2;session.process(idle);return finish(session.recording());
    }
    std::vector<double> play(Precision precision) {
        Session session;SessionSpec format{2,48000,128};format.precision=precision;session.graph().setFadeFrames(0);
        if(!session.load(*store,{},format) || !session.problems().empty())throw std::runtime_error("load attached recording");
        session.transport().play();std::array<float,256> narrow{};std::array<double,256> wide{};float* out[]={narrow.data(),narrow.data()+128};double* out64[]={wide.data(),wide.data()+128};std::vector<double> result(1024);
        for(std::size_t first=0;first<result.size();first+=128) {
            if(!session.clips()->prime(std::chrono::seconds(5)))throw std::runtime_error("prime recorded clip");
            AudioIo io;io.frames=128;io.numOut=2;io.out=out;if(precision==Precision::Float64)io.out64=out64;session.process(io);
            for(std::size_t frame=0;frame<128;++frame)result[first+frame]=precision==Precision::Float64?wide[frame]:narrow[frame];
        }
        return result;
    }
};
void publicationStagingOwnership() {
    namespace fs=std::filesystem;
    Fixture f;auto recorded=f.capture();const auto before=digestProject(*f.store).text;
    const auto audioFolder=media::projectFolder(*f.store)/"audio";
    const auto unrelated=audioFolder/".adi-media-unrelated";fs::create_directories(unrelated);
    const auto sentinel=unrelated/"keep.txt";{std::ofstream out(sentinel);out<<"unowned staging";}
    const auto sentinelHash=media::blake3File(sentinel),takeHash=media::blake3File(recorded.tracks[0].file);
    const std::array<recording::ClipIds,1> ids={{{7,8,"Staging"}}};
    std::vector<fs::path> owned;
    {
        const auto prepared=recording::prepareCommit(*f.store,recorded,ids);
        check(prepared.ok && digestProject(*f.store).text==before,"prepare leaves project rows unchanged while staging publication beside project media");
        for(const auto& entry:fs::directory_iterator(audioFolder))
            if(entry.is_directory() && entry.path()!=unrelated && entry.path().filename().string().starts_with(".adi-media-"))owned.push_back(entry.path());
        check(owned.size()==1 && fs::is_regular_file(owned.front()/"asset-0.wav") &&
            media::blake3File(owned.front()/"asset-0.wav").hex==takeHash.hex,
            "the verified publication copy lives in one owned .adi-media- directory on the project filesystem");
        std::size_t captureDirectories=0;
        for(const auto& entry:fs::directory_iterator(recorded.directory()))if(entry.is_directory())++captureDirectories;
        check(captureDirectories==0,"preparation adds no publication staging beneath the recoverable capture directory");
    }
    bool cleaned=true;for(const auto& path:owned)cleaned=cleaned && !fs::exists(path);
    check(cleaned && media::blake3File(recorded.tracks[0].file).hex==takeHash.hex &&
        media::blake3File(sentinel).hex==sentinelHash.hex && digestProject(*f.store).text==before,
        "abandoning preparation removes only its publication directory and preserves the private take and unrelated stage");
    const std::array<recording::ClipIds,1> invalid={{{0,8,"Invalid"}}};
    {const auto refused=recording::prepareCommit(*f.store,recorded,invalid);check(!refused.ok,"invalid recording allocation refuses after reserving its private stage");}
    std::size_t remaining=0;for(const auto& entry:fs::directory_iterator(audioFolder))if(entry.is_directory())++remaining;
    check(remaining==1 && fs::exists(sentinel) && media::blake3File(recorded.tracks[0].file).hex==takeHash.hex,
        "failed preparation cleans its project-side stage without removing foreign directories or the capture");
    recorded.discard();
}
void crossVolumePublication(const std::filesystem::path& workFolder) {
#if defined(_WIN32)
    namespace fs=std::filesystem;
    if(!workFolder.is_absolute() || !fs::is_directory(workFolder))throw std::runtime_error("cross-volume work folder must be an existing absolute directory");
    Fixture f;auto recorded=f.capture(false,Precision::Float32,false,300,workFolder);
    const auto source=recorded.tracks[0].file;
    std::error_code error;
    auto sourceLease=media::ProjectFileLease::openShared(source,error);
    auto projectLease=media::ProjectFileLease::openShared(f.temporary.path()/"song.adi",error);
    if(!sourceLease || !projectLease || sourceLease->identity().volume==projectLease->identity().volume)
        throw std::runtime_error("cross-volume regression requires distinct native capture and project volumes");
    std::printf("CROSS_VOLUME capture=%llu project=%llu\n",static_cast<unsigned long long>(sourceLease->identity().volume),
        static_cast<unsigned long long>(projectLease->identity().volume));
    sourceLease.reset();projectLease.reset();
    const auto control=recorded.directory()/"cross-volume-control.wav",target=f.temporary.path()/"control.wav";
    fs::copy_file(source,control);
    const auto rejected=media::publishFile(control,target,{},media::PublishPolicy::AtomicOnly);
    check(rejected.status==media::PublishStatus::failed && !rejected.usedFallback && fs::exists(control) && !fs::exists(target),
        "real cross-volume AtomicOnly control refuses a direct publication and retains its source");
    fs::remove(control);
    const auto hash=media::blake3File(source);const auto before=digestProject(*f.store);
    const std::array<recording::ClipIds,1> ids={{{7,8,"Other volume"}}};
    auto prepared=recording::prepareCommit(*f.store,recorded,ids);
    check(prepared.ok,"prepare a real recording captured on another native volume");
    const auto committed=recording::commit(*f.store,prepared);
    if(!committed.ok)std::printf("cross-volume commit: %s\n",committed.error.c_str());
    check(committed.ok && committed.seqs.size()==3,"cross-volume capture commits through project-side AtomicOnly publication as one group");
    if(committed.ok) {
        const auto owned=media::projectFolder(*f.store)/media::pathFromUtf8(f.store->db().execAndGet("SELECT rel_path FROM media_files").getString());
        check(owned.parent_path()==media::projectFolder(*f.store)/"audio" && media::blake3File(owned).hex==hash.hex && !fs::exists(recorded.directory()),
            "cross-volume commit preserves exact captured bytes in project audio and discards its private take");
        const auto heard=f.play(Precision::Float32);bool exact=true;
        for(std::size_t frame=0;frame<heard.size();++frame)exact=exact && heard[frame]==(frame>=512 && frame<812?Fixture::sample(frame-512):0);
        check(exact,"cross-volume recording plays its captured PCM at the original sample positions");
        const auto after=digestProject(*f.store).text;History history(*f.store);
        check(history.undo().ok && digestProject(*f.store).text==before.text && fs::exists(owned) &&
            history.redo().ok && digestProject(*f.store).text==after && f.play(Precision::Float32)==heard,
            "cross-volume recording has one undo step and restores the same retained audio on redo");
    } else recorded.discard();
#else
    (void)workFolder;throw std::runtime_error("the opt-in cross-volume native identity regression is Windows-only");
#endif
}
void heardPlacementAndHistory() {
    for(const auto precision:{Precision::Float32,Precision::Float64}) {
        Fixture f;auto recorded=f.capture(false,precision);const auto before=digestProject(*f.store).text;
        const std::array<recording::ClipIds,1> ids={{{7,8,"Take 1"}}};auto prepared=recording::prepareCommit(*f.store,recorded,ids);
        if(!prepared.ok)std::printf("prepare: %s\n",prepared.error.c_str());
        check(prepared.ok && digestProject(*f.store).text==before,"recording attachment captures without project writes");
        const auto committed=recording::commit(*f.store,prepared);if(!committed.ok)std::printf("commit: %s\n",committed.error.c_str());
        check(committed.ok && committed.seqs.size()==3,"recorded media, clip and source attach as one native undo group");
        auto& db=f.store->db();const auto owned=media::projectFolder(*f.store)/media::pathFromUtf8(db.execAndGet("SELECT rel_path FROM media_files").getString());
        check(std::filesystem::exists(owned) && !std::filesystem::exists(recorded.directory()),"successful recording publication retains owned audio and removes only private take staging");
        const auto heard=f.play(precision);bool exact=true;
        for(std::size_t frame=0;frame<heard.size();++frame)exact=exact && heard[frame]==(frame>=512 && frame<812?Fixture::sample(frame-512):0);
        check(exact,"actual recorded input is heard at exact original sample placement and duration in either graph width");
        const auto after=digestProject(*f.store).text;History history(*f.store);
        check(history.undo().ok && digestProject(*f.store).text==before && std::filesystem::exists(owned),"undo removes captured rows while retaining recorded audio for redo");
        check(history.redo().ok && digestProject(*f.store).text==after && f.play(precision)==heard,"redo restores literal media and heard PCM without recording again");
        check(!recording::commit(*f.store,prepared).ok && std::filesystem::exists(owned),"repeat commit cannot remove an already published recording");
        f.store.reset();StoreError opened{};f.store=Store::open(f.temporary.path()/"song.adi",opened);
        check(f.store && digestProject(*f.store).text==after && f.play(precision)==heard,"reopened recording history and owned media restore the same project and heard PCM");
    }
}
void existingMediaAndHistoryPaths() {
    for(const bool sourceAlreadyInPool:{false,true}) {
        Fixture f;auto recorded=f.capture();const auto external=f.temporary.path()/"external.wav";
        std::filesystem::copy_file(recorded.tracks[0].file,external);
        const auto priorPath=sourceAlreadyInPool?recorded.tracks[0].file:external;
        check(media::importMedia(*f.store,priorPath,8,0).ok,"prepare existing media with the recording's content identity");
        const auto before=digestProject(*f.store).text;const std::array<recording::ClipIds,1> ids={{{7,18,"Reuse"}}};auto prepared=recording::prepareCommit(*f.store,recorded,ids);
        check(prepared.ok && recording::commit(*f.store,prepared).ok && f.store->db().execAndGet("SELECT count(*) FROM media_files").getInt()==1,"recording reuses existing media identity while ensuring an owned playable asset");
        std::string error;const auto owned=media::resolveMedia(media::projectFolder(*f.store),media::mediaRow(f.store->db(),8),error);
        check(!owned.empty() && owned.parent_path().filename()=="audio" && std::filesystem::exists(owned),"existing external content is attached through a verified owned recording file");
        check(History(*f.store).undo().ok && digestProject(*f.store).text==before && std::filesystem::exists(priorPath),"undo restores exact prior media paths with their original files still available");
        recorded.discard();
    }
}
bool rollbackReported(const CommitResult& result,const std::filesystem::path& path) {
    return result.error.find("rollback")!=std::string::npos &&
        result.error.find(media::pathUtf8(path))!=std::string::npos;
}
struct PublicationCommitVeto {
    std::filesystem::path published,moved;
    std::optional<media::FileIdentity> originalIdentity,foreignIdentity;
    std::exception_ptr exception;
    unsigned calls=0;
    static constexpr const char* foreignBytes="later writer owns this path\n";
    static int invoke(void* raw)noexcept {
        auto& self=*static_cast<PublicationCommitVeto*>(raw);++self.calls;
        try {
            // This hook executes at SQLite's actual COMMIT, after the journal's
            // beforeCommit publication. Do not execute SQLite from this hook.
            std::error_code error;
            self.originalIdentity=media::readFileIdentity(self.published,error);
            if(!self.originalIdentity || !std::filesystem::is_regular_file(std::filesystem::symlink_status(self.published)))
                throw std::runtime_error("commit hook did not observe the newly published regular WAV");
            std::filesystem::rename(self.published,self.moved);
            {std::ofstream foreign;foreign.exceptions(std::ios::badbit|std::ios::failbit);
                foreign.open(self.published,std::ios::binary);foreign<<foreignBytes;foreign.close();}
            self.foreignIdentity=media::readFileIdentity(self.published,error);
            if(!self.foreignIdentity || self.foreignIdentity==self.originalIdentity)
                throw std::runtime_error("commit hook failed to create a distinct later file identity");
        }catch(...){self.exception=std::current_exception();}
        return 1; // Veto COMMIT even if test setup failed; never unwind through C.
    }
};
struct ScopedPublicationCommitVeto {
    sqlite3* database;
    ScopedPublicationCommitVeto(SQLite::Database& db,PublicationCommitVeto& state):database(db.getHandle()) {
        // Fixture owns this new Store and has installed no previous commit hook.
        sqlite3_commit_hook(database,&PublicationCommitVeto::invoke,&state);
    }
    ~ScopedPublicationCommitVeto(){sqlite3_commit_hook(database,nullptr,nullptr);}
    ScopedPublicationCommitVeto(const ScopedPublicationCommitVeto&)=delete;
    ScopedPublicationCommitVeto& operator=(const ScopedPublicationCommitVeto&)=delete;
};
void rollbackNeverDeletesLaterWriter() {
    namespace fs=std::filesystem;
    Fixture f;auto recorded=f.capture();const auto before=digestProject(*f.store).text;
    const auto privateSource=recorded.tracks[0].file;
    const auto sourceHash=media::blake3File(privateSource);
    const std::array<recording::ClipIds,1> ids={{{7,8,"Late replacement"}}};
    auto prepared=recording::prepareCommit(*f.store,recorded,ids);
    check(prepared.ok,"prepare actual captured WAV for commit-veto identity regression");
    if(!prepared.ok)throw std::runtime_error(prepared.error);
    PublicationCommitVeto veto;
    for(const auto& request:prepared.requests)if(request.opType=="media.import")
        veto.published=media::projectFolder(*f.store)/media::pathFromUtf8(request.payload.at("row").at("rel_path").get<std::string>());
    if(veto.published.empty())throw std::runtime_error("recording fixture has no captured publication path");
    veto.moved=veto.published.parent_path()/"moved-owned-recording.wav";
    const auto reference=f.temporary.path()/"foreign-reference.txt";
    {std::ofstream foreign(reference,std::ios::binary);foreign<<PublicationCommitVeto::foreignBytes;}
    const auto foreignHash=media::blake3File(reference);
    CommitResult failed;
    {ScopedPublicationCommitVeto hook(f.store->db(),veto);failed=recording::commit(*f.store,prepared);}
    check(veto.calls==1 && !veto.exception,"actual SQLite commit hook replaced a published WAV before vetoing once");
    if(veto.exception)std::rethrow_exception(veto.exception);
    check(!failed.ok && digestProject(*f.store).text==before &&
        f.store->db().execAndGet("SELECT count(*) FROM media_files").getInt()==0 &&
        f.store->db().execAndGet("SELECT count(*) FROM clips").getInt()==0,
        "commit veto rolls back all recording/media/journal rows despite completed file publication");
    const auto foreignAfter=media::blake3File(veto.published),movedAfter=media::blake3File(veto.moved);
    check(foreignAfter && foreignAfter.hex==foreignHash.hex,"recording rollback retains exact bytes of the later foreign ordinary file");
    std::error_code foreignError,movedError;
    const auto currentForeign=media::readFileIdentity(veto.published,foreignError);
    const auto currentMoved=media::readFileIdentity(veto.moved,movedError);
    check(currentForeign && currentForeign==veto.foreignIdentity,"recording rollback does not remove or replace the later writer's file identity");
    check(movedAfter && movedAfter.hex==sourceHash.hex && currentMoved && currentMoved==veto.originalIdentity,
        "recording rollback never searches for or deletes its moved original publication");
    check(rollbackReported(failed,veto.published),"failed recording commit reports the retained or changed publication path");
    check(fs::is_regular_file(privateSource) && media::blake3File(privateSource).hex==sourceHash.hex,
        "commit-veto identity conflict preserves the original private capture for recovery");
    recorded.discard();
}

void lateSqlAndOwnedReuse() {
    Fixture f;auto recorded=f.capture();const std::array<recording::ClipIds,1> ids={{{7,8,"One"}}};auto& db=f.store->db();
    db.exec("CREATE TABLE late_parent(id INTEGER PRIMARY KEY);CREATE TABLE late_child(id INTEGER PRIMARY KEY,parent INTEGER REFERENCES late_parent(id) DEFERRABLE INITIALLY DEFERRED);CREATE TRIGGER reject_recording AFTER INSERT ON audio_clips BEGIN INSERT INTO late_child(id,parent) VALUES(NEW.clip_id,999);END");
    const auto before=digestProject(*f.store).text;auto prepared=recording::prepareCommit(*f.store,recorded,ids);auto published=std::filesystem::path{};
    for(const auto& request:prepared.requests)if(request.opType=="media.import")published=media::projectFolder(*f.store)/media::pathFromUtf8(request.payload.at("row").at("rel_path").get<std::string>());
    const auto failed=recording::commit(*f.store,prepared);
    const bool retained=std::filesystem::exists(published);
#if defined(_WIN32)
    const bool cleanupAccepted=!retained;
#else
    // POSIX pathname unlink cannot atomically compare-and-remove an identity.
    const auto sourceHash=media::blake3File(recorded.tracks[0].file);
    const bool cleanupAccepted=retained && sourceHash && media::blake3File(published).hex==sourceHash.hex && rollbackReported(failed,published);
#endif
    check(prepared.ok && !failed.ok && digestProject(*f.store).text==before && cleanupAccepted && std::filesystem::exists(recorded.tracks[0].file),"late SQL failure rolls back rows, safely cleans or reports retained owned publication, and preserves the private take");
    db.exec("DROP TRIGGER reject_recording;DROP TABLE late_child;DROP TABLE late_parent");
    prepared=recording::prepareCommit(*f.store,recorded,ids);const auto retried=recording::commit(*f.store,prepared);check(retried.ok,"retry the same completed take after fixing the SQL failure");
    const auto originalHash=media::blake3File(published);check(History(*f.store).undo().ok && std::filesystem::exists(published),"undo keeps the owned recording available to a later recording branch");
    auto repeated=f.capture();db.exec("CREATE TABLE late_parent(id INTEGER PRIMARY KEY);CREATE TABLE late_child(id INTEGER PRIMARY KEY,parent INTEGER REFERENCES late_parent(id) DEFERRABLE INITIALLY DEFERRED);CREATE TRIGGER reject_recording AFTER INSERT ON audio_clips BEGIN INSERT INTO late_child(id,parent) VALUES(NEW.clip_id,999);END");
    const auto reusedBefore=digestProject(*f.store).text;auto reuse=recording::prepareCommit(*f.store,repeated,ids);
    check(reuse.ok && !recording::commit(*f.store,reuse).ok && digestProject(*f.store).text==reusedBefore && media::blake3File(published).hex==originalHash.hex && std::filesystem::exists(repeated.tracks[0].file),"failed SQL after identical owned-file reuse never deletes the old asset or the new private take");
    db.exec("DROP TRIGGER reject_recording;DROP TABLE late_child;DROP TABLE late_parent");
    {std::ofstream foreign(published,std::ios::binary|std::ios::trunc);foreign<<"different content";}
    const auto foreignHash=media::blake3File(published);auto collision=recording::prepareCommit(*f.store,repeated,ids);
    check(collision.ok && !recording::commit(*f.store,collision).ok && media::blake3File(published).hex==foreignHash.hex && std::filesystem::exists(repeated.tracks[0].file),"recording publication never overwrites a different existing file");repeated.discard();
}
void groupDedupAndGuards() {
    Fixture f;auto recorded=f.capture(true);const std::array<recording::ClipIds,2> ids={{{7,8,"One"},{9,10,"Two"}}};auto prepared=recording::prepareCommit(*f.store,recorded,ids);
    check(prepared.ok,"prepare a two-track recording before media rows exist");
    const auto committed=recording::commit(*f.store,prepared);check(committed.ok && committed.seqs.size()==5,"identical simultaneous takes deduplicate inside the captured batch");
    auto& db=f.store->db();check(db.execAndGet("SELECT count(*) FROM media_files").getInt()==1 && db.execAndGet("SELECT count(*) FROM audio_clips WHERE media_id=8").getInt()==2,"both recorded clips reference the one captured content identity");
    check(History(*f.store).undo().ok && db.execAndGet("SELECT count(*) FROM clips").getInt()==0,"one undo removes every track in the recording pass");
    auto another=f.capture();const std::array<recording::ClipIds,1> one={{{17,18,"Another"}}};auto stale=recording::prepareCommit(*f.store,another,one);
    OpRequest rename;rename.opType="track.rename";rename.payload={{"id",21},{"name","Changed"}};
    check(OpJournal(*f.store).commit(rename).ok && !recording::commit(*f.store,stale).ok && std::filesystem::exists(another.tracks[0].file),"stale attachment rejects without losing the private recording");
    Fixture other;check(!recording::prepareCommit(*other.store,another,one).ok,"another project's connection cannot attach a stale document recording");
    auto changed=recording::prepareCommit(*f.store,another,one);const auto before=digestProject(*f.store).text;
    {std::ofstream bytes(another.tracks[0].file,std::ios::binary|std::ios::app);bytes<<"changed";}
    check(changed.ok && !recording::commit(*f.store,changed).ok && digestProject(*f.store).text==before && std::filesystem::exists(another.tracks[0].file),"changed take bytes roll back the complete attachment while preserving recovery files");another.discard();
}
void loopPublicationRefusesFlattening() {
    for(const auto frames:{32,300}) {
        Fixture fixture;auto recorded=fixture.capture(false,Precision::Float32,true,frames);
        check(recorded.failure==RecordingFailure::None && recorded.loopSpec() && recorded.passes().size()==(frames==32?1u:3u),"actual Session supplies valid partial-single and multiple-pass loop sources");
        const auto before=digestProject(*fixture.store).text;
        const std::array<recording::ClipIds,1> ids={{{7,8,"Loop"}}};
        const auto edit=recording::prepareCommit(*fixture.store,recorded,ids);
        check(!edit.ok && edit.requests.empty() && edit.error.find("take-pass publication")!=std::string::npos,"linear materializer explicitly refuses opted-in loop captures, even a single partial pass");
        check(digestProject(*fixture.store).text==before && std::filesystem::exists(recorded.tracks[0].file),"refused loop publication leaves project and recoverable continuous source unchanged");
        recorded.discard();
    }
}
std::vector<recording::LoopTrackIds> loopIds(const AudioRecordingResult& recorded) {
    std::vector<recording::LoopTrackIds> result;
    for(std::size_t track=0;track<recorded.tracks.size();++track) {
        recording::LoopTrackIds ids;ids.media=300+static_cast<std::int64_t>(track);
        for(std::size_t pass=0;pass<recorded.passes().size();++pass) {
            const auto offset=static_cast<std::int64_t>(track*16+pass);
            ids.passes.push_back({100+offset,200+offset,{}});
        }
        result.push_back(std::move(ids));
    }
    return result;
}
std::vector<double> loopOracle(int frames,std::int64_t start,std::vector<double> initial=std::vector<double>(1024)) {
    auto at=start;
    for(int source=0;source<frames;++source) {
        if(at>=640)at=512+(at-640)%128;
        initial.at(static_cast<std::size_t>(at))=Fixture::sample(static_cast<std::size_t>(source));++at;
    }
    return initial;
}
void loopPassesAndHistory(const std::filesystem::path& workFolder={}) {
    for(const auto precision:{Precision::Float32,Precision::Float64}) {
        Fixture fixture;auto recorded=fixture.capture(false,precision,true,448,workFolder);
        check(recorded.failure==RecordingFailure::None && recorded.passes().size()==4,"actual Session finishes one source containing three and a half loop passes");
        const auto before=digestProject(*fixture.store).text;const auto ids=loopIds(recorded);
        const std::array<std::optional<Payload>,4> selections={Payload{{"clips",{91}}},Payload{{"clips",{92}}},Payload{{"clips",{93}}},Payload{{"clips",{94}}}};
        auto prepared=recording::prepareLoopCommit(*fixture.store,recorded,ids,selections);
        if(!prepared.ok)std::printf("loop prepare: %s\n",prepared.error.c_str());
        check(prepared.ok && digestProject(*fixture.store).text==before,"loop preparation stages source once without changing project rows");
        if(!prepared.ok)continue;
        SQLite::Database outside((fixture.temporary.path()/"song.adi").string(),SQLite::OPEN_READONLY);
        int hooks=0;
        prepared.options.beforeCommit=[&](SQLite::Database& db,std::int64_t,std::string&) {
            ++hooks;
            return db.execAndGet("SELECT count(*) FROM lanes").getInt()==4 &&
                db.execAndGet("SELECT count(*) FROM media_files").getInt()==1 &&
                outside.execAndGet("SELECT count(*) FROM lanes").getInt()==0 &&
                outside.execAndGet("SELECT count(*) FROM media_files").getInt()==0;
        };
        const auto committed=recording::commit(*fixture.store,prepared);
        if(!committed.ok)std::printf("loop commit: %s\n",committed.error.c_str());
        check(committed.ok && committed.groupTxnIds.size()==4 && hooks==1,"four pass Undo groups and one publication callback share one atomic commit");
        if(!committed.ok)continue;
        auto& db=fixture.store->db();
        check(db.execAndGet("SELECT count(*) FROM media_files").getInt()==1 && db.execAndGet("SELECT count(*) FROM clips WHERE lane_id IS NOT NULL").getInt()==4,
            "four takes share exactly one media row");
        SQLite::Statement windows(db,"SELECT c.name,a.src_start_frames,a.src_len_frames FROM clips c JOIN audio_clips a ON a.clip_id=c.id WHERE c.lane_id IS NOT NULL ORDER BY c.id");
        bool exactWindows=true;std::size_t pass=0;
        while(windows.executeStep()) {
            const auto expectedName=pass==0?std::string("Audio"):"Audio "+std::to_string(pass+1);
            exactWindows=exactWindows && pass<4 && windows.getColumn(0).getString()==expectedName &&
                windows.getColumn(1).getInt64()==static_cast<std::int64_t>(pass*128) && windows.getColumn(2).getInt64()==(pass==3?64:128);++pass;
        }
        check(exactWindows && pass==4 && db.execAndGet("SELECT count(*) FROM lanes WHERE name='Lane'").getInt()==4,"take names, source offsets, partial final extent and new Lane defaults are literal");
        SQLite::Statement laneOrder(db,"SELECT id FROM lanes ORDER BY ord,id");
        bool chronological=true;pass=0;
        while(laneOrder.executeStep()) {
            chronological=chronological && pass<ids.front().passes.size() &&
                laneOrder.getColumn(0).getInt64()==ids.front().passes[pass].lane;
            ++pass;
        }
        check(chronological && pass==4,"recording presents its take lanes in pass order");
        check(db.execAndGet("SELECT count(*) FROM clips WHERE lane_id IS NULL").getInt()==2 && fixture.play(precision)==loopOracle(448,512),
            "partial pass four replaces its prefix while pass three retains the uncovered tail, with exact PCM");
        const auto owned=media::projectFolder(*fixture.store)/media::pathFromUtf8(db.execAndGet("SELECT rel_path FROM media_files").getString());
        check(std::filesystem::exists(owned) && !std::filesystem::exists(recorded.directory()),"successful loop publication retains one owned source and removes private capture");
        const auto after=digestProject(*fixture.store).text;History history(*fixture.store);
        for(int remaining=3;remaining>=0;--remaining) {
            const auto next=history.nextUndo();const auto undone=history.undo();
            check(next && next->txnId==committed.groupTxnIds[static_cast<std::size_t>(remaining)] && next->label=="Record" &&
                undone.ok && undone.selBefore==selections[static_cast<std::size_t>(remaining)] &&
                db.execAndGet("SELECT count(*) FROM lanes").getInt()==remaining &&
                db.execAndGet("SELECT count(*) FROM media_files").getInt()==(remaining?1:0),"one Undo restores the preceding pass, selection and shared-media lifetime");
            check(fixture.play(precision)==loopOracle(remaining*128,512),"each undo restores exact preceding main-lane PCM");
        }
        check(digestProject(*fixture.store).text==before && std::filesystem::exists(owned),"undoing all passes restores the project but keeps owned audio for Redo");
        for(int restored=1;restored<=4;++restored)check(history.redo().ok,"redo restores one recorded pass");
        check(digestProject(*fixture.store).text==after && fixture.play(precision)==loopOracle(448,512),"redo restores exact complete loop comp and journal material");
    }
}
void loopPartialEdgesAndPriorComp() {
    for(const auto start:{480,560}) {
        Fixture fixture;auto previous=fixture.capture(false,Precision::Float32,false,256,{},448);
        const std::array<recording::ClipIds,1> oldIds={{{7,8,"Original"}}};
        auto old=recording::prepareCommit(*fixture.store,previous,oldIds);
        check(old.ok && recording::commit(*fixture.store,old).ok,"establish real pre-existing main audio on both sides of the loop");
        const auto heardBefore=fixture.play(Precision::Float32);const auto before=digestProject(*fixture.store).text;
        auto recorded=fixture.capture(false,Precision::Float32,true,350,{},start);const auto ids=loopIds(recorded);
        auto prepared=recording::prepareLoopCommit(*fixture.store,recorded,ids);
        if(!prepared.ok)std::printf("edge loop prepare: %s\n",prepared.error.c_str());
        const auto committed=recording::commit(*fixture.store,prepared);
        if(!committed.ok)std::printf("edge loop commit: %s\n",committed.error.c_str());
        check(committed.ok && fixture.play(Precision::Float32)==loopOracle(350,start,heardBefore),
            "partial first and last passes preserve preceding main material outside every captured span");
        History history(*fixture.store);
        for(std::size_t pass=0;pass<recorded.passes().size();++pass)check(history.undo().ok,"edge pass undoes independently");
        check(digestProject(*fixture.store).text==before && fixture.play(Precision::Float32)==heardBefore,"undo restores original comp identities, source windows and exact PCM");
    }
}
void loopSinglePassBoundaries() {
    for(const auto frames:{32,128}) {
        Fixture fixture;auto recorded=fixture.capture(false,Precision::Float32,true,frames);const auto ids=loopIds(recorded);
        auto malformed=recorded;--malformed.frames;
        check(!recording::prepareLoopCommit(*fixture.store,malformed,ids).ok,"pass log cannot be attached to a different source extent");
        auto prepared=recording::prepareLoopCommit(*fixture.store,recorded,ids);
        const auto committed=recording::commit(*fixture.store,prepared);
        check(committed.ok && committed.groupTxnIds.size()==1 && fixture.store->db().execAndGet("SELECT count(*) FROM lanes").getInt()==1 &&
            fixture.play(Precision::Float32)==loopOracle(frames,512),"single partial and exact-end loop stops create one nonempty take and exact main material");
        check(History(*fixture.store).undo().ok && fixture.store->db().execAndGet("SELECT count(*) FROM lanes").getInt()==0,
            "single loop pass still owns one complete Undo group");
    }
}
void loopAtomicRefusalAndDedup() {
    Fixture fixture;auto recorded=fixture.capture(true,Precision::Float32,true,448);const auto ids=loopIds(recorded);
    const auto before=digestProject(*fixture.store).text;auto& db=fixture.store->db();
    db.exec("CREATE TRIGGER reject_last_take BEFORE INSERT ON clips WHEN NEW.name='Audio 4' BEGIN SELECT RAISE(ABORT,'last pass refused');END");
    auto rejected=recording::prepareLoopCommit(*fixture.store,recorded,ids);const auto result=recording::commit(*fixture.store,rejected);
    check(rejected.ok && !result.ok && result.groupTxnIds.empty() && digestProject(*fixture.store).text==before &&
        std::filesystem::exists(recorded.tracks[0].file),"failure in the final pass rolls back every earlier lane, take, main edit and media row");
    db.exec("DROP TRIGGER reject_last_take");
    auto veto=recording::prepareLoopCommit(*fixture.store,recorded,ids);
    veto.options.beforeCommit=[](SQLite::Database&,std::int64_t,std::string& error){error="owned fixture veto";return false;};
    check(veto.ok && !recording::commit(*fixture.store,veto).ok && digestProject(*fixture.store).text==before,
        "publication veto leaves all passes and shared media absent");
    auto prepared=recording::prepareLoopCommit(*fixture.store,recorded,ids);const auto committed=recording::commit(*fixture.store,prepared);
    check(committed.ok && committed.groupTxnIds.size()==4 && db.execAndGet("SELECT count(*) FROM lanes").getInt()==8 &&
        db.execAndGet("SELECT count(*) FROM media_files").getInt()==1,"two targets commit four shared pass groups and deduplicate identical continuous sources");
    auto doubled=loopOracle(448,512);for(auto& sample:doubled)sample*=2;
    check(fixture.play(Precision::Float32)==doubled,"two target clips hear the same shared source with exact summed PCM");
    const auto after=digestProject(*fixture.store).text;
    auto fresh=fixture.capture(false,Precision::Float32,true,32);const auto occupied=loopIds(fresh);
    check(!recording::prepareLoopCommit(*fixture.store,fresh,occupied).ok && digestProject(*fixture.store).text==after &&
        std::filesystem::exists(fresh.tracks[0].file),"occupied lane/clip allocations refuse without deleting prior takes or private audio");
    fresh.discard();
}


std::int64_t fixtureTicks(std::int64_t sample) {
    TempoMap tempo;tempo.events.push_back({0,120,0});return tempo.secondsToTicks(static_cast<double>(sample)/48000);
}
void existingTake(Store& store,std::int64_t lane,std::int64_t order,std::int64_t track=21) {
    SQLite::Statement row(store.db(),"INSERT INTO lanes(id,track_id,ord,kind,name) VALUES(?,?,?,'take','Existing')");
    row.bind(1,lane);row.bind(2,track);row.bind(3,order);row.exec();
}
void existingTakeClip(Store& store,std::int64_t id,std::int64_t lane,std::int64_t begin,
    std::optional<std::int64_t> length,bool nanos=false,std::int64_t track=21) {
    const auto sql=nanos?"INSERT INTO clips(id,track_id,lane_id,kind,time_base,pos_ns,length_ns) VALUES(?,?,?,'midi',1,?,?)":
        "INSERT INTO clips(id,track_id,lane_id,kind,time_base,pos_ticks,length_ticks) VALUES(?,?,?,'midi',0,?,?)";
    SQLite::Statement row(store.db(),sql);row.bind(1,id);row.bind(2,track);row.bind(3,lane);row.bind(4,begin);
    if(length)row.bind(5,*length);else row.bind(5);row.exec();
}
std::vector<std::int64_t> takeOrder(Store& store) {
    SQLite::Statement rows(store.db(),"SELECT id FROM lanes WHERE track_id=21 AND kind='take' ORDER BY ord,id");
    std::vector<std::int64_t> result;while(rows.executeStep())result.push_back(rows.getColumn(0).getInt64());return result;
}
std::int64_t preparedTakeLane(const recording::RecordingEdit& prepared,std::int64_t clip) {
    for(const auto& request:prepared.requests)if(request.opType=="clip.duplicate" && request.payload.at("id")==clip)
        for(const auto& change:request.payload.at("changes"))if(change.at("table")=="clips")
            return change.at("after").at("lane_id").get<std::int64_t>();
    return -1;
}
void loopReusesOrderedLanes() {
    Fixture fixture;auto recorded=fixture.capture(false,Precision::Float32,true,448);auto& db=fixture.store->db();
    // Visible order differs from ID order. The first lane has both clocks and
    // ends at the exact musical boundary; ns floor is strictly before it.
    existingTake(*fixture.store,30,4);existingTake(*fixture.store,20,9);existingTake(*fixture.store,10,17);
    existingTake(*fixture.store,5,0,22);
    existingTakeClip(*fixture.store,40,30,0,fixtureTicks(512));
    existingTakeClip(*fixture.store,41,30,0,10666666,true);
    existingTakeClip(*fixture.store,42,20,0,fixtureTicks(480));
    const auto before=digestProject(*fixture.store).text;const auto ids=loopIds(recorded);
    auto prepared=recording::prepareLoopCommit(*fixture.store,recorded,ids);
    check(prepared.ok && preparedTakeLane(prepared,200)==30 && preparedTakeLane(prepared,201)==20 &&
        preparedTakeLane(prepared,202)==10 && preparedTakeLane(prepared,203)==103,
        "reuse takes visible same-track lanes once each, including exact ends, then a fresh fallback");
    check(prepared.ok && digestProject(*fixture.store).text==before,"reuse preparation leaves prior lanes and material unchanged");
    const auto result=recording::commit(*fixture.store,prepared);
    if(!result.ok)std::printf("reuse commit: %s\n",result.error.c_str());
    check(result.ok && result.groupTxnIds.size()==4,"reused and new lanes share one transaction with four pass Undo groups");
    if(!result.ok)return;
    check(takeOrder(*fixture.store)==std::vector<std::int64_t>{103,30,20,10} &&
        db.execAndGet("SELECT count(*) FROM lanes WHERE id IN (100,101,102)").getInt()==0,
        "unused fallback IDs stay absent and pre-existing lanes retain relative order");
    check(db.execAndGet("SELECT count(*) FROM clips WHERE id IN (40,41,42)").getInt()==3 &&
        db.execAndGet("SELECT count(*) FROM clips WHERE lane_id=5").getInt()==0 &&
        fixture.play(Precision::Float32)==loopOracle(448,512),
        "existing take content and other-track lanes survive while the main comp sounds exact");
    const auto after=digestProject(*fixture.store).text;History history(*fixture.store);
    for(int remaining=3;remaining>=0;--remaining) {
        const auto undone=history.undo();
        check(undone.ok && takeOrder(*fixture.store)==std::vector<std::int64_t>{30,20,10} &&
            db.execAndGet("SELECT count(*) FROM clips WHERE id IN (40,41,42)").getInt()==3 &&
            fixture.play(Precision::Float32)==loopOracle(remaining*128,512),
            "each reused pass Undo preserves pre-existing lanes and restores preceding main audio");
    }
    check(digestProject(*fixture.store).text==before,"all reuse Undo restores exact old clip and sparse lane ordinal bytes");
    for(int pass=0;pass<4;++pass)check(history.redo().ok,"reuse pass redoes independently");
    check(digestProject(*fixture.store).text==after,"reuse Redo restores literal assigned lanes and source windows");
}
void loopMixedReuseAndFallback() {
    Fixture fixture;auto recorded=fixture.capture(false,Precision::Float32,true,448,{},480);
    existingTake(*fixture.store,77,12);
    existingTakeClip(*fixture.store,40,77,0,fixtureTicks(500));
    const auto ids=loopIds(recorded);const auto before=digestProject(*fixture.store).text;
    auto prepared=recording::prepareLoopCommit(*fixture.store,recorded,ids);
    check(prepared.ok && preparedTakeLane(prepared,200)==100 && preparedTakeLane(prepared,201)==77 &&
        preparedTakeLane(prepared,202)==102 && preparedTakeLane(prepared,203)==103,
        "partial pre-loop pass falls back before a later pass can safely reuse the same visible lane");
    prepared.options.beforeCommit=[](SQLite::Database&,std::int64_t,std::string& error){error="reuse veto";return false;};
    check(!recording::commit(*fixture.store,prepared).ok && digestProject(*fixture.store).text==before &&
        std::filesystem::exists(recorded.tracks[0].file),"reuse veto rolls every pass back and preserves old rows and private audio");
    auto retry=recording::prepareLoopCommit(*fixture.store,recorded,ids);const auto committed=recording::commit(*fixture.store,retry);
    check(committed.ok && takeOrder(*fixture.store)==std::vector<std::int64_t>{100,102,103,77} &&
        fixture.play(Precision::Float32)==loopOracle(448,480),"new fallback lanes stay in pass order around a reused pass");
    if(!committed.ok)return;
    History history(*fixture.store);
    const std::array<std::vector<std::int64_t>,4> previous={std::vector<std::int64_t>{100,102,77},
        std::vector<std::int64_t>{100,77},std::vector<std::int64_t>{100,77},std::vector<std::int64_t>{77}};
    for(const auto& order:previous)check(history.undo().ok && takeOrder(*fixture.store)==order,
        "mixed pass Undo restores the preceding chronological fallback group without moving old lanes");
    check(digestProject(*fixture.store).text==before,"mixed reuse restores exact original lane ordinal and material");
}
void loopReuseRefusesUnprovedExtents() {
    Fixture fixture;auto recorded=fixture.capture(false,Precision::Float32,true,32);auto& db=fixture.store->db();
    for(std::int64_t lane=50;lane<57;++lane)existingTake(*fixture.store,lane,lane);
    existingTakeClip(*fixture.store,40,50,0,fixtureTicks(513)); // Overlaps start.
    existingTakeClip(*fixture.store,41,51,fixtureTicks(900),fixtureTicks(10)); // Later, even without overlap.
    existingTakeClip(*fixture.store,42,52,0,10666667,true); // One ns beyond the safe floor.
    existingTakeClip(*fixture.store,43,53,0,std::nullopt); // Unbounded.
    existingTakeClip(*fixture.store,44,54,std::numeric_limits<std::int64_t>::max(),1); // Sum overflow.
    existingTakeClip(*fixture.store,45,55,0,0,false,22); // Inconsistent owning track.
    existingTakeClip(*fixture.store,46,56,0,-1); // Malformed native extent.
    db.exec("UPDATE clips SET muted=1 WHERE id=41");
    const auto before=digestProject(*fixture.store).text;const auto ids=loopIds(recorded);
    auto prepared=recording::prepareLoopCommit(*fixture.store,recorded,ids);
    check(prepared.ok && preparedTakeLane(prepared,200)==100 && digestProject(*fixture.store).text==before,
        "overlap, future muted material, fractional-ns ambiguity, unknown, overflow and wrong ownership all prevent reuse safely");
    // An exact physical boundary is eligible too: 480 samples at 48 kHz is 10 ms.
    Fixture exact;auto shortTake=exact.capture(false,Precision::Float32,true,32,{},480);
    existingTake(*exact.store,70,9);existingTakeClip(*exact.store,40,70,0,10000000,true);
    auto physical=recording::prepareLoopCommit(*exact.store,shortTake,loopIds(shortTake));
    check(physical.ok && preparedTakeLane(physical,200)==70,"nanosecond clip ending exactly at pass start is reusable");
}

}
int main(int argc,char** argv){try{
    if(argc!=1 && (argc!=3 || std::string(argv[1])!="--cross-volume-work-folder"))throw std::runtime_error("usage: recording tests [--cross-volume-work-folder PATH]");
    publicationStagingOwnership();heardPlacementAndHistory();rollbackNeverDeletesLaterWriter();lateSqlAndOwnedReuse();existingMediaAndHistoryPaths();groupDedupAndGuards();loopPublicationRefusesFlattening();
    loopPassesAndHistory();loopPartialEdgesAndPriorComp();loopSinglePassBoundaries();loopAtomicRefusalAndDedup();
    loopReusesOrderedLanes();loopMixedReuseAndFallback();loopReuseRefusesUnprovedExtents();
    if(argc==3) {
        const auto captureFolder=media::pathFromUtf8(argv[2]);
        crossVolumePublication(captureFolder);
        loopPassesAndHistory(captureFolder);
    }
}catch(const std::exception& error){check(false,error.what());}std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
