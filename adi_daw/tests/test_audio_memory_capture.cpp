// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/recording_commands.hpp"
#include "adi/engine/audio_memory.hpp"
#include "adi/engine/session.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/media/blake3.hpp"
#include "adi/digest.hpp"
#include "adi/history.hpp"
#include "adi/store.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <array>
#include <bit>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <limits>
#include <stdexcept>
namespace {
using namespace adi;using namespace adi::engine;
int checks=0,failures=0;
void check(bool ok,const char* label){++checks;if(!ok){++failures;std::printf("FAIL: %s\n",label);}}
struct Fixture {
    test::TempDirectory temp{"capture","memory_clip"};std::unique_ptr<Store> store;
    Fixture(){StoreError error{};store=Store::createEmpty(temp.path()/"a.adi",error);if(!store)throw std::runtime_error("store");
        store->db().exec("INSERT INTO tracks(id,kind,name) VALUES(1,'audio','Capture'),(9,'master','Main')");}
    static AudioMemorySnapshot memory(std::uint32_t rate=48000,std::uint32_t channels=2) {
        AudioMemory ring(12,rate,channels,0.1);ring.setListening(true,false);
        std::vector<float> source(2048,0.25f+0x1p-23f);std::array<const float*,2> input{source.data(),source.data()};
        if(ring.push(input.data(),2048,(std::int64_t{1}<<53)+19)!=2048)throw std::runtime_error("input");
        AudioMemorySnapshot result;std::string error;if(!ring.snapshot(result,error))throw std::runtime_error(error);return result;
    }
    AudioRecordingResult take(const AudioMemorySnapshot& memory,std::int64_t timeline=512) {
        return recording::captureMemory(*store,memory,1,timeline,temp.path()/"work");
    }
    std::vector<double> play(Precision precision) {
        Session session;SessionSpec spec{2,48000,128};spec.precision=precision;session.graph().setFadeFrames(0);
        if(!session.load(*store,{},spec) || !session.problems().empty())throw std::runtime_error("load memory clip");
        session.transport().play();std::array<float,256> narrow{};std::array<double,256> wide{};
        float* out[]={narrow.data(),narrow.data()+128};double* out64[]={wide.data(),wide.data()+128};std::vector<double> heard(3072);
        for(std::size_t first=0;first<heard.size();first+=128) {
            if(!session.clips()->prime(std::chrono::seconds(5)))throw std::runtime_error("prime memory clip");
            AudioIo io;io.out=out;if(precision==Precision::Float64)io.out64=out64;io.numOut=2;io.frames=128;session.process(io);
            for(std::size_t i=0;i<128;++i)heard[first+i]=precision==Precision::Float64?wide[i]:narrow[i];
        }
        return heard;
    }
};
void materialization() {
    for(const auto precision:{Precision::Float32,Precision::Float64}) {
        Fixture f;const auto memory=Fixture::memory();const auto before=digestProject(*f.store).text;auto take=f.take(memory);
        check(take.failure==RecordingFailure::None && take.frames==memory.frames() && take.startSample==512 &&
            take.sampleRate==memory.sampleRate && take.tracks.size()==1 && take.tracks[0].trackId==1,"memory creates an owned take at explicit project placement, independent of >2^53 input clock");
        if(take.failure!=RecordingFailure::None)throw std::runtime_error(take.error);
        {audio::WavReader reader(take.tracks[0].file);std::vector<float> samples(memory.samples.size());const auto read=reader.read(samples.data(),static_cast<std::uint32_t>(memory.frames()));
        check(read==memory.frames() && samples==memory.samples && digestProject(*f.store).text==before,"materialization writes literal source float samples without changing history");}
        const std::array<recording::ClipIds,1> ids={{{7,8,"Captured audio"}}};auto edit=recording::prepareCommit(*f.store,take,ids);
        check(edit.ok && digestProject(*f.store).text==before,"Capture reuses native take preparation without project writes");
        const auto committed=recording::commit(*f.store,edit);check(committed.ok && committed.seqs.size()==3,"Capture media, clip and audio attachment form one native transaction");
        if(!committed.ok)throw std::runtime_error(committed.error);
        std::string error;const auto owned=media::resolveMedia(media::projectFolder(*f.store),media::mediaRow(f.store->db(),8),error);
        check(!owned.empty() && std::filesystem::exists(owned) && !std::filesystem::exists(take.directory()),"successful Capture retains owned media and cleans its private take");
        const auto heard=f.play(precision);bool exact=true;
        for(std::size_t frame=0;frame<heard.size();++frame)exact=exact && heard[frame]==(frame>=512 && frame<2560?static_cast<double>(memory.samples[0]):0);
        check(exact,"actual captured input plays at exact placement and duration in both graph widths");
        const auto after=digestProject(*f.store).text;
        check(History(*f.store).undo().ok && digestProject(*f.store).text==before && std::filesystem::exists(owned),"Capture undo restores exact pre-capture project and keeps PCM for redo");
        check(History(*f.store).redo().ok && digestProject(*f.store).text==after && f.play(precision)==heard,"Capture redo restores literal playing audio without recapturing");
        f.store.reset();StoreError errorCode{};f.store=Store::open(f.temp.path()/"a.adi",errorCode);
        check(f.store && digestProject(*f.store).text==after && f.play(precision)==heard,"reopened project retains Capture history and heard audio");
    }
}
void sampleBitsAndRates() {
    bool rates=true;
    for(std::uint32_t rate:{44100u,48000u,96000u,192000u,384000u,768000u}) {
        Fixture f;const auto memory=Fixture::memory(rate,1);auto take=f.take(memory,0);
        const std::array<recording::ClipIds,1> ids={{{7,8,"Rate"}}};auto edit=recording::prepareCommit(*f.store,take,ids);
        rates=rates && take.failure==RecordingFailure::None && edit.ok && recording::commit(*f.store,edit).ok;
    }
    check(rates,"Capture materializes and attaches all session rates without resampling source frames");
    Fixture f;auto memory=Fixture::memory();memory.samples[0]=std::bit_cast<float>(0x7fc12345u);memory.samples[1]=-0.0f;auto take=f.take(memory);
    {audio::WavReader reader(take.tracks[0].file);std::array<float,2> read{};
    check(reader.read(read.data(),1)==1 && std::bit_cast<std::uint32_t>(read[0])==0x7fc12345u && std::bit_cast<std::uint32_t>(read[1])==0x80000000u,"private memory take preserves source NaN payload and signed zero");}take.discard();
}
void guardsAndRecovery() {
    Fixture f;const auto memory=Fixture::memory();auto take=f.take(memory);const auto source=take.tracks[0].file;
    const std::array<recording::ClipIds,1> ids={{{7,8,"Capture"}}};auto edit=recording::prepareCommit(*f.store,take,ids);
    const auto before=digestProject(*f.store).text;
    f.store.reset();StoreError reopened{};f.store=Store::open(f.temp.path()/"a.adi",reopened);
    check(!recording::commit(*f.store,edit).ok && digestProject(*f.store).text==before && std::filesystem::exists(source),"prepared Capture cannot cross a reopened Store even with the same path and head");
    edit=recording::prepareCommit(*f.store,take,ids);OpRequest rename;rename.opType="track.rename";rename.payload={{"id",1},{"name","Changed"}};
    check(edit.ok && OpJournal(*f.store).commit(rename).ok && recording::commit(*f.store,edit).stale && std::filesystem::exists(source),"stale Capture head refuses while preserving private PCM for retry");
    edit=recording::prepareCommit(*f.store,take,ids);edit.options.beforeCommit=[](SQLite::Database&,std::int64_t,std::string& why){why="reject test publication";return false;};
    const auto current=digestProject(*f.store).text;
    check(!recording::commit(*f.store,edit).ok && digestProject(*f.store).text==current && std::filesystem::exists(source),"late Capture transaction rejection rolls back rows and retains source for retry");
    edit=recording::prepareCommit(*f.store,take,ids);check(edit.ok && recording::commit(*f.store,edit).ok,"fresh Capture preparation retries successfully without re-reading the input");
    auto second=f.take(memory,3000);const std::array<recording::ClipIds,1> secondIds={{{17,18,"Again"}}};auto next=recording::prepareCommit(*f.store,second,secondIds);
    check(next.ok && recording::commit(*f.store,next).ok && f.store->db().execAndGet("SELECT count(*) FROM media_files").getInt()==1 &&
        f.store->db().execAndGet("SELECT count(*) FROM clips").getInt()==2,"repeated identical Capture deduplicates media without losing either clip");
    Fixture other;auto privateTake=f.take(memory);check(!recording::prepareCommit(*other.store,privateTake,ids).ok,"memory take retains originating project identity");privateTake.discard();
}
void refusals() {
    Fixture f;const auto memory=Fixture::memory();const auto before=digestProject(*f.store).text;
    int refused=0;
    for(int which=0;which<10;++which) {
        auto bad=memory;
        if(which==0)bad.samples.clear();
        if(which==1)bad.channels=0;
        if(which==2)bad.channels=65;
        if(which==3)bad.samples.pop_back();
        if(which==4)bad.sampleRate=0;
        if(which==5)bad.sampleRate=768001;
        if(which==6)bad.inputId=0;
        if(which==7)bad.epoch=0;
        if(which==8)bad.firstSample=-1;
        if(which==9)bad.firstSample=std::numeric_limits<std::int64_t>::max();
        const auto take=f.take(bad);if(take.failure==RecordingFailure::InvalidCapture && take.directory().empty() && !take.error.empty())++refused;
    }
    check(refused==10 && !std::filesystem::exists(f.temp.path()/"work"),"malformed memory snapshots refuse before reserving files");
    check(f.take(memory,-1).failure==RecordingFailure::InvalidCapture && f.take(memory,(std::int64_t{1}<<50)).failure==RecordingFailure::InvalidCapture,"negative or overflowing project placement is refused");
    {std::ofstream file(f.temp.path()/"blocked");file<<"preserve";}
    const auto failed=recording::captureMemory(*f.store,memory,1,0,f.temp.path()/"blocked");
    check(failed.failure==RecordingFailure::Io && failed.directory().empty() && std::filesystem::file_size(f.temp.path()/"blocked")==8,"failed staging preserves a pre-existing file and exposes no partial take");
    unsigned queries=0;
    sqlite3_trace_v2(f.store->db().getHandle(),SQLITE_TRACE_STMT,[](unsigned,void* context,void*,void*)->int {
        ++*static_cast<unsigned*>(context);return 0;
    },&queries);
    f.store->db().exec("SELECT 1");check(queries==1,"database trace observes actual owner SQL statements");queries=0;
    const auto ioBefore=audio::callbackFileIo.load();AudioRecordingResult callback;
    {audio::CallbackScope scope;callback=recording::captureMemory(*f.store,memory,1,0,f.temp.path()/"callback");}
    sqlite3_trace_v2(f.store->db().getHandle(),0,nullptr,nullptr);
    check(callback.failure==RecordingFailure::InvalidCapture && queries==0 && audio::callbackFileIo==ioBefore && !std::filesystem::exists(f.temp.path()/"callback"),"Capture materialization refuses callback before database or file access");
    check(digestProject(*f.store).text==before,"all rejected memory materializations leave project and history unchanged");
}
}
int main(){try{materialization();sampleBitsAndRates();guardsAndRecovery();refusals();}catch(const std::exception& e){std::printf("EXCEPTION: %s\n",e.what());check(false,"fixture must complete");}
std::printf("audio_memory_capture: %d checks, %d failures\n",checks,failures);return failures?1:0;}
