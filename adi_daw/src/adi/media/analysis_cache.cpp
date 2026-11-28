// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/media/analysis_cache.hpp"
#include "adi/media/blake3.hpp"
#include "adi/media/publish_file.hpp"
#include "adi/audio/decode.hpp"
#include "adi/audio/io_audit.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <limits>
#include <mutex>
#include <stdexcept>
namespace adi::media {
namespace fs=std::filesystem;
namespace {
constexpr std::size_t header=168,maximum=32*1024*1024,sectionHeader=16;
std::mutex ioMutex; // all instances in this process; no reader remains open
template<class String> bool hashValid(const String& hash){return hash.size()==64 && std::all_of(hash.begin(),hash.end(),[](auto c){return (c>='0' && c<='9') || (c>='a' && c<='f');});}
bool identityValid(const AnalysisIdentity& id){return hashValid(id.hash) && id.sampleRate>0 && id.sampleRate<=768000 && id.channels>0 && id.channels<=64 && id.frames<=static_cast<std::uint64_t>(INT64_MAX);}
bool tagValid(const std::string& tag){return tag.size()==4 && std::all_of(tag.begin(),tag.end(),[](char c){return c>='A' && c<='Z';});}
void put(std::vector<std::byte>& data,std::size_t at,std::uint64_t value,unsigned width){for(unsigned i=0;i<width;++i)data[at+i]=static_cast<std::byte>((value>>(8*i))&255);}
std::uint64_t get(std::span<const std::byte> data,std::size_t at,unsigned width){std::uint64_t value=0;for(unsigned i=0;i<width;++i)value|=static_cast<std::uint64_t>(std::to_integer<unsigned>(data[at+i]))<<(8*i);return value;}
void text(std::vector<std::byte>& data,std::size_t at,const std::string& value){for(std::size_t i=0;i<value.size();++i)data[at+i]=static_cast<std::byte>(value[i]);}
std::string text(std::span<const std::byte> data,std::size_t at,std::size_t count){std::string result(count,' ');for(std::size_t i=0;i<count;++i)result[i]=static_cast<char>(data[at+i]);return result;}
// No defaulted message: it would construct and move a second string before the
// callback refusals return (an allocation under MSVC's Debug iterator proxies).
AnalysisCacheResult failure(AnalysisCacheProblem p){return {p,{},{}};}
AnalysisCacheResult failure(AnalysisCacheProblem p,std::string error){return {p,std::move(error),{}};}
AnalysisCacheResult readFile(const fs::path& file,const AnalysisIdentity* expected,std::stop_token stop) {
    if(stop.stop_requested())return failure(AnalysisCacheProblem::Cancelled);
    std::error_code ec;const auto status=fs::symlink_status(file,ec);
    if(ec==std::errc::no_such_file_or_directory || status.type()==fs::file_type::not_found)return failure(AnalysisCacheProblem::Missing);
    if(ec || !fs::is_regular_file(status))return failure(AnalysisCacheProblem::Io,"analysis is not a real regular file");
    const auto size=fs::file_size(file,ec);if(ec)return failure(AnalysisCacheProblem::Io,ec.message());
    if(size<header || size>maximum)return failure(AnalysisCacheProblem::Invalid,"analysis file exceeds bounds");
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    {audio::fileIoPoint();std::ifstream in(file,std::ios::binary);if(!in || !in.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size())))return failure(AnalysisCacheProblem::Io,"cannot read analysis");char extra=0;if(in.get(extra))return failure(AnalysisCacheProblem::Invalid,"analysis grew during read");}
    if(stop.stop_requested())return failure(AnalysisCacheProblem::Cancelled);
    auto result=decodeAnalysis(bytes);if(result && expected && result.data->identity!=*expected)return failure(AnalysisCacheProblem::Invalid,"analysis identity changed");return result;
}
}
std::vector<std::byte> encodeAnalysis(const SampleAnalysis& data) {
    if(audio::onAudioThread)return {};
    if(!identityValid(data.identity) || data.sections.size()>32)throw std::invalid_argument("invalid analysis identity/count");
    std::size_t size=header;
    for(const auto& [tag,section]:data.sections) {
        if(!tagValid(tag) || !section.version || section.bytes.size()>maximum-sectionHeader || size>maximum-sectionHeader-section.bytes.size())throw std::invalid_argument("invalid analysis section/bounds");
        size+=sectionHeader+section.bytes.size();
    }
    std::vector<std::byte> bytes(size);text(bytes,0,"ADIA");put(bytes,4,1,2);put(bytes,6,header,2);text(bytes,8,data.identity.hash);
    put(bytes,72,data.identity.sampleRate,4);put(bytes,76,data.identity.channels,4);put(bytes,80,data.identity.frames,8);put(bytes,88,data.sections.size(),4);put(bytes,92,size-header,4);
    std::size_t at=header;
    for(const auto& [tag,section]:data.sections) {
        text(bytes,at,tag);put(bytes,at+4,section.version,4);put(bytes,at+8,section.configuration,4);put(bytes,at+12,section.bytes.size(),4);at+=sectionHeader;
        std::copy(section.bytes.begin(),section.bytes.end(),bytes.begin()+static_cast<std::ptrdiff_t>(at));at+=section.bytes.size();
    }
    const auto digest=blake3Bytes(std::span(bytes).subspan(header));if(!digest)throw std::runtime_error("cannot hash analysis payload");text(bytes,96,digest.hex);return bytes;
}
AnalysisCacheResult decodeAnalysis(std::span<const std::byte> bytes) {
    if(audio::onAudioThread)return failure(AnalysisCacheProblem::Realtime);
    try {
        if(bytes.size()<header || bytes.size()>maximum || text(bytes,0,4)!="ADIA" || get(bytes,4,2)!=1 || get(bytes,6,2)!=header || get(bytes,92,4)!=bytes.size()-header || get(bytes,88,4)>32 || get(bytes,160,8)!=0)return failure(AnalysisCacheProblem::Invalid,"invalid analysis envelope");
        SampleAnalysis data;data.identity={text(bytes,8,64),static_cast<std::uint32_t>(get(bytes,72,4)),static_cast<std::uint32_t>(get(bytes,76,4)),get(bytes,80,8)};
        if(!identityValid(data.identity))return failure(AnalysisCacheProblem::Invalid,"invalid analysis source identity");
        const auto digest=blake3Bytes(bytes.subspan(header));if(!digest || digest.hex!=text(bytes,96,64))return failure(AnalysisCacheProblem::Invalid,"analysis payload digest mismatch");
        std::size_t at=header;const auto count=get(bytes,88,4);
        // Bounds/duplicates/version are checked before allocating section data.
        std::map<std::string,std::size_t> offsets;
        for(std::uint64_t i=0;i<count;++i) {
            if(bytes.size()-at<sectionHeader)return failure(AnalysisCacheProblem::Invalid,"short analysis section");
            const auto tag=text(bytes,at,4);const auto length=get(bytes,at+12,4);
            if(!tagValid(tag) || !get(bytes,at+4,4) || length>bytes.size()-at-sectionHeader || !offsets.emplace(tag,at).second)return failure(AnalysisCacheProblem::Invalid,"invalid analysis section");
            at+=sectionHeader+static_cast<std::size_t>(length);
        }
        if(at!=bytes.size())return failure(AnalysisCacheProblem::Invalid,"trailing analysis bytes");
        for(const auto& [tag,offset]:offsets) {
            const auto length=static_cast<std::size_t>(get(bytes,offset+12,4));AnalysisSection section;
            section.version=static_cast<std::uint32_t>(get(bytes,offset+4,4));section.configuration=static_cast<std::uint32_t>(get(bytes,offset+8,4));
            section.bytes.assign(bytes.begin()+static_cast<std::ptrdiff_t>(offset+sectionHeader),bytes.begin()+static_cast<std::ptrdiff_t>(offset+sectionHeader+length));data.sections.emplace(tag,std::move(section));
        }
        return {AnalysisCacheProblem::None,{},std::move(data)};
    }catch(const std::bad_alloc&){return failure(AnalysisCacheProblem::Resource);}
    catch(const std::exception& e){return failure(AnalysisCacheProblem::Invalid,e.what());}
}
AnalysisCacheResult AnalysisCache::read(const AnalysisIdentity& id,std::stop_token stop)const {
    if(audio::onAudioThread)return failure(AnalysisCacheProblem::Realtime);
    if(!identityValid(id))return failure(AnalysisCacheProblem::Invalid);
    if(folder_.empty() || !folder_.is_absolute())return failure(AnalysisCacheProblem::Io,"analysis folder must be absolute");
    try {
        std::lock_guard lock(ioMutex);const auto file=folder_/(id.hash+".adia");
        auto result=readFile(file,&id,stop);
        // Cache recency is optional metadata: failure to stamp a readable cache
        // must not turn a usable waveform into a miss (e.g. a read-only volume).
        if(result){std::error_code ec;fs::last_write_time(file,fs::file_time_type::clock::now(),ec);}
        return result;
    }
    catch(const std::bad_alloc&){return failure(AnalysisCacheProblem::Resource);}
    catch(const std::exception& e){return failure(AnalysisCacheProblem::Io,e.what());}
}
AnalysisCacheProblem AnalysisCache::update(const AnalysisIdentity& id,const std::string& tag,const AnalysisSection& section,std::string& error,std::stop_token stop) {
    if(audio::onAudioThread)return AnalysisCacheProblem::Realtime;
    if(stop.stop_requested())return AnalysisCacheProblem::Cancelled;
    if(!identityValid(id) || !tagValid(tag) || !section.version || section.bytes.size()>maximum-sectionHeader-header)return AnalysisCacheProblem::Invalid;
    if(folder_.empty() || !folder_.is_absolute()){error="analysis folder must be absolute";return AnalysisCacheProblem::Io;}
    fs::path directory;
    struct Cleanup {fs::path& path;~Cleanup(){if(!path.empty()){std::error_code ec;fs::remove_all(path,ec);}}} cleanup{directory};
    try {
        std::lock_guard lock(ioMutex);const auto target=folder_/(id.hash+".adia");
        auto previous=readFile(target,&id,stop);if(previous.problem==AnalysisCacheProblem::Cancelled)return previous.problem;
        if(previous.problem==AnalysisCacheProblem::Io || previous.problem==AnalysisCacheProblem::Resource){error=previous.error;return previous.problem;}
        SampleAnalysis data=previous?std::move(*previous.data):SampleAnalysis{id,{}};data.sections[tag]=section;const auto bytes=encodeAnalysis(data);
        if(stop.stop_requested())return AnalysisCacheProblem::Cancelled;
        fs::create_directories(folder_);static std::atomic<std::uint64_t> serial{0};
        for(unsigned i=0;i<128;++i) {
            const auto stamp=std::chrono::steady_clock::now().time_since_epoch().count();const auto candidate=folder_/(".stage-"+std::to_string(stamp)+"-"+std::to_string(serial.fetch_add(1)));
            std::error_code ec;if(fs::create_directory(candidate,ec)){directory=candidate;break;}if(ec && ec!=std::errc::file_exists)throw std::system_error(ec);
        }
        if(directory.empty())throw std::runtime_error("cannot reserve private analysis stage");
        fs::permissions(directory,fs::perms::owner_all,fs::perm_options::replace);
        const auto stage=directory/"analysis";
        {audio::fileIoPoint();std::ofstream out(stage,std::ios::binary);out.exceptions(std::ios::badbit|std::ios::failbit);out.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));out.flush();out.close();}
        if(stop.stop_requested())return AnalysisCacheProblem::Cancelled;
        const auto replaced=replaceCacheFile(stage,target);if(replaced){error=replaced.message();return AnalysisCacheProblem::Io;}error.clear();return AnalysisCacheProblem::None;
    }catch(const std::bad_alloc&){return AnalysisCacheProblem::Resource;}
    catch(const std::exception& e){error=e.what();return AnalysisCacheProblem::Io;}
}
AnalysisCacheProblem AnalysisCache::remove(const std::string& hash,std::string& error) {
    if(audio::onAudioThread)return AnalysisCacheProblem::Realtime;
    if(!hashValid(hash))return AnalysisCacheProblem::Invalid;
    if(folder_.empty() || !folder_.is_absolute()){error="analysis folder must be absolute";return AnalysisCacheProblem::Io;}
    try {std::lock_guard lock(ioMutex);const auto target=folder_/(hash+".adia");std::error_code ec;const auto status=fs::symlink_status(target,ec);
        if(ec==std::errc::no_such_file_or_directory || status.type()==fs::file_type::not_found){error.clear();return AnalysisCacheProblem::None;}
        if(ec || !fs::is_regular_file(status)){error="analysis is not a real regular file";return AnalysisCacheProblem::Io;}
        fs::remove(target);error.clear();return AnalysisCacheProblem::None;
    }catch(const std::exception& e){error=e.what();return AnalysisCacheProblem::Io;}
}
AnalysisTrimResult AnalysisCache::trimToBytes(std::uint64_t maximumBytes,std::string& error,std::stop_token stop) {
    AnalysisTrimResult result;
    if(audio::onAudioThread){result.problem=AnalysisCacheProblem::Realtime;return result;}
    if(stop.stop_requested()){result.problem=AnalysisCacheProblem::Cancelled;return result;}
    if(folder_.empty() || !folder_.is_absolute()){error="analysis folder must be absolute";result.problem=AnalysisCacheProblem::Io;return result;}
    struct Candidate {fs::path path;std::uint64_t bytes;fs::file_time_type stamp;};
    std::string firstError;
    try {
        std::lock_guard lock(ioMutex);
        if(stop.stop_requested()){result.problem=AnalysisCacheProblem::Cancelled;return result;}
        std::error_code ec;const auto root=fs::symlink_status(folder_,ec);
        if(ec==std::errc::no_such_file_or_directory || root.type()==fs::file_type::not_found) {
            result.inventoryComplete=true;result.budgetMet=true;error.clear();return result;
        }
        if(ec)throw std::system_error(ec);
        if(!fs::is_directory(root))throw std::runtime_error("analysis folder is not a real directory");
        std::vector<Candidate> candidates;
        bool unvalidatedSkipped=false;
        const auto named=[](const fs::path& path,const std::string& why) {
            const auto text=path.u8string();return std::string(text.begin(),text.end())+": "+why;
        };
        const auto skip=[&](const fs::path& path,const std::string& why,bool unvalidated) {
            ++result.skippedEntries;result.problem=AnalysisCacheProblem::Io;
            unvalidatedSkipped=unvalidatedSkipped || unvalidated;
            if(firstError.empty()){firstError=named(path,why);error=firstError;}
        };
        audio::fileIoPoint();
        // Inventory and validate everything before deleting anything. Unknown,
        // damaged and future-envelope files remain for explicit repair, not GC.
        for(const auto& entry:fs::directory_iterator(folder_)) {
            if(stop.stop_requested()){result.problem=AnalysisCacheProblem::Cancelled;return result;}
            const auto& path=entry.path();
            // Reject foreign native names before a Windows narrow conversion:
            // an unrelated Unicode .adia filename must not abort maintenance.
            if(path.extension()!=".adia" || !hashValid(path.stem().native())){++result.ignoredEntries;continue;}
            const auto status=entry.symlink_status(ec);
            if(ec){skip(path,ec.message(),true);continue;}
            if(!fs::is_regular_file(status)){++result.ignoredEntries;continue;}
            const auto hash=path.stem().string(); // proven lowercase ASCII
            const auto size=fs::file_size(path,ec);if(ec){skip(path,ec.message(),true);continue;}
            const auto stamp=fs::last_write_time(path,ec);if(ec){skip(path,ec.message(),true);continue;}
            auto data=readFile(path,nullptr,stop);
            if(data.problem==AnalysisCacheProblem::Cancelled || stop.stop_requested()){result.problem=AnalysisCacheProblem::Cancelled;return result;}
            if(data.problem==AnalysisCacheProblem::Invalid || (data && data.data->identity.hash!=hash)) {++result.ignoredEntries;continue;}
            if(data.problem==AnalysisCacheProblem::Io || data.problem==AnalysisCacheProblem::Missing) {
                skip(path,data.error.empty()?"analysis disappeared during inventory":data.error,true);continue;
            }
            if(!data){result.problem=data.problem;error=std::move(data.error);return result;}
            const auto checkedSize=fs::file_size(path,ec);if(ec){skip(path,ec.message(),true);continue;}
            const auto checkedStamp=fs::last_write_time(path,ec);if(ec){skip(path,ec.message(),true);continue;}
            if(checkedSize!=size || checkedStamp!=stamp)throw std::runtime_error(named(path,"analysis changed during inventory"));
            if(size>std::numeric_limits<std::uint64_t>::max()-result.bytesBefore)throw std::overflow_error("analysis inventory size overflow");
            candidates.push_back({path,static_cast<std::uint64_t>(size),stamp});
            result.bytesBefore+=static_cast<std::uint64_t>(size);result.bytesAfter=result.bytesBefore;
        }
        result.inventoryComplete=true;
        std::sort(candidates.begin(),candidates.end(),[](const Candidate& a,const Candidate& b){
            return a.stamp!=b.stamp?a.stamp<b.stamp:a.path<b.path;
        });
        for(const auto& candidate:candidates) {
            if(result.bytesAfter<=maximumBytes)break;
            if(stop.stop_requested()){result.problem=AnalysisCacheProblem::Cancelled;return result;}
            // This detects ordinary external changes; it is not a cross-process
            // compare-and-unlink. Ownership and the in-process mutex are needed.
            const auto status=fs::symlink_status(candidate.path,ec);
            if(ec==std::errc::no_such_file_or_directory || status.type()==fs::file_type::not_found)
                throw std::runtime_error(named(candidate.path,"analysis disappeared before eviction"));
            if(ec){skip(candidate.path,ec.message(),false);continue;}
            if(!fs::is_regular_file(status))throw std::runtime_error(named(candidate.path,"analysis changed before eviction"));
            const auto size=fs::file_size(candidate.path,ec);if(ec){skip(candidate.path,ec.message(),false);continue;}
            const auto stamp=fs::last_write_time(candidate.path,ec);if(ec){skip(candidate.path,ec.message(),false);continue;}
            if(size!=candidate.bytes || stamp!=candidate.stamp)
                throw std::runtime_error(named(candidate.path,"analysis changed before eviction"));
            const auto removed=fs::remove(candidate.path,ec);
            if(ec){skip(candidate.path,ec.message(),false);continue;}
            if(!removed)throw std::runtime_error(named(candidate.path,"analysis disappeared before eviction"));
            result.bytesAfter-=candidate.bytes;++result.filesRemoved;
        }
        if(stop.stop_requested()){result.problem=AnalysisCacheProblem::Cancelled;return result;}
        result.budgetMet=!unvalidatedSkipped && result.bytesAfter<=maximumBytes;
        if(firstError.empty())error.clear();
        return result;
    }catch(const std::bad_alloc&){result.problem=AnalysisCacheProblem::Resource;return result;}
    catch(const std::exception& e){result.problem=AnalysisCacheProblem::Io;error=firstError.empty()?e.what():firstError+"; trim aborted: "+e.what();return result;}
}
AnalysisCache& defaultAnalysisCache() {
    static AnalysisCache cache([] {
        const auto root = audio::defaultMediaCacheRoot();
        return root.empty() ? fs::path{} : root / "analysed";
    }());
    return cache;
}
} // namespace adi::media
