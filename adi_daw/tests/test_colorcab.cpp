// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/colorcab.hpp"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <numbers>
#include <stdexcept>
#include <vector>
namespace {bool counting=false;std::size_t allocations=0;}
void* operator new(std::size_t n){if(counting)++allocations;if(auto p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return operator new(n);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
namespace {
using namespace adi::dsp;
int checks=0,failures=0;
void check(bool ok,const char* name){++checks;if(!ok){++failures;std::printf("FAIL %s\n",name);}}
constexpr double pi=std::numbers::pi;
double magnitude(const ColorCabKernel& h,double hz,double rate=48000){
    std::complex<double> sum=0,phase=1,step=std::polar(1.,-2*pi*hz/rate);
    for(std::size_t i=0;i<h.size;++i){sum+=h.taps[i]*phase;phase*=step;}return std::abs(sum);
}
// White excitation through two damped resonances, plus dry noise to avoid
// unrepresentable deep nulls: an explicitly bounded vocal-like test fixture.
std::vector<float> sample(){
    std::vector<float> x(48000);unsigned r=1;double a=0,b=0,c=0,d=0;
    const double radius=std::exp(-2*pi*300/48000);
    for(auto& v:x){r=1664525u*r+1013904223u;const double noise=static_cast<double>(r)/2147483648.-1;
        const double y=noise+2*radius*std::cos(2*pi*2200/48000)*a-radius*radius*b;b=a;a=y;
        const double z=noise+2*radius*std::cos(2*pi*5500/48000)*c-radius*radius*d;d=c;c=z;
        v=static_cast<float>(noise+0.12*y+0.15*z);
    }return x;
}
double peakHz(const ColorCabKernel& k,double lo,double hi){
    for(int i=0;i<36;++i){double a=(2*lo+hi)/3,b=(lo+2*hi)/3;if(magnitude(k,a)<magnitude(k,b))lo=a;else hi=b;}return (lo+hi)/2;
}
void design(){
    const auto x=sample();ColorCabOptions o;o.smoothingOctaves=0.5;
    const auto a=buildColorCab(x,48000,o),b=buildColorCab(x,48000,o);
    check(a.kernel.taps==b.kernel.taps && a.target==b.target,"builder deterministic bit for bit");
    auto scaled=x;for(auto& v:scaled)v*=0.5f;
    check(buildColorCab(scaled,48000,o).kernel.taps==a.kernel.taps,"source level normalization is deterministic");
    auto longer=x;longer.resize(96000);
    for(std::size_t i=x.size();i<longer.size();++i)longer[i]=static_cast<float>(10*std::sin(2*pi*8000*static_cast<double>(i)/48000));
    const auto tail=buildColorCab(longer,48000,o);
    check(tail.target!=a.target && magnitude(tail.kernel,8000)>magnitude(a.kernel,8000),"analysis includes the file tail");
    double worst=0;
    for(std::size_t k=1;k<a.target.size();++k){
        double hz=static_cast<double>(k)*24000/static_cast<double>(a.target.size()-1);
        if(hz>=200&&hz<=16000)worst=std::max(worst,std::abs(20*std::log10(magnitude(a.kernel,hz)/a.target[k])));
    }
    std::printf("METRIC Size 256 target error 200..16000 Hz %.6f dB\n",worst);
    check(worst<=1,"256-tap vocal-like fixture within 1 dB of smoothed target");
    // A symmetric linear-phase response with the SAME magnitude has its
    // energy centroid at its midpoint. Compare to a 256-tap midpoint; an
    // exact long spectral reconstruction would have an even later centroid.
    double energy=0,moment=0,early=0;
    for(std::size_t i=0;i<a.kernel.size;++i){const double e=a.kernel.taps[i]*a.kernel.taps[i];energy+=e;moment+=static_cast<double>(i)*e;if(i<a.kernel.size/2)early+=e;}
    std::printf("METRIC energy centroid %.6f samples; first-half energy %.6f%%\n",moment/energy,100*early/energy);
    check(moment/energy<127.5 && early/energy>0.95,"minimum-phase factor energy front-loaded");
    // Independently synthesize a symmetric linear-phase kernel from the
    // DESIGNED FIR's magnitude (not from the builder's cepstrum). A 1025-tap
    // inverse cosine series with delay 512 samples preserves the same sampled
    // magnitude. Measure its energy centroid rather than assume the midpoint.
    constexpr std::size_t linearSize=1025, midpoint=linearSize/2;
    std::vector<double> amplitudes(midpoint+1),linear(linearSize);
    for(std::size_t k=0;k<=midpoint;++k)
        amplitudes[k]=magnitude(a.kernel,48000*static_cast<double>(k)/linearSize);
    double linearEnergy=0,linearMoment=0;
    for(std::size_t i=0;i<linearSize;++i){
        double sum=amplitudes[0];
        for(std::size_t k=1;k<=midpoint;++k)
            sum+=2*amplitudes[k]*std::cos(2*pi*static_cast<double>(k)*(static_cast<double>(i)-midpoint)/linearSize);
        linear[i]=sum/linearSize;
        linearEnergy+=linear[i]*linear[i];linearMoment+=static_cast<double>(i)*linear[i]*linear[i];
    }
    check(std::abs(linearMoment/linearEnergy-midpoint)<1e-8 && moment/energy<linearMoment/linearEnergy,
          "measured centroid precedes actual same-magnitude linear-phase kernel");
    double sameMagnitude=0;
    for(std::size_t k=0;k<=midpoint;k+=8){
        std::complex<double> response=0;
        for(std::size_t i=0;i<linearSize;++i)
            response+=linear[i]*std::polar(1.,-2*pi*static_cast<double>(k*i)/linearSize);
        sameMagnitude=std::max(sameMagnitude,std::abs(std::abs(response)-amplitudes[k]));
    }
    check(sameMagnitude<1e-10,"linear-phase comparison really has the same magnitude");
    // Single formant from a damped sinusoid, centered within the analysis file.
    std::vector<float> formant(24000,0);
    for(std::size_t i=0;i<2000;++i)formant[8192+i]=static_cast<float>(std::exp(-static_cast<double>(i)/20)*std::cos(2*pi*3000*static_cast<double>(i)/48000));
    o.gamma=1;o.smoothingOctaves=1./6;
    const auto base=buildColorCab(formant,48000,o);const double basePeak=peakHz(base.kernel,2200,3800);
    for(double shift:{-12.,7.,12.}){
        o.pitchSemitones=shift;const auto shifted=buildColorCab(formant,48000,o);
        const double ratio=std::exp2(shift/12),actual=peakHz(shifted.kernel,basePeak*ratio*0.85,basePeak*ratio*1.15);
        const double error=std::abs(actual/(basePeak*ratio)-1);
        std::printf("METRIC formant shift %.0f semitones error %.6f%%\n",shift,100*error);
        check(error<0.01,"Pitch moves FIR formant peak within one percent");
    }
    std::vector<float> zeros(64,0);const auto silent=buildColorCab(zeros,48000);
    check(std::all_of(silent.kernel.taps.begin(),silent.kernel.taps.end(),[](double v){return v==0;}),"silent source yields silence");
    o.gamma=0;o.pitchSemitones=0;const auto flat=buildColorCab(x,48000,o);
    check(std::abs(flat.kernel.taps[0]-1)<1e-12 && magnitude(flat.kernel,200)>0.999999,"gamma zero yields identity");
    for(std::size_t size:{64,128,256,512,1024}){o.size=size;check(buildColorCab(x,48000,o).kernel.size==size,"Size menu supported");}
    bool rejected=false;try{(void)buildColorCab({},48000);}catch(const std::invalid_argument&){rejected=true;}check(rejected,"empty source rejected");
    auto bad=x;bad.back()=std::numeric_limits<float>::quiet_NaN();rejected=false;
    try{(void)buildColorCab(bad,48000);}catch(const std::invalid_argument&){rejected=true;}check(rejected,"nonfinite source rejected");
}
void runtime(){
    ColorCabOptions options;options.smoothingOctaves=0.5;
    const auto kernel=buildColorCab(sample(),48000,options).kernel;
    ColorCab c;c.prepare(48000);check(c.setKernel(kernel),"accept first kernel");c.reset();
    std::vector<float> input(4096),out(input.size());input[0]=1;c.process(input.data(),out.data(),input.size());
    double error=0;for(std::size_t i=0;i<kernel.size;++i)error=std::max(error,std::abs(out[i]-kernel.taps[i]));
    check(out[0]!=0 && error<1e-7,"zero-latency impulse matches FIR taps");
    // Triangle inequality on the convex fade: |dy| <=
    // 2*A*sin(w/2)*max(||h0||1,||h1||1) + A*||h1-h0||1/N.
    for(double rate:{44100.,48000.,96000.}){
        c.prepare(rate);const double hz=997,amplitude=0.25;
        std::vector<float> sine(5000),y(5000);
        for(std::size_t i=0;i<sine.size();++i)sine[i]=static_cast<float>(amplitude*std::sin(2*pi*hz*static_cast<double>(i)/rate));
        c.process(sine.data(),y.data(),2000);check(c.setKernel(kernel),"accept swap during sine");
        check(!c.setKernel(kernel),"busy fade refuses replacement");
        const auto n=static_cast<std::size_t>(std::llround(rate*0.020));
        counting=true;allocations=0;
        c.process(sine.data()+2000,y.data()+2000,n-1);const bool still=c.fading();
        c.process(sine.data()+2000+n-1,y.data()+2000+n-1,1);const bool done=!c.fading();
        c.process(sine.data()+2000+n,y.data()+2000+n,y.size()-2000-n);
        counting=false;
        check(allocations==0,"zero allocations during crossfade");check(still&&done,"fade is rounded 20 ms");
        double l1=0,diff=0;for(std::size_t i=0;i<kernel.size;++i){l1+=std::abs(kernel.taps[i]);diff+=std::abs(kernel.taps[i]-(i==0?1:0));}
        const double bound=2*amplitude*std::sin(pi*hz/rate)*std::max(1.,l1)+amplitude*diff/static_cast<double>(n);
        double maxStep=0;for(std::size_t i=2000;i<2000+n+1;++i)maxStep=std::max(maxStep,std::abs(static_cast<double>(y[i]-y[i-1])));
        std::printf("METRIC swap rate %.0f step %.9f bound %.9f\n",rate,maxStep,bound);check(maxStep<=bound+1e-7,"swap obeys derived bound");
    }
    for(std::size_t i=0;i<input.size();++i)input[i]=static_cast<float>(std::sin(static_cast<double>(i)*0.1));
    c.prepare(48000);(void)c.setKernel(kernel);std::vector<float> reference(out.size());c.process(input.data(),reference.data(),input.size());
    for(std::size_t block:{32,64,128,256,512,1024,2048,4096}){
        c.prepare(48000);(void)c.setKernel(kernel);
        for(std::size_t i=0;i<input.size();i+=block)c.process(input.data()+i,out.data()+i,std::min(block,input.size()-i));
        check(out==reference,"crossfade bit-identical across block sizes");
    }
    c.setMix(0);c.process(input.data(),out.data(),input.size());check(out==input,"dry mix identity");
    auto bad=kernel;bad.taps[0]=std::numeric_limits<double>::infinity();check(!c.setKernel(bad),"invalid kernel refused");
}
}
int main(){design();runtime();std::printf("%s -- %d checks, %d failure(s)\n",failures?"FAIL":"PASS",checks,failures);return failures?1:0;}
