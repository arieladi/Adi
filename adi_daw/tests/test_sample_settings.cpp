// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/library/sample_settings.hpp"
#include "adi/library/index.hpp"
#include "adi/audio/warp_map.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/media/media_ops.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
namespace {std::atomic<unsigned> allocations{0},frees{0};}
namespace {
// MSVC's Debug CRT (_ITERATOR_DEBUG_LEVEL 2) allocates an iterator proxy whenever a
// std::string or std::vector is constructed and frees it when that container is
// destroyed, so an empty result returned from the callback path costs one allocation
// and one free per container there and none on the other CRTs. The refusal checks
// below allow exactly what the empty results cost on this CRT, measured once before
// each baseline is read.
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
using namespace adi;using namespace adi::library;using Json=nlohmann::json;namespace fs=std::filesystem;
int checks=0,failures=0;
void check(bool good,const char* label){++checks;if(!good){++failures;std::printf("FAIL: %s\n",label);}}
const std::string hash(64,'a'),other(64,'b');
SampleSettings sample() {
    SampleSettings s;s.sampleRate=48000;s.channels=2;s.frames=192000;s.sourceStart=1000;s.sourceLength=96000;
    const std::array<WarpMarker,3> markers{WarpMarker{0,0},WarpMarker{48000,11531520},WarpMarker{96000,23063040}};const auto map=audio::writeWarpMap(markers,s.sourceLength);if(!map)throw std::runtime_error("author marker map");
    s.warpEnabled=true;s.warpMode="complexPro";s.warpMarkers=map.bytes;s.transposeSemis=3.25;s.formantShift=-2;s.gainDb=-6.020599913279624;s.reverse=true;s.channelMode=3;s.loopEnabled=true;s.muted=true;
    s.contentOffsetTicks=1441440;s.loopStartTicks=2882880;s.loopLengthTicks=11531520;s.endMarkerTicks=23063040;s.fadeInTicks=720720;s.fadeOutTicks=360360;s.fadeInCurve=3;s.fadeOutCurve=5;s.fadeInTension=-.33;s.fadeOutTension=.2;
    s.contentOffsetNs=5000000;s.loopBeginNs=100000000;s.loopEndNs=800000000;s.sourceEndNs=2000000000;s.fadeClockNs={0,10000000,1900000000,2000000000};s.fadeClockTicks={0,720720,22342320,23063040};s.signatureNumerator=7;s.signatureDenominator=8;return s;
}
void codec() {
    const auto s=sample();const auto json=encodeSampleSettings(s);const auto decoded=decodeSampleSettings(json);
    check(decoded && *decoded.data==s,"all authored sound, source, loop, fade-clock and signature defaults round trip");
    check(json.size()==34 && json.at("warp_markers").is_string() && !json.contains("clip_id") && !json.contains("track_id") && !json.contains("media_id"),"portable sample settings carry typed controls and native bytes without project identities");
    const auto text=json.dump();check(decodeSampleSettings(Json::parse(text)).data==std::optional(s),"canonical JSON text exchange preserves exact doubles and native marker bytes");
    auto newer=s;StreamHeader header{};std::memcpy(&header,newer.warpMarkers->data(),sizeof header);header.version=2;header.rec_size=24;
    std::vector<std::byte> future(sizeof header+3*24+1,std::byte{0xa5});std::memcpy(future.data(),&header,sizeof header);
    for(std::size_t i=0;i<3;++i)std::memcpy(future.data()+sizeof header+i*24,s.warpMarkers->data()+sizeof header+i*sizeof(WarpMarker),sizeof(WarpMarker));
    newer.warpMarkers=future;const auto retained=decodeSampleSettings(encodeSampleSettings(newer));check(retained && retained.data->warpMarkers==newer.warpMarkers && audio::readWarpMap(*retained.data->warpMarkers,newer.sourceLength).futureLayout,"readable future AWRP record tails, version and trailer survive as exact authored bytes");
    for(const auto key:{"sample_rate","frames","source_start","source_length","channel_mode","content_offset_ticks"}) {
        auto invalid=json;invalid[key]=-1;check(!decodeSampleSettings(invalid),"negative dimensions or controls refuse without a partial settings object");
    }
    for(const auto key:{"sample_rate","frames","loop_start_ticks","fade_in_ticks"}) {
        auto invalid=json;invalid[key]=1.0;check(!decodeSampleSettings(invalid),"integer controls refuse floating-point coercion");
    }
    auto outside=json;outside["source_start"]=s.frames;check(!decodeSampleSettings(outside),"saved source window cannot extend past the exact source length");
    auto invalid=json;invalid["frames"]=std::numeric_limits<std::uint64_t>::max();check(!decodeSampleSettings(invalid),"unsigned integers outside native signed frame bounds refuse");
    invalid=json;invalid["version"]=2;check(decodeSampleSettings(invalid).problem==SampleSettingsProblem::Unsupported,"future sample-settings versions remain explicitly unsupported for editing");
    invalid=json;invalid["future"]=true;check(decodeSampleSettings(invalid).problem==SampleSettingsProblem::Unsupported,"unknown settings fields are not silently dropped");
    invalid=json;invalid["warp_markers"]="00zz";check(!decodeSampleSettings(invalid),"malformed authored Warp bytes refuse");
    invalid=json;invalid["warp_markers"]=nullptr;check(!decodeSampleSettings(invalid),"enabled Warp cannot lose its authored marker map");
    invalid=json;invalid["source_length"]=95999;check(!decodeSampleSettings(invalid),"native marker coverage is checked against the saved source window");
    invalid=json;invalid["loop_end_ns"]=nullptr;check(!decodeSampleSettings(invalid),"partial source-clock loops refuse");
    invalid=json;invalid["fade_clock_ns"][1]=0;check(!decodeSampleSettings(invalid),"a fade clock must have a positive ordered extent");
    invalid=json;invalid["signature_denominator"]=3;check(!decodeSampleSettings(invalid),"unsupported time signatures refuse");
    auto malformed=s;malformed.gainDb=std::numeric_limits<double>::quiet_NaN();std::string error;check(validateSampleSettings(malformed,error)==SampleSettingsProblem::Invalid,"nonfinite authored controls refuse before serialization");
    // The refused decode returns one empty SampleSettingsResult; the refused validate and encode return no container.
    const auto cost=emptyResultCost<SampleSettingsResult>();
    const auto io=audio::callbackFileIo.load(),a=allocations.load(),f=frees.load();SampleSettingsProblem decodedProblem{},validatedProblem{};bool empty=false,noException=true;error="keep";
    {audio::CallbackScope callback;try{decodedProblem=decodeSampleSettings(json).problem;validatedProblem=validateSampleSettings(s,error);empty=encodeSampleSettings(s).is_null();}catch(const std::exception&){noException=false;}}
    check(noException && empty && decodedProblem==SampleSettingsProblem::Realtime && validatedProblem==SampleSettingsProblem::Realtime && error=="keep" && io==audio::callbackFileIo.load() && a+cost.allocated==allocations.load() && f+cost.freed==frees.load(),"pure settings codec refuses callbacks without allocating, freeing, I/O or caller mutation");
}
void persistence() {
    test::TempDirectory temp{"sample_settings","library"};const auto file=temp.path()/"data/library.sqlite";std::string error;auto index=Index::open(file,error,false);if(!index)throw std::runtime_error(error);const auto s=sample();
    check(index->sampleSettings(hash).problem==SampleSettingsProblem::Missing,"sample defaults may be absent before any library scan");
    Json tags={{"version",1},{"media",{{hash,{{"tags",{"warm"}},{"bpm",123.5},{"rating",4}}}}}};
    check(index->importJson(tags.dump(),error),"existing version-one tag/BPM/rating exchange still imports");
    check(index->saveSampleSettings(hash,s,error)==SampleSettingsProblem::None,"authored sample settings save into the persistent library by content hash");
    check(index->sampleSettings(hash).data==std::optional(s) && index->entries().empty(),"saved defaults are independent of mounted volumes and file-index rows");
    auto exported=Json::parse(index->exportJson());check(exported.at("version")==2 && exported["media"][hash]["tags"]==Json::array({"warm"}) && exported["media"][hash]["bpm"]==123.5,"version-two exchange retains authored defaults and existing annotation fields");
    SQLite::Database observer(media::pathUtf8(file),SQLite::OPEN_READONLY);const auto version=observer.execAndGet("PRAGMA data_version").getInt64();
    check(index->saveSampleSettings(hash,s,error)==SampleSettingsProblem::None && observer.execAndGet("PRAGMA data_version").getInt64()==version,"saving identical defaults performs no persistent update");
    index.reset();index=Index::open(file,error,false);check(index && index->sampleSettings(hash).data==std::optional(s),"closed and reopened native library retains all authored defaults");
    const auto copied=temp.path()/"elsewhere/library.sqlite";auto second=Index::open(copied,error,false);if(!second)throw std::runtime_error(error);
    check(second->importJson(index->exportJson(),error) && second->sampleSettings(hash).data==std::optional(s),"settings exchange binds by bytes, independent of source paths and library location");
    tags["media"][hash]["tags"]=Json::array({"soft"});check(index->importJson(tags.dump(),error) && index->sampleSettings(hash).data==std::optional(s),"legacy metadata merges preserve the complete authored sample-settings object");
    const auto before=index->exportJson();auto invalid=exported;invalid["media"][other]=exported["media"][hash];invalid["media"][other]["sample_settings"]["source_length"]=0;invalid["media"][hash]["rating"]=1;
    check(!index->importJson(invalid.dump(),error) && index->exportJson()==before,"a bad later imported sample rolls back every earlier metadata/default change");
    invalid=exported;invalid["version"]=1;check(!index->importJson(invalid.dump(),error) && index->exportJson()==before,"legacy envelope cannot silently admit a new authored-settings contract");
    // The refused read returns one empty SampleSettingsResult and the refused export one empty string.
    const auto resultCost=emptyResultCost<SampleSettingsResult>(),textCost=emptyResultCost<std::string>();
    const auto io=audio::callbackFileIo.load(),a=allocations.load(),f=frees.load();SampleSettingsProblem read{},write{},clear{};bool noExport=false,noImport=false;error="keep";
    {audio::CallbackScope callback;read=index->sampleSettings(hash).problem;write=index->saveSampleSettings(hash,s,error);clear=index->clearSampleSettings(hash,error);noExport=index->exportJson().empty();noImport=!index->importJson(before,error);}
    check(read==SampleSettingsProblem::Realtime && write==SampleSettingsProblem::Realtime && clear==SampleSettingsProblem::Realtime && noExport && noImport && error=="keep" && io==audio::callbackFileIo.load() && a+resultCost.allocated+textCost.allocated==allocations.load() && f+resultCost.freed+textCost.freed==frees.load(),"native settings and exchange APIs refuse callbacks without allocation, free, I/O or caller mutation");
    check(index->exportJson()==before,"callback misuse changes no authored library data");
    check(index->clearSampleSettings(hash,error)==SampleSettingsProblem::None && index->sampleSettings(hash).problem==SampleSettingsProblem::Missing,"explicitly clearing sample defaults affects only that hash's authored settings");
    exported=Json::parse(index->exportJson());check(exported.at("version")==1 && exported["media"][hash]["tags"]==Json::array({"soft","warm"}) && exported["media"][hash]["rating"]==4,"clearing defaults preserves tags/BPM/rating and legacy-compatible exchange");
    check(second->sampleSettings(hash).data==std::optional(s),"clearing one library never deletes another persistent copy");
}
void futureAndFailures() {
    test::TempDirectory temp{"sample_settings","future"};const auto file=temp.path()/"library.sqlite";std::string error;auto index=Index::open(file,error,false);if(!index)throw std::runtime_error(error);const auto s=sample();if(index->saveSampleSettings(hash,s,error)!=SampleSettingsProblem::None)throw std::runtime_error("seed settings");
    SQLite::Database native(media::pathUtf8(file),SQLite::OPEN_READWRITE);auto payload=Json::parse(index->exportJson())["media"][hash];payload["sample_settings"]["version"]=99;payload["sample_settings"]["unknown"]="retain";
    {SQLite::Statement put(native,"UPDATE annotations SET payload=? WHERE hash=?");put.bind(1,payload.dump());put.bind(2,hash);put.exec();}
    const auto before=index->exportJson();check(index->sampleSettings(hash).problem==SampleSettingsProblem::Unsupported,"unsupported authored settings stay stored for a later reader");
    check(index->saveSampleSettings(hash,s,error)==SampleSettingsProblem::Unsupported && index->exportJson()==before,"a current save cannot overwrite future authored data");
    Json tags={{"version",1},{"media",Json::object()}};tags["media"][hash]={{"tags",Json::array({"future-safe"})}};check(index->importJson(tags.dump(),error) && Json::parse(index->exportJson())["media"][hash]["sample_settings"]==payload["sample_settings"],"unrelated metadata updates preserve future authored defaults opaquely");
    check(index->clearSampleSettings(hash,error)==SampleSettingsProblem::None && index->sampleSettings(hash).problem==SampleSettingsProblem::Missing,"an explicit user clear can remove unsupported defaults while preserving other metadata");
    if(index->saveSampleSettings(hash,s,error)!=SampleSettingsProblem::None)throw std::runtime_error("restore settings");
    const auto stable=index->exportJson();
    native.exec("CREATE TRIGGER refuse_settings BEFORE UPDATE ON annotations BEGIN SELECT RAISE(ABORT,'controlled failure'); END");auto changed=s;changed.gainDb=-12;
    check(index->saveSampleSettings(hash,changed,error)==SampleSettingsProblem::Database && index->exportJson()==stable,"late native SQL failure preserves all previous authored settings");native.exec("DROP TRIGGER refuse_settings");
    check(index->saveSampleSettings(hash,changed,error)==SampleSettingsProblem::None && index->sampleSettings(hash).data==std::optional(changed),"the same authored update can retry after native write failure");
    const auto saved=index->exportJson();
    auto originalPayload=Json::parse(saved)["media"][hash];auto deepPayload=originalPayload;Json nested=0;
    for(unsigned i=0;i<40;++i)nested=Json::array({std::move(nested)});
    deepPayload["future_metadata"]=std::move(nested);const auto deepText=deepPayload.dump();
    {SQLite::Statement put(native,"UPDATE annotations SET payload=? WHERE hash=?");put.bind(1,deepText);put.bind(2,hash);put.exec();}
    const auto deepProblem=index->sampleSettings(hash).problem;bool untouched=false;
    {SQLite::Statement get(native,"SELECT payload FROM annotations WHERE hash=?");get.bind(1,hash);untouched=get.executeStep() && get.getColumn(0).getString()==deepText;}
    {SQLite::Statement restore(native,"UPDATE annotations SET payload=? WHERE hash=?");restore.bind(1,originalPayload.dump());restore.bind(2,hash);restore.exec();}
    check(deepProblem==SampleSettingsProblem::Database && untouched,"deep but otherwise readable future metadata refuses in preflight without deleting authored bytes");
    const std::string duplicate="{\"version\":1,\"version\":2,\"media\":{}}";check(!index->importJson(duplicate,error) && index->exportJson()==saved,"duplicate JSON keys refuse before persistent changes");
    const std::string escaped="{\"version\":1,\"ver\\u0073ion\":2,\"media\":{}}";check(!index->importJson(escaped,error) && index->exportJson()==saved,"escaped duplicate key spellings cannot bypass exchange validation");
    std::string deep(40,'[');deep+="0";deep.append(40,']');check(!index->importJson(deep,error) && index->exportJson()==saved,"excessively nested exchange refuses in the bounded preflight before DOM parsing");
    check(index->saveSampleSettings("../outside",s,error)==SampleSettingsProblem::Invalid && index->sampleSettings("../outside").problem==SampleSettingsProblem::Invalid && index->exportJson()==saved,"path-shaped settings keys never reach persistent row writes");
}
}
int main(){try{codec();persistence();futureAndFailures();}catch(const std::exception& e){++failures;std::printf("EXCEPTION: %s\n",e.what());}std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
