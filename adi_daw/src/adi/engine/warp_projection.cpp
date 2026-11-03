// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/warp_projection.hpp"
#include "adi/textproj.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
namespace adi::engine {
namespace {
double seconds(const TempoMap& tempo,std::int64_t ticks) {
    return ticks<0?static_cast<double>(ticks)/textproj::kPPQ*60/tempo.bpmAt(0):tempo.ticksToSeconds(ticks);
}
}
std::int64_t warpSourceSamples(const TempoMap& tempo,std::int64_t origin,std::int64_t tick,double rate) {
    if(tick<0 || origin>std::numeric_limits<std::int64_t>::max()-tick || !std::isfinite(rate) || rate<=0)
        throw std::runtime_error("warp source clock is outside its supported range");
    const double value=(seconds(tempo,origin+tick)-seconds(tempo,origin))*rate;
    if(!std::isfinite(value) || value<0 || value>0x1p50)throw std::runtime_error("warp source clock exceeds sample precision");
    return static_cast<std::int64_t>(std::llround(value));
}
std::vector<audio::WarpAnchor> projectWarpAnchors(std::span<const WarpMarker> markers,const TempoMap& tempo,
                                                std::int64_t origin,double rate) {
    if(markers.size()<2 || markers.front()!=WarpMarker{0,0})throw std::runtime_error("warp needs a complete canonical marker map");
    if(markers.size()>1000000)throw std::runtime_error("warp marker count exceeds its reader bound");
    for(std::size_t index=1;index<markers.size();++index)
        if(markers[index].sourceFrame<=markers[index-1].sourceFrame || markers[index].tick<=markers[index-1].tick)
            throw std::runtime_error("warp marker clocks must increase");
    std::vector<WarpMarker> points(markers.begin(),markers.end());
    for(const auto& event:tempo.events) {
        if(event.posTicks<0)throw std::runtime_error("warp needs a nonnegative project tempo map");
        if(origin<0 && event.posTicks>std::numeric_limits<std::int64_t>::max()+origin)
            throw std::runtime_error("warp source tick exceeds signed range");
        const auto tick=event.posTicks-origin;
        if(tick<=0 || tick>=markers.back().tick)continue;
        const auto next=std::lower_bound(markers.begin(),markers.end(),tick,[](const WarpMarker& point,auto at){return point.tick<at;});
        if(next->tick==tick)continue;
        const auto& a=*(next-1);const auto& b=*next;
        const long double progress=static_cast<long double>(tick-a.tick)/(b.tick-a.tick);
        const auto source=a.sourceFrame+static_cast<std::int64_t>(std::llround(progress*(b.sourceFrame-a.sourceFrame)));
        points.push_back({source,tick});
    }
    std::sort(points.begin(),points.end(),[](const auto& a,const auto& b){return a.tick<b.tick;});
    std::vector<audio::WarpAnchor> result;result.reserve(points.size());
    for(const auto& point:points) {
        if(point.sourceFrame<0)throw std::runtime_error("warp source frame is negative");
        const auto frame=warpSourceSamples(tempo,origin,point.tick,rate);
        if(!result.empty() && (static_cast<std::uint64_t>(point.sourceFrame)<=result.back().sourceFrame || static_cast<std::uint64_t>(frame)<=result.back().outputFrame))
            throw std::runtime_error("warp tempo boundary is below source or output sample precision");
        result.push_back({static_cast<std::uint64_t>(point.sourceFrame),static_cast<std::uint64_t>(frame)});
    }
    return result;
}
}
