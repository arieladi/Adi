// SPDX-License-Identifier: GPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/audio/decode.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/media/peaks.hpp"
#include "adi/media/blake3.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <array>
#include <cstdio>
#include <limits>
#include <stdexcept>
namespace {
using namespace adi;using namespace adi::media;
int checks=0,failures=0;
void check(bool ok,const std::string& text){++checks;if(!ok){++failures;std::printf("FAIL %s\n",text.c_str());}}
void importAudio(Store& store,audio::DecodeCache& cache,const std::filesystem::path& path,std::int64_t id) {
    OpRequest request;const auto prepared=prepareImport(store,path,id,1,request,&cache);
    if(!prepared.ok)throw std::runtime_error(prepared.error);
    if(!request.opType.empty()){const auto committed=OpJournal(store).commit(request);if(!committed.ok)throw std::runtime_error(committed.error);}
}
void cache(Store& store,const std::vector<std::byte>& bytes) {
    SQLite::Statement update(store.db(),"UPDATE media_files SET peaks=? WHERE id=1");static const char empty=0;
    update.bind(1,bytes.empty()?static_cast<const void*>(&empty):bytes.data(),static_cast<int>(bytes.size()));update.exec();
}
int trace(unsigned kind,void* opaque,void*,void*){if(kind==SQLITE_TRACE_STMT)++*static_cast<int*>(opaque);return 0;}
void run() {
    test::TempDirectory directory{"media_peaks","project"};StoreError error{};auto store=Store::create(directory.path()/"song.adi",error);if(!store)throw std::runtime_error(toString(error));
    const auto source=directory.path()/"source.wav";audio::DecodeCache decoding(directory.path()/"decoded",{32u<<20,0});
    const std::array<float,10> samples={-1,.25f,.5f,-.5f,2,std::numeric_limits<float>::quiet_NaN(),-.75f,.75f,.125f,-2};
    {audio::WavWriter writer(source,48000,2,audio::WavFormat::Float32);writer.write(samples.data(),5);writer.close();}
    importAudio(*store,decoding,source,1);
    check(readMediaPeaks(*store,1).problem==PeakProblem::noCache,"import with no peaks reports no cache");
    const auto opCount=store->db().execAndGet("SELECT count(*) FROM ops").getInt64();
    auto built=buildMediaPeaks(*store,1,2,{},&decoding);check(static_cast<bool>(built),"streaming source peaks build: "+built.detail);if(!built)return;
    const auto data=*built.data;
    check(data.sampleRate==48000 && data.channels==2 && data.frames==5 && data.framesPerBin==3 && data.bins.size()==4,"source rate, channels and partial last bin survive project-rate differences");
    check(data.bins==std::vector<PeakBin>{{-1,2},{-.5f,.25f},{-.75f,.125f},{-2,.75f}},"stereo extrema retain overs and short tail; non-finite source becomes zero only for drawing");
    check(store->db().execAndGet("SELECT peaks IS NULL FROM media_files WHERE id=1").getInt()==1 && store->db().execAndGet("SELECT count(*) FROM ops").getInt64()==opCount,"building never writes a pool cache, op or history row");
    const auto bytes=encodePeaks(data);const auto decoded=decodePeaks(bytes);
    check(decoded && *decoded.data==data,"bounded cache codec round trips all channels and content identity");
    check(bytes.size()==112 && bytes[0]==std::byte{'A'} && bytes[4]==std::byte{1} && bytes[6]==std::byte{80} && bytes[8]==std::byte{0x80} && bytes[9]==std::byte{0xbb} && bytes[82]==std::byte{0x80} && bytes[83]==std::byte{0xbf},"APKS header and negative float have fixed little-endian bytes independent of native layout");
    cache(*store,bytes);const auto cached=readMediaPeaks(*store,1);check(cached && *cached.data==data,"pool cache reader validates hash and dimensions");
    std::filesystem::remove(source);auto readonly=Store::open(store->path(),error,true);if(!readonly)throw std::runtime_error(toString(error));
    const auto missing=buildMediaPeaks(*readonly,1,16,{},&decoding);check(missing && *missing.data==data,"read-only worker can draw a valid pool cache when source audio is missing");
    check(readMediaPeaks(*store,999).problem==PeakProblem::unknownMedia,"unknown media is explicit");
    store->db().exec("UPDATE media_files SET frames=6 WHERE id=1");check(readMediaPeaks(*store,1).problem==PeakProblem::invalidCache,"changed pool dimensions invalidate cached waveform");store->db().exec("UPDATE media_files SET frames=5,hash_blake3='other' WHERE id=1");
    check(readMediaPeaks(*store,1).problem==PeakProblem::invalidCache,"changed content identity invalidates cached waveform");
    {SQLite::Statement update(store->db(),"UPDATE media_files SET hash_blake3=? WHERE id=1");update.bind(1,data.contentHash);update.exec();}
    cache(*store,{std::byte{0},std::byte{1}});check(readMediaPeaks(*store,1).problem==PeakProblem::unsupportedCache,"unknown legacy peaks encoding stays opaque");
    check(!buildMediaPeaks(*store,1,2,{},&decoding) && store->db().execAndGet("SELECT hex(peaks)='0001' FROM media_files WHERE id=1").getInt()==1,"missing source cannot rebuild unknown cache and never overwrites it");
    store->db().exec("UPDATE media_files SET peaks=zeroblob(8388689) WHERE id=1");check(readMediaPeaks(*store,1).problem==PeakProblem::invalidCache,"oversized cache refuses before materializing its payload");
    for(const std::size_t length:std::array<std::size_t,4>{0,4,79,111})check(!decodePeaks(std::span(bytes).first(length)),"truncated cache refuses without partial data");
    for(const std::size_t offset:std::array<std::size_t,7>{4,6,12,24,32,72,80}) {
        auto malformed=bytes;malformed[offset]=std::byte{0xff};malformed[offset+1]=std::byte{0xff};malformed[offset+2]=std::byte{0xff};malformed[offset+3]=std::byte{0xff};
        check(!decodePeaks(malformed),"unknown version, impossible shape, reserved flags or nonfinite bins refuse");
    }
    auto invalid=data;invalid.bins.front()={2,-1};bool rejected=false;try{(void)encodePeaks(invalid);}catch(const std::invalid_argument&){rejected=true;}check(rejected,"encoder refuses reversed extrema");
    invalid=data;invalid.framesPerBin=0;rejected=false;try{(void)encodePeaks(invalid);}catch(const std::invalid_argument&){rejected=true;}check(rejected,"encoder refuses a zero bin width before division");
    std::stop_source stop;stop.request_stop();int cancelledSql=0;sqlite3_trace_v2(store->db().getHandle(),SQLITE_TRACE_STMT,trace,&cancelledSql);
    const auto cancelled=buildMediaPeaks(*store,1,2,stop.get_token(),&decoding);sqlite3_trace_v2(store->db().getHandle(),0,nullptr,nullptr);
    check(cancelled.problem==PeakProblem::cancelled && cancelledSql==0,"cancelled request refuses before cache/source work");
    check(buildMediaPeaks(*store,1,0,{},&decoding).problem==PeakProblem::invalidRequest,"zero requested bins refuses");
    int sql=0;sqlite3_trace_v2(store->db().getHandle(),SQLITE_TRACE_STMT,trace,&sql);const auto io=audio::callbackFileIo.load();PeakProblem readProblem{},buildProblem{},decodeProblem{};bool emptyEncoding=false;
    {audio::CallbackScope callback;readProblem=readMediaPeaks(*store,1).problem;buildProblem=buildMediaPeaks(*store,1,2,{},&decoding).problem;decodeProblem=decodePeaks(bytes).problem;emptyEncoding=encodePeaks(data).empty();}
    sqlite3_trace_v2(store->db().getHandle(),0,nullptr,nullptr);
    check(readProblem==PeakProblem::audioThread && buildProblem==PeakProblem::audioThread && decodeProblem==PeakProblem::audioThread && emptyEncoding && sql==0 && audio::callbackFileIo.load()==io,"callback misuse refuses before SQL, allocation-heavy codec or file I/O");
    const auto fixture=std::filesystem::path(ADI_AUDIO_FIXTURE_DIR);importAudio(*store,decoding,fixture/"tone16.flac",3);importAudio(*store,decoding,fixture/"src16.wav",4);
    const auto flac=buildMediaPeaks(*store,3,8,{},&decoding);const auto pcm=buildMediaPeaks(*store,4,8,{},&decoding);
    check(flac && pcm && flac.data->bins==pcm.data->bins && flac.data->frames==9600 && flac.data->sampleRate==48000,"compressed source uses the same decode path and has the PCM source's exact extrema");
    const auto empty=directory.path()/"empty.wav";{audio::WavWriter writer(empty,44100,1,audio::WavFormat::Pcm16);writer.close();}
    // Legacy zero-frame pool fixture: the current importer refuses empty audio.
    {SQLite::Statement row(store->db(),"INSERT INTO media_files(id,hash_blake3,orig_name,rel_path,sample_rate,channels,frames) VALUES(5,?,'empty.wav','empty.wav',44100,1,0)");row.bind(1,blake3File(empty).hex);row.exec();}
    const auto silent=buildMediaPeaks(*store,5,8,{},&decoding);
    check(silent && silent.data->frames==0 && silent.data->bins.empty() && decodePeaks(encodePeaks(*silent.data)),"empty mono source and zero-bin cache remain valid");
}
}
int main(){try{run();}catch(const std::exception& error){check(false,error.what());}std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
