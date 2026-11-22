// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/audio/decode.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/media/blake3.hpp"
#include "temp_directory.hpp"
#include <array>
#include <cstdio>
#include <fstream>
#include <limits>
#include <string>

namespace {
namespace fs=std::filesystem;
using namespace adi::audio;
using Json=nlohmann::json;
int checks=0,failures=0;
void check(bool okay,const std::string& name){++checks;if(!okay){++failures;std::printf("FAIL %s\n",name.c_str());}}
fs::path fixture(const char* name){return fs::path(ADI_DECODE_FIXTURES)/name;}
DecodeCacheHooks hooks(){DecodeCacheHooks out;out.clock=[]{return std::int64_t{100};};out.freeSpace=[](const fs::path&){return std::uint64_t{1}<<40;};return out;}
template<class F> DecodeProblem problem(F&& action,std::string* text=nullptr){
    try{action();}catch(const DecodeError& e){if(text)*text=e.what();return e.problem();}
    catch(const std::exception& e){if(text)*text=e.what();return static_cast<DecodeProblem>(-1);}
    return static_cast<DecodeProblem>(-2);
}
bool noPartial(const fs::path& folder){
    for(const auto& file:fs::directory_iterator(folder)) {
        if(file.path().filename().native().find(fs::path(".partial-").native())!=fs::path::string_type::npos)return false;
    }
    return true;
}
std::string hash(const fs::path& path){return adi::media::blake3File(path).hex;}
void oversized(bool damaged){
    adi::test::TempDirectory temp("decode_consistency",damaged?"claimed":"genuine");
    DecodeCache cache(temp.path()/"cache",{1ULL<<30,0},hooks());
    const auto a=cache.playable(fixture("tone24.flac")),b=cache.playable(fixture("rate8k.flac"));
    cache.release(a);cache.release(b);const auto ah=hash(a),bh=hash(b);const auto bytes=cache.totalBytes();
    const auto before=cache.stats();cache.setLimits({32768,0});
    auto source=fixture("tone16.flac");
    if(damaged){
        source=temp.path()/"claimed-four-billion.flac";fs::copy_file(fixture("tone16.flac"),source);
        // STREAMINFO's low 36 bits at bytes18..25 are total PCM frames.
        std::fstream edit(source,std::ios::binary|std::ios::in|std::ios::out);
        std::array<unsigned char,8> info{};edit.seekg(18);edit.read(reinterpret_cast<char*>(info.data()),8);
        std::uint64_t packed=0;for(auto byte:info)packed=(packed<<8)|byte;
        packed=(packed&0xfffffff000000000ULL)|4000000000ULL;
        for(std::size_t n=info.size();n-- >0;){info[n]=static_cast<unsigned char>(packed&255);packed>>=8;}
        edit.seekp(18);edit.write(reinterpret_cast<const char*>(info.data()),8);edit.close();
    }
    std::string reason;
    check(problem([&]{(void)cache.playable(source);},&reason)==(damaged?DecodeProblem::Corrupt:DecodeProblem::NoSpace),
          damaged?"claimed huge FLAC refuses before decoding":"genuine file exceeding estimate cap refuses");
    check(damaged?reason.find("encoded byte length")!=std::string::npos:
          reason.find("maximum")!=std::string::npos && reason.find("estimated")!=std::string::npos,
          "refusal names the physical FLAC contradiction or estimate maximum, not pinned files");
    check(fs::exists(a)&&fs::exists(b)&&hash(a)==ah&&hash(b)==bh,"oversize refusal preserves both real unpinned cache files");
    check(cache.totalBytes()==bytes&&cache.entries().size()==2&&cache.stats().evictions==before.evictions&&cache.stats().decodes==before.decodes,
          "oversize refusal leaves accounting and decode/eviction counters unchanged");
    check(cache.problems().empty()&&noPartial(cache.folder()),"oversize refusal creates no false busy warning or private leftover");
}
void malformedHints(){
    const std::array<Json,7> invalid={nullptr,"bad",true,Json::array({1}),Json::object({{"x",1}}),1.5,std::numeric_limits<std::uint64_t>::max()};
    for(std::size_t n=0;n<invalid.size();++n){
        adi::test::TempDirectory temp("decode_consistency",("hint"+std::to_string(n)).c_str());
        const std::string good(64,'a'),bad(64,'b');
        for(const auto& key:{good,bad}){WavWriter writer(temp.path()/(key+".wav"),48000,1,WavFormat::Float32);std::array<float,64> samples{};writer.write(samples.data(),64);writer.close();}
        const auto a=temp.path()/(good+".wav"),b=temp.path()/(bad+".wav");const auto ah=hash(a),bh=hash(b);
        const Json index={{"schema",1},{"entries",{{good,{{"bytes","ignored-size-hint"},{"lastUse",42}}},{bad,{{"bytes",999},{"lastUse",invalid[n]}}}}}};
        {std::ofstream out(temp.path()/"index.json");out<<index;}
        bool goodHint=false,badAdopted=false;try{
            DecodeCache cache(temp.path(),{1ULL<<30,0},hooks());
            for(const auto& entry:cache.entries()){
                if(entry.hash==good)goodHint=entry.lastUse==42&&entry.bytes==fs::file_size(a);
                if(entry.hash==bad)badAdopted=entry.lastUse==0&&entry.pins==0&&entry.bytes==fs::file_size(b);
            }
        }catch(const std::exception&){}
        check(goodHint&&badAdopted,"malformed lastUse is ignored per entry; real files remain accounted: "+std::to_string(n));
        check(hash(a)==ah&&hash(b)==bh,"malformed hint never alters cache PCM bytes: "+std::to_string(n));
    }
}
void extremeStamp(){
    adi::test::TempDirectory temp("decode_consistency","stamp-boundary");
    const auto source=fixture("rate8k.flac");
    DecodeCache seed(temp.path(),{1ULL<<30,0},hooks());const auto file=seed.playable(source);seed.release(file);
    const auto maximum=std::numeric_limits<std::int64_t>::max();
    const Json index={{"schema",1},{"entries",{{hash(source),{{"lastUse",maximum}}}}}};
    {std::ofstream out(temp.path()/"index.json");out<<index;}
    DecodeCache cache(temp.path(),{1ULL<<30,0},hooks());
    const auto first=cache.playable(source),second=cache.playable(source);
    check(first==file&&second==file&&cache.entries().front().lastUse==maximum,
          "largest representable use hint never overflows the next stamp");
    check(cache.entries().front().pins==2,"saturated stamp preserves independent cache hit pins");
    cache.release(first);cache.release(second);
}
void publishedCancellation(bool concurrent){
    adi::test::TempDirectory temp("decode_consistency",concurrent?"adopted":"published");
    const auto source=fixture("tone16.flac"),folder=temp.path()/"cache";
    const auto target=folder/(hash(source)+".wav");
    DecodeCache reference(temp.path()/"reference",{1ULL<<30,0},hooks());
    const auto expected=reference.playable(source);std::string winnerHash;bool copied=false;
    std::stop_source stop;auto controlled=hooks();unsigned boundaries=0;
    controlled.afterPublish=[&]{++boundaries;check(fs::exists(target),"cancellation seam is after target publication/adoption");stop.request_stop();};
    if(concurrent)controlled.freeSpace=[&](const fs::path&){if(!copied){fs::copy_file(expected,target);std::ofstream(target,std::ios::binary|std::ios::app)<<"winner";winnerHash=hash(target);copied=true;}return std::uint64_t{1}<<40;};
    DecodeCache cache(folder,{1ULL<<30,0},controlled);
    check(problem([&]{(void)cache.playable(source,stop.get_token());})==DecodeProblem::Cancelled&&boundaries==1,"post-publication cancellation is returned to caller");
    const auto entries=cache.entries();
    check(entries.size()==1&&entries[0].bytes==fs::file_size(target)&&entries[0].pins==0&&cache.totalBytes()==fs::file_size(target),
          "cancelled published file is immediately counted with no unreturned lease pin");
    check(cache.stats().decodes==1&&noPartial(folder),"completed decode is counted and private partial is gone");
    Json persisted;{std::ifstream in(folder/"index.json");persisted=Json::parse(in,nullptr,false);}
    check(persisted.is_object()&&persisted.contains("entries")&&persisted["entries"].contains(hash(source)),"index save happens before cancellation is returned");
    if(concurrent)check(hash(target)==winnerHash,"cancelled adoption preserves concurrent winner bytes");
    const auto retry=cache.playable(source);check(retry==target&&cache.stats().hits==1&&cache.entries().front().pins==1,"retry hits complete target and owns exactly one pin");
    cache.release(retry);cache.setLimits({0,0});cache.trim();
    check(cache.entries().empty()&&!fs::exists(target),"cancelled decode does not leak a pin or evade later trim");
}
}
int main(){try{oversized(false);oversized(true);malformedHints();extremeStamp();publishedCancellation(false);publishedCancellation(true);}
    catch(const std::exception& e){check(false,std::string("unexpected: ")+e.what());}
    std::printf("%s -- %d checks, %d failures\n",failures?"FAIL":"PASS",checks,failures);return failures?1:0;}
