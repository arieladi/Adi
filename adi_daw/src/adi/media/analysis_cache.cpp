// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/media/analysis_cache.hpp"
#include "adi/media/blake3.hpp"
#include "adi/media/publish_file.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/appdata.hpp"
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
bool hashValid(const std::string& hash){return hash.size()==64 && std::all_of(hash.begin(),hash.end(),[](char c){return (c>='0' && c<='9') || (c>='a' && c<='f');});}
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
AnalysisCacheResult readFile(const fs::path& file,const AnalysisIdentity& id,std::stop_token stop) {
    if(stop.stop_requested())return failure(AnalysisCacheProblem::Cancelled);
    std::error_code ec;const auto status=fs::symlink_status(file,ec);
    if(ec==std::errc::no_such_file_or_directory || status.type()==fs::file_type::not_found)return failure(AnalysisCacheProblem::Missing);
    if(ec || !fs::is_regular_file(status))return failure(AnalysisCacheProblem::Io,"analysis is not a real regular file");
    const auto size=fs::file_size(file,ec);if(ec)return failure(AnalysisCacheProblem::Io,ec.message());
    if(size<header || size>maximum)return failure(AnalysisCacheProblem::Invalid,"analysis file exceeds bounds");
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    {audio::fileIoPoint();std::ifstream in(file,std::ios::binary);if(!in || !in.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size())))return failure(AnalysisCacheProblem::Io,"cannot read analysis");char extra=0;if(in.get(extra))return failure(AnalysisCacheProblem::Invalid,"analysis grew during read");}
    if(stop.stop_requested())return failure(AnalysisCacheProblem::Cancelled);
    auto result=decodeAnalysis(bytes);if(result && result.data->identity!=id)return failure(AnalysisCacheProblem::Invalid,"analysis identity changed");return result;
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
    try {std::lock_guard lock(ioMutex);return readFile(folder_/(id.hash+".adia"),id,stop);}
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
        auto previous=readFile(target,id,stop);if(previous.problem==AnalysisCacheProblem::Cancelled)return previous.problem;
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
AnalysisCache& defaultAnalysisCache(){static AnalysisCache cache([]{const auto paths=appdata::pathsFor(appdata::App::Daw);return paths.cache.empty()?fs::path{}:paths.cache/"analysed";}());return cache;}
} // namespace adi::media
