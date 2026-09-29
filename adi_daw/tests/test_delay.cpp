// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/live_delay.hpp"
#include <algorithm>
#include <cmath>
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
using D=adi::dsp::LiveDelay;int checks=0,failures=0;
void check(bool v,const char* s){++checks;if(!v){++failures;std::printf("FAIL %s\n",s);}}
void clean(D& d){d.set(D::Feedback,0);d.set(D::Mix,100);d.set(D::Transition,2);d.prepare(48000);}
std::vector<float> render(D& d,const std::vector<float>& input,std::size_t block=64){std::vector<float> l(input.size()),r(input.size());for(std::size_t i=0;i<input.size();i+=block)d.process(input.data()+i,input.data()+i,l.data()+i,r.data()+i,std::min(block,input.size()-i));return l;}
void timing(){for(double ms:{1.,10.,125.,1000.,5000.}){D d;clean(d);d.set(D::LeftMs,ms);std::vector<float> in(240128);in[0]=1;const auto out=render(d,in);const auto index=static_cast<std::size_t>(ms*48);check(out[index]==1,"millisecond impulse arrives at exact sample");check(std::count(out.begin(),out.end(),1.f)==1,"no duplicate impulse");}
 for(double bpm:{60.,90.,120.,180.}){D d;clean(d);d.set(D::LeftSync,1);d.set(D::LeftNotes,4);d.tempo(bpm);std::vector<float> in(49000);in[0]=1;const auto out=render(d,in);check(out[static_cast<std::size_t>(48000*60/bpm)]==1,"four sixteenths equal one quarter at known tempo");}
 D d;clean(d);d.set(D::LeftSync,1);d.set(D::LeftOffset,10);std::vector<float> in(30000),right(in.size());in[0]=1;auto out=render(d,in);check(out[26400]==1,"positive offset extends sync time");
 d.prepare(48000);d.set(D::RightMs,20);d.set(D::RightSync,0);d.set(D::LeftOffset,0);out=render(d,in);check(out[960]==1,"Stereo Link follows right-channel edits");
}
void feedback(){D d;clean(d);d.set(D::LeftMs,1);d.set(D::Feedback,50);d.set(D::PingPong,1);std::vector<float> in(512),zero(512),l(512),r(512);in[0]=1;d.process(in.data(),zero.data(),l.data(),r.data(),512);check(l[48]==1 && r[96]==0.5f && l[144]==0.25f,"Ping Pong alternates feedback between channels");
 d.prepare(48000);d.set(D::PingPong,0);d.process(in.data(),zero.data(),l.data(),r.data(),512);check(r==zero,"ordinary feedback stays independent");
 d.prepare(48000);d.set(D::LeftMs,2);d.process(in.data(),zero.data(),l.data(),r.data(),32);d.set(D::Freeze,1);std::fill(in.begin(),in.end(),0.25f);d.process(in.data(),in.data(),l.data(),r.data(),512);check(l[64]==1 && l[160]==1 && l[256]==1,"Freeze repeats without loss");check(r==zero,"Freeze rejects new input");
}
void transitions(){
 std::vector<float> input(16384);for(std::size_t i=0;i<input.size();++i)input[i]=static_cast<float>(0.2*std::sin(2*std::numbers::pi*733*static_cast<double>(i)/48000));
 double steps[3]{};std::vector<float> outputs[3];
 for(int mode=0;mode<3;++mode){D d;clean(d);d.set(D::Transition,mode);d.set(D::LeftMs,20);outputs[mode].resize(input.size());std::vector<float> right(input.size());d.process(input.data(),input.data(),outputs[mode].data(),right.data(),8192);d.set(D::LeftMs,25);d.process(input.data()+8192,input.data()+8192,outputs[mode].data()+8192,right.data()+8192,input.size()-8192);for(std::size_t i=8192;i<9192;++i)steps[mode]=std::max(steps[mode],std::abs(static_cast<double>(outputs[mode][i])-outputs[mode][i-1]));}
 std::printf("METRIC time edit max steps Repitch %.6f Fade %.6f Jump %.6f\n",steps[0],steps[1],steps[2]);
 check(steps[2]>steps[1]*3,"Fade reduces discontinuity compared with Jump");check(outputs[0]!=outputs[1] && outputs[1]!=outputs[2],"three time-change modes have distinct audible behavior");
 D d;clean(d);d.set(D::LeftMs,10.123);auto linear=render(d,input);d.prepare(48000);d.set(D::HiQuality,1);auto cubic=render(d,input);check(linear!=cubic,"HiQuality selects different fractional interpolation");
 d.prepare(48000);d.set(D::Filter,1);d.set(D::Freq,8000);d.set(D::Width,1);auto filtered=render(d,input);double a=0,b=0;for(std::size_t i=8192;i<input.size();++i){a+=linear[i]*linear[i];b+=filtered[i]*filtered[i];}check(b<a*0.05,"bandpass rejects frequencies outside its passband");
}
void partition(){std::vector<float> in(32768);for(std::size_t i=0;i<in.size();++i)in[i]=static_cast<float>(0.2*std::sin(2*std::numbers::pi*733*static_cast<double>(i)/48000));
 for(int mode=0;mode<3;++mode){std::vector<float> expected;for(std::size_t block:{32u,64u,128u,256u,512u,1024u,2048u,4096u}){D d;d.prepare(48000);d.set(D::LeftMs,10.5);d.set(D::Transition,mode);d.set(D::LfoDelay,20);d.set(D::Filter,1);d.set(D::LfoFilter,10);d.set(D::HiQuality,1);auto out=render(d,in,block);if(expected.empty())expected=out;check(out==expected,"all transition modes are block-independent");}}
 D d;clean(d);d.set(D::LeftMs,2);std::vector<float> l(in.size()),r(in.size());counting=true;allocations=0;d.process(in.data(),in.data(),l.data(),r.data(),in.size());counting=false;check(allocations==0,"delay process allocates nothing");
 for(int wave=0;wave<7;++wave)for(int rate=0;rate<6;++rate){d.set(D::Wave,wave);d.set(D::RateMode,rate);d.set(D::LfoDelay,100);d.set(D::LfoFilter,100);d.set(D::Filter,1);auto out=render(d,in);check(std::all_of(out.begin(),out.end(),[](float v){return std::isfinite(v);}),"all LFO waveform/rate combinations remain finite");}
}
}
int main(){timing();feedback();transitions();partition();std::printf("%s -- %d checks, %d failure(s)\n",failures?"FAIL":"PASS",checks,failures);return failures?1:0;}

