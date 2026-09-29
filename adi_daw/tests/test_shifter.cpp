// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/shifter.hpp"
#include "adi/dsp/spectrum.hpp"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
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
using S=adi::dsp::Shifter;
int checks=0,failures=0;
void check(bool c,const char* t){++checks;if(!c){++failures;std::printf("FAIL %s\n",t);}}
std::vector<float> tone(double hz,std::size_t n=131072){std::vector<float> a(n);for(std::size_t i=0;i<n;++i)a[i]=static_cast<float>(0.3*std::sin(2*std::numbers::pi*hz*static_cast<double>(i)/48000));return a;}
std::vector<float> render(S& s,const std::vector<float>& in,std::size_t block=64){std::vector<float> l(in.size()),r(in.size());for(std::size_t i=0;i<in.size();i+=block)s.process(in.data()+i,in.data()+i,l.data()+i,r.data()+i,std::min(block,in.size()-i));return l;}
double bin(const std::vector<float>& x,double hz){std::complex<double> z=0,ph=1,step=std::polar(1.,-2*std::numbers::pi*hz/48000);for(std::size_t i=x.size()/2;i<x.size();++i){z+=static_cast<double>(x[i])*ph;ph*=step;}return 4*std::abs(z)/static_cast<double>(x.size());}
double fftPeak(const std::vector<float>& x,double hz){adi::dsp::Spectrum fft(65536);std::vector<float> bins(32769);fft.analyse(std::span<const float>(x).last(65536),bins);const int center=static_cast<int>(hz*65536/48000);int at=center;for(int i=center-8;i<=center+8;++i)if(bins[static_cast<std::size_t>(i)]>bins[static_cast<std::size_t>(at)])at=i;
 const double a=std::log(std::max(1e-20f,bins[static_cast<std::size_t>(at-1)])),b=std::log(std::max(1e-20f,bins[static_cast<std::size_t>(at)])),c=std::log(std::max(1e-20f,bins[static_cast<std::size_t>(at+1)]));return (at+0.5*(a-c)/(a-2*b+c))*48000/65536;}
void measured(){const auto input=tone(1000);
 for(double cents:{-1200.,-700.,13.,700.,1200.}){S s;s.prepare(48000);s.set(S::Fine,cents-std::floor(cents/100)*100);s.set(S::Coarse,std::floor(cents/100));auto out=render(s,input);const double expected=1000*std::exp2(cents/1200),found=fftPeak(out,expected),error=1200*std::log2(found/expected);std::printf("METRIC pitch %.0f cents: %.5f Hz error %.4f cents\n",cents,found,error);check(std::abs(error)<2,"FFT pitch error below 2 cents");check(bin(out,expected)>0.03,"pitch produces energy at expected frequency");}
 S s;s.prepare(48000);s.set(S::Mode,1);s.set(S::Coarse,0.25);auto freq=render(s,input);check(bin(freq,1250)>0.25,"Freq translates 1 kHz upward by 250 Hz");check(bin(freq,750)<0.01,"Freq rejects opposite sideband");
 s.prepare(48000);s.set(S::Mode,2);auto ring=render(s,input);check(bin(ring,750)>0.14 && bin(ring,1250)>0.14,"Ring produces sum and difference");s.set(S::DriveOn,1);s.set(S::Drive,24);auto driven=render(s,input);check(bin(driven,2250)>bin(ring,2250)+0.01,"Ring Drive creates distortion harmonics");
 s.prepare(48000);s.set(S::Mode,1);s.set(S::DriveOn,0);s.set(S::MidiMode,1);s.note(69,100);auto midi=render(s,input);check(bin(midi,1440)>0.25,"MIDI note tunes Freq carrier");s.bend(1);s.set(S::BendRange,12);auto bend=render(s,input);check(bin(bend,1880)>0.25,"pitch bend applies configured range");
 s.prepare(48000);s.set(S::MidiMode,0);s.set(S::Coarse,0.25);s.set(S::Delay,1);s.set(S::DelaySync,1);s.set(S::DelayBeats,1);s.tempo(120);auto delayed=render(s,input);check(std::all_of(delayed.begin(),delayed.begin()+24000,[](float v){return v==0;}),"sync delay holds first quarter note at 120 BPM");check(bin(delayed,1250)>0.25,"delayed signal remains audible");
}
void invariants(){auto input=tone(733,32768);std::vector<float> expected;
 for(std::size_t block:{32u,64u,128u,256u,512u,1024u,2048u,4096u}){S s;s.prepare(48000);s.set(S::Coarse,7);s.set(S::LfoAmount,0.2);s.set(S::EnvOn,1);s.set(S::EnvAmount,0.1);auto out=render(s,input,block);if(expected.empty())expected=out;check(out==expected,"pitch/modulation identical across block sizes");}
 S s;s.prepare(48000);std::vector<float> l(input.size()),r(input.size());allocations=0;counting=true;s.process(input.data(),input.data(),l.data(),r.data(),input.size());counting=false;check(allocations==0,"zero processing allocations including FFT");
 for(int shape=0;shape<10;++shape){s.set(S::LfoShape,shape);s.set(S::LfoAmount,1);auto out=render(s,input);check(std::all_of(out.begin(),out.end(),[](float v){return std::isfinite(v);}),"all ten LFO waveforms remain finite");}
 s.prepare(48000);s.set(S::Mode,1);s.set(S::LfoAmount,0);s.set(S::EnvAmount,0);s.set(S::Spread,10);s.set(S::Wide,1);s.process(input.data(),input.data(),l.data(),r.data(),input.size());check(l!=r,"Wide inverts stereo Spread");
 s.prepare(48000);s.set(S::Spread,0);s.process(input.data(),input.data(),l.data(),r.data(),input.size());check(l==r,"Wide has no effect at zero Spread");
 s.set(S::Mode,0);s.set(S::Window,5);const auto shortWindow=s.latency();s.set(S::Window,170);check(s.latency()>shortWindow,"Window selects longer pitch analysis and reports its latency");
}
}
int main(){measured();invariants();std::printf("%s -- %d checks, %d failure(s)\n",failures?"FAIL":"PASS",checks,failures);return failures?1:0;}
