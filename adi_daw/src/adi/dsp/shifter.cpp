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
// Pitch mode, after Live 12.4 as measured (the director's research, DV-06):
// the device reports no latency, its Window runs 10..350 ms, and at unison its
// output is a clean copy of the input exactly half the Window late (240, 1920
// and 8400 samples at 10, 80 and 350 ms, 48 kHz), compensated or not. A
// rotating-tap delay line has that property by construction: two read taps
// sweep a delay of 0..Window, half a cycle apart, crossfaded by sin^2 windows
// that always sum to one; at unison they rest with one tap at Window/2 and
// gain one. The same model put Live's measured +7 st onset and peak within a
// few samples once the sweep phase was free; Live's phase reference is not
// known, so the shape away from unison is not claimed (DEV-22).
struct Pitch {
    /// DF-E37 (b): how long the taps take to come back to rest at unison.
    static constexpr double kRestSeconds=0.020;
    std::vector<float> line;
    std::size_t write=0;
    double phase=0.5,rest=0;   ///< rest: 0..1 through the return to the clean read
    void prepare(double maximumWindow){
        line.assign(static_cast<std::size_t>(std::ceil(maximumWindow))+4,0);
        write=0;phase=0.5;rest=0;
    }
    /// The two sin^2-weighted taps at `ph`. At ph 0 or 0.5 it is one clean
    /// copy, half the Window late.
    double taps(double ph,double window) const noexcept {
        const auto size=line.size();
        double out=0;
        for(const double offset:{0.0,0.5}){
            const double at=wrap(ph+offset);
            const double gain=std::sin(pi*at);
            double position=static_cast<double>(write)-at*window;
            if(position<0)position+=static_cast<double>(size);
            const double whole=std::floor(position);
            const double fraction=position-whole;
            const auto first=static_cast<std::size_t>(whole)%size;
            const auto second=(first+1)%size;
            out+=gain*gain*(line[first]*(1-fraction)+line[second]*fraction);
        }
        return out;
    }
    /// `restStep` is 1 / (kRestSeconds * rate).
    float sample(float in,double ratio,double window,double restStep) noexcept {
        line[write]=in;
        double out;
        // At unison the phase does not move, so a phase a pitch move left
        // between the rests (0 and 0.5) would comb for good. Crossfade from
        // the two taps where they are to the clean read, then rest there: no
        // pitch glide, no jump. Any move away from unison cancels the return.
        const bool away=std::abs(phase-0.5*std::round(2*phase))>1e-12;
        if(ratio==1.0 && (away || rest>0)){
            rest=std::min(1.0,rest+restStep);
            out=(1-rest)*taps(phase,window)+rest*taps(0.5,window);
            if(rest>=1){phase=0.5;rest=0;}
        }else{
            rest=0;
            out=taps(phase,window);
        }
        write=(write+1)%line.size();
        phase=wrap(phase+(1-ratio)/window);
        return static_cast<float>(out);
    }
};
struct Lane {
    shifter_detail::HilbertTransformMonoFloat hilbert;
    Pitch pitch;
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
    bool prepared=false;
    State(){for(int i=0;i<Count;++i)p[static_cast<std::size_t>(i)]=ranges[i].initial;}
    /// The Window in samples: Live's milliseconds at this rate, not rounded.
    double window()const {return p[Window]*rate/1000;}
};
Shifter::Shifter():state_(std::make_unique<State>()){}
Shifter::~Shifter()=default;
void Shifter::prepare(double rate){
    auto& s=*state_;s.rate=std::isfinite(rate)&&rate>0?rate:48000;
    for(std::size_t c=0;c<2;++c){auto& l=s.lanes[c];
        l.hilbert.setSampleRate(static_cast<float>(s.rate));
        l.pitch.prepare(ranges[Window].high*s.rate/1000);
        l.delay.assign(static_cast<std::size_t>(s.rate*30)+2,0);l.dry.assign(8193,0);
        l.write=l.dryWrite=0;l.oscillator=l.lfo=l.envelope=l.tone=l.randomFrom=l.randomTo=0;
        l.random=0x12345678u+static_cast<std::uint32_t>(c);
    }s.noteCurrent=s.noteTarget;s.prepared=true;
}
void Shifter::set(Param p,double value) noexcept {
    if(p<0 || p>=Count || !std::isfinite(value))return;
    state_->p[static_cast<std::size_t>(p)]=std::clamp(value,ranges[p].low,ranges[p].high);
}
void Shifter::tempo(double bpm) noexcept {if(std::isfinite(bpm))state_->bpm=std::clamp(bpm,20.,999.);}
void Shifter::note(double n,double velocity) noexcept {if(std::isfinite(n)&&velocity>0)state_->noteTarget=std::clamp(n,0.,127.);}
void Shifter::bend(double v) noexcept {if(std::isfinite(v))state_->bendValue=std::clamp(v,-1.,1.);}
// Live 12.4 reports no latency for Shifter in any mode (DV-06), and leaves the
// half-Window delay of Pitch mode uncompensated; so does ADI (ADR-0203 d4).
int Shifter::latency()const noexcept{return 0;}
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
            if(mode==0)wet=l.pitch.sample(static_cast<float>(feed),std::exp2(std::clamp(shift,-48.,48.)/12),s.window(),1/(Pitch::kRestSeconds*s.rate));
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
