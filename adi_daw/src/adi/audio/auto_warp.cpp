// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/auto_warp.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/textproj.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
namespace adi::audio {
namespace {
struct Failure {AutoWarpError code;};
void cancellation(std::stop_token stop){if(stop.stop_requested())throw Failure{AutoWarpError::Cancelled};}
double subdivision(double quarters) {
    return std::exp2(std::clamp(std::round(std::log2(quarters)),-2.0,10.0));
}
struct Estimate {double bpm=0,fit=0;};
Estimate estimate(std::span<const std::uint64_t> onsets,std::uint32_t rate,double hint,std::stop_token stop) {
    if(onsets.size()<3)throw Failure{AutoWarpError::NoTempo};
    std::array<std::uint32_t,9991> votes{};
    for(std::size_t index=1;index<onsets.size();++index) {
        if(index%256==0)cancellation(stop);
        const double seconds=static_cast<double>(onsets[index]-onsets[index-1])/rate;
        for(const double step:{0.25,0.5,1.0,2.0,4.0}) {
            const double bpm=60*step/seconds;
            if(bpm>=10 && bpm<=999)++votes[static_cast<std::size_t>(std::llround(bpm*10))];
        }
    }
    std::vector<std::size_t> candidates;
    for(std::size_t index=100;index<votes.size();++index)if(votes[index])candidates.push_back(index);
    const auto count=std::min<std::size_t>(24,candidates.size());
    std::partial_sort(candidates.begin(),candidates.begin()+static_cast<std::ptrdiff_t>(count),candidates.end(),
        [&](auto a,auto b){return votes[a]!=votes[b]?votes[a]>votes[b]:std::abs(std::log2((static_cast<double>(a)/10)/hint))<std::abs(std::log2((static_cast<double>(b)/10)/hint));});
    candidates.resize(count);
    Estimate result;double best=-1e30;
    for(const auto candidate:candidates) {
        cancellation(stop);const double bpm=static_cast<double>(candidate)/10;double fit=0,steps=0,seconds=0;
        for(std::size_t index=1;index<onsets.size();++index) {
            if(index%256==0)cancellation(stop);
            const double duration=static_cast<double>(onsets[index]-onsets[index-1])/rate;
            const double quarters=duration*bpm/60,step=subdivision(quarters),error=std::abs(quarters-step)/step;
            fit+=std::exp(-36*error*error);
            if(error<0.25){steps+=step;seconds+=duration;}
        }
        fit/=static_cast<double>(onsets.size()-1);
        const double score=fit-0.03*std::abs(std::log2(bpm/hint));
        if(score>best && seconds>0){best=score;result={60*steps/seconds,fit};}
    }
    if(result.bpm<=0 || result.bpm>=1000 || result.fit<0.5)throw Failure{AutoWarpError::NoTempo};
    return result;
}
struct Beat {std::uint64_t frame;std::int64_t tick;};
std::int64_t tick(double quarters) {
    const long double value=static_cast<long double>(quarters)*textproj::kPPQ;
    if(!std::isfinite(value) || value<0 || value>static_cast<long double>(std::numeric_limits<std::int64_t>::max())-4096)
        throw Failure{AutoWarpError::Precision};
    return static_cast<std::int64_t>(std::llround(value));
}
}
AutoWarpResult autoWarpFromOnsets(std::span<const std::uint64_t> onsets,std::uint64_t frames,std::uint32_t rate,
                                  const AutoWarpOptions& options,std::stop_token stop) {
    AutoWarpResult result;
    if(onAudioThread){result.error=AutoWarpError::Realtime;return result;}
    try {
        cancellation(stop);
        if(frames==0 || frames>static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
           rate<8000 || rate>768000 || onsets.size()>1000000)throw Failure{AutoWarpError::InvalidSource};
        for(std::size_t index=0;index<onsets.size();++index)
            if(onsets[index]>=frames || (index && onsets[index]<=onsets[index-1]))throw Failure{AutoWarpError::InvalidSource};
        if(options.numerator<1 || options.numerator>255 || options.denominator<1 || options.denominator>128 ||
           (options.denominator & (options.denominator-1))!=0 || !std::isfinite(options.tempoHint) ||
           options.tempoHint<=0 || options.tempoHint>=1000 ||
           (options.sourceBpm && (!std::isfinite(*options.sourceBpm) || *options.sourceBpm<=0 || *options.sourceBpm>=1000)))
            throw Failure{AutoWarpError::InvalidOptions};
        if(options.sourceBpm){result.estimatedBpm=*options.sourceBpm;result.confidence=1;}
        else {const auto clock=estimate(onsets,rate,options.tempoHint,stop);result.estimatedBpm=clock.bpm;result.confidence=clock.fit;}
        result.trimFrames=options.fromFirstBeat && !onsets.empty()?onsets.front():0;
        const auto sourceFrames=frames-result.trimFrames;
        const auto barTicks=static_cast<std::int64_t>(textproj::kPPQ)*options.numerator*4/options.denominator;
        const double globalPeriod=static_cast<double>(rate)*60/result.estimatedBpm;
        std::vector<Beat> beats={{0,0}};
        double quarters=0,period=globalPeriod,beatStep=1;
        if(onsets.size()>=2)beatStep=subdivision(static_cast<double>(onsets[1]-onsets[0])/period);
        std::uint64_t last=0;std::size_t firstOnset=0;
        if(options.detectTempoChanges && !options.sourceBpm && !options.fromFirstBeat && !onsets.empty() && onsets.front()>0) {
            quarters=static_cast<double>(onsets.front())/period;const auto firstTick=tick(quarters);
            if(firstTick<=0)throw Failure{AutoWarpError::Precision};
            beats.push_back({onsets.front(),firstTick});last=onsets.front();firstOnset=1;
        }
        if(options.detectTempoChanges && !options.sourceBpm)for(std::size_t index=firstOnset;index<onsets.size();++index) {
            if(index%256==0)cancellation(stop);
            const auto frame=onsets[index]-result.trimFrames;if(frame==0)continue;
            const double gap=static_cast<double>(frame-last),relative=gap/(period*beatStep);
            const double next=index+1<onsets.size()?static_cast<double>(onsets[index+1]-onsets[index]):gap;
            const bool confirmed=std::abs(next-gap)/gap<0.15;
            double step=0;
            if((relative>=0.85 && relative<=1.18) || (confirmed && relative>=0.45 && relative<=2.2)) {
                step=beatStep;const double nextPeriod=gap/step;
                if(std::abs(nextPeriod-globalPeriod)/globalPeriod>0.02)result.variableTempo=true;
                period=nextPeriod;
            } else {
                step=std::round(gap/period*4)/4;
                if(step<0.25 || std::abs(gap/period-step)/step>0.35)continue;
            }
            quarters+=step;const auto position=tick(quarters);
            if(position<=beats.back().tick)throw Failure{AutoWarpError::Precision};
            beats.push_back({frame,position});last=frame;
        }
        if(beats.size()==1) {
            const auto end=tick(static_cast<double>(sourceFrames)/globalPeriod);
            if(end<=0)throw Failure{AutoWarpError::Precision};
            beats.push_back({sourceFrames,end});
        } else {
            const auto end=tick(quarters+static_cast<double>(sourceFrames-last)/period);
            if(end<=beats.back().tick)throw Failure{AutoWarpError::Precision};
            beats.push_back({sourceFrames,end});
        }
        const auto endTick=beats.back().tick;
        if(endTick/barTicks>999998)throw Failure{AutoWarpError::TooManyMarkers};
        result.markers.push_back({0,0});std::size_t segment=0;
        for(std::int64_t position=barTicks;position<endTick;position+=barTicks) {
            cancellation(stop);
            while(segment+2<beats.size() && beats[segment+1].tick<position)++segment;
            const auto& a=beats[segment];const auto& b=beats[segment+1];
            const long double fraction=static_cast<long double>(position-a.tick)/static_cast<long double>(b.tick-a.tick);
            const auto frame=a.frame+static_cast<std::uint64_t>(std::llround(fraction*static_cast<long double>(b.frame-a.frame)));
            if(frame<=static_cast<std::uint64_t>(result.markers.back().sourceFrame) || frame>=sourceFrames)throw Failure{AutoWarpError::Precision};
            result.markers.push_back({static_cast<std::int64_t>(frame),position});
        }
        result.markers.push_back({static_cast<std::int64_t>(sourceFrames),endTick});
    } catch(const Failure& failure){result.error=failure.code;result.markers.clear();}
    return result;
}
}
