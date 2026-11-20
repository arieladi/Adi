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
 s.set(S::Mode,0);s.set(S::Window,10);check(s.latency()==0,"Pitch reports no latency at the shortest Window, as Live 12.4");s.set(S::Window,350);check(s.latency()==0,"nor at the longest");
}
// DEV-22, Live 12.4 as measured (the director's research, DV-06; Shifter's
// default preset, Pitch, Dry/Wet 100 %, an impulse of 0.5, 48 kHz, offline
// export): Status Bar latency 0 at every Window; at 0 st the output is the
// impulse alone, exactly half the Window late, with Delay Compensation on or
// off. Window runs 10..350 ms, default 80 ms.
void liveUnison(){
 for(const auto& [ms,gap]:{std::pair{10.,240},std::pair{80.,1920},std::pair{350.,8400}}){
  S s;s.prepare(48000);s.set(S::Window,ms);
  std::vector<float> in(1000+static_cast<std::size_t>(gap)+1000,0.f);in[1000]=0.5f;
  const auto out=render(s,in);
  bool clean=true;for(std::size_t i=0;i<out.size();++i)clean&=out[i]==(i==1000+static_cast<std::size_t>(gap)?0.5f:0.f);
  std::printf("METRIC Shifter unison Window %.0f ms: delay %d samples, clean copy %s\n",ms,gap,clean?"yes":"no");
  check(clean && s.latency()==0,"at unison the output is the input alone, half the Window late (Live 12.4: 240, 1920, 8400 samples)");
 }
 check(S::ranges[S::Window].low==10 && S::ranges[S::Window].high==350 && S::ranges[S::Window].initial==80,"Window runs 10..350 ms, default 80 ms (Live 12.4)");
 // Away from unison the shape depends on the sweep's phase when the sound
 // arrives, which Live's export does not give; it is printed, not claimed.
 for(const double ms:{10.,80.,350.}){S p;p.prepare(48000);p.set(S::Window,ms);p.set(S::Coarse,7);
  std::vector<float> in(48000+40000,0.f);in[48000]=0.5f;const auto out=render(p,in);
  std::size_t onset=0,peak=0;double energy=0,moment=0;for(std::size_t i=48000;i<out.size();++i){const double v=out[i];if(!onset&&std::abs(v)>1e-6)onset=i-48000;if(std::abs(v)>std::abs(out[peak]))peak=i;energy+=v*v;moment+=static_cast<double>(i-48000)*v*v;}
  std::printf("METRIC Shifter +7 st Window %.0f ms: onset +%zu, peak +%zu (%.3f), centre +%.1f\n",ms,onset,peak-48000,static_cast<double>(out[peak]),moment/energy);}
}
}
// DF-E37 (b): after any pitch move, unison must come back clean. The taps'
// phase moved only while the ratio was not 1 and was reset only in prepare(),
// so +7 st for 1,926 samples and back left two half-gain copies (about 0.25 at
// 960 and 2880 for an impulse of 0.5) and nulls every 25 Hz above 12.5 Hz, for
// good. At unison the taps now crossfade to the clean Window/2 read over 20 ms
// (Pitch::kRestSeconds); after that the impulse comes out once, at 1920.
void unisonAfterAMove(){
 S s;s.prepare(48000);s.set(S::Window,80);
 std::vector<float> silence(1926,0.f),l(1926),r(1926);
 s.set(S::Coarse,7);s.process(silence.data(),silence.data(),l.data(),r.data(),silence.size());
 s.set(S::Coarse,0);
 std::vector<float> in(48000,0.f);in[2000]=0.5f;   // 2000 samples: past the 20 ms return
 const auto out=render(s,in);
 std::size_t peaks=0,at=0;for(std::size_t i=0;i<out.size();++i)if(std::abs(out[i])>1e-6f){++peaks;at=i;}
 std::printf("METRIC Shifter back at unison: %zu nonzero samples, last at +%zu, value %.6f\n",peaks,at>=2000?at-2000:0,at<out.size()?static_cast<double>(out[at]):0.);
 check(peaks==1 && at==2000+1920 && out[at]==0.5f,"back at unison after +7 st: the impulse once, 1920 late, at 0.5 (no lasting comb)");
 // And the return itself does not click: a 100 Hz tone through +7 st and
 // back, its sample-to-sample step never above the tone's own steepest.
 S t;t.prepare(48000);t.set(S::Window,80);
 auto x=tone(100,48000);std::vector<float> a(48000),b(48000);
 t.set(S::Coarse,7);t.process(x.data(),x.data(),a.data(),b.data(),24000);
 t.set(S::Coarse,0);t.process(x.data()+24000,x.data()+24000,a.data()+24000,b.data()+24000,24000);
 double step=0;for(std::size_t i=24001;i<48000;++i)step=std::max(step,static_cast<double>(std::abs(a[i]-a[i-1])));
 const double steepest=0.3*2*std::numbers::pi*100/48000;
 std::printf("METRIC Shifter return to unison: largest step %.5f (tone's own %.5f)\n",step,steepest);
 check(step<=steepest*1.5,"the return to unison does not click");
}
int main(){measured();invariants();liveUnison();unisonAfterAMove();std::printf("%s -- %d checks, %d failure(s)\n",failures?"FAIL":"PASS",checks,failures);return failures?1:0;}
