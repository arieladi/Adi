// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/sample_defaults.hpp"
#include "adi/material_commands.hpp"
#include "adi/library/index.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/audio/warp_map.hpp"
#include "adi/engine/session.hpp"
#include "adi/history.hpp"
#include "adi/edit_plan.hpp"
#include "adi/store.hpp"
#include "adi/textproj.hpp"
#include "adi/media/blake3.hpp"
#include "adi/media/media_ops.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <new>
#include <numbers>
namespace {std::atomic<unsigned> allocations{0},frees{0};}
namespace {
// MSVC's Debug CRT (_ITERATOR_DEBUG_LEVEL 2) allocates an iterator proxy whenever a
// std::string or std::vector is constructed and frees it when that container is
// destroyed, so an empty result returned from the callback path costs one allocation
// and one free per container there and none on the other CRTs. The refusal check
// below allows exactly what the empty results cost on this CRT, measured once before
// the baseline is read.
struct ResultCost {unsigned allocated=0,freed=0;};
template<class Result> ResultCost emptyResultCost(){const auto a=allocations.load(),f=frees.load();{adi::audio::CallbackScope scope;Result empty;(void)empty;}return {allocations.load()-a,frees.load()-f};}
}
void* operator new(std::size_t n){if(adi::audio::onAudioThread)++allocations;if(void* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return operator new(n);}
void operator delete(void* p)noexcept{if(p && adi::audio::onAudioThread)++frees;std::free(p);}
void operator delete[](void* p)noexcept{operator delete(p);}
void operator delete(void* p,std::size_t)noexcept{operator delete(p);}
void operator delete[](void* p,std::size_t)noexcept{operator delete(p);}
namespace {
using namespace adi;using namespace adi::engine;namespace fs=std::filesystem;constexpr auto q=textproj::kPPQ;
int checks=0,failures=0;
void check(bool good,const char* label){++checks;if(!good){++failures;std::printf("FAIL: %s\n",label);}}
struct Project {
    test::TempDirectory temp{"sample_defaults","project"};std::unique_ptr<Store> store;
    Project(){StoreError error{};store=Store::createEmpty(temp.path()/"song.adi",error);if(!store)throw std::runtime_error("project");store->db().exec("INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(1,'audio','Audio',0),(9,'master','Main',1)");}
    fs::path source()const{return temp.path()/"source.wav";}
    clip_edit::ImportOptions options(const fs::path& path)const{clip_edit::ImportOptions out;out.source=path;out.track=1;out.clipId=7;out.mediaId=8;out.importedUtc=123;out.at={1,0};return out;}
};
void sourceWave(const fs::path& path){std::vector<float> samples(96000*2);for(std::size_t i=0;i<96000;++i){samples[i*2]=static_cast<float>(.3*std::sin(2*std::numbers::pi*1000*static_cast<double>(i)/48000));samples[i*2+1]=static_cast<float>(.2*std::sin(2*std::numbers::pi*1500*static_cast<double>(i)/48000));}audio::WavWriter writer(path,48000,2,audio::WavFormat::Float32);writer.write(samples.data(),96000);writer.close();}
void original(Project& p,bool warp=false){sourceWave(p.source());auto edit=clip_edit::prepareImportAudio(*p.store,p.options(p.source()),48000);if(!edit.edit.ok || !clip_edit::commitMaterial(*p.store,edit).ok)throw std::runtime_error("source import");
    p.store->db().exec("UPDATE clips SET length_ns=750000000,gain_db=-6.020599913279624,content_offset_ticks=576576,fade_in_ticks=576576,fade_out_ticks=576576,fade_in_curve=3,fade_out_curve=5; UPDATE audio_clips SET src_start_frames=2400,src_len_frames=48000; UPDATE clip_playback_details SET content_offset_ns=50000000,source_end_ns=800000000,in_tension=0.25,out_tension=-0.4; INSERT INTO clip_properties(clip_id,signature_numerator,signature_denominator) VALUES(7,255,128)");
    if(warp){const std::array<WarpMarker,2> points{WarpMarker{0,0},WarpMarker{48000,2*q}};auto map=audio::writeWarpMap(points,48000);StreamHeader header{};std::memcpy(&header,map.bytes.data(),sizeof header);header.version=2;header.rec_size=24;std::vector<std::byte> future(sizeof header+48+1,std::byte{0xa5});std::memcpy(future.data(),&header,sizeof header);for(std::size_t i=0;i<2;++i)std::memcpy(future.data()+sizeof header+i*24,&points[i],sizeof(WarpMarker));
        p.store->db().exec("UPDATE clips SET time_base=0,pos_ns=NULL,length_ns=NULL,pos_ticks=0,length_ticks=11531520,content_offset_ticks=0,fade_in_ticks=0,fade_out_ticks=0; UPDATE clip_playback_details SET content_offset_ns=NULL,source_end_ns=NULL,in_tension=NULL,out_tension=NULL");SQLite::Statement update(p.store->db(),"UPDATE audio_clips SET warp_enabled=1,warp_mode='re-pitch',warp_markers=? WHERE clip_id=7");update.bind(1,future.data(),static_cast<int>(future.size()));update.exec();}
}
std::vector<double> render(Store& store,double rate,Precision precision,std::size_t frames){Session session;session.graph().setFadeFrames(0);SessionSpec spec{2,rate,512};spec.precision=precision;if(!session.load(store,{},spec))throw std::runtime_error(session.error());if(!session.problems().empty())throw std::runtime_error(session.problems().front());session.transport().play();std::vector<double> out(frames*2);std::array<float,1024> narrow{};std::array<double,1024> wide{};float* channels[]{narrow.data(),narrow.data()+512};double* wideChannels[]{wide.data(),wide.data()+512};
    const auto io=audio::callbackFileIo.load(),a=allocations.load(),f=frees.load();for(std::size_t first=0;first<frames;first+=512){const auto count=static_cast<std::int32_t>(std::min<std::size_t>(512,frames-first));if(!session.clips()->prime(std::chrono::seconds(10)))throw std::runtime_error("prime sample defaults");AudioIo block{channels,nullptr,2,0,count,0};if(precision==Precision::Float64)block.out64=wideChannels;{audio::CallbackScope callback;session.process(block);}for(std::size_t i=0;i<static_cast<std::size_t>(count);++i){out[(first+i)*2]=precision==Precision::Float64?wide[i]:narrow[i];out[(first+i)*2+1]=precision==Precision::Float64?wide[512+i]:narrow[512+i];}}
    check(session.clips()->readErrors()==0 && session.clips()->underrunSamples()==0 && io==audio::callbackFileIo.load() && a==allocations.load() && f==frees.load(),"actual Session callbacks preserve ready PCM with zero allocation/free/file I/O");return out;
}
void ordinary() {
    Project source,target,replay;original(source);test::TempDirectory home{"sample_defaults","library"};std::string error;auto index=library::Index::open(home.path()/"data/library.sqlite",error,false);if(!index)throw std::runtime_error(error);const auto before=source.store->db().execAndGet("SELECT count(*) FROM ops").getInt64();StoreError openError{};auto readonly=Store::open(source.store->path(),openError,true);if(!readonly)throw std::runtime_error("read-only source");
    auto captured=clip_edit::captureSampleDefaults(*readonly,7,48000);if(!captured.ok)std::printf("capture: %s\n",captured.error.c_str());check(captured.ok && captured.settings->sourceStart==2400 && captured.settings->sourceLength==48000 && captured.settings->signatureNumerator==255 && captured.settings->signatureDenominator==128,"actual native clip capture keeps source window and complete native signature bounds");if(!captured.ok)return;
    check(captured.settings->fadeClockNs[0].has_value() && captured.settings->fadeClockNs[2].has_value(),"existing native clip-copy logic freezes legacy fades into intrinsic source clocks");
    check(clip_edit::saveSampleDefaults(*readonly,7,48000,*index,error)==library::SampleSettingsProblem::None && source.store->db().execAndGet("SELECT count(*) FROM ops").getInt64()==before,"a read-only project can save authored defaults without music/history writes");
    const auto saved=index->sampleSettings(captured.hash);check(saved && *saved.data==*captured.settings,"native Save persists exactly the captured clip settings by actual source hash");
    fs::create_directories(home.path()/"cache/analysed");{std::ofstream cache(home.path()/"cache/analysed/derived");cache<<"temporary";}fs::remove_all(home.path()/"cache");index.reset();index=library::Index::open(home.path()/"data/library.sqlite",error,false);check(index && index->sampleSettings(captured.hash).data==saved.data,"actual persistent library reopens with authored data after cache cleanup");
    target.store->db().exec("DELETE FROM tempo_map; INSERT INTO tempo_map(pos_ticks,bpm,curve) VALUES(0,60,0)");auto options=target.options(source.source());options.warp=true;options.warpAnalysis.tempoHint=0;auto prepared=clip_edit::prepareImportAudio(*target.store,options,48000,index.get());if(!prepared.edit.ok)std::printf("recall: %s\n",prepared.edit.error.c_str());check(prepared.edit.ok && target.store->db().execAndGet("SELECT count(*) FROM clips").getInt()==0,"library-aware import captures authored defaults instead of automatic Warp, with no project writes");if(!prepared.edit.ok)return;
    check(index->clearSampleSettings(captured.hash,error)==library::SampleSettingsProblem::None,"library can change after import capture");index.reset();check(clip_edit::commitMaterial(*target.store,prepared).ok,"native import commit owns its values after the library closes");
    check(target.store->db().execAndGet("SELECT src_start_frames=2400 AND src_len_frames=48000 AND warp_enabled=0 FROM audio_clips WHERE clip_id=7").getInt()==1 && target.store->db().execAndGet("SELECT length_ns FROM clips WHERE id=7").getInt64()==750000000,"recalled source trim and source-end offset define the native playing clip");
    check(target.store->db().execAndGet("SELECT count(*) FROM clip_properties WHERE clip_id=7 AND signature_numerator=255 AND signature_denominator=128").getInt()==1,"native recalled signature properties survive actual project rows");
    for(const auto rate:{48000.0,44100.0})for(const auto width:{Precision::Float32,Precision::Float64}){const auto frames=static_cast<std::size_t>(rate*.8);const auto expected=render(*source.store,rate,width,frames),heard=render(*target.store,rate,width,frames);check(expected==heard && std::any_of(heard.begin(),heard.end(),[](double x){return x!=0;}),"saved source window, gain and curved fades play identically at both widths/rates across project tempo");}
    std::vector<OpRequest> requests;SQLite::Statement logged(target.store->db(),"SELECT op_type,payload FROM ops ORDER BY seq");while(logged.executeStep()){OpRequest request;request.opType=logged.getColumn(0).getString();const auto blob=logged.getColumn(1);const auto* bytes=static_cast<const std::uint8_t*>(blob.getBlob());request.payload=Payload::from_cbor(bytes,bytes+blob.getBytes());requests.push_back(std::move(request));}
    check(OpJournal(*replay.store).commit(requests).ok,"actual journal CBOR replays the import with no sample library");
    History history(*target.store);check(history.undo().ok && target.store->db().execAndGet("SELECT count(*) FROM clips").getInt()==0,"one native undo removes the recalled import");check(history.redo().ok && target.store->db().execAndGet("SELECT src_start_frames FROM audio_clips WHERE clip_id=7").getInt64()==2400,"native redo restores source defaults without consulting a library");
    check(render(*replay.store,48000,Precision::Float64,38000)==render(*target.store,48000,Precision::Float64,38000),"journal replay and redo produce the same actual native PCM");
}
void warped() {
    Project source,target;original(source,true);test::TempDirectory home{"sample_defaults","warp_library"};std::string error;auto index=library::Index::open(home.path()/"library.sqlite",error,false);if(!index)throw std::runtime_error(error);const auto captured=clip_edit::captureSampleDefaults(*source.store,7,48000);if(!captured.ok)std::printf("warp capture: %s\n",captured.error.c_str());check(captured.ok && audio::readWarpMap(*captured.settings->warpMarkers,48000).futureLayout,"native authored capture retains readable future Warp records");if(!captured.ok)return;
    check(clip_edit::saveSampleDefaults(*source.store,7,48000,*index,error)==library::SampleSettingsProblem::None,"native warped clip saves authored source mapping");target.store->db().exec("DELETE FROM tempo_map; INSERT INTO tempo_map(pos_ticks,bpm,curve) VALUES(0,60,0)");auto edit=clip_edit::prepareImportAudio(*target.store,target.options(source.source()),48000,index.get());if(!edit.edit.ok)std::printf("warp recall: %s\n",edit.edit.error.c_str());check(edit.edit.ok && clip_edit::commitMaterial(*target.store,edit).ok,"saved Warp is recalled through normal native material import at a new tempo");if(!edit.edit.ok)return;
    SQLite::Statement map(target.store->db(),"SELECT warp_markers FROM audio_clips WHERE clip_id=7");const bool hasMap=map.executeStep() && !map.getColumn(0).isNull();if(!hasMap){check(false,"recalled native Warp marker attachment exists");return;}const auto blob=map.getColumn(0);const auto* raw=static_cast<const std::byte*>(blob.getBlob());check(std::vector<std::byte>(raw,raw+blob.getBytes())==*captured.settings->warpMarkers,"future native Warp bytes reach actual project CBOR/rows unchanged");
    check(target.store->db().execAndGet("SELECT length_ticks FROM clips WHERE id=7").getInt64()==2*q,"initial warped clip has the authored musical source span");for(const auto width:{Precision::Float32,Precision::Float64}){const auto heard=render(*target.store,48000,width,97000);unsigned crossings=0;for(std::size_t i=24001;i<72000;++i)if(heard[(i-1)*2]<=0 && heard[i*2]>0)++crossings;check(crossings>=498 && crossings<=502 && std::all_of(heard.begin()+96000*2,heard.end(),[](double x){return x==0;}),"actual Re-Pitch recall follows the new tempo, halves tone frequency and ends at authored duration");}
}
void loopsAndMarkers() {
    Project source,target;original(source);test::TempDirectory home{"sample_defaults","loops"};std::string error;auto index=library::Index::open(home.path()/"library.sqlite",error,false);if(!index)throw std::runtime_error(error);
    source.store->db().exec("UPDATE clips SET loop_enabled=1,loop_start_ticks=1153152,loop_len_ticks=3459456; UPDATE clip_playback_details SET loop_begin_ns=100000000,loop_end_ns=400000000");
    const auto captured=clip_edit::captureSampleDefaults(*source.store,7,48000);check(captured.ok && captured.settings->loopBeginNs==100000000 && captured.settings->loopEndNs==400000000,"native loop capture retains the source-clock region");if(!captured.ok)return;
    auto onlyNs=*captured.settings;onlyNs.loopStartTicks=0;onlyNs.loopLengthTicks.reset();check(index->saveSampleSettings(captured.hash,onlyNs,error)==library::SampleSettingsProblem::None,"authored fixed-clock loop can be recalled without a legacy musical length");
    target.store->db().exec("DELETE FROM tempo_map; INSERT INTO tempo_map(pos_ticks,bpm,curve) VALUES(0,60,0)");auto prepared=clip_edit::prepareImportAudio(*target.store,target.options(source.source()),48000,index.get());if(!prepared.edit.ok)std::printf("loop recall: %s\n",prepared.edit.error.c_str());check(prepared.edit.ok && clip_edit::commitMaterial(*target.store,prepared).ok,"fixed-clock loop imports through native material operations at another tempo");if(!prepared.edit.ok)return;
    check(target.store->db().execAndGet("SELECT length_ns FROM clips WHERE id=7").getInt64()==350000000 && target.store->db().execAndGet("SELECT loop_len_ticks>0 FROM clips WHERE id=7").getInt()==1,"initial loop span ends at its source loop boundary and has usable legacy handle coordinates");
    // Arrangement extension is deliberately a separate project edit; sample
    // defaults contain intrinsic ranges, not the old arrangement rectangle.
    target.store->db().exec("UPDATE clips SET length_ns=750000000 WHERE id=7");
    for(const auto width:{Precision::Float32,Precision::Float64})check(render(*source.store,48000,width,38000)==render(*target.store,48000,width,38000),"extending the recalled loop repeats exact source PCM and retained source fades at both widths");
    Project markerSource,markerTarget;original(markerSource);OpRequest end;end.opType="clip.setEndMarker";end.payload={{"id",7},{"ticks",8072064}};check(OpJournal(*markerSource.store).commit(end).ok,"native End edit clears its prior physical pin before a legacy marker capture");const auto marker=clip_edit::captureSampleDefaults(*markerSource.store,7,48000);
    check(marker.ok && marker.settings->sourceEndNs==700000000 && !marker.settings->endMarkerTicks,"legacy musical End is frozen into its actual fixed source time for an unwarped sample");if(!marker.ok)return;
    check(index->saveSampleSettings(marker.hash,*marker.settings,error)==library::SampleSettingsProblem::None,"canonical source End persists as authored sample data");markerTarget.store->db().exec("DELETE FROM tempo_map; INSERT INTO tempo_map(pos_ticks,bpm,curve) VALUES(0,60,0)");auto recalled=clip_edit::prepareImportAudio(*markerTarget.store,markerTarget.options(markerSource.source()),48000,index.get());check(recalled.edit.ok && clip_edit::commitMaterial(*markerTarget.store,recalled).ok,"fixed source End recalls without double-applying the old project tempo");if(!recalled.edit.ok)return;
    check(markerTarget.store->db().execAndGet("SELECT length_ns FROM clips WHERE id=7").getInt64()==650000000,"recalled initial source End subtracts the saved content offset exactly");
    check(render(*markerSource.store,48000,Precision::Float64,38000)==render(*markerTarget.store,48000,Precision::Float64,38000),"actual native playback preserves the old End marker's audible boundary across tempo");
}
void pinnedEndAndSignedTrim() {
    Project source,target;original(source);
    source.store->db().exec("DELETE FROM tempo_map;INSERT INTO tempo_map(pos_ticks,bpm) VALUES(0,240);"
                           "UPDATE clip_properties SET end_marker_ticks=5765760 WHERE clip_id=7");
    // A clip moved to a faster tempo keeps its independent 800ms End pin;
    // the old tick display marker must not become a second 250ms limit.
    const auto captured=clip_edit::captureSampleDefaults(*source.store,7,48000);
    check(captured.ok && captured.settings->sourceEndNs==800000000 && !captured.settings->endMarkerTicks,
          "sample capture retains a pre-existing source End pin instead of re-timing it from display ticks");
    if(!captured.ok)return;
    test::TempDirectory home{"sample_defaults","pinned_end"};std::string error;
    auto index=library::Index::open(home.path()/"library.sqlite",error,false);if(!index)throw std::runtime_error(error);
    check(index->saveSampleSettings(captured.hash,*captured.settings,error)==library::SampleSettingsProblem::None,
          "authoritative source End persists in the existing sample-defaults format");
    target.store->db().exec("DELETE FROM tempo_map;INSERT INTO tempo_map(pos_ticks,bpm) VALUES(0,60)");
    auto recalled=clip_edit::prepareImportAudio(*target.store,target.options(source.source()),48000,index.get());
    check(recalled.edit.ok && clip_edit::commitMaterial(*target.store,recalled).ok,
          "pinned End defaults recall at another tempo through native material operations");
    const auto expected=render(*source.store,48000,Precision::Float64,38000);
    const auto actual=render(*target.store,48000,Precision::Float64,38000);
    check(expected==actual && std::any_of(actual.begin(),actual.end(),[](double value){return value!=0;}),
          "saved pinned End retains exact audible PCM after tempo-changing recall");
    Project signedSource;original(signedSource);
    signedSource.store->db().exec("DELETE FROM tempo_map;INSERT INTO tempo_map(pos_ticks,bpm) VALUES(0,120),(5765760,240);"
                                 "UPDATE clips SET pos_ns=750000000 WHERE id=7");
    const std::array<std::int64_t,1> tracks{1};edit_plan::Options options;options.policy.automationFollowsClips=false;
    const auto trim=edit_plan::prepareTrim(*signedSource.store,
        edit_plan::materialScope(tracks,edit_plan::PlacementClock::Nanoseconds,{0,2000000000}),{7,{1,0}},options);
    check(trim.ok && clip_edit::commitPrepared(*signedSource.store,trim).ok &&
              signedSource.store->db().execAndGet("SELECT content_offset_ticks FROM clips WHERE id=7").getInt64()<0,
          "native source-zero extension creates the tempo-relocated signed display offset");
    const auto before=signedSource.store->db().execAndGet("SELECT count(*) FROM ops").getInt64();
    const auto canonical=clip_edit::captureSampleDefaults(*signedSource.store,7,48000);
    check(canonical.ok && canonical.settings->contentOffsetTicks==0 && canonical.settings->contentOffsetNs==0 &&
              signedSource.store->db().execAndGet("SELECT content_offset_ticks FROM clips WHERE id=7").getInt64()<0 &&
              signedSource.store->db().execAndGet("SELECT count(*) FROM ops").getInt64()==before,
          "portable defaults canonicalize only the unused negative tick fallback without writing the project");
}
void guards() {
    Project source,target;original(source);test::TempDirectory home{"sample_defaults","guards"};std::string error;auto index=library::Index::open(home.path()/"library.sqlite",error,false);if(!index)throw std::runtime_error(error);const auto captured=clip_edit::captureSampleDefaults(*source.store,7,48000);if(!captured.ok)throw std::runtime_error(captured.error);auto wrong=*captured.settings;wrong.frames++;if(index->saveSampleSettings(captured.hash,wrong,error)!=library::SampleSettingsProblem::None)throw std::runtime_error(error);
    auto rejected=clip_edit::prepareImportAudio(*target.store,target.options(source.source()),48000,index.get());check(!rejected.edit.ok && target.store->db().execAndGet("SELECT count(*) FROM ops").getInt()==0 && target.store->db().execAndGet("SELECT count(*) FROM media_files").getInt()==0,"mismatched authored dimensions refuse without native rows or history");
    // The refused capture returns one empty SampleDefaultsCapture and the refused import one empty MaterialEdit; the refused save returns only its enum.
    const auto captureCost=emptyResultCost<clip_edit::SampleDefaultsCapture>(),importCost=emptyResultCost<clip_edit::MaterialEdit>();
    const auto io=audio::callbackFileIo.load(),a=allocations.load(),f=frees.load();error="keep";bool capturedAudio=false,importedAudio=false;library::SampleSettingsProblem savedAudio{};const auto options=target.options(source.source());
    {audio::CallbackScope callback;capturedAudio=clip_edit::captureSampleDefaults(*source.store,7,48000).ok;savedAudio=clip_edit::saveSampleDefaults(*source.store,7,48000,*index,error);importedAudio=clip_edit::prepareImportAudio(*target.store,options,48000,index.get()).edit.ok;}
    check(!capturedAudio && !importedAudio && savedAudio==library::SampleSettingsProblem::Realtime && error=="keep" && io==audio::callbackFileIo.load() && a+captureCost.allocated+importCost.allocated==allocations.load() && f+captureCost.freed+importCost.freed==frees.load(),"native Save/capture/import callback boundaries allocate, free, query and read nothing");
    source.store->db().exec("ALTER TABLE audio_clips ADD COLUMN future_control BLOB NOT NULL DEFAULT X'00FF'");check(!clip_edit::captureSampleDefaults(*source.store,7,48000).ok,"capturing defaults refuses unknown authored native source fields instead of dropping them");
}
void atomicRecall() {
    Project source,target;original(source);test::TempDirectory home{"sample_defaults","atomic"};std::string error;const auto libraryPath=home.path()/"library.sqlite";auto index=library::Index::open(libraryPath,error,false);if(!index)throw std::runtime_error(error);
    const auto captured=clip_edit::captureSampleDefaults(*source.store,7,48000);if(!captured.ok)throw std::runtime_error(captured.error);if(index->saveSampleSettings(captured.hash,*captured.settings,error)!=library::SampleSettingsProblem::None)throw std::runtime_error(error);
    auto options=target.options(source.source());options.copyIn=true;auto prepared=clip_edit::prepareImportAudio(*target.store,options,48000,index.get());check(prepared.edit.ok,"authored recall prepares an owned copy through the native material path");if(!prepared.edit.ok)return;
    auto& db=target.store->db();db.exec("CREATE TABLE late_failure(id INTEGER PRIMARY KEY,parent INTEGER REFERENCES tracks(id) DEFERRABLE INITIALLY DEFERRED) STRICT; CREATE TRIGGER fail_recall AFTER INSERT ON clips BEGIN INSERT INTO late_failure VALUES(new.id,999);END");
    const auto failed=clip_edit::commitMaterial(*target.store,prepared);bool empty=true;for(const auto& entry:fs::directory_iterator(media::projectFolder(*target.store)/"audio")){(void)entry;empty=false;}
    check(!failed.ok && db.execAndGet("SELECT count(*) FROM clips").getInt()==0 && db.execAndGet("SELECT count(*) FROM media_files").getInt()==0 && db.execAndGet("SELECT count(*) FROM ops").getInt()==0 && empty,"a late COMMIT failure rolls back authored recall and retires only its published copy");
    check(index->sampleSettings(captured.hash).data==captured.settings && fs::exists(source.source()),"failed recall leaves persistent authored defaults and original source intact");
    db.exec("DROP TRIGGER fail_recall;DROP TABLE late_failure");check(clip_edit::commitMaterial(*target.store,prepared).ok && db.execAndGet("SELECT src_start_frames=2400 AND src_len_frames=48000 FROM audio_clips").getInt()==1,"the same captured authored import retries successfully after late SQL rollback");
    options.clipId=17;options.mediaId=18;auto reused=clip_edit::prepareImportAudio(*target.store,options,48000,index.get());check(reused.edit.ok && clip_edit::commitMaterial(*target.store,reused).ok && db.execAndGet("SELECT count(*) FROM media_files").getInt()==1 && db.execAndGet("SELECT src_start_frames FROM audio_clips WHERE clip_id=17").getInt64()==2400,"deduplicated existing native media resolves defaults by its stored content identity");
    auto future=library::encodeSampleSettings(*captured.settings);future["version"]=2;Payload payload={{"sample_settings",future}};const auto text=payload.dump();{SQLite::Database native(media::pathUtf8(libraryPath),SQLite::OPEN_READWRITE);SQLite::Statement put(native,"UPDATE annotations SET payload=? WHERE hash=?");put.bind(1,text);put.bind(2,captured.hash);put.exec();}
    Project other;auto refused=clip_edit::prepareImportAudio(*other.store,other.options(source.source()),48000,index.get());check(!refused.edit.ok && other.store->db().execAndGet("SELECT count(*) FROM ops").getInt()==0,"unsupported authored library versions refuse a new import rather than silently discarding settings");
    {SQLite::Database native(media::pathUtf8(libraryPath),SQLite::OPEN_READONLY);SQLite::Statement get(native,"SELECT payload FROM annotations WHERE hash=?");get.bind(1,captured.hash);check(get.executeStep() && get.getColumn(0).getString()==text,"failed future-data recall preserves the original opaque annotation bytes");}
    auto legacy=clip_edit::prepareImportAudio(*other.store,other.options(source.source()),48000);check(legacy.edit.ok && clip_edit::commitMaterial(*other.store,legacy).ok && other.store->db().execAndGet("SELECT src_start_frames=0 AND src_len_frames=96000 FROM audio_clips").getInt()==1,"explicit import without a library retains the established original-source workflow");
}
}
int main(){try{ordinary();warped();loopsAndMarkers();pinnedEndAndSignedTrim();guards();atomicRecall();}catch(const std::exception& e){++failures;std::printf("EXCEPTION: %s\n",e.what());}std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
