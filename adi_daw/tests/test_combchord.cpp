// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/combchord.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <numbers>
#include <vector>
namespace { bool counting=false; std::size_t allocations=0; }
void* operator new(std::size_t n) { if(counting) ++allocations; if(auto p=std::malloc(n?n:1)) return p; throw std::bad_alloc(); }
void* operator new[](std::size_t n) {return operator new(n);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
namespace {
using adi::dsp::CombChord;
int checks=0, failures=0;
void check(bool ok,const char* name) {++checks; if(!ok) {++failures;std::printf("FAIL %s\n",name);}}
constexpr double pi=std::numbers::pi;
void note(CombChord& c,double hz) { CombChord::Chord chord; chord.fill(69+12*std::log2(hz/440));c.setChord(0,chord); }
// Direct DFT of RENDERED samples; recurrence avoids per-sample trig.
double magnitude(const std::vector<float>& x,double rate,double hz,std::size_t first=0,std::size_t count=0) {
    if(count==0) count=x.size()-first;
    std::complex<double> sum=0, phase=1, step=std::polar(1.0,-2*pi*hz/rate);
    for(std::size_t i=0;i<count;++i) {sum+=static_cast<double>(x[first+i])*phase;phase*=step;}
    return std::abs(sum)/static_cast<double>(count);
}
double findPeak(const std::vector<float>& x,double rate,double hz) {
    double lo=hz*std::exp2(-15.0/1200), hi=hz*std::exp2(15.0/1200);
    // Ternary maximum search, 28 reductions resolve much less than 0.01 cent.
    for(int i=0;i<28;++i) {
        const double a=(2*lo+hi)/3,b=(lo+2*hi)/3;
        if(magnitude(x,rate,a)<magnitude(x,rate,b))lo=a;else hi=b;
    }
    return (lo+hi)/2;
}
void tuning() {
    double worst=0;
    for(double rate:{44100.,48000.,96000.}) for(auto mode:{CombChord::Mode::Saw,CombChord::Mode::Square})
    for(double color:{0.,1.}) for(double hz:{40.,63.7,110.,220.3,440.,777.7,1000.,1500.}) {
        CombChord c;c.prepare(rate);note(c,hz);c.setMode(mode);c.setColor(color);c.setDecay(1);
        std::vector<float> in(static_cast<std::size_t>(rate/2),0),out(in.size());in[0]=1;
        c.process(in.data(),out.data(),in.size());
        const double measured=findPeak(out,rate,hz), cents=1200*std::log2(measured/hz);
        worst=std::max(worst,std::abs(cents));
        if(std::abs(cents)>3)std::printf("pitch rate %.0f hz %.2f color %.0f square %d cents %.4f\n",rate,hz,color,mode==CombChord::Mode::Square,cents);
        check(std::abs(cents)<=3,"rendered resonance pitch within 3 cents");
    }
    std::printf("METRIC worst pitch error %.6f cents (96 combinations)\n",worst);
    // Exact integer half-period isolates Square's polarity from interpolator
    // dispersion: 200 Hz at 48k has a 120-sample half-period.
    CombChord c;c.prepare(48000);note(c,200);c.setMode(CombChord::Mode::Square);c.setDecay(2);
    std::vector<float> in(96000),out(in.size());in[0]=1;c.process(in.data(),out.data(),in.size());
    const double fundamental=magnitude(out,48000,200);
    double evenRatio=0;
    for(double harmonic:{2.,4.,6.})evenRatio=std::max(evenRatio,magnitude(out,48000,200*harmonic)/fundamental);
    check(magnitude(out,48000,100)<fundamental/100,"Square does not resonate one octave low");
    check(evenRatio<0.01,"Square even harmonics suppressed by at least 40 dB");
    check(magnitude(out,48000,600)>fundamental*0.99 && magnitude(out,48000,1000)>fundamental*0.99,"Square retains odd harmonics");
    std::printf("METRIC Square even/fundamental %.3f dB\n",20*std::log10(evenRatio));
}
void decay() {
    for(double rate:{44100.,48000.,96000.}) for(auto mode:{CombChord::Mode::Saw,CombChord::Mode::Square}) for(double hz:{40.,1500.}) {
        CombChord c;c.prepare(rate);note(c,hz);c.setMode(mode);c.setDecay(1);
        const auto start=static_cast<std::size_t>(rate/2);
        std::vector<float> in(static_cast<std::size_t>(rate*1.2)),out(in.size());
        for(std::size_t i=0;i<start;++i)in[i]=static_cast<float>(std::sin(2*pi*hz*static_cast<double>(i)/rate));
        c.process(in.data(),out.data(),in.size());
        // Compare equal 100 ms windows, separated by 400 ms, after excitation
        // ends. T60=-60*deltaTime/(20*log10(A2/A1)).
        const auto a=static_cast<std::size_t>(rate*0.6),b=static_cast<std::size_t>(rate);
        const auto length=static_cast<std::size_t>(rate/10);
        const double ratio=magnitude(out,rate,hz,b,length)/magnitude(out,rate,hz,a,length);
        const double t60=-60*0.4/(20*std::log10(ratio));
        std::printf("METRIC T60 rate %.0f hz %.0f square %d: %.6f s\n",rate,hz,mode==CombChord::Mode::Square,t60);
        check(std::abs(t60-1)<=0.1,"measured T60 within ten percent at Color zero");
    }
}
void runtime() {
    // A six-note bank equals the average of six independent unison banks.
    // This catches a missing voice, a state index ignored, or summing without
    // the 1/6 gain. Different chords exercise every stored state audibly.
    CombChord bank;bank.prepare(48000);
    std::vector<float> impulse(4096),bankOut(4096),single(4096),sum(4096);impulse[0]=1;
    for(std::size_t state=0;state<CombChord::states;++state){
        CombChord::Chord notes{40,43.25,47,52.5,55,59.75};
        for(auto& n:notes)n+=static_cast<double>(state);
        bank.setChord(state,notes);bank.setState(state);bank.reset();
        bank.process(impulse.data(),bankOut.data(),impulse.size());std::fill(sum.begin(),sum.end(),0.0f);
        for(double midi:notes){CombChord voice;voice.prepare(48000);CombChord::Chord unison;unison.fill(midi);voice.setChord(0,unison);
            voice.process(impulse.data(),single.data(),impulse.size());
            for(std::size_t i=0;i<sum.size();++i)sum[i]+=single[i]/6;
        }
        double difference=0;for(std::size_t i=0;i<sum.size();++i)difference=std::max(difference,std::abs(static_cast<double>(sum[i]-bankOut[i])));
        check(difference<1e-8,"each stored state renders six independently tuned parallel voices");
    }
    std::vector<float> in(20000),reference(in.size()),out(in.size());
    for(std::size_t i=0;i<in.size();++i)in[i]=static_cast<float>(std::sin(static_cast<double>(i)*0.31));
    CombChord c;c.prepare(48000);c.setColor(1);c.process(in.data(),reference.data(),in.size());
    for(std::size_t block:std::array<std::size_t,8>{32,64,128,256,512,1024,2048,4096}) {
        c.reset();
        for(std::size_t n=0;n<in.size();n+=block)c.process(in.data()+n,out.data()+n,std::min(block,in.size()-n));
        check(out==reference,"bit-identical output across block sizes");
    }
    counting=true;allocations=0;
    c.setDecay(20);c.setColor(0.75);c.setState(7);c.setMode(CombChord::Mode::Square);
    c.process(in.data(),out.data(),in.size());
    counting=false;check(allocations==0,"no allocations in setters and process");
    // Distinct stored chords, state switching and fractional MIDI persistence.
    auto chord=c.chord(0);chord[0]+=0.25;c.setChord(3,chord);
    check(c.chord(3)==chord && c.chord(0)!=chord,"eight independent chord slots retain fractional MIDI");
    c.setMix(0);c.setOutputDb(0);c.process(in.data(),out.data(),in.size());check(out==in,"dry mix is identity");
    c.setOutputDb(-6);c.process(in.data(),out.data(),in.size());
    check(std::abs(out[1]/in[1]-std::pow(10.,-6./20))<1e-7,"output trim uses amplitude dB");
    c.setMix(1);c.setOutputDb(0);
    // Both modes and endpoint Colors, 60 seconds noise THEN 60 seconds silence
    // at 96k: silence tests the excited recursive tail rather than empty state.
    std::vector<float> noise(4096),silence(4096),tail(4096);
    for(auto mode:{CombChord::Mode::Saw,CombChord::Mode::Square})for(double color:{0.,1.}) {
        c.prepare(96000);c.setMode(mode);c.setDecay(20);c.setColor(color);
        unsigned random=1;bool finite=true;
        for(int pass=0;pass<2;++pass)for(std::size_t n=0;n<60*96000;n+=noise.size()) {
            for(auto& x:noise) {random=1664525u*random+1013904223u;x=static_cast<float>(static_cast<double>(random)/2147483648.0-1);}
            c.process(pass==0?noise.data():silence.data(),tail.data(),std::min(noise.size(),60*96000-n));
            for(float x:tail)finite &= std::isfinite(x);
        }
        check(finite,"60s full-scale noise and 60s silence remain finite");
    }
    c.setColor(std::numeric_limits<double>::quiet_NaN());c.setDecay(std::numeric_limits<double>::infinity());
    c.process(silence.data(),tail.data(),tail.size());
    check(std::all_of(tail.begin(),tail.end(),[](float x){return std::isfinite(x);}),"nonfinite parameters use finite defaults");
}
}
int main(){tuning();decay();runtime();std::printf("%s -- %d checks, %d failure(s)\n",failures?"FAIL":"PASS",checks,failures);return failures?1:0;}
