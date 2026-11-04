// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/media/separated_stems.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/media/blake3.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/digest.hpp"
#include "adi/engine/session.hpp"
#include "adi/history.hpp"
#include "adi/store.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <new>
#include <stdexcept>
namespace {std::atomic<unsigned> allocations{0},frees{0};}
void* operator new(std::size_t n){if(adi::audio::onAudioThread)++allocations;if(void* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return operator new(n);}
void operator delete(void* p) noexcept{if(p && adi::audio::onAudioThread)++frees;std::free(p);}
void operator delete[](void* p) noexcept{operator delete(p);}
void operator delete(void* p,std::size_t) noexcept{operator delete(p);}
void operator delete[](void* p,std::size_t) noexcept{operator delete(p);}
namespace {
using namespace adi;using namespace adi::media;namespace fs=std::filesystem;
int checks=0,failures=0;
void check(bool ok,const char* label){++checks;if(!ok){++failures;std::printf("FAIL: %s\n",label);}}
std::int64_t count(Store& store,const char* table){return store.db().execAndGet(std::string("SELECT count(*) FROM ")+table).getInt64();}
void wave(const fs::path& path,std::uint32_t rate,std::uint32_t frames,float level,std::uint16_t channels=2,bool nonfinite=false) {
    std::vector<float> samples(static_cast<std::size_t>(frames)*channels,0.0f);
    for(std::size_t frame=0;frame<frames;++frame)for(std::size_t channel=0;channel<channels;++channel)
        samples[frame*channels+channel]=frame%13==0?level*(channel==0?1.0f:-0.75f):0.0f;
    if(!samples.empty())samples.back()=nonfinite?std::numeric_limits<float>::quiet_NaN():-level;
    audio::WavWriter writer(path,rate,channels,audio::WavFormat::Float32);writer.write(samples.data(),frames);writer.close();
}
struct Fixture {
    test::TempDirectory temp{"separated_stems","project"};
    fs::path project=temp.path()/"project"/"song.adi";
    std::unique_ptr<Store> store;
    SeparationResult result;SeparationImportOptions options;
    std::uint32_t rate,frames;
    Fixture(std::size_t number=4,std::uint32_t hz=48000,std::uint32_t length=4800):rate(hz),frames(length) {
        fs::create_directories(project.parent_path());StoreError error{};store=Store::createNewProject(project,error);
        if(!store)throw std::runtime_error("create stem project");
        const auto source=project.parent_path()/"source.wav";wave(source,rate,frames,0.5f);
        if(!importMedia(*store,source,50,100).ok)throw std::runtime_error("register source media");
        result.sourceMedia=50;result.sourceHash=blake3File(source).hex;result.frames=frames;
        result.service="controlled-local";result.model="fixture separator";result.version="v1-double-check";
        result.weightsHash=std::string(64,'c');result.request="Separate this source\nKeep all stems, caf\xc3\xa9";
        result.folder=temp.path()/"results";fs::create_directory(result.folder);
        options.group=1000;options.index=0;options.name="Separated caf\xc3\xa9";options.createdUtc=123456;options.documentGeneration=7;
        options.selectionBefore=Payload{{"clips",Payload::array({99})}};
        for(std::size_t i=0;i<number;++i) {
            const auto name="stem-"+std::to_string(i)+".wav";const auto path=result.folder/name;
            wave(path,rate,frames,0.05f*static_cast<float>(i+1),i%2==0?2:1);
            result.stems.push_back({name,"Authored "+std::to_string(i),blake3File(path).hex});
            const auto id=static_cast<std::int64_t>(i);
            options.stems.push_back({1100+id,1200+id,1300+id,static_cast<int>(i%70)});
        }
    }
    PreparedSeparation prepare(){return prepareSeparatedStems(*store,result,options);}
};
std::vector<float> samples(const fs::path& path) {
    audio::WavReader reader(path);std::vector<float> result(static_cast<std::size_t>(reader.frames())*reader.channels());
    if(reader.read(result.data(),static_cast<std::uint32_t>(reader.frames()))!=reader.frames())throw std::runtime_error("read comparison wave");
    return result;
}
void nativeLifecycle() {
    Fixture f;const auto before=digestProject(*f.store).text;const auto head=OpJournal(*f.store).headSeq();
    auto prepared=f.prepare();check(prepared.ready(),prepared.error().empty()?"complete four-stem result captures without inference":prepared.error().c_str());if(!prepared.ready())return;
    check(digestProject(*f.store).text==before && OpJournal(*f.store).headSeq()==head && count(*f.store,"agent_requests")==0,"preparation writes no project rows, head or request audit");
    auto copy=prepared;const auto committed=commitSeparatedStems(*f.store,prepared,7);
    check(committed.ok,committed.error.empty()?"one native action commits the returned group and stems":committed.error.c_str());if(!committed.ok)return;
    check(!prepared.ready() && !copy.ready() && !commitSeparatedStems(*f.store,copy,7).ok,"shared prepared results publish once, even through copied holders");
    auto& db=f.store->db();
    check(db.execAndGet("SELECT count(*) FROM tracks WHERE id=1000 AND kind='group'").getInt()==1 &&
        db.execAndGet("SELECT count(*) FROM tracks WHERE parent_id=1000 AND kind='audio'").getInt()==4 &&
        db.execAndGet("SELECT count(*) FROM clips WHERE track_id BETWEEN 1100 AND 1103").getInt()==4,"one group and exactly one ordinary audio track/clip per stem");
    check(db.execAndGet("SELECT count(DISTINCT pos_ticks) FROM clips WHERE track_id BETWEEN 1100 AND 1103").getInt()==1 &&
        db.execAndGet("SELECT count(*) FROM audio_clips WHERE clip_id BETWEEN 1200 AND 1203 AND src_start_frames=0 AND src_len_frames=4800 AND warp_enabled=0").getInt()==4,"every returned source begins together with exact sample span and Warp off");
    bool exact=true;
    for(std::size_t i=0;i<f.result.stems.size();++i) {
        const auto& stem=f.result.stems[i];const auto pool=f.project.parent_path()/"audio"/("separated-"+stem.hash+".wav");
        exact=exact && blake3File(pool).hex==stem.hash && samples(pool)==samples(f.result.folder/stem.file);
    }
    check(exact,"owned pooled WAVs preserve every returned sample and byte hash, including mono/stereo");
    {
    SQLite::Statement audit(db,"SELECT request,actor_detail,created_utc FROM agent_requests WHERE txn_id=?");audit.bind(1,committed.txnId);
    const bool recorded=audit.executeStep();const auto detail=recorded?Payload::parse(audit.getColumn(1).getString()):Payload{};
    check(recorded && audit.getColumn(0).getString()==f.result.request && audit.getColumn(2).getInt64()==123456 &&
        detail["model"]==f.result.model && detail["model_version"]==f.result.version && detail["source_hash"]==f.result.sourceHash && detail["weights_hash"]==*f.result.weightsHash,"model/version/weights/source/request provenance is recorded exactly with the native transaction");
    }
    const auto after=digestProject(*f.store).text;History history(*f.store);const auto step=history.nextUndo();
    check(step && step->txnId==committed.txnId && step->label=="Separate stems","the undo menu exposes one named separation action");
    const auto undone=history.undo();check(undone.ok && undone.selBefore==f.options.selectionBefore && digestProject(*f.store).text==before,"one undo restores all original music and advisory selection");
    check(count(*f.store,"agent_requests")==1,"request provenance remains audit metadata after undo");
    check(history.redo().ok && digestProject(*f.store).text==after,"one redo restores the exact group, media and stem content references");
    const auto path=f.project;f.store.reset();StoreError error{};f.store=Store::open(path,error);
    check(f.store && count(*f.store,"agent_requests")==1 && digestProject(*f.store).text==after,"reopen preserves separated material and request audit");
    bool sounding=true;
    for(const auto precision:std::array{engine::Precision::Float32,engine::Precision::Float64}) {
        engine::Session session;engine::SessionSpec spec;spec.precision=precision;spec.sampleRate=f.rate;spec.maxFrames=64;
        if(!session.load(*f.store,{},spec))throw std::runtime_error(session.error());
        session.transport().play();
        if(!session.clips()->prime(std::chrono::seconds(30)))throw std::runtime_error("prime separated clips");
        std::array<float,64> left{},right{};std::array<double,64> dl{},dr{};float* out[]{left.data(),right.data()};double* out64[]{dl.data(),dr.data()};
        engine::AudioIo io;io.out=out;io.out64=out64;io.numOut=2;io.frames=64;session.process(io);
        const bool audible=precision==engine::Precision::Float64?
            std::any_of(dl.begin(),dl.end(),[](double value){return std::isfinite(value) && std::abs(value)>1e-5;}):
            std::any_of(left.begin(),left.end(),[](float value){return std::isfinite(value) && std::abs(value)>1e-5f;});
        if(!audible || session.clips()->underrunSamples()!=0)
            std::printf("playback diagnostic: precision=%d audible=%d underruns=%llu readErrors=%llu float=%g double=%g\n",
                static_cast<int>(precision),audible?1:0,static_cast<unsigned long long>(session.clips()->underrunSamples()),
                static_cast<unsigned long long>(session.clips()->readErrors()),static_cast<double>(left[0]),dl[0]);
        sounding=sounding && session.clips()->underrunSamples()==0 && audible;
    }
    check(sounding,"actual Session plays the separated native clips in both precisions");
    // Real CBOR BLOB replay, never a call back to the model or input result.
    const auto replayDir=f.temp.path()/"replay";fs::create_directory(replayDir);auto replay=Store::createNewProject(replayDir/"song.adi",error);
    replay->db().exec("INSERT INTO session_state(key,value) VALUES('selection','other'),('playhead','123456')");
    SQLite::Statement log(f.store->db(),"SELECT op_type,payload,actor_detail FROM ops ORDER BY seq");std::vector<OpRequest> ops;
    while(log.executeStep()) {
        const auto column=log.getColumn(1);std::string decodeError;
        const auto payload=decodeFromLog({static_cast<const std::byte*>(column.getBlob()),static_cast<std::size_t>(column.getBytes())},decodeError);
        if(!payload)throw std::runtime_error(decodeError);
        OpRequest op;op.opType=log.getColumn(0).getString();op.payload=*payload;op.actorDetail=log.getColumn(2).getString();ops.push_back(std::move(op));
    }
    check(OpJournal(*replay).commit(ops).ok && digestProject(*replay).text==after,"binary native payloads replay identically under different UI state without inference or source reads");
}
void failuresAndRetry() {
    Fixture f;auto prepared=f.prepare();if(!prepared.ready())throw std::runtime_error(prepared.error());const auto original=digestProject(*f.store).text;
    check(!commitSeparatedStems(*f.store,prepared,8).ok && prepared.ready(),"wrong document generation refuses without consuming the result");
    CommitOptions wrongHead;wrongHead.expectHead.emplace(std::nullopt);
    check(!commitSeparatedStems(*f.store,prepared,7,Actor::Import,wrongHead).ok,"caller head policies cannot be silently weakened");
    CommitOptions veto;veto.beforeCommit=[](SQLite::Database&,std::int64_t,std::string& error){error="explicit caller veto";return false;};
    const auto failed=commitSeparatedStems(*f.store,prepared,7,Actor::Import,veto);
    check(!failed.ok && prepared.ready() && digestProject(*f.store).text==original && count(*f.store,"agent_requests")==0,"late caller veto rolls back all music, head and provenance atomically");
    check(!fs::is_empty(f.project.parent_path()/"audio"),"failed SQL retains content-addressed published audio rather than deleting another possible owner's asset");
    const auto retry=commitSeparatedStems(*f.store,prepared,7);
    check(retry.ok,"same immutable private result retries successfully against retained published content");
    Fixture stale;auto pending=stale.prepare();OpRequest rename;rename.opType="project.setName";rename.payload={{"name","changed"}};
    if(!OpJournal(*stale.store).commit(rename).ok)throw std::runtime_error("stale rename");
    const auto staleResult=commitSeparatedStems(*stale.store,pending,7);
    check(!staleResult.ok && staleResult.stale && count(*stale.store,"tracks")==7 && count(*stale.store,"agent_requests")==0,"changed journal head refuses inside commit before any separation action");
    Fixture reopened;auto old=reopened.prepare();reopened.store.reset();StoreError error{};reopened.store=Store::open(reopened.project,error);
    check(!commitSeparatedStems(*reopened.store,old,7).ok,"reopened same filename/generation cannot accept an old client's prepared result");
    Fixture changed;auto captured=changed.prepare();fs::remove_all(changed.result.folder);
    check(commitSeparatedStems(*changed.store,captured,7).ok,"closed producer files may disappear after immutable capture");
    Fixture corrupt;auto capturedBad=corrupt.prepare();
    for(const auto& entry:fs::directory_iterator(corrupt.project.parent_path()))if(entry.path().filename().string().starts_with(".adi-separated-"))
        std::ofstream(entry.path()/"stem-0.wav",std::ios::binary|std::ios::app)<<'x';
    check(!commitSeparatedStems(*corrupt.store,capturedBad,7).ok && count(*corrupt.store,"tracks")==7,"changed private captured audio refuses publication and rolls back rows");
    Fixture destination;auto destinationJob=destination.prepare();fs::create_directory(destination.project.parent_path()/"audio");
    const auto target=destination.project.parent_path()/"audio"/("separated-"+destination.result.stems.front().hash+".wav");std::ofstream(target)<<"occupied";
    check(!commitSeparatedStems(*destination.store,destinationJob,7).ok && count(*destination.store,"tracks")==7 && fs::file_size(target)==8,"different pre-existing destination is preserved and no partial group commits");
}
void shapesAndValidation() {
    Fixture f;const auto baseline=digestProject(*f.store).text;
    bool refused=true;
    for(unsigned which=0;which<15;++which) {
        auto result=f.result;auto options=f.options;
        switch(which) {
            case 0:result.sourceHash=std::string(64,'b');break;
            case 1:result.sourceBegin=-1;break;
            case 2:result.sourceBegin=1;break;
            case 3:result.frames=0;break;
            case 4:result.stems.pop_back();break;
            case 5:result.stems.front().file="../source.wav";break;
            case 6:result.stems.back().file=result.stems.front().file;break;
            case 7:result.stems.front().hash=std::string(64,'a');break;
            case 8:result.stems.front().name=std::string("bad\0name",8);break;
            case 9:result.version="bad\xff";break;
            case 10:result.service="builtin";result.weightsHash.reset();break;
            case 11:options.stems.back().track=options.group;break;
            case 12:options.stems.back().paletteIndex=70;break;
            case 13:options.maximumBytes=1;break;
            default:options.parent=1;break;
        }
        refused=refused && !prepareSeparatedStems(*f.store,result,options).ready();
    }
    check(refused && digestProject(*f.store).text==baseline,"fifteen malformed provenance/window/path/hash/identity/colour/budget/parent cases refuse without project writes");
    Fixture wrongRate;wave(wrongRate.result.folder/wrongRate.result.stems.front().file,44100,wrongRate.frames,0.2f);
    wrongRate.result.stems.front().hash=blake3File(wrongRate.result.folder/wrongRate.result.stems.front().file).hex;
    check(!wrongRate.prepare().ready(),"returned audio with a different source rate cannot silently lose phase alignment");
    Fixture wrongLength;wave(wrongLength.result.folder/wrongLength.result.stems.front().file,wrongLength.rate,wrongLength.frames-1,0.2f);
    wrongLength.result.stems.front().hash=blake3File(wrongLength.result.folder/wrongLength.result.stems.front().file).hex;
    check(!wrongLength.prepare().ready(),"returned audio must preserve the exact source window frame count");
    Fixture nan;wave(nan.result.folder/nan.result.stems.front().file,nan.rate,nan.frames,0.2f,2,true);
    nan.result.stems.front().hash=blake3File(nan.result.folder/nan.result.stems.front().file).hex;
    check(!nan.prepare().ready(),"nonfinite model output is refused instead of normalized to silence");
    Fixture many(6);auto six=many.prepare();const auto imported=commitSeparatedStems(*many.store,six,7);
    check(imported.ok && many.store->db().execAndGet("SELECT count(*) FROM tracks WHERE parent_id=1000").getInt()==6,"named services may return N stems beyond the built-in four");
    Fixture dedup;for(std::size_t i=1;i<dedup.result.stems.size();++i) {
        fs::copy_file(dedup.result.folder/dedup.result.stems.front().file,dedup.result.folder/dedup.result.stems[i].file,fs::copy_options::overwrite_existing);
        dedup.result.stems[i].hash=dedup.result.stems.front().hash;
    }
    auto same=dedup.prepare();check(commitSeparatedStems(*dedup.store,same,7).ok && count(*dedup.store,"media_files")==2 && count(*dedup.store,"audio_clips")==4,"identical outputs deduplicate pool identity while retaining every authored stem track");
    bool supported=true;
    for(const auto rate:std::array<std::uint32_t,7>{44100,48000,96000,192000,384000,768000,88200}) {
        Fixture tiny(1,rate,1);auto captured=tiny.prepare();const auto complete=commitSeparatedStems(*tiny.store,captured,7);
        if(!complete.ok){supported=false;continue;}
        const auto snapshot=engine::SnapshotBuilder::fromStore(*tiny.store);const auto end=tiny.store->db().execAndGet("SELECT length_ticks FROM clips WHERE id=1200").getInt64();
        supported=supported && snapshot->tempo->ticksToSeconds(end)>=1.0/static_cast<double>(rate);
    }
    check(supported,"single-frame results at all native rates use a covering common tick window without trimming their final sample");
}
void cancellationAndCallbacks() {
    Fixture f;std::stop_source stop;auto options=f.options;options.stop=stop.get_token();stop.request_stop();
    check(!prepareSeparatedStems(*f.store,f.result,options).ready(),"already-cancelled result refuses preparation");
    std::stop_source during;options=f.options;options.stop=during.get_token();unsigned calls=0;
    options.checkpoint=[&]{if(++calls==3)during.request_stop();};
    check(!prepareSeparatedStems(*f.store,f.result,options).ready() && count(*f.store,"tracks")==7,"live cancellation during capture leaves no project changes");
    auto captured=f.prepare();if(!captured.ready())throw std::runtime_error(captured.error());
    PreparedSeparation refused;CommitResult commit;
    {audio::CallbackScope scope;refused=prepareSeparatedStems(*f.store,f.result,f.options);commit=commitSeparatedStems(*f.store,captured,7);}
    check(!refused.ready() && !commit.ok && allocations==0 && frees==0 && audio::callbackFileIo==0,"both entry points refuse callbacks before SQL/file I/O, allocations or frees");
    check(captured.ready() && commitSeparatedStems(*f.store,captured,7).ok,"callback refusal leaves the owner's valid prepared result intact");
}
void boundaries() {
    Fixture window;window.result.sourceBegin=1200;window.result.frames=1200;window.result.weightsHash.reset();
    window.options.positionTicks=5765760;
    for(auto& stem:window.result.stems){wave(window.result.folder/stem.file,window.rate,1200,0.1f);stem.hash=blake3File(window.result.folder/stem.file).hex;}
    auto partial=window.prepare();
    check(commitSeparatedStems(*window.store,partial,7).ok && window.store->db().execAndGet("SELECT count(*) FROM clips WHERE pos_ticks=5765760").getInt()==4 &&
        window.store->db().execAndGet("SELECT count(*) FROM audio_clips WHERE src_start_frames=0 AND src_len_frames=1200").getInt()==4,
        "a named service without a weights hash can return a source subwindow at a nonzero common placement");
    Fixture growing;bool changed=false;growing.options.checkpoint=[&]{if(!changed){changed=true;std::ofstream(growing.result.folder/growing.result.stems.front().file,std::ios::binary|std::ios::app)<<'x';}};
    check(!growing.prepare().ready() && count(*growing.store,"tracks")==7,"producer growth after size preflight refuses instead of escaping the byte budget");
    Fixture changing;bool active=false;unsigned checkpoints=0;
    changing.options.checkpoint=[&]{if(active && ++checkpoints==3)for(const auto& entry:fs::directory_iterator(changing.project.parent_path()))
        if(entry.path().filename().string().starts_with(".adi-separated-")){
            std::fstream file(entry.path()/"stem-0.wav",std::ios::binary|std::ios::in|std::ios::out);file.seekp(-1,std::ios::end);file.put('x');
        }};
    auto pending=changing.prepare();active=true;
    check(!commitSeparatedStems(*changing.store,pending,7).ok && count(*changing.store,"tracks")==7 && count(*changing.store,"agent_requests")==0,
        "same-size private mutation between prehash and publication copy refuses the complete transaction");
    Fixture readOnly;auto writableJob=readOnly.prepare();StoreError error{};
    auto reader=Store::open(readOnly.project,error,true);
    check(reader && !prepareSeparatedStems(*reader,readOnly.result,readOnly.options).ready() && !commitSeparatedStems(*reader,writableJob,7).ok,
        "read-only Store refuses both preparation and a foreign writable owner's result");
    Fixture occupied;occupied.options.group=1;
    check(!occupied.prepare().ready(),"occupied allocated group identity refuses before capture");
    Fixture reuse;const auto before=digestProject(*reuse.store).text;
    for(auto& stem:reuse.result.stems){fs::copy_file(reuse.project.parent_path()/"source.wav",reuse.result.folder/stem.file,fs::copy_options::overwrite_existing);stem.hash=reuse.result.sourceHash;}
    auto existing=reuse.prepare();const auto reused=commitSeparatedStems(*reuse.store,existing,7);
    check(reused.ok && count(*reuse.store,"media_files")==1 && reuse.store->db().execAndGet("SELECT count(*) FROM audio_clips WHERE media_id=50").getInt()==4 &&
        reuse.store->db().execAndGet("SELECT rel_path FROM media_files WHERE id=50").getString().starts_with("audio/separated-"),
        "pre-existing identical media is relinked to owned audio without duplicating its native identity");
    check(History(*reuse.store).undo().ok && digestProject(*reuse.store).text==before && fs::is_regular_file(reuse.project.parent_path()/"source.wav"),
        "undo of a reused media identity restores its prior path and keeps the original source");
    auto empty=reuse.result;empty.stems.clear();auto noIds=reuse.options;noIds.stems.clear();
    auto excessive=reuse.result;excessive.stems.resize(1025,excessive.stems.front());auto extraIds=reuse.options;extraIds.stems.resize(1025);
    check(!prepareSeparatedStems(*reuse.store,empty,noIds).ready() && !prepareSeparatedStems(*reuse.store,excessive,extraIds).ready(),
        "zero and over-bound N-stem results refuse before opening producer files");
    Fixture linked;const auto link=linked.result.folder/"linked.wav";std::error_code ec;
    fs::create_symlink(linked.result.folder/linked.result.stems.front().file,link,ec);
    if(!ec){linked.result.stems.front().file="linked.wav";check(!linked.prepare().ready(),"result file symlink refuses without following outside the result boundary");}
}
} // namespace
int main(){try{nativeLifecycle();failuresAndRetry();shapesAndValidation();cancellationAndCallbacks();boundaries();}
    catch(const std::exception& error){++failures;std::printf("EXCEPTION: %s\n",error.what());}
    std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
