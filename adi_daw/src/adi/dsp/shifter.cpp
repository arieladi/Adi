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

// SPDX-License-Identifier: GPL-3.0-or-later
// Freq mode adapts Surge Synth Team's sst-effects FreqShiftMod analytic
// modulation (2018-2023, GPL-3-or-later); scalar Hilbert header retains its
// upstream notices. Pitch, modulation and Live-style controls are ADI code.
#include "adi/dsp/shifter.hpp"
#include "adi/dsp/shifter_hilbert.hpp"
extern "C" {
#include "pffft.h"
}
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <stdexcept>
#include <vector>
namespace adi::dsp {
namespace {
constexpr double pi=std::numbers::pi;
double wrap(double x){return x-std::floor(x);}
struct Pitch {
    int size=0, hop=0, position=0, count=0;
    PFFFT_Setup* setup=nullptr;
    float *time=nullptr,*frequency=nullptr,*work=nullptr;
    std::vector<float> input,output,window;
    std::vector<double> previous,phase,magnitude,weighted;
    ~Pitch(){if(setup)pffft_destroy_setup(setup);pffft_aligned_free(time);pffft_aligned_free(frequency);pffft_aligned_free(work);}
    void prepare(int n){
        size=n;hop=n/4;position=count=0;
        if(setup)pffft_destroy_setup(setup);
        pffft_aligned_free(time);pffft_aligned_free(frequency);pffft_aligned_free(work);
        setup=pffft_new_setup(n,PFFFT_REAL);
        const auto bytes=static_cast<std::size_t>(n)*sizeof(float);
        time=static_cast<float*>(pffft_aligned_malloc(bytes));frequency=static_cast<float*>(pffft_aligned_malloc(bytes));work=static_cast<float*>(pffft_aligned_malloc(bytes));
        if(!setup || !time || !frequency || !work)throw std::bad_alloc();
        input.assign(static_cast<std::size_t>(n),0);output=input;window=input;
        previous.assign(static_cast<std::size_t>(n/2+1),0);phase=previous;magnitude=previous;weighted=previous;
        for(int i=0;i<n;++i)window[static_cast<std::size_t>(i)]=static_cast<float>(0.5-0.5*std::cos(2*pi*i/n));
    }
    float sample(float in,double ratio) noexcept {
        const auto pos=static_cast<std::size_t>(position);
        const float result=output[pos];output[pos]=0;input[pos]=in;
        position=(position+1)%size;
        if(++count<hop)return result;
        count=0;
        for(int i=0;i<size;++i)time[i]=input[static_cast<std::size_t>((position+i)%size)]*window[static_cast<std::size_t>(i)];
        pffft_transform_ordered(setup,time,frequency,work,PFFFT_FORWARD);
        std::fill(magnitude.begin(),magnitude.end(),0);std::fill(weighted.begin(),weighted.end(),0);
        for(int k=1;k<size/2;++k){
            const auto index=static_cast<std::size_t>(k);
            const double angle=std::atan2(frequency[2*k+1],frequency[2*k]);
            const double omega=2*pi*k/size;
            const double actual=omega+std::remainder(angle-previous[index]-omega*hop,2*pi)/hop;
            previous[index]=angle;
            const int destination=static_cast<int>(std::round(k*ratio));
            if(destination>0 && destination<size/2){
                const auto d=static_cast<std::size_t>(destination);
                const double amplitude=std::hypot(frequency[2*k],frequency[2*k+1]);
                magnitude[d]+=amplitude;weighted[d]+=amplitude*actual*ratio;
            }
        }
        std::fill(frequency,frequency+size,0.f);
        for(int k=1;k<size/2;++k){
            const auto i=static_cast<std::size_t>(k);
            if(magnitude[i]>1e-15)phase[i]=std::remainder(phase[i]+hop*weighted[i]/magnitude[i],2*pi);
            frequency[2*k]=static_cast<float>(magnitude[i]*std::cos(phase[i]));
            frequency[2*k+1]=static_cast<float>(magnitude[i]*std::sin(phase[i]));
        }
        pffft_transform_ordered(setup,frequency,time,work,PFFFT_BACKWARD);
        for(int i=0;i<size;++i)output[static_cast<std::size_t>((position+i)%size)]+=time[i]*window[static_cast<std::size_t>(i)]*(2.f/(3.f*static_cast<float>(size)));
        return result;
    }
};
struct Lane {
    shifter_detail::HilbertTransformMonoFloat hilbert;
    std::array<Pitch,6> pitch;
    std::vector<float> delay,dry;
    std::size_t write=0,dryWrite=0;
    double oscillator=0,lfo=0,envelope=0,tone=0,randomFrom=0,randomTo=0;
    std::uint32_t random=1;
};
}
struct Shifter::State {
    std::array<double,Count> p{};
    std::array<Lane,2> lanes;
    double rate=48000,bpm=120,noteTarget=60,noteCurrent=60,bendValue=0;
    int pitchIndex=3;
    bool prepared=false;
    State(){for(int i=0;i<Count;++i)p[static_cast<std::size_t>(i)]=ranges[i].initial;}
    int window()const {return 256<<pitchIndex;}
    void selectWindow(){
        const double desired=p[Window]*rate/1000;
        pitchIndex=std::clamp(static_cast<int>(std::round(std::log2(desired/256))),0,5);
    }
};
Shifter::Shifter():state_(std::make_unique<State>()){}
Shifter::~Shifter()=default;
void Shifter::prepare(double rate){
    auto& s=*state_;s.rate=std::isfinite(rate)&&rate>0?rate:48000;s.selectWindow();
    for(std::size_t c=0;c<2;++c){auto& l=s.lanes[c];
        l.hilbert.setSampleRate(static_cast<float>(s.rate));
        for(int i=0;i<6;++i)l.pitch[static_cast<std::size_t>(i)].prepare(256<<i);
        l.delay.assign(static_cast<std::size_t>(s.rate*30)+2,0);l.dry.assign(8193,0);
        l.write=l.dryWrite=0;l.oscillator=l.lfo=l.envelope=l.tone=l.randomFrom=l.randomTo=0;
        l.random=0x12345678u+static_cast<std::uint32_t>(c);
    }s.noteCurrent=s.noteTarget;s.prepared=true;
}
void Shifter::set(Param p,double value) noexcept {
    if(p<0 || p>=Count || !std::isfinite(value))return;
    state_->p[static_cast<std::size_t>(p)]=std::clamp(value,ranges[p].low,ranges[p].high);
    if(p==Window)state_->selectWindow();
}
void Shifter::tempo(double bpm) noexcept {if(std::isfinite(bpm))state_->bpm=std::clamp(bpm,20.,999.);}
void Shifter::note(double n,double velocity) noexcept {if(std::isfinite(n)&&velocity>0)state_->noteTarget=std::clamp(n,0.,127.);}
void Shifter::bend(double v) noexcept {if(std::isfinite(v))state_->bendValue=std::clamp(v,-1.,1.);}
int Shifter::latency()const noexcept{return state_->p[Mode]<0.5?state_->window():0;}
void Shifter::process(const float* inL,const float* inR,float* outL,float* outR,std::size_t frames) noexcept {
    auto& s=*state_;auto& p=s.p;
    if(!s.prepared){std::fill(outL,outL+frames,0.f);std::fill(outR,outR+frames,0.f);return;}
    const int mode=static_cast<int>(std::round(p[Mode]));
    const double lfoHz=p[LfoSync]>=0.5?s.bpm/(60*p[LfoBeats]):p[LfoRate];
    const double delaySamples=std::min(s.rate*29,(p[DelaySync]>=0.5?60*p[DelayBeats]/s.bpm:1/p[DelayRate])*s.rate);
    for(std::size_t frame=0;frame<frames;++frame){
        const double glide=p[Glide]<=0?1:1-std::exp(-1000/(p[Glide]*s.rate));
        s.noteCurrent+=(s.noteTarget-s.noteCurrent)*glide;
        double leftRandom=0;
        const std::array<float,2> inputs{inL[frame],inR[frame]};
        for(std::size_t channel=0;channel<2;++channel){auto& l=s.lanes[channel];
            const double input=inputs[channel];
            l.dry[l.dryWrite]=static_cast<float>(input);
            const double dry=l.dry[(l.dryWrite+l.dry.size()-static_cast<std::size_t>(latency()))%l.dry.size()];
            l.dryWrite=(l.dryWrite+1)%l.dry.size();
            const double speed=lfoHz*(channel==1 && p[StereoMode]>=0.5 && p[StereoMode]<1.5?1+p[Spin]/100:1);
            l.lfo+=speed/s.rate;
            if(l.lfo>=1){l.lfo=wrap(l.lfo);l.randomFrom=l.randomTo;l.random^=l.random<<13;l.random^=l.random>>17;l.random^=l.random<<5;l.randomTo=2*static_cast<double>(l.random)/4294967295.-1;}
            double phase=wrap(l.lfo+p[Offset]/360+(channel==1 && p[StereoMode]<0.5?p[Phase]/360:0));
            const double duty=p[Duty]/100;
            phase=phase<duty?phase/(2*duty):0.5+(phase-duty)/(2*(1-duty));
            const int shape=static_cast<int>(std::round(p[LfoShape]));
            double modulation=std::sin(2*pi*phase);
            const double triangle=1-4*std::abs(phase-0.5);
            if(shape==1)modulation=triangle;
            if(shape==2)modulation=std::sin(triangle*pi/2);
            if(shape==3 || shape==4){const double steps=shape==3?8:16;modulation=std::round(triangle*steps)/steps;}
            if(shape==5)modulation=2*phase-1;
            if(shape==6)modulation=1-2*phase;
            if(shape==7)modulation=phase<0.5?1:-1;
            if(shape>=8){modulation=shape==8?l.randomFrom+(l.randomTo-l.randomFrom)*phase:l.randomTo;
                if(channel==0)leftRandom=modulation;else if(p[StereoMode]>=1.5)modulation=leftRandom*(1-2*p[Width]/100);}
            const double target=std::abs(input);
            const double time=target>l.envelope?p[Attack]:p[Release];
            l.envelope+=(target-l.envelope)*(1-std::exp(-1000/(s.rate*time)));
            const double motion=p[LfoAmount]*modulation+(p[EnvOn]>=0.5?p[EnvAmount]*l.envelope:0);
            const double spread=p[Spread]*(channel==1 && p[Wide]>=0.5?-1:1);
            const double tunedNote=s.noteCurrent+s.bendValue*p[BendRange];
            double shift=mode==0?p[Coarse]+(p[Fine]+spread)/100+motion:p[Coarse]*1000+p[Fine]+spread+motion*1000;
            if(p[MidiMode]>=0.5)shift=mode==0?tunedNote-60+motion:440*std::exp2((tunedNote-69)/12)+motion*1000;
            const auto offset=static_cast<std::size_t>(delaySamples);
            const double frac=delaySamples-static_cast<double>(offset);
            const auto at=(l.write+l.delay.size()-offset)%l.delay.size();
            const double delayed=l.delay[at]*(1-frac)+l.delay[(at+l.delay.size()-1)%l.delay.size()]*frac;
            l.tone+=(delayed-l.tone)*(1-std::exp(-2*pi*std::min(p[Tone],s.rate*0.45)/s.rate));
            const double feed=input+(p[Delay]>=0.5?l.tone*p[Feedback]/100:0);
            double wet=0;
            if(mode==0)wet=l.pitch[static_cast<std::size_t>(s.pitchIndex)].sample(static_cast<float>(feed),std::exp2(std::clamp(shift,-48.,48.)/12));
            else {
                l.oscillator=wrap(l.oscillator+std::clamp(shift,-s.rate*0.45,s.rate*0.45)/s.rate);
                if(mode==1){const auto [re,im]=l.hilbert.stepPair(static_cast<float>(feed));wet=re*std::cos(2*pi*l.oscillator)+im*std::sin(2*pi*l.oscillator);}
                else {wet=feed*std::sin(2*pi*l.oscillator);if(p[DriveOn]>=0.5)wet=std::tanh(wet*std::pow(10.,p[Drive]/20));}
            }
            l.delay[l.write]=static_cast<float>(wet);l.write=(l.write+1)%l.delay.size();
            if(p[Delay]>=0.5)wet=delayed;
            const float output=static_cast<float>(dry+(wet-dry)*p[Mix]/100);
            if(channel==0)outL[frame]=output;else outR[frame]=output;
        }
    }
}
}
