/*
 * sst-effects - an open source library of audio effects
 * built by Surge Synth Team.
 *
 * Copyright 2018-2023, various authors, as described in the GitHub
 * transaction log.
 *
 * sst-effects is released under the GNU General Public Licence v3
 * or later (GPL-3.0-or-later). The license is found in the "LICENSE"
 * file in the root of this repository, or at
 * https://www.gnu.org/licenses/gpl-3.0.en.html
 *
 * The majority of these effects at initiation were factored from
 * Surge XT, and so git history prior to April 2023 is found in the
 * surge repo, https://github.com/surge-synthesizer/surge
 *
 * All source in sst-effects available at
 * https://github.com/surge-synthesizer/sst-effects
 */

// ADI scalar adaptation of Surge's dual circular-buffer feedback/crossfeed
// topology. Live controls and interpolation/transition implementations are ADI.
#include "adi/dsp/live_delay.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>
namespace adi::dsp {
LiveDelay::LiveDelay(){for(int i=0;i<Count;++i)p_[static_cast<std::size_t>(i)]=ranges[i].initial;}
void LiveDelay::prepare(double sr){rate_=std::isfinite(sr)&&sr>0?sr:48000;for(auto& l:lanes_){l.ring.assign(static_cast<std::size_t>(rate_*21)+8,0);l.current=l.old=l.target=l.low=l.high=0;l.fade=1;}write_=0;started_=false;phase_=from_=to_=0;random_=0x12345678u;}
void LiveDelay::set(Param key,double v) noexcept {
    if(key<0||key>=Count||!std::isfinite(v))return;
    v=std::clamp(v,ranges[key].low,ranges[key].high);p_[static_cast<std::size_t>(key)]=v;
    if(p_[Link]>=0.5){
        if(key==LeftMs)p_[RightMs]=v;if(key==RightMs)p_[LeftMs]=v;
        if(key==LeftSync)p_[RightSync]=v;if(key==RightSync)p_[LeftSync]=v;
        if(key==LeftNotes)p_[RightNotes]=v;if(key==RightNotes)p_[LeftNotes]=v;
        if(key==Link){p_[RightMs]=p_[LeftMs];p_[RightSync]=p_[LeftSync];p_[RightNotes]=p_[LeftNotes];}
    }
}
void LiveDelay::tempo(double v) noexcept {if(std::isfinite(v))bpm_=std::clamp(v,20.,999.);}
double LiveDelay::read(const Lane& l,double delay) const noexcept {
    delay=std::clamp(delay,2.,static_cast<double>(l.ring.size()-4));
    const auto whole=static_cast<std::size_t>(delay);const double t=delay-static_cast<double>(whole);
    const auto at=(write_+l.ring.size()-whole)%l.ring.size();
    const double b=l.ring[at],c=l.ring[(at+l.ring.size()-1)%l.ring.size()];
    if(p_[HiQuality]<0.5)return b+(c-b)*t;
    const double a=l.ring[(at+1)%l.ring.size()],d=l.ring[(at+l.ring.size()-2)%l.ring.size()];
    return b+0.5*t*(c-a+t*(2*a-5*b+4*c-d+t*(3*(b-c)+d-a)));
}
void LiveDelay::process(const float* inL,const float* inR,float* outL,float* outR,std::size_t n) noexcept {
    if(lanes_[0].ring.empty()){std::fill(outL,outL+n,0.f);std::fill(outR,outR+n,0.f);return;}
    auto& p=p_;const int rateMode=static_cast<int>(std::round(p[RateMode]));
    double hz=rateMode==0?p[RateHz]:rateMode==1?1000/p[RateMs]:bpm_/(60*p[RateBeats]);
    if(rateMode==3)hz*=1.5;if(rateMode==4)hz/=1.5;if(rateMode==5)hz*=4;
    for(std::size_t i=0;i<n;++i){
        const std::array<double,2> input{inL[i],inR[i]};
        phase_+=hz/rate_;if(phase_>=1){phase_-=std::floor(phase_);from_=to_;random_^=random_<<13;random_^=random_>>17;random_^=random_<<5;to_=2*static_cast<double>(random_)/4294967295.-1;}
        const double morph=0.01+0.98*p[Morph]/100;
        const double phase=phase_<morph?phase_/(2*morph):0.5+(phase_-morph)/(2*(1-morph));
        const int wave=static_cast<int>(std::round(p[Wave]));
        double lfo=std::sin(2*std::numbers::pi*phase);
        if(wave==1)lfo=1-4*std::abs(phase-0.5);if(wave==2)lfo=2*phase-1;if(wave==3)lfo=1-2*phase;
        if(wave==4)lfo=phase<0.5?1:-1;if(wave==5)lfo=to_;if(wave==6)lfo=from_+(to_-from_)*phase;
        std::array<double,2> wet{};
        for(std::size_t c=0;c<2;++c){auto& lane=lanes_[c];
            const bool sync=p[c==0?LeftSync:RightSync]>=0.5;
            const double seconds=sync?std::round(p[c==0?LeftNotes:RightNotes])*15/bpm_:p[c==0?LeftMs:RightMs]/1000;
            const double offset=1+p[c==0?LeftOffset:RightOffset]/100;
            const double target=std::clamp(seconds*offset*rate_*(1+0.25*p[LfoDelay]/100*lfo),2.,static_cast<double>(lane.ring.size()-4));
            if(!started_)lane.current=lane.target=lane.old=target;
            const int transition=static_cast<int>(std::round(p[Transition]));
            if(transition==0){lane.current+=(target-lane.current)*(1-std::exp(-1/(rate_*(p[HiQuality]>=0.5?0.02:0.005))));wet[c]=read(lane,lane.current);}
            else if(transition==1){
                if(lane.fade>=1 && std::abs(target-lane.target)>0.001){lane.old=lane.target;lane.target=target;lane.fade=0;}
                lane.fade=std::min(1.,lane.fade+1/(rate_*0.01));
                wet[c]=read(lane,lane.old)*(1-lane.fade)+read(lane,lane.target)*lane.fade;lane.current=lane.target;
            }else {lane.current=lane.target=target;wet[c]=read(lane,target);}
        }
        started_=true;
        for(std::size_t c=0;c<2;++c){auto& lane=lanes_[c];
            const double feedback=wet[p[PingPong]>=0.5?1-c:c];
            double value=p[Freeze]>=0.5?feedback:input[c]+feedback*p[Feedback]/100;
            if(p[Filter]>=0.5 && p[Freeze]<0.5){
                const double center=p[Freq]*std::exp2(4*p[LfoFilter]/100*lfo);
                const double lo=std::clamp(center*std::exp2(-p[Width]/2),10.,rate_*0.45);
                const double hi=std::clamp(center*std::exp2(p[Width]/2),lo,rate_*0.49);
                lane.low+=(value-lane.low)*(1-std::exp(-2*std::numbers::pi*lo/rate_));
                const double highpassed=value-lane.low;
                lane.high+=(highpassed-lane.high)*(1-std::exp(-2*std::numbers::pi*hi/rate_));value=lane.high;
            }
            lane.ring[write_]=static_cast<float>(value);
            const double mix=p[Mix]/100;
            const float result=static_cast<float>(p[EqualLoudness]>=0.5?input[c]*std::cos(mix*std::numbers::pi/2)+wet[c]*std::sin(mix*std::numbers::pi/2):input[c]*(1-mix)+wet[c]*mix);
            if(c==0)outL[i]=result;else outR[i]=result;
        }
        write_=(write_+1)%lanes_[0].ring.size();
    }
}
}
