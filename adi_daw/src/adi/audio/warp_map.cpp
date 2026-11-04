// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/warp_map.hpp"
#include "adi/audio/io_audit.hpp"
#include <limits>
namespace adi::audio {
namespace {
constexpr std::size_t maxMarkers=1000000;
WarpMapError validate(std::span<const WarpMarker> markers,std::uint64_t frames) {
    if(frames==0 || frames>static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))return WarpMapError::Bounds;
    if(markers.size()<2 || markers.size()>maxMarkers)return WarpMapError::Count;
    for(std::size_t index=0;index<markers.size();++index) {
        const auto& marker=markers[index];
        if(marker.sourceFrame<0 || marker.tick<0 || static_cast<std::uint64_t>(marker.sourceFrame)>frames)return WarpMapError::Bounds;
        if(index && (marker.sourceFrame<=markers[index-1].sourceFrame || marker.tick<=markers[index-1].tick))return WarpMapError::Order;
    }
    if(markers.front()!=WarpMarker{0,0} || static_cast<std::uint64_t>(markers.back().sourceFrame)!=frames)return WarpMapError::Coverage;
    return WarpMapError::None;
}
}
WarpMapRead readWarpMap(std::span<const std::byte> bytes,std::uint64_t frames) {
    WarpMapRead result;
    if(onAudioThread){result.error=WarpMapError::Realtime;return result;}
    StreamReader<WarpMarker> reader(bytes,FourCC::WarpMap);
    if(!reader.ok() || reader.header().version==0 || (reader.header().flags & ~StreamFlags::SortedByTime)!=0) {
        result.error=WarpMapError::Header;return result;
    }
    if(reader.count()<2 || reader.count()>maxMarkers){result.error=WarpMapError::Count;return result;}
    result.futureLayout=reader.header().rec_size>sizeof(WarpMarker) || reader.header().version>1;
    result.markers.reserve(reader.count());
    for(std::uint32_t index=0;index<reader.count();++index)result.markers.push_back(*reader.at(index));
    result.error=validate(result.markers,frames);return result;
}
WarpMapWrite writeWarpMap(std::span<const WarpMarker> markers,std::uint64_t frames) {
    WarpMapWrite result;
    if(onAudioThread){result.error=WarpMapError::Realtime;return result;}
    result.error=validate(markers,frames);
    if(result.error==WarpMapError::None)result.bytes=writeStream<WarpMarker>(FourCC::WarpMap,markers);
    return result;
}
}
