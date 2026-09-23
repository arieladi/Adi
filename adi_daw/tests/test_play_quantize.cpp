// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/engine/play_quantize.hpp"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <thread>
namespace { std::atomic<long> allocations{0}; std::atomic<bool> counting{false}; }
void* operator new(std::size_t n) {
    if(counting.load(std::memory_order_relaxed)) allocations.fetch_add(1,std::memory_order_relaxed);
    if(void* p=std::malloc(n?n:1))return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) {return operator new(n);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
namespace {
using namespace adi::engine;
int checks=0, failures=0;
void check(bool ok,const char* name) {++checks;if(!ok){++failures;std::printf("FAIL %s\n",name);}}
constexpr std::array<PlayTempoPoint,1> steady{{{0,120}}};
void basic() {
    check(playQuantizeRelease(0,steady,48000,240)==0,"origin is a grid line");
    check(playQuantizeRelease(6000,steady,48000,240)==6000,"sixteenth exact line");
    check(playQuantizeRelease(1,steady,48000,240)==6000,"sixteenth next line");
    check(playQuantizeRelease(5999,steady,48000,240)==6000,"sixteenth just before");
    check(playQuantizeRelease(6001,steady,48000,240)==12000,"sixteenth just after");
    check(playQuantizeRelease(1,steady,48000,160)==4000,"triplet next line");
    check(playQuantizeRelease(4000,steady,48000,160)==4000,"triplet exact line");
    check(playQuantizeRelease(4001,steady,48000,160)==8000,"triplet just after");
    check(playQuantizeRelease(1,steady,44100,240)==5513,"fractional line rounds up");
    check(playQuantizeRelease(5513,steady,44100,240)==5513,"rounded fractional line is now");
    constexpr std::array<PlayTempoPoint,3> map{{{0,120},{3000,60},{15000,240}}};
    check(playQuantizeRelease(2000,map,48000,240)==9000,"tempo change before next line");
    check(playQuantizeRelease(3000,map,48000,240)==9000,"event at tempo boundary");
    check(playQuantizeRelease(9000,map,48000,240)==9000,"phase continues through change");
    check(playQuantizeRelease(9001,map,48000,240)==16500,"second tempo change before next line");
    check(playQuantizeRelease(16500,map,48000,240)==16500,"post-change exact line");
    constexpr std::array<PlayTempoPoint,3> multiple{{{0,120},{1000,60},{2000,120}}};
    check(playQuantizeRelease(1,multiple,48000,240)==6500,"two changes before next line");
    check(playQuantizeRelease(6099,steady,48000,240,100)==6099,"forgiveness inside");
    check(playQuantizeRelease(6100,steady,48000,240,100)==6100,"forgiveness inclusive edge");
    check(playQuantizeRelease(6101,steady,48000,240,100)==12000,"forgiveness just past edge");
    check(playQuantizeRelease(8999,map,48000,240,100)==9000,"forgiveness never early");
    check(playQuantizeRelease(9100,map,48000,240,100)==9100,"forgiveness after changed grid");
}
void blocksAndAllocations() {
    constexpr std::array<PlayTempoPoint,3> map{{{0,120},{3000,60},{15000,240}}};
    constexpr std::array<std::int64_t,9> events{0,1,2999,3000,8999,9000,9100,9101,17000};
    constexpr std::array<std::int64_t,9> expected{0,1,9000,9000,9000,9000,9100,16500,19500};
    for(const std::int64_t block:{32,64,128,256,512,1024,2048,4096}) {
        bool same=true;
        for(std::int64_t start=0;start<20000;start+=block) {
            for(std::size_t i=0;i<events.size();++i) if(events[i]>=start && events[i]<start+block) {
                const auto local=events[i]-start;
                same &= playQuantizeRelease(start+local,map,48000,240,100)==expected[i];
            }
        }
        check(same,"absolute releases invariant across block sizes");
    }
    allocations.store(0);counting.store(true);bool valid=true;
    for(std::int64_t i=0;i<20000;++i)valid &= playQuantizeRelease(i,map,48000,240,100).has_value();
    counting.store(false);check(valid && allocations.load()==0,"zero allocations across map and forgiveness paths");
    // Shared immutable inputs can be evaluated on independent threads.
    std::array<bool,2> ok{};
    auto run=[&](std::size_t slot){bool good=true;for(int i=0;i<1000;++i)good &= playQuantizeRelease(9101,map,48000,240,100)==16500;ok[slot]=good;};
    std::thread a(run,0),b(run,1);a.join();b.join();check(ok[0]&&ok[1],"concurrent pure calls");
}
void invalid() {
    check(!playQuantizeRelease(-1,steady,48000,240),"negative event rejected");
    check(!playQuantizeRelease(1,{},48000,240),"empty map rejected");
    check(!playQuantizeRelease(1,steady,0,240),"zero rate rejected");
    check(!playQuantizeRelease(1,steady,48000,0),"zero grid rejected");
    check(!playQuantizeRelease(1,steady,48000,240,-1),"negative forgiveness rejected");
    const std::array<PlayTempoPoint,2> bad{{{0,120},{0,60}}};
    check(!playQuantizeRelease(1,bad,48000,240),"unordered map rejected");
    const std::array<PlayTempoPoint,1> nan{{{0,std::numeric_limits<double>::quiet_NaN()}}};
    check(!playQuantizeRelease(1,nan,48000,240),"nonfinite tempo rejected");
    check(!playQuantizeRelease(1,steady,48000,std::numeric_limits<double>::infinity()),"nonfinite grid rejected");
    check(!playQuantizeRelease(1,steady,48000,1e300),"unrepresentable release rejected");
}
}
int main() {basic();blocksAndAllocations();invalid();std::printf("%s -- %d checks, %d failure(s)\n",failures?"FAIL":"PASS",checks,failures);return failures?1:0;}
