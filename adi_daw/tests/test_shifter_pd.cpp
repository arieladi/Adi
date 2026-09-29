// SPDX-License-Identifier: GPL-3.0-or-later
#include "juce/pd_engine.hpp"
#include <bit>
#include <cstdint>
#include "juce/pd_builtins.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <new>
#include <numbers>
#include <sstream>
#include <thread>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif
namespace { thread_local bool countAllocations = false; thread_local std::size_t allocations = 0; }
#if defined(_MSC_VER) && defined(_DEBUG)
int allocationHook(int kind, void*, std::size_t, int, long, const unsigned char*, int) {
    if(countAllocations && (kind==_HOOK_ALLOC || kind==_HOOK_REALLOC)) ++allocations;
    return 1;
}
#endif
void* operator new(std::size_t n) { if(countAllocations) ++allocations; if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc(); }
void* operator new[](std::size_t n) {return operator new(n);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
namespace {
int checks=0,failures=0;
void check(bool ok,const char* text){++checks;if(!ok){++failures;std::printf("FAIL %s\n",text);}}
double bin(const std::vector<float>& x,double hz) {
    std::complex<double> sum=0,phase=1,step=std::polar(1.,-2*std::numbers::pi*hz/48000);
    for(float v:x){sum+=static_cast<double>(v)*phase;phase*=step;}
    return 2*std::abs(sum)/static_cast<double>(x.size());
}
void wav(const std::vector<float>& x) {
    // Test artifact only: IEEE float mono, little endian, never an audio device.
    std::ofstream f("shifter-pd-render.wav",std::ios::binary);
    auto u16=[&](std::uint16_t v){for(unsigned i=0;i<2;++i)f.put(static_cast<char>((v>>(8*i))&255u));};
    auto u32=[&](std::uint32_t v){for(unsigned i=0;i<4;++i)f.put(static_cast<char>((v>>(8*i))&255u));};
    const auto bytes=static_cast<std::uint32_t>(x.size()*sizeof(float));
    f.write("RIFF",4);u32(36+bytes);f.write("WAVEfmt ",8);u32(16);u16(3);u16(1);u32(48000);u32(192000);u16(4);u16(32);
    f.write("data",4);u32(bytes);for(float v:x)u32(std::bit_cast<std::uint32_t>(v));f.flush();
    check(f.good(),"float WAV artifact written successfully");
}
void run() {
    using namespace adi;
    device::LibPdEngine engine(ADI_SHIFTER_PATCH_DIR,"Shifter.pd",2,2);engine.addSearchPath(ADI_PD_PATCH_DIR);
    device::PdLatencyReceiver latency;std::string error;
    const bool opened=engine.open(latency,error);check(opened,"Shifter top-level patch opens");if(!opened)return;
    std::ifstream f(std::string(ADI_SHIFTER_PATCH_DIR)+"/Shifter.pd");std::stringstream text;text<<f.rdbuf();
    const auto declarations=device::parsePdDeclarations(text.str());check(declarations.problems.empty(),"Shifter parameter declarations valid");
    engine.prepare(48000,64);engine.bindParameters(declarations);engine.requestLatencyReport();
    check(latency.latencySamples()>0,"Pitch reports intrinsic window latency");
    check(engine.sendParameter(1,1),"Mode parameter routes to external");
    check(engine.sendParameter(2,1),"Coarse parameter routes to external");
    std::array<float,64> l{},r{},ol{},orr{};std::array<const float*,2> in{l.data(),r.data()};std::array<float*,2> out{ol.data(),orr.data()};
    engine::NodeIo io;io.in=in.data();io.out=out.data();io.channels=2;io.frames=64;
    std::size_t position=0;
    auto block=[&]{for(std::size_t i=0;i<64;++i){l[i]=static_cast<float>(0.4*std::sin(2*std::numbers::pi*7000*static_cast<double>(position++)/48000));r[i]=l[i];}engine.process(io);};
    for(int i=0;i<750;++i)block();
    std::vector<float> audio(96000);bool stereo=true;float stereoError=0;
    allocations=0;countAllocations=true;
    for(std::size_t i=0;i<audio.size();i+=64){block();std::copy(ol.begin(),ol.end(),audio.begin()+static_cast<std::ptrdiff_t>(i));for(std::size_t c=0;c<64;++c){stereoError=std::max(stereoError,std::abs(ol[c]-orr[c]));stereo &= ol[c]==orr[c];}}
    countAllocations=false;
    check(allocations==0,"LibPdEngine Shifter processing allocates nothing");
    std::printf("METRIC stereo error %.12f\n",stereoError);check(stereo,"stereo channels agree with zero Spread");
    const double floor=bin(audio,1234);check(floor<1e-6,"far bin at floor BEFORE peak assertion");
    const double peak=bin(audio,8000);check(peak>0.35 && peak<0.45,"real patch shifts 7 kHz to 8 kHz");
    std::printf("METRIC LibPd Shifter peak %.9f far %.12f\n",peak,floor);
    wav(audio);
    check(engine.sendParameter(1,0) && engine.sendParameter(2,12),"select Pitch +12 semitones through host parameters");
    for(int i=0;i<750;++i)block();
    allocations=0;countAllocations=true;
    for(std::size_t i=0;i<audio.size();i+=64){block();std::copy(ol.begin(),ol.end(),audio.begin()+static_cast<std::ptrdiff_t>(i));}
    countAllocations=false;
    check(allocations==0,"Pitch FFT through LibPdEngine has zero C/CRT/C++ allocations");
    check(bin(audio,1234)<1e-6,"Pitch far bin checked first");
    check(bin(audio,14000)>0.01,"Pitch raises 7 kHz by an octave through real patch");

    for(int id=1;id<=33;++id)check(engine.sendParameter(id,0),"every declared control accepts a host update");
    block();
    const auto console=engine.consoleLines();for(const auto& line:console)std::printf("PD %s\n",line.c_str());
    check(console.empty(),"console hook clean through creation, parameter messages and process");
}
}
int main(){
#if defined(_MSC_VER) && defined(_DEBUG)
    _CrtSetAllocHook(allocationHook);
#endif
    run();std::printf("%s -- %d checks, %d failure(s)\n",failures?"FAIL":"PASS",checks,failures);return failures?1:0;
}
