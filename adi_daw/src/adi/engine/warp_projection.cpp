// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/warp_projection.hpp"
#include "adi/textproj.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>
namespace adi::engine {
namespace {
double seconds(const TempoMap& tempo,std::int64_t ticks) {
    return ticks<0?static_cast<double>(ticks)/textproj::kPPQ*60/tempo.bpmAt(0):tempo.ticksToSeconds(ticks);
}
double warpClockFrame(const TempoMap& tempo,std::int64_t origin,std::int64_t tick,double rate) {
    if(tick<0 || origin>std::numeric_limits<std::int64_t>::max()-tick || !std::isfinite(rate) || rate<=0)
        throw std::runtime_error("warp source clock is outside its supported range");
    const double value=(seconds(tempo,origin+tick)-seconds(tempo,origin))*rate;
    if(!std::isfinite(value) || value<0 || value>0x1p50)throw std::runtime_error("warp source clock exceeds sample precision");
    return value;
}
}
std::int64_t warpSourceSamples(const TempoMap& tempo,std::int64_t origin,std::int64_t tick,double rate) {
    return static_cast<std::int64_t>(std::llround(warpClockFrame(tempo,origin,tick,rate)));
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
        const long double progress=static_cast<long double>(tick-a.tick)/static_cast<long double>(b.tick-a.tick);
        const auto source=a.sourceFrame+static_cast<std::int64_t>(std::llround(progress*static_cast<long double>(b.sourceFrame-a.sourceFrame)));
        points.push_back({source,tick});
    }
    std::sort(points.begin(),points.end(),[](const auto& a,const auto& b){return a.tick<b.tick;});
    std::vector<audio::WarpAnchor> result;result.reserve(points.size());
    const bool hasRamps = std::any_of(tempo.events.begin(), tempo.events.end(),
        [](const TempoEvent& point) { return point.curve == 1 || point.curve == 2; });
    const auto append = [&](const WarpMarker& point, double exactFrame) {
        if(point.sourceFrame<0)throw std::runtime_error("warp source frame is negative");
        const auto frame=static_cast<std::uint64_t>(std::llround(exactFrame));
        if(!result.empty() && (static_cast<std::uint64_t>(point.sourceFrame)<=result.back().sourceFrame || static_cast<std::uint64_t>(frame)<=result.back().outputFrame))
            throw std::runtime_error("warp tempo boundary is below source or output sample precision");
        if (result.size() >= 1000000) throw std::runtime_error("warp tempo projection exceeds the anchor bound");
        result.push_back({static_cast<std::uint64_t>(point.sourceFrame),frame});
    };
    // A ramp makes tick-to-output time nonlinear inside a warp marker pair.
    // Between tempo boundaries its derivative is monotone, so the largest
    // chord error is at most twice the midpoint error. Keep the generated
    // approximation below half an output frame before rounding anchors.
    const auto subdivide = [&](auto&& self, const WarpMarker& left, double leftFrame,
                              const WarpMarker& right, double rightFrame) -> void {
        const auto source = std::midpoint(left.sourceFrame, right.sourceFrame);
        if (source > left.sourceFrame && source < right.sourceFrame) {
            const auto progress = static_cast<long double>(source - left.sourceFrame) /
                                  static_cast<long double>(right.sourceFrame - left.sourceFrame);
            const auto tick = left.tick + std::llround(progress * static_cast<long double>(right.tick-left.tick));
            if (tick > left.tick && tick < right.tick) {
                const auto midFrame = warpClockFrame(tempo, origin, tick, rate);
                const auto rounded = std::llround(midFrame);
                if (rounded > std::llround(leftFrame) && rounded < std::llround(rightFrame) &&
                    std::abs(midFrame - std::lerp(leftFrame, rightFrame, static_cast<double>(progress))) > 0.25) {
                    const WarpMarker middle{source,tick};
                    self(self,left,leftFrame,middle,midFrame);
                    self(self,middle,midFrame,right,rightFrame);
                    return;
                }
            }
        }
        append(right,rightFrame);
    };
    auto previous = warpClockFrame(tempo, origin, points.front().tick, rate);
    append(points.front(),previous);
    for (std::size_t i=1; i<points.size(); ++i) {
        const auto next = warpClockFrame(tempo, origin, points[i].tick, rate);
        if (hasRamps && points[i].sourceFrame > points[i-1].sourceFrame)
            subdivide(subdivide,points[i-1],previous,points[i],next);
        else append(points[i],next);
        previous = next;
    }
    return result;
}
}
