// SPDX-License-Identifier: GPL-3.0-or-later
#include "juce/pd_engine.hpp"
#include "adi/pd_builtins/colorcab_sample.hpp"
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
using namespace adi;
constexpr double pi = std::numbers::pi;
int checks = 0, failures = 0;
void check(bool ok, const char* name) { ++checks; if(!ok){++failures;std::printf("FAIL %s\n",name);} }
struct Audio {
    std::array<float,64> left{},right{},outLeft{},outRight{};
    std::array<const float*,2> in{left.data(),right.data()};
    std::array<float*,2> out{outLeft.data(),outRight.data()};
    engine::NodeIo io;
    std::size_t position=0;
    Audio(){io.in=in.data();io.out=out.data();io.channels=2;io.frames=64;}
    void render(device::LibPdEngine& e) {
        for(std::size_t i=0;i<left.size();++i){
            left[i]=static_cast<float>(0.2*std::sin(2*pi*88*static_cast<double>(position++)/8192));
            right[i]=left[i]*0.5f;
        }
        e.process(io);
    }
};
double bin(const std::vector<float>& x, double k) {
    std::complex<double> sum{};
    for(std::size_t i=0;i<x.size();++i)
        sum+=static_cast<double>(x[i])*std::polar(1.,-2*pi*k*static_cast<double>(i)/static_cast<double>(x.size()));
    return 2*std::abs(sum)/static_cast<double>(x.size());
}
std::unique_ptr<device::PdSampleBuffer> sample(bool silence=false) {
    auto s=std::make_unique<device::PdSampleBuffer>();s->sampleRate=48000;s->frames=2048;s->channels=1;
    s->interleaved.resize(2048);s->blake3="fixture-source-hash";
    if(!silence)for(std::size_t i=0;i<s->interleaved.size();++i)
        s->interleaved[i]=static_cast<float>(std::exp(-static_cast<double>(i)/60)*std::cos(2*pi*1800*static_cast<double>(i)/48000));
    dsp::ColorCabOptions options; options.gamma=1; options.smoothingOctaves=0.5;
    return device::prepareColorCabSample(std::move(s),options);
}
void deviceTest(const char* file, bool cab) {
    device::LibPdEngine e(ADI_COLORBASS_PATCH_DIR,file,2,2);
    e.addSearchPath(ADI_PD_PATCH_DIR);
    device::PdLatencyReceiver latency; std::string error;
    check(e.open(latency,error),"top-level device opens");
    std::ifstream stream(std::string(ADI_COLORBASS_PATCH_DIR)+"/"+file);std::stringstream text;text<<stream.rdbuf();
    const auto declarations=device::parsePdDeclarations(text.str());
    check(declarations.problems.empty(),"device declarations parse cleanly");
    e.prepare(48000,64);e.bindParameters(declarations);e.bindSamples(declarations);
    (void)latency.report(99);
    e.requestLatencyReport();
    check(latency.latencySamples()==0,"device reports zero intrinsic latency after host query");
    double expected=0;
    if(cab){
        auto data=sample();const auto& kernel=dynamic_cast<const device::PdColorCabSample&>(*data).design.kernel;
        std::complex<double> response{};
        for(std::size_t i=0;i<kernel.size;++i)response+=kernel.taps[i]*std::polar(1.,-2*pi*88*static_cast<double>(i)/8192);
        expected=0.2*std::abs(response);
        check(e.publishSample(1,std::move(data)),"publish declared immutable sample slot");
        check(!e.publishSample(999,sample()),"undeclared slot refused");
    }else check(e.sendParameter(3,0.05f),"Decay reaches the external through adi.param");
    Audio audio;
    for(int i=0;i<1500;++i)audio.render(e);
    std::vector<float> output;output.reserve(8192);
    bool stereo=true;
    allocations=0;countAllocations=true;
    for(int i=0;i<128;++i){audio.render(e);output.insert(output.end(),audio.outLeft.begin(),audio.outLeft.end());
        for(std::size_t j=0;j<64;++j)stereo &= std::abs(audio.outRight[j]-audio.outLeft[j]*0.5f)<1e-6f;}
    countAllocations=false;
    check(allocations==0,"zero C++ allocations during LibPdEngine process and handoff");
    check(stereo,"stereo channels retain independent amplitudes");
    const double floor=bin(output,777);
    check(floor<1e-7,"far bin is at the floor BEFORE trusting the sine peak");
    const double peak=bin(output,88);
    std::printf("METRIC %s peak %.9f far bin %.12f\n",file,peak,floor);
    check(peak>1e-5 && std::abs(peak-0.2)>1e-3,"real external makes filtered sound rather than bypass or silence");
    if(cab){
        check(std::abs(peak-expected)<1e-5,"published sample produces the independently predicted FIR response");
        std::atomic<bool> stop{false},ready{false};std::atomic<std::size_t> rendered{0},rtAlloc{0};std::atomic<bool> finite{true};
        std::thread renderer([&]{Audio a;countAllocations=true;allocations=0;ready.store(true);
            while(!stop.load()){a.render(e);for(float v:a.outLeft)if(!std::isfinite(v))finite.store(false);++rendered;}
            countAllocations=false;rtAlloc.store(allocations);});
        while(!ready.load())std::this_thread::yield();
        bool published=true;
        for(int i=0;i<24;++i){published &= e.publishSample(1,sample(i%2==0));e.collectSamples();}
        stop.store(true);renderer.join();
        check(published && rendered.load()>24 && finite.load(),"new samples publish safely while blocks render");
        check(rtAlloc.load()==0,"concurrent sample replacement allocates nothing on audio thread");
        check(e.publishSample(1,sample(true)),"publish silent replacement");
        for(int i=0;i<100;++i)audio.render(e);
        check(std::all_of(audio.outLeft.begin(),audio.outLeft.end(),[](float x){return x==0;}),"silent published sample actually silences the wet device");
        e.collectSamples();
    }
    const auto console=e.consoleLines();
    for(const auto& line:console)std::printf("PD: %s\n",line.c_str());
    check(console.empty(),"Pd console hook stays clean through open, parameters and rendering");
}
}
int main(){
#if defined(_MSC_VER) && defined(_DEBUG)
    _CrtSetAllocHook(allocationHook);
#endif
    deviceTest("Chord Comb.pd",false);deviceTest("Color Cab.pd",true);
    std::printf("%s -- %d checks, %d failure(s)\n",failures?"FAIL":"PASS",checks,failures);return failures?1:0;}
