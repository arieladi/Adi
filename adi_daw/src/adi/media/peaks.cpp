// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/media/peaks.hpp"
#include "adi/media/blake3.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/audio/decode.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <filesystem>
#include <limits>
#include <stdexcept>

namespace adi::media {
namespace {
constexpr std::size_t headerSize=80,maxBytes=headerSize+8*kMaxPeakValues;
PeakResult failed(PeakProblem problem,std::string detail={}){return {problem,std::move(detail),{}};}
PeakResult success(MediaPeaks data){return {PeakProblem::none,{},std::move(data)};}
int hex(char c){return c>='0' && c<='9'?c-'0':c>='a' && c<='f'?c-'a'+10:-1;}
bool validHash(const std::string& hash){return hash.size()==64 && std::all_of(hash.begin(),hash.end(),[](char c){return hex(c)>=0;});}
std::uint64_t countBins(std::uint64_t frames,std::uint64_t step){return frames/step+(frames%step!=0?1u:0u);}
bool shape(const MediaPeaks& data) {
    if(!validHash(data.contentHash) || data.sampleRate==0 || data.sampleRate>audio::kMaxDecodeRate || data.channels==0 || data.channels>audio::kMaxDecodeChannels || data.framesPerBin==0 || data.frames>static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))return false;
    const auto count=countBins(data.frames,data.framesPerBin);
    return count<=kMaxPeakValues/data.channels && data.bins.size()==count*data.channels;
}
void put(std::vector<std::byte>& bytes,std::size_t at,std::uint64_t value,std::size_t width) {
    for(std::size_t index=0;index<width;++index)bytes[at+index]=static_cast<std::byte>((value>>(8*index))&255u);
}
std::uint64_t get(std::span<const std::byte> bytes,std::size_t at,std::size_t width) {
    std::uint64_t value=0;for(std::size_t index=0;index<width;++index)value|=static_cast<std::uint64_t>(std::to_integer<unsigned>(bytes[at+index]))<<(8*index);return value;
}
struct Plan {
    std::string hash,name;std::optional<std::string> relative,hint;
    std::optional<std::int64_t> rate,channels,frames;
};
std::optional<Plan> plan(const Store& store,std::int64_t id) {
    SQLite::Statement query(store.db(),"SELECT hash_blake3,orig_name,rel_path,abs_path_hint,sample_rate,channels,frames FROM media_files WHERE id=?");query.bind(1,id);
    if(!query.executeStep())return {};
    Plan result;result.hash=query.getColumn(0).getString();result.name=query.getColumn(1).getString();
    if(!query.getColumn(2).isNull())result.relative=query.getColumn(2).getString();
    if(!query.getColumn(3).isNull())result.hint=query.getColumn(3).getString();
    if(!query.getColumn(4).isNull())result.rate=query.getColumn(4).getInt64();
    if(!query.getColumn(5).isNull())result.channels=query.getColumn(5).getInt64();
    if(!query.getColumn(6).isNull())result.frames=query.getColumn(6).getInt64();
    return result;
}
bool matches(const MediaPeaks& data,const Plan& source) {
    return data.contentHash==source.hash && (!source.rate || *source.rate==data.sampleRate) && (!source.channels || *source.channels==data.channels) && (!source.frames || (*source.frames>=0 && static_cast<std::uint64_t>(*source.frames)==data.frames));
}
struct Cancelled {};
void cancelled(std::stop_token stop){if(stop.stop_requested())throw Cancelled{};}
std::filesystem::path locate(const Store& store,const Plan& source,std::stop_token stop) {
    const auto folder=projectFolder(store);std::vector<std::filesystem::path> candidates;
    if(source.relative){const auto path=pathFromUtf8(*source.relative);if(path.is_absolute())throw std::runtime_error("absolute relative media path");candidates.push_back(folder/path);}
    const auto name=pathFromUtf8(source.name).filename();if(!name.empty())candidates.push_back(folder/"audio"/name);
    if(source.hint){const auto path=pathFromUtf8(*source.hint);if(!path.is_absolute())throw std::runtime_error("non-absolute media hint");candidates.push_back(path);}
    for(const auto& path:candidates) {
        cancelled(stop);std::error_code error;const bool exists=std::filesystem::exists(path,error);
        if(error)throw std::runtime_error("cannot inspect media path");
        if(!exists)continue;
        const auto hash=blake3File(path,stop);if(hash.error==HashError::cancelled)throw Cancelled{};
        if(!hash || hash.hex!=source.hash)throw std::runtime_error("media content hash mismatch");
        return path;
    }
    return {};
}
}
std::vector<std::byte> encodePeaks(const MediaPeaks& data) {
    if(audio::onAudioThread)return {};
    if(!shape(data))throw std::invalid_argument("invalid peak cache shape");
    for(const auto& bin:data.bins)if(!std::isfinite(bin.minimum) || !std::isfinite(bin.maximum) || bin.minimum>bin.maximum)throw std::invalid_argument("invalid peak cache range");
    std::vector<std::byte> bytes(headerSize+8*data.bins.size());
    bytes[0]=std::byte{'A'};bytes[1]=std::byte{'P'};bytes[2]=std::byte{'K'};bytes[3]=std::byte{'S'};
    put(bytes,4,1,2);put(bytes,6,headerSize,2);put(bytes,8,data.sampleRate,4);put(bytes,12,data.channels,4);
    put(bytes,16,data.frames,8);put(bytes,24,data.framesPerBin,8);put(bytes,32,countBins(data.frames,data.framesPerBin),8);
    for(std::size_t index=0;index<32;++index)bytes[40+index]=static_cast<std::byte>(hex(data.contentHash[2*index])*16+hex(data.contentHash[2*index+1]));
    for(std::size_t index=0;index<data.bins.size();++index){put(bytes,headerSize+8*index,std::bit_cast<std::uint32_t>(data.bins[index].minimum),4);put(bytes,headerSize+8*index+4,std::bit_cast<std::uint32_t>(data.bins[index].maximum),4);}
    return bytes;
}
PeakResult decodePeaks(std::span<const std::byte> bytes) {
    if(audio::onAudioThread)return failed(PeakProblem::audioThread);
    try {
        if(bytes.size()<4 || bytes[0]!=std::byte{'A'} || bytes[1]!=std::byte{'P'} || bytes[2]!=std::byte{'K'} || bytes[3]!=std::byte{'S'})return failed(PeakProblem::unsupportedCache,"unknown peaks encoding");
        if(bytes.size()<headerSize || bytes.size()>maxBytes)return failed(PeakProblem::invalidCache,"invalid peaks byte count");
        if(get(bytes,4,2)!=1 || get(bytes,6,2)!=headerSize || get(bytes,72,8)!=0)return failed(PeakProblem::unsupportedCache,"unknown peaks version/header");
        MediaPeaks data;data.sampleRate=static_cast<std::uint32_t>(get(bytes,8,4));data.channels=static_cast<std::uint32_t>(get(bytes,12,4));data.frames=get(bytes,16,8);data.framesPerBin=get(bytes,24,8);const auto count=get(bytes,32,8);
        if(data.sampleRate==0 || data.sampleRate>audio::kMaxDecodeRate || data.channels==0 || data.channels>audio::kMaxDecodeChannels || data.framesPerBin==0 || data.frames>static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) || count!=countBins(data.frames,data.framesPerBin) || count>kMaxPeakValues/data.channels || bytes.size()!=headerSize+8*count*data.channels)return failed(PeakProblem::invalidCache,"invalid peaks dimensions");
        constexpr char digits[]="0123456789abcdef";data.contentHash.reserve(64);
        for(std::size_t index=0;index<32;++index){const auto byte=std::to_integer<unsigned>(bytes[40+index]);data.contentHash+=digits[byte>>4];data.contentHash+=digits[byte&15];}
        data.bins.resize(static_cast<std::size_t>(count*data.channels));
        for(std::size_t index=0;index<data.bins.size();++index) {
            auto& bin=data.bins[index];bin.minimum=std::bit_cast<float>(static_cast<std::uint32_t>(get(bytes,headerSize+8*index,4)));bin.maximum=std::bit_cast<float>(static_cast<std::uint32_t>(get(bytes,headerSize+8*index+4,4)));
            if(!std::isfinite(bin.minimum) || !std::isfinite(bin.maximum) || bin.minimum>bin.maximum)return failed(PeakProblem::invalidCache,"invalid peaks range");
        }
        return success(std::move(data));
    }catch(const std::bad_alloc&){return failed(PeakProblem::resource);}
    catch(const std::exception& exception){return failed(PeakProblem::invalidCache,exception.what());}
}
PeakResult readMediaPeaks(const Store& store,std::int64_t id) {
    if(audio::onAudioThread)return failed(PeakProblem::audioThread);
    try {
        SQLite::Statement query(store.db(),"SELECT hash_blake3,sample_rate,channels,frames,length(peaks),CASE WHEN typeof(peaks)='blob' AND length(peaks)<=? THEN peaks END FROM media_files WHERE id=?");query.bind(1,static_cast<std::int64_t>(maxBytes));query.bind(2,id);
        if(!query.executeStep())return failed(PeakProblem::unknownMedia);
        if(query.getColumn(4).isNull())return failed(PeakProblem::noCache);
        const auto column=query.getColumn(5);
        if(column.getType()!=SQLITE_BLOB || column.getBytes()<0 || static_cast<std::size_t>(column.getBytes())>maxBytes)return failed(PeakProblem::invalidCache,"peaks cache exceeds bounds");
        auto result=decodePeaks({static_cast<const std::byte*>(column.getBlob()),static_cast<std::size_t>(column.getBytes())});
        if(!result)return result;
        const auto& data=*result.data;
        if(data.contentHash!=query.getColumn(0).getString() || (!query.getColumn(1).isNull() && query.getColumn(1).getInt64()!=data.sampleRate) || (!query.getColumn(2).isNull() && query.getColumn(2).getInt64()!=data.channels) || (!query.getColumn(3).isNull() && (query.getColumn(3).getInt64()<0 || static_cast<std::uint64_t>(query.getColumn(3).getInt64())!=data.frames)))return failed(PeakProblem::invalidCache,"peaks cache identity/dimensions changed");
        return result;
    }catch(const std::bad_alloc&){return failed(PeakProblem::resource);}
    catch(const std::exception& exception){return failed(PeakProblem::database,exception.what());}
}
PeakResult buildMediaPeaks(const Store& store,std::int64_t id,std::uint32_t maximum,std::stop_token stop,audio::DecodeCache* supplied) {
    if(audio::onAudioThread)return failed(PeakProblem::audioThread);
    if(stop.stop_requested())return failed(PeakProblem::cancelled);
    if(maximum==0 || maximum>kMaxPeakValues)return failed(PeakProblem::invalidRequest);
    auto cached=readMediaPeaks(store,id);if(cached)return cached;
    if(cached.problem==PeakProblem::unknownMedia || cached.problem==PeakProblem::database || cached.problem==PeakProblem::resource)return cached;
    try {
        const auto source=plan(store,id);if(!source)return failed(PeakProblem::unknownMedia);
        if(!validHash(source->hash))return failed(PeakProblem::invalidSource,"invalid source identity");
        const auto path=locate(store,*source,stop);if(path.empty())return failed(PeakProblem::missingMedia);
        cancelled(stop);auto& cache=supplied?*supplied:audio::defaultDecodeCache();const auto playable=cache.playable(path);
        struct Release {audio::DecodeCache& cache;const std::filesystem::path& path;~Release(){cache.release(path);}} release{cache,playable};
        cancelled(stop);audio::WavReader reader(playable);MediaPeaks data;data.contentHash=source->hash;data.sampleRate=reader.sampleRate();data.channels=reader.channels();data.frames=reader.frames();
        if(data.sampleRate==0 || data.sampleRate>audio::kMaxDecodeRate || data.channels==0 || data.channels>audio::kMaxDecodeChannels || data.frames>static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) || !matches(data,*source))return failed(PeakProblem::invalidSource,"source dimensions changed");
        const auto binsLimit=std::min(static_cast<std::uint64_t>(maximum),static_cast<std::uint64_t>(kMaxPeakValues/data.channels));
        data.framesPerBin=std::max(std::uint64_t{1},countBins(data.frames,binsLimit));const auto count=countBins(data.frames,data.framesPerBin);
        data.bins.resize(static_cast<std::size_t>(count*data.channels));std::vector<float> block(static_cast<std::size_t>(4096)*data.channels);
        std::uint64_t position=0;
        for(std::uint64_t bin=0;bin<count;++bin) {
            for(std::uint32_t channel=0;channel<data.channels;++channel)data.bins[static_cast<std::size_t>(bin*data.channels+channel)]={std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity()};
            std::uint64_t remaining=std::min(data.frames-position,data.framesPerBin);
            while(remaining) {
                cancelled(stop);const auto wanted=static_cast<std::uint32_t>(std::min(remaining,std::uint64_t{4096}));
                if(reader.read(block.data(),wanted)!=wanted)return failed(PeakProblem::read,"short audio read");
                for(std::uint32_t frame=0;frame<wanted;++frame)for(std::uint32_t channel=0;channel<data.channels;++channel) {
                    const float raw=block[static_cast<std::size_t>(frame)*data.channels+channel];const float value=std::isfinite(raw)?raw:0;
                    auto& range=data.bins[static_cast<std::size_t>(bin*data.channels+channel)];range.minimum=std::min(range.minimum,value);range.maximum=std::max(range.maximum,value);
                }
                position+=wanted;remaining-=wanted;
            }
        }
        cancelled(stop);const auto hash=blake3File(path,stop);if(hash.error==HashError::cancelled)throw Cancelled{};
        if(!hash || hash.hex!=data.contentHash)return failed(PeakProblem::invalidSource,"source changed during peaks read");
        return success(std::move(data));
    }catch(const Cancelled&){return failed(PeakProblem::cancelled);}
    catch(const std::bad_alloc&){return failed(PeakProblem::resource);}
    catch(const std::exception& exception){return failed(PeakProblem::read,exception.what());}
}
}
