// SPDX-License-Identifier: AGPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/material_commands.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/digest.hpp"
#include "adi/engine/session.hpp"
#include "adi/history.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/media/blake3.hpp"
#include <fstream>
#include "adi/store.hpp"
#include "adi/textproj.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
namespace {std::atomic<unsigned> callbackAllocations{0};}
void* operator new(std::size_t n){if(adi::audio::onAudioThread)++callbackAllocations;if(void* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return operator new(n);}
void operator delete(void* p)noexcept{std::free(p);}
void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}
void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
namespace {
using namespace adi;using namespace adi::engine;constexpr std::int64_t q=textproj::kPPQ;
int checks=0,failures=0;
void check(bool ok,const char* what){++checks;if(!ok){++failures;std::printf("FAIL %s\n",what);}}
struct Project {
    test::TempDirectory temporary{"warp_import","project"};std::unique_ptr<Store> store;
    explicit Project(std::uint32_t rate=44100) {
        StoreError error{};store=Store::createEmpty(temporary.path()/"song.adi",error);if(!store)throw std::runtime_error(toString(error));
        store->db().exec("INSERT INTO tracks(id,kind,name) VALUES(1,'audio','Imported'),(9,'master','Main')");
        std::vector<float> source(rate+1234,0);
        for(std::uint32_t beat=0;beat<4;++beat)source[1234+beat*(rate/4)]=1;
        audio::WavWriter writer(temporary.path()/"source.wav",rate,1,audio::WavFormat::Float32);
        writer.write(source.data(),static_cast<std::uint32_t>(source.size()));writer.close();
    }
    clip_edit::ImportOptions options()const {
        clip_edit::ImportOptions result;result.source=temporary.path()/"source.wav";result.track=1;result.clipId=7;result.mediaId=8;
        result.at={0,q};result.copyIn=true;result.warp=true;result.warpAnalysis.tempoHint=240;return result;
    }
};
std::vector<double> render(Session& session,Precision width,std::size_t frames) {
    std::vector<double> result(frames);std::array<float,2048> narrow{};std::array<double,2048> wide{};
    float* out[]={narrow.data(),narrow.data()+1024};double* out64[]={wide.data(),wide.data()+1024};
    session.transport().locate(0);session.transport().play();
    for(std::size_t first=0;first<frames;) {
        const auto count=static_cast<std::int32_t>(std::min<std::size_t>(1024,frames-first));
        if(!session.clips()->prime(std::chrono::seconds(5)))throw std::runtime_error("prime Warp import");
        AudioIo io;io.frames=count;io.numOut=2;if(width==Precision::Float64)io.out64=out64;else io.out=out;
        {audio::CallbackScope callback;session.process(io);}
        for(std::int32_t frame=0;frame<count;++frame)result[first+static_cast<std::size_t>(frame)]=width==Precision::Float64?wide[static_cast<std::size_t>(frame)]:narrow[static_cast<std::size_t>(frame)];
        first+=static_cast<std::size_t>(count);
    }
    return result;
}
void importedAudioAndHistory() {
    for(const auto width:{Precision::Float32,Precision::Float64}) {
        Project p;auto options=p.options();auto& db=p.store->db();const auto before=digestProject(*p.store).text;
        auto prepared=clip_edit::prepareImportAudio(*p.store,options,48000);
        if(!prepared.edit.ok)std::printf("prepare: %s\n",prepared.edit.error.c_str());
        check(prepared.edit.ok && digestProject(*p.store).text==before,"Warp import stages detection, media and a clip without project writes");
        const auto committed=clip_edit::commitMaterial(*p.store,prepared);if(!committed.ok)std::printf("commit: %s\n",committed.error.c_str());
        check(committed.ok && committed.seqs.size()==2,"detected source map and media attach in one native undo group");
        check(db.execAndGet("SELECT warp_enabled FROM audio_clips").getInt()==1 &&
              db.execAndGet("SELECT warp_mode FROM audio_clips").getString()=="beats" &&
              db.execAndGet("SELECT src_start_frames FROM audio_clips").getInt64()==1234 &&
              db.execAndGet("SELECT src_len_frames FROM audio_clips").getInt64()==44100 &&
              db.execAndGet("SELECT time_base FROM clips").getInt()==0 &&
              db.execAndGet("SELECT length_ticks FROM clips").getInt64()==4*q,"stored import uses the detected first beat and source musical length");
        const auto after=digestProject(*p.store).text;
        {Session session;SessionSpec spec{2,48000,1024};spec.precision=width;session.graph().setFadeFrames(0);
        check(session.load(*p.store,{},spec) && session.problems().empty(),"actual Session plays the imported Warp clip with no unsupported-source problem");
        const auto heard=render(session,width,128000);bool onBeat=true;
        for(std::int64_t beat=0;beat<4;++beat) {
            const auto at=24000+beat*24000;
            const auto peak=std::max_element(heard.begin()+at-2,heard.begin()+at+3,[](double a,double b){return std::abs(a)<std::abs(b);});
            onBeat=onBeat && std::abs(*peak)>0.5;
        }
        check(onBeat && session.clips()->underrunSamples()==0,"heard 44.1-kHz source attacks land on the 48-kHz project grid in both precisions");
        check(std::all_of(heard.begin(),heard.begin()+24000,[](double value){return value==0;}) &&
              std::all_of(heard.begin()+120000,heard.end(),[](double value){return value==0;}),"imported warped audio respects its insert point and full timeline extent");
        check(History(*p.store).undo().ok && session.refresh(*p.store) && digestProject(*p.store).text==before,"one undo removes the complete imported Warp state");
        const auto silent=render(session,width,1024);check(std::all_of(silent.begin(),silent.end(),[](double value){return value==0;}),"undo removes the heard source");
        check(History(*p.store).redo().ok && session.refresh(*p.store) && digestProject(*p.store).text==after &&
              render(session,width,128000)==heard,"redo regenerates identical heard Warp PCM from literal stored markers");
        // Release the live reader before reopening the project/history.
        }
        p.store.reset();StoreError error{};p.store=Store::open(p.temporary.path()/"song.adi",error);
        check(p.store && digestProject(*p.store).text==after && History(*p.store).undo().ok &&
              digestProject(*p.store).text==before,"reopened Warp import history needs no new beat analysis");
    }
}
void choicesAndGuards() {
    Project p(48000);auto options=p.options();auto& db=p.store->db();options.warp=false;
    auto plain=clip_edit::prepareImportAudio(*p.store,options,48000);
    check(plain.edit.ok && clip_edit::commitMaterial(*p.store,plain).ok && db.execAndGet("SELECT warp_enabled FROM audio_clips").getInt()==0,"default deviation keeps an ordinary import unwarped");
    check(History(*p.store).undo().ok,"restore root after plain import");
    options.warp=true;options.warpAnalysis.fromFirstBeat=false;options.warpMode=audio::WarpMode::Complex;
    auto fromStart=clip_edit::prepareImportAudio(*p.store,options,48000);
    const auto captured=clip_edit::commitMaterial(*p.store,fromStart);
    if(!captured.ok)std::printf("sample-start import: %s / %s\n",fromStart.edit.error.c_str(),captured.error.c_str());
    check(fromStart.edit.ok && captured.ok &&
          db.execAndGet("SELECT src_start_frames FROM audio_clips").getInt64()==0 &&
          db.execAndGet("SELECT warp_mode FROM audio_clips").getString()=="complex","explicit sample-start and warp-mode choices are captured");
    check(History(*p.store).undo().ok,"restore source for stale Warp import");
    auto stale=clip_edit::prepareImportAudio(*p.store,options,48000);
    OpRequest rename;rename.opType="track.rename";rename.payload={{"id",1},{"name","Changed"}};check(OpJournal(*p.store).commit(rename).ok,"intervening native edit");
    const auto before=digestProject(*p.store).text;const auto refused=clip_edit::commitMaterial(*p.store,stale);
    check(!refused.ok && refused.stale && digestProject(*p.store).text==before,"stale Warp import publishes no rows or file");
    std::stop_source stop;stop.request_stop();options.stop=stop.get_token();
    const auto cancelled=clip_edit::prepareImportAudio(*p.store,options,48000);
    check(!cancelled.edit.ok && cancelled.edit.error.find("cancelled")!=std::string::npos && digestProject(*p.store).text==before,"cancelled Warp analysis leaves the project unchanged");
}
void ownedFileReuseAndCancellation() {
    Project p;auto options=p.options();options.warp=false;auto initial=clip_edit::prepareImportAudio(*p.store,options,48000);
    check(clip_edit::commitMaterial(*p.store,initial).ok,"owned-file reuse fixture import");
    const auto relative=p.store->db().execAndGet("SELECT rel_path FROM media_files").getString();
    const auto target=p.temporary.path()/media::pathFromUtf8(relative);const auto original=media::blake3File(target);
    std::stop_source stop;options.clipId=17;options.mediaId=18;options.copyIn=false;options.stop=stop.get_token();
    auto deduplicated=clip_edit::prepareImportAudio(*p.store,options,48000);const auto before=digestProject(*p.store).text;
    stop.request_stop();const auto cancelled=clip_edit::commitMaterial(*p.store,deduplicated);
    check(deduplicated.edit.ok && !cancelled.ok && cancelled.error.find("cancelled")!=std::string::npos &&
          digestProject(*p.store).text==before,"cancellation after capture also refuses a deduplicated import with no file to publish");
    check(History(*p.store).undo().ok,"undo retains owned bytes for a new history branch");
    options=p.options();options.warp=true;
    p.store->db().exec("CREATE TABLE future_parent(id INTEGER PRIMARY KEY) STRICT;"
        "CREATE TABLE future_link(id INTEGER PRIMARY KEY,parent INTEGER REFERENCES future_parent(id) DEFERRABLE INITIALLY DEFERRED) STRICT;"
        "CREATE TRIGGER fail_reuse AFTER INSERT ON clips BEGIN INSERT INTO future_link VALUES(new.id,999);END");
    auto reused=clip_edit::prepareImportAudio(*p.store,options,48000);const auto root=digestProject(*p.store).text;
    const auto late=clip_edit::commitMaterial(*p.store,reused);
    check(reused.edit.ok && !late.ok && digestProject(*p.store).text==root &&
          std::filesystem::exists(target) && media::blake3File(target).hex==original.hex,"late SQL failure never removes an identical pre-existing owned asset that was reused");
    p.store->db().exec("DROP TRIGGER fail_reuse");
    {std::ofstream foreign(target,std::ios::binary|std::ios::trunc);foreign<<"preserve this foreign content";}
    auto collision=clip_edit::prepareImportAudio(*p.store,options,48000);
    const auto refused=clip_edit::commitMaterial(*p.store,collision);std::ifstream existing(target,std::ios::binary);
    const std::string content((std::istreambuf_iterator<char>(existing)),std::istreambuf_iterator<char>());
    check(collision.edit.ok && !refused.ok && content=="preserve this foreign content" &&
          digestProject(*p.store).text==root,"a conflicting existing file is preserved and rejects the entire import");
}

}
int main(){try{importedAudioAndHistory();choicesAndGuards();ownedFileReuseAndCancellation();}catch(const std::exception& error){check(false,error.what());}check(callbackAllocations.load()==0 && audio::callbackFileIo.load()==0,"Warp import playback keeps allocation and I/O off the callback");std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
