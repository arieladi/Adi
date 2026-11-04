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
#include <SQLiteCpp/SQLiteCpp.h>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
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
    AudioRecordingResult capture(bool two=false,Precision precision=Precision::Float32) {
        Session session;SessionSpec format{2,48000,128};format.precision=precision;session.graph().setFadeFrames(0);
        if(!session.load(*store,{},format))throw std::runtime_error(session.error());
        AudioRecordingSpec request;request.workFolder=temporary.path();request.sampleRate=48000;request.maxFrames=128;request.ringFrames=4096;request.targets={{21,{0}}};if(two)request.targets.push_back({22,{0}});
        std::string error;if(!session.startRecording(request,error))throw std::runtime_error(error);
        session.transport().locate(512);session.transport().play();std::array<float,128> source{};const float* inputs[]={source.data()};std::array<float,256> out{};float* outputs[]={out.data(),out.data()+128};
        std::int64_t stream=0;
        for(const auto count:{128,128,44}) {
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
void lateSqlAndOwnedReuse() {
    Fixture f;auto recorded=f.capture();const std::array<recording::ClipIds,1> ids={{{7,8,"One"}}};auto& db=f.store->db();
    db.exec("CREATE TABLE late_parent(id INTEGER PRIMARY KEY);CREATE TABLE late_child(id INTEGER PRIMARY KEY,parent INTEGER REFERENCES late_parent(id) DEFERRABLE INITIALLY DEFERRED);CREATE TRIGGER reject_recording AFTER INSERT ON audio_clips BEGIN INSERT INTO late_child(id,parent) VALUES(NEW.clip_id,999);END");
    const auto before=digestProject(*f.store).text;auto prepared=recording::prepareCommit(*f.store,recorded,ids);auto published=std::filesystem::path{};
    for(const auto& request:prepared.requests)if(request.opType=="media.import")published=media::projectFolder(*f.store)/media::pathFromUtf8(request.payload.at("row").at("rel_path").get<std::string>());
    const auto failed=recording::commit(*f.store,prepared);
    check(prepared.ok && !failed.ok && digestProject(*f.store).text==before && !std::filesystem::exists(published) && std::filesystem::exists(recorded.tracks[0].file),"late SQL commit failure rolls back rows and new publication while retaining the private take for retry");
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
}
int main(){try{heardPlacementAndHistory();lateSqlAndOwnedReuse();existingMediaAndHistoryPaths();groupDedupAndGuards();}catch(const std::exception& error){check(false,error.what());}std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
