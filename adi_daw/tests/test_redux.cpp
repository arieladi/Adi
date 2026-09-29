// SPDX-License-Identifier: MIT
#include "adi/dsp/redux.hpp"
#include <algorithm>
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
using adi::dsp::Redux;
int checks=0,failures=0;
void check(bool ok,const char* what){++checks;if(!ok){++failures;std::printf("FAIL %s\n",what);}}
constexpr double pi=std::numbers::pi;
std::vector<float> tone(double hz,double amplitude=0.4) {
    std::vector<float> x(96000);
    for(std::size_t i=0;i<x.size();++i)x[i]=static_cast<float>(amplitude*std::sin(2*pi*hz*static_cast<double>(i)/48000));
    return x;
}
std::vector<float> render(Redux& c,const std::vector<float>& in,std::size_t block=64) {
    std::vector<float> out(in.size()),right(in.size());
    for(std::size_t i=0;i<in.size();i+=block)c.process(in.data()+i,in.data()+i,out.data()+i,right.data()+i,std::min(block,in.size()-i));
    return out;
}
double bin(const std::vector<float>& x,double hz) {
    std::complex<double> sum=0,phase=1,step=std::polar(1.,-2*pi*hz/48000);
    for(std::size_t i=48000;i<x.size();++i){sum+=static_cast<double>(x[i])*phase;phase*=step;}
    return 2*std::abs(sum)/48000;
}
double error(const std::vector<float>& a,const std::vector<float>& b) {
    double sum=0;for(std::size_t i=48000;i<a.size();++i)sum+=std::pow(static_cast<double>(a[i])-b[i],2);return sum/48000;
}
void measurements() {
    Redux c;c.prepare(48000);c.setRate(8000);c.setBits(24);
    auto input=tone(7000);auto alias=render(c,input);
    check(bin(alias,1234)<1e-7,"alias measurement far bin first");
    check(bin(alias,1000)>0.35,"7 kHz folds to 1 kHz at an 8 kHz clock");
    c.prepare(48000);c.setRate(7500);input=tone(6500);auto fractional=render(c,input);
    check(bin(fractional,1000)>0.3,"fractional 6.4-sample hold folds 6.5 kHz to 1 kHz");
    c.prepare(48000);c.setRate(8000);c.setPre(1);input=tone(7000);auto pre=render(c,input);
    check(bin(pre,1000)<bin(alias,1000)*0.4,"Pre limits alias input bandwidth");
    c.prepare(48000);c.setPre(0);c.setPost(1);input=tone(1000);auto post=render(c,input);
    c.prepare(48000);c.setPost(0);auto raw=render(c,input);
    check(bin(post,7000)<bin(raw,7000)*0.4,"Post suppresses the 7 kHz image");
    c.prepare(48000);c.setPost(1);c.setOctave(-2);auto low=render(c,input);
    check(bin(low,1000)<bin(post,1000)*0.7,"negative Octave lowers post cutoff");
    c.prepare(48000);c.setPost(0);c.setRate(48000);c.setBits(1);auto square=render(c,input);
    check(std::all_of(square.begin(),square.end(),[](float v){return v==0 || v==0.5f || v==-0.5f;}),"one bit gives two nonzero levels");
    c.prepare(48000);c.setBits(4);input=tone(1000,0.005);auto linear=render(c,input);
    c.prepare(48000);c.setShape(100);auto shaped=render(c,input);
    check(error(shaped,input)<error(linear,input)*0.02,"Shape improves quiet-signal resolution");
    c.prepare(48000);c.setShape(0);c.setDcShift(1);auto offset=render(c,input);
    check(offset!=linear,"DC Shift changes quantization before reduction");
    c.prepare(48000);c.setMix(0);auto dry=render(c,input);check(dry==input,"Dry/Wet zero is exact bypass");
    c.prepare(48000);c.setMix(100);auto wet=render(c,input);
    c.prepare(48000);c.setMix(50);auto half=render(c,input);
    check(error(half,input)<error(wet,input)*0.251,"Dry/Wet interpolates amplitudes");
    std::printf("METRIC alias %.8f fractional %.8f pre %.8f post-image %.8f\n",bin(alias,1000),bin(fractional,1000),bin(pre,1000),bin(post,7000));
}
void continuity() {
    const auto input=tone(7311);std::vector<float> expected;
    for(std::size_t block:{32u,64u,128u,256u,512u,1024u,2048u,4096u}) {
        Redux c;c.prepare(48000);c.setRate(7333);c.setJitter(70);c.setShape(42);c.setBits(7.5);c.setPre(1);c.setPost(1);
        auto out=render(c,input,block);if(expected.empty())expected=out;
        check(out==expected,"all block partitions are sample-identical with jitter and filters");
    }
    Redux c;c.prepare(48000);c.setRate(8000);c.setJitter(80);c.setBits(24);
    std::vector<float> left(input.size()),right(input.size());
    allocations=0;counting=true;c.process(input.data(),input.data(),left.data(),right.data(),input.size());counting=false;
    check(allocations==0,"zero process allocations");check(error(left,right)>0.01,"Jitter creates stereo differences");
    const double width=error(left,right);c.prepare(48000);c.setPre(1);
    c.process(input.data(),input.data(),left.data(),right.data(),input.size());
    check(error(left,right)<width*0.4,"Pre reduces jitter width on high-frequency material");
    c.prepare(48000);c.setJitter(0);c.process(input.data(),input.data(),left.data(),right.data(),input.size());
    check(left==right,"zero jitter keeps stereo clocks locked");
    c.setRate(std::numeric_limits<double>::quiet_NaN());c.setBits(std::numeric_limits<double>::infinity());
    c.process(input.data(),input.data(),left.data(),right.data(),input.size());
    check(std::all_of(left.begin(),left.end(),[](float v){return std::isfinite(v);}),"invalid parameter messages cannot poison audio");
}
}
int main(){measurements();continuity();std::printf("%s -- %d checks, %d failure(s)\n",failures?"FAIL":"PASS",checks,failures);return failures?1:0;}
