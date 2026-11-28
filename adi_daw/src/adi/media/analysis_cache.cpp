// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/media/analysis_cache.hpp"
#include "adi/media/blake3.hpp"
#include "adi/media/publish_file.hpp"
#include "adi/audio/decode.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/appdata.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <unordered_set>
namespace adi::media {
namespace fs=std::filesystem;
namespace {
constexpr std::size_t header=168,maximum=32*1024*1024,sectionHeader=16;
std::mutex ioMutex; // all instances in this process; no reader remains open
std::mutex limitsMutex; // settings updates never wait for cache inventory
AnalysisCacheLimits appLimits{audio::kCacheMaxSizeMbDefault << 20, audio::kCacheMinFreeMbDefault << 20};
std::atomic<std::uint64_t> decodedBytesHint{0};
template<class String> bool hashValid(const String& hash){return hash.size()==64 && std::all_of(hash.begin(),hash.end(),[](auto c){return (c>='0' && c<='9') || (c>='a' && c<='f');});}
bool identityValid(const AnalysisIdentity& id){return hashValid(id.hash) && id.sampleRate>0 && id.sampleRate<=768000 && id.channels>0 && id.channels<=64 && id.frames<=static_cast<std::uint64_t>(INT64_MAX);}
bool tagValid(const std::string& tag){return tag.size()==4 && std::all_of(tag.begin(),tag.end(),[](char c){return c>='A' && c<='Z';});}
bool realOrMissingDirectory(const fs::path& folder) {
    std::error_code error;
    const auto state = fs::symlink_status(folder, error);
    return error == std::errc::no_such_file_or_directory || (!error && (fs::is_directory(state) ||
        state.type() == fs::file_type::not_found));
}
template<class Clock, class Duration>
std::int64_t unixMicros(std::chrono::time_point<Clock, Duration> value) {
    if constexpr (requires { Clock::to_sys(value); }) {
        return std::chrono::duration_cast<std::chrono::microseconds>(Clock::to_sys(value).time_since_epoch()).count();
    } else {
        auto utc = Clock::to_utc(value);
        using UtcClock = typename decltype(utc)::clock;
        return std::chrono::duration_cast<std::chrono::microseconds>(UtcClock::to_sys(utc).time_since_epoch()).count();
    }
}
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
    return readChecked(id.hash,&id,stop);
}
AnalysisCacheResult AnalysisCache::readByHash(const std::string& hash,std::stop_token stop)const {
    if(audio::onAudioThread)return failure(AnalysisCacheProblem::Realtime);
    if(!hashValid(hash))return failure(AnalysisCacheProblem::Invalid);
    return readChecked(hash,nullptr,stop);
}
AnalysisCacheResult AnalysisCache::readChecked(const std::string& hash,const AnalysisIdentity* expected,std::stop_token stop)const {
    if(folder_.empty() || !folder_.is_absolute())return failure(AnalysisCacheProblem::Io,"analysis folder must be absolute");
    try {
        std::unique_lock lock(ioMutex);const auto file=folder_/(hash+".adia");
        if (!realOrMissingDirectory(folder_)) return failure(AnalysisCacheProblem::Io,"analysis folder is not a real directory");
        auto result=readFile(file,expected,stop);
        if(result && result.data->identity.hash!=hash)return failure(AnalysisCacheProblem::Invalid,"analysis source hash changed");
        // Cache recency is optional metadata: failure to stamp a readable cache
        // must not turn a usable waveform into a miss (e.g. a read-only volume).
        if(result){
            std::error_code ec;fs::last_write_time(file,fs::file_time_type::clock::now(),ec);
            const auto shared = maintainLocked(0,stop);
            lock.unlock();
            if (shared) maintainApplication(stop);
        }
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
        std::unique_lock lock(ioMutex);const auto target=folder_/(id.hash+".adia");
        if (!realOrMissingDirectory(folder_)) {error="analysis folder is not a real directory";return AnalysisCacheProblem::Io;}
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
        const auto replaced=replaceCacheFile(stage,target);if(replaced){error=replaced.message();return AnalysisCacheProblem::Io;}
        // Reads and publications share the same time source. Inherited write
        // times can be rounded by the kernel below a hit stamped just before
        // this publication, which would evict the new result first.
        std::error_code stampError;
        fs::last_write_time(target,fs::file_time_type::clock::now(),stampError);
        const auto shared = maintainLocked(bytes.size(),stop);
        lock.unlock();
        if (shared) maintainApplication(stop);
        error.clear();return AnalysisCacheProblem::None;
    }catch(const std::bad_alloc&){return AnalysisCacheProblem::Resource;}
    catch(const std::exception& e){error=e.what();return AnalysisCacheProblem::Io;}
}
AnalysisCacheProblem AnalysisCache::remove(const std::string& hash,std::string& error) {
    if(audio::onAudioThread)return AnalysisCacheProblem::Realtime;
    if(!hashValid(hash))return AnalysisCacheProblem::Invalid;
    if(folder_.empty() || !folder_.is_absolute()){error="analysis folder must be absolute";return AnalysisCacheProblem::Io;}
    try {std::lock_guard lock(ioMutex);const auto target=folder_/(hash+".adia");std::error_code ec;
        if (!realOrMissingDirectory(folder_)) {error="analysis folder is not a real directory";return AnalysisCacheProblem::Io;}
        const auto status=fs::symlink_status(target,ec);
        if(ec==std::errc::no_such_file_or_directory || status.type()==fs::file_type::not_found){error.clear();return AnalysisCacheProblem::None;}
        if(ec || !fs::is_regular_file(status)){error="analysis is not a real regular file";return AnalysisCacheProblem::Io;}
        fs::remove(target);estimatedBytes_.reset();error.clear();return AnalysisCacheProblem::None;
    }catch(const std::exception& e){error=e.what();return AnalysisCacheProblem::Io;}
}
AnalysisTrimResult AnalysisCache::trimToBytes(std::uint64_t maximumBytes,std::string& error,std::stop_token stop) {
    AnalysisTrimResult result;
    if(audio::onAudioThread){result.problem=AnalysisCacheProblem::Realtime;return result;}
    if(stop.stop_requested()){result.problem=AnalysisCacheProblem::Cancelled;return result;}
    if(folder_.empty() || !folder_.is_absolute()){error="analysis folder must be absolute";result.problem=AnalysisCacheProblem::Io;return result;}
    std::lock_guard lock(ioMutex);
    result=trimLocked({maximumBytes,0},false,error,stop);
    if(result.inventoryComplete)estimatedBytes_=result.bytesAfter;
    return result;
}

std::optional<std::uint64_t> AnalysisCache::freeSpace() const {
    if (available_) return available_(folder_);
    std::error_code error;
    audio::fileIoPoint();
    const auto space = fs::space(folder_, error);
    if (error || space.available == static_cast<std::uintmax_t>(-1)) return {};
    return static_cast<std::uint64_t>(space.available);
}

void AnalysisCache::setAutomaticLimits(AnalysisCacheLimits limits) {
    if (audio::onAudioThread) return;
    std::lock_guard lock(limitsMutex);
    automaticLimits_ = limits;
}

void setDefaultAnalysisCacheLimits(AnalysisCacheLimits limits) {
    if (audio::onAudioThread) return;
    std::lock_guard lock(limitsMutex);
    appLimits = limits;
}

AnalysisCacheLimits defaultAnalysisCacheLimits() {
    std::lock_guard lock(limitsMutex);
    return appLimits;
}

bool AnalysisCache::maintainLocked(std::size_t publishedBytes, std::stop_token stop) const noexcept {
    if (stop.stop_requested()) return false;
    try {
        std::optional<AnalysisCacheLimits> limits;
        bool shared = false;
        {
            std::lock_guard lock(limitsMutex);
            limits = automaticLimits_;
            shared = !limits && followsAppLimits_;
            if (shared) limits = appLimits;
        }
        if (!limits) return false;
        // Keep warm startup and ordinary reads independent of a full library
        // inventory. A publication or a changed policy can request a trim.
        if (!publishedBytes && appliedLimits_ == limits && limits->maximumBytes != 0) return false;
        // Count complete publications conservatively between checks. An
        // overwritten record may cause an early inventory, which uses exact
        // on-disk bytes before deciding whether anything needs eviction.
        if (estimatedBytes_) {
            *estimatedBytes_ += std::min<std::uint64_t>(publishedBytes,
                std::numeric_limits<std::uint64_t>::max() - *estimatedBytes_);
        }
        const auto available = publishedBytes && limits->minimumFreeBytes ? freeSpace() : std::nullopt;
        const bool diskLow = available && *available < limits->minimumFreeBytes;
        const auto decodedBytes = shared ? decodedBytesHint.load(std::memory_order_relaxed) : 0;
        if (appliedLimits_ == limits && estimatedBytes_ && decodedBytes <= limits->maximumBytes &&
            *estimatedBytes_ <= limits->maximumBytes - decodedBytes && !diskLow) return false;
        if (shared) return true;

        std::string ignored;
        const auto result = trimLocked(*limits, true, ignored, stop);
        if (result.inventoryComplete) {
            estimatedBytes_ = result.bytesAfter;
            appliedLimits_ = limits;
        }
    } catch (...) {
        // Maintenance is optional. A complete file or a readable snapshot
        // stays usable even when cache inventory cannot be performed.
    }
    return false;
}

void AnalysisCache::maintainApplication(std::stop_token stop) const noexcept {
    if (stop.stop_requested()) return;
    try {
        auto& decoder = audio::defaultDecodeCache();
        std::lock_guard decodeLock(decoder.mutex_);
        maintainForDecoder(decoder, 0, {}, false, stop);
    } catch (...) {
        // The already published analysis remains useful.
    }
}

void AnalysisCache::maintainForDecoder(audio::DecodeCache& decoder, std::uint64_t incoming,
                                       std::string_view hash, bool checkReachable, std::stop_token stop) const {
    // The caller holds the decoded lock. No ADIA caller enters this method
    // until it has released ioMutex.
    std::lock_guard lock(ioMutex);
    const auto limits = defaultAnalysisCacheLimits();
    const auto budget = limits.maximumBytes - std::min(limits.maximumBytes, incoming);
    const auto minimum = limits.minimumFreeBytes + std::min(incoming,
        std::numeric_limits<std::uint64_t>::max() - limits.minimumFreeBytes);
    const auto decodedTotal = [&] {
        std::uint64_t total = 0;
        for (const auto& entry : decoder.items_) total += std::min(entry.second.bytes,
            std::numeric_limits<std::uint64_t>::max() - total);
        return total;
    };
    const auto decoded = decodedTotal();
    decodedBytesHint.store(decoded, std::memory_order_relaxed);
    if (stop.stop_requested() || folder_.empty()) return;
    const auto available = decoder.folder_.empty() ? freeSpace() : std::optional(decoder.freeBytes());
    if (appliedLimits_ == limits && estimatedBytes_ && decoded <= budget &&
        *estimatedBytes_ <= budget - decoded && (!available || *available >= minimum)) return;

    std::string error;
    const auto result = trimLocked({budget, minimum}, true, error, stop, &decoder, hash, checkReachable);
    if (result.inventoryComplete) appliedLimits_ = limits;
    decodedBytesHint.store(decodedTotal(), std::memory_order_relaxed);
    if (result.inventoryComplete && !result.budgetMet && !stop.stop_requested() && !checkReachable)
        decoder.problems_.push_back(error.empty() ? "media cache budget unmet: used or unavailable entries remain" : error);
}

AnalysisTrimResult AnalysisCache::cleanupWithDecoder(audio::DecodeCache& decoder,
    std::span<const std::string> current, std::string& error, std::stop_token stop) const {
    AnalysisTrimResult result;
    if (audio::onAudioThread) { result.problem = AnalysisCacheProblem::Realtime; return result; }
    if (stop.stop_requested()) { result.problem = AnalysisCacheProblem::Cancelled; return result; }
    if (!folder_.is_absolute() || !decoder.folder_.is_absolute() || folder_ == decoder.folder_ ||
        folder_.parent_path() != decoder.folder_.parent_path()) {
        error = "the decoded and analysed folders must share an application cache root";
        result.problem = AnalysisCacheProblem::Io;
        return result;
    }
    std::lock_guard decodeLock(decoder.mutex_);
    std::lock_guard analysisLock(ioMutex);
    result = trimLocked({0, 0}, false, error, stop, &decoder, {}, false, current);
    if (decoder.usesApplicationBudget_) {
        std::uint64_t decoded = 0;
        for (const auto& entry : decoder.items_) decoded += std::min(entry.second.bytes,
            std::numeric_limits<std::uint64_t>::max() - decoded);
        decodedBytesHint.store(decoded, std::memory_order_relaxed);
    }
    return result;
}

AnalysisTrimResult cleanupMediaCache(AnalysisCache& analysis, audio::DecodeCache& decoder,
    std::span<const std::string> current, std::string& error, std::stop_token stop) {
    return analysis.cleanupWithDecoder(decoder, current, error, stop);
}

AnalysisTrimResult AnalysisCache::trimLocked(AnalysisCacheLimits limits, bool automatic,
    std::string& error, std::stop_token stop, audio::DecodeCache* decoder, std::string_view incomingHash,
    bool checkReachable, std::optional<std::span<const std::string>> current) const {
    AnalysisTrimResult result;
    struct Candidate {
        fs::path path;
        std::uint64_t bytes;
        fs::file_time_type stamp;
        std::int64_t lastUse;
        std::string decodedHash;
    };
    std::string firstError;
    std::uint64_t analysisBytes = 0, retainedBytes = 0;
    bool decodedChanged = false;
    const auto finish = [&]() noexcept {
        if (result.inventoryComplete) estimatedBytes_ = analysisBytes;
        if (decodedChanged) try { decoder->saveIndex(); } catch (...) {}
    };
    struct Finish { decltype(finish)& action; ~Finish() { action(); } } finalise{finish};
    try {
        if(stop.stop_requested()){result.problem=AnalysisCacheProblem::Cancelled;return result;}
        std::error_code ec;const auto root=fs::symlink_status(folder_,ec);
        const bool missingRoot = ec==std::errc::no_such_file_or_directory || root.type()==fs::file_type::not_found;
        if(missingRoot && !decoder) {
            result.inventoryComplete=true;result.budgetMet=true;error.clear();return result;
        }
        if(ec && !missingRoot)throw std::system_error(ec);
        if(!missingRoot && !fs::is_directory(root))throw std::runtime_error("analysis folder is not a real directory");
        if (decoder && !decoder->folder_.empty() && !realOrMissingDirectory(decoder->folder_))
            throw std::runtime_error("decoded cache folder is not a real directory");
        std::vector<Candidate> candidates;
        std::unordered_set<std::string_view> protectedHashes;
        if (current) {
            protectedHashes.insert(current->begin(), current->end());
            for (const auto& [hash, item] : decoder->items_)
                if (item.pins > 0) protectedHashes.insert(hash);
        }
        bool unvalidatedSkipped=false;
        const auto named=[](const fs::path& path,const std::string& why) {
            const auto text=path.u8string();return std::string(text.begin(),text.end())+": "+why;
        };
        const auto skip=[&](const fs::path& path,const std::string& why,bool unvalidated) {
            ++result.skippedEntries;result.problem=AnalysisCacheProblem::Io;
            unvalidatedSkipped=unvalidatedSkipped || unvalidated;
            if(firstError.empty()){firstError=named(path,why);error=firstError;}
        };
        const auto charge = [&](std::uint64_t size, bool retained) {
            if(size>std::numeric_limits<std::uint64_t>::max()-result.bytesBefore)
                throw std::overflow_error("media cache inventory size overflow");
            result.bytesBefore += size;
            result.bytesAfter = result.bytesBefore;
            if (retained) {
                ++result.retainedEntries;
                retainedBytes += size;
            }
        };
        audio::fileIoPoint();
        // Inventory and validate everything before deleting anything. Unknown,
        // damaged and future-envelope files remain for explicit repair, not GC.
        if (!missingRoot) for(const auto& entry:fs::directory_iterator(folder_)) {
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
            const bool retained = protectedHashes.contains(hash);
            charge(static_cast<std::uint64_t>(size), retained);
            analysisBytes += static_cast<std::uint64_t>(size);
            if (!retained) candidates.push_back({path,static_cast<std::uint64_t>(size),stamp,unixMicros(stamp),{}});
        }
        if (decoder && !decoder->folder_.empty()) for (const auto& [hash, item] : decoder->items_) {
            if (stop.stop_requested()) { result.problem = AnalysisCacheProblem::Cancelled; return result; }
            const auto path = decoder->fileFor(hash);
            const auto status = fs::symlink_status(path, ec);
            if (ec == std::errc::no_such_file_or_directory || status.type() == fs::file_type::not_found) continue;
            if (ec) { skip(path, ec.message(), true); continue; }
            if (!fs::is_regular_file(status)) { ++result.ignoredEntries; continue; }
            const auto size = fs::file_size(path, ec);
            if (ec) { skip(path, ec.message(), true); continue; }
            const auto stamp = fs::last_write_time(path, ec);
            if (ec) { skip(path, ec.message(), true); continue; }
            try {
                const audio::WavReader reader(path);
                if (!reader.frames()) { ++result.ignoredEntries; continue; }
            } catch (const std::exception& exception) {
                skip(path, exception.what(), true);
                continue;
            }
            const bool retained = item.pins > 0 || hash == incomingHash || protectedHashes.contains(hash);
            charge(static_cast<std::uint64_t>(size), retained);
            if (!retained) candidates.push_back({path,static_cast<std::uint64_t>(size),stamp,
                                                 item.lastUse == 0 ? unixMicros(stamp) : item.lastUse, hash});
        }
        result.inventoryComplete=true;
        auto target = limits.maximumBytes;
        // A large automatic trim leaves a small amount of headroom so a
        // library near the limit does not rescan its whole cache per sample.
        if (automatic && result.bytesBefore > target && target >= (16u << 20))
            target -= std::min(target / 20, std::uint64_t{8} << 20);
        const auto space = [&] {
            return decoder && !decoder->folder_.empty() ? std::optional(decoder->freeBytes()) : freeSpace();
        };
        auto available = limits.minimumFreeBytes ? space() : std::nullopt;
        if (checkReachable && available && *available < limits.minimumFreeBytes &&
            result.bytesBefore - retainedBytes < limits.minimumFreeBytes - *available) return result;
        std::sort(candidates.begin(),candidates.end(),[decoder](const Candidate& a,const Candidate& b){
            if (!decoder && a.stamp != b.stamp) return a.stamp < b.stamp;
            return a.lastUse!=b.lastUse?a.lastUse<b.lastUse:a.path<b.path;
        });
        for(const auto& candidate:candidates) {
            if(!current && result.bytesAfter<=target && (!available || *available>=limits.minimumFreeBytes))break;
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
            if (candidate.decodedHash.empty()) analysisBytes -= candidate.bytes;
            else {
                decoder->items_.erase(candidate.decodedHash);
                ++decoder->stats_.evictions;
                decodedChanged = true;
            }
            if (available && *available < limits.minimumFreeBytes) available=space();
        }
        if(stop.stop_requested()){result.problem=AnalysisCacheProblem::Cancelled;return result;}
        result.budgetMet=!unvalidatedSkipped && result.bytesAfter<=(current ? retainedBytes : limits.maximumBytes) &&
            (!limits.minimumFreeBytes || (available && *available>=limits.minimumFreeBytes));
        if(firstError.empty())error.clear();
        return result;
    }catch(const std::bad_alloc&){result.problem=AnalysisCacheProblem::Resource;return result;}
    catch(const std::exception& e){result.problem=AnalysisCacheProblem::Io;error=firstError.empty()?e.what():firstError+"; trim aborted: "+e.what();return result;}
}
AnalysisCache& defaultAnalysisCache() {
    static AnalysisCache cache([] {
        const auto& root = appdata::processCacheRoot();
        return root.empty() ? fs::path{} : root / "analysed";
    }(), {}, [] {
        std::lock_guard lock(limitsMutex);
        return appLimits;
    }());
    return cache;
}
} // namespace adi::media
