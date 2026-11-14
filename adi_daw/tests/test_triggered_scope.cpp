// SPDX-License-Identifier: GPL-3.0-only
#include "adi/dsp/triggered_scope.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <vector>

bool scopeFastProcess(adi::dsp::TriggeredScope &, std::span<const float>, std::int64_t) noexcept;

namespace {
using Scope = adi::dsp::TriggeredScope;
int checks = 0, failures = 0;
bool processing = false;
int failAfter = -1;
std::size_t allocations = 0, frees = 0;
void check(bool value, const char *name) {
    ++checks; if (!value) { ++failures; std::printf("FAIL %s\n", name); }
}
bool same(Scope::Capture a, Scope::Capture b) {
    return a.epoch == b.epoch && a.serial == b.serial && a.triggerSequence == b.triggerSequence &&
        a.columns.size() == b.columns.size() && std::equal(a.columns.begin(), a.columns.end(), b.columns.begin());
}
void basicAndOrdering() {
    Scope s;
    check(!s.prepared() && !s.settings() && s.current().columns.empty() && s.previous().columns.empty(), "unprepared is empty");
    check(!s.processChecked({}, 0), "unprepared process refuses");
    s.reset();
    auto settings = Scope::Settings{}; settings.columns = 3; settings.columnsPerSample = 1;
    check(s.prepare(settings) && s.prepared() && *s.settings() == settings, "prepare explicit source-independent dimensions");
    const std::array<float, 4> input{.1f, .2f, -.3f, .4f};
    processing = true;
    const bool accepted = s.processChecked(input, 100);
    processing = false;
    check(accepted && s.current().triggerSequence == 103 && s.current().columns.size() == 1 &&
          s.current().columns[0] == Scope::Column{.4f, .4f, 103, 103}, "free starts next trace on sample AFTER full capture");
    const auto previous = s.previous();
    check(previous.serial == 1 && previous.triggerSequence == -1 && previous.columns.size() == 3 &&
          previous.columns[0] == Scope::Column{.1f,.1f,100,100} &&
          previous.columns[2] == Scope::Column{-.3f,-.3f,102,102}, "previous capture excludes current trigger sample and retains chronological ranges");
    settings.trigger = Scope::Trigger::Rising; settings.threshold = 1; settings.columns = 8; settings.columnsPerSample = .25;
    check(s.prepare(settings), "decimation prepares");
    const std::array<float, 6> extrema{0, .7f, -.8f, .4f, .2f, -.1f};
    check(s.processChecked(extrema, 0), "decimation accepts contiguous input");
    const auto c = s.current();
    check(c.columns.size() == 2 && c.columns[0] == Scope::Column{0,0,0,0} &&
          c.columns[1] == Scope::Column{.7f,-.8f,1,3}, "strict extrema retain final extremum order and inclusive sample ranges");
}
void triggersAndHoldoff() {
    for (auto type : {Scope::Trigger::Rising, Scope::Trigger::Falling}) {
        Scope s; Scope::Settings settings;
        settings.columns = 16; settings.columnsPerSample = 1; settings.trigger = type; settings.holdoffSamples = 4;
        check(s.prepare(settings), "edge trigger prepares");
        const std::array<float, 7> rising{-.5f, 0, -.5f, 0, 0, -.5f, .5f};
        std::array<float, 7> data = rising;
        if (type == Scope::Trigger::Falling) for (auto &v : data) v = -v;
        check(s.processChecked(data, 0), "edge trigger accepts");
        check(s.current().triggerSequence == 3 && s.current().columns.size() == 4 && s.previous().columns.size() == 3,
              "increment-before-test holdoff rejects early edge and accepts equality, equal previous is not a crossing");
    }
    Scope s; Scope::Settings settings; settings.trigger = Scope::Trigger::Internal;
    settings.internalCyclesPerSample = .25; settings.holdoffSamples = 10000;
    settings.columnsPerSample = 1; settings.columns = 16;
    check(s.prepare(settings), "internal trigger prepares");
    const std::array<float,8> a{.1f,.2f,.3f,.4f,.5f,.6f,.7f,.8f};
    check(s.processChecked(a, 0) && s.current().triggerSequence == 7 && s.previous().triggerSequence == 3 &&
          s.previous().columns.size() == 4 && s.previous().columns[0].first == .4f &&
          s.previous().columns.back().lastSequence == 6, "internal phase survives retrigger and ignores edge holdoff");
    settings.trigger = Scope::Trigger::Free; settings.columns = 2;
    check(s.prepare(settings) && s.processChecked(a, 0) && s.current().triggerSequence == 6,
          "free trigger ignores edge holdoff");
    settings.trigger = Scope::Trigger::Rising; settings.columns = 16; settings.holdoffSamples = 10000;
    check(s.prepare(settings), "long holdoff prepares");
    std::vector<float> quiet(12000, -.5f);
    check(s.processChecked(quiet, 0), "holdoff saturates during long acquisition");
    const std::array<float,1> edge{0};
    check(s.processChecked(edge, 12000) && s.current().triggerSequence == 12000, "saturated holdoff admits next inclusive crossing");
}
void partitionAndDc() {
    std::vector<float> signal(20003);
    for (std::size_t i = 0; i < signal.size(); ++i)
        signal[i] = static_cast<float>(.3 + .4 * std::sin(static_cast<double>(i)*.137));
    for (auto trigger : {Scope::Trigger::Free, Scope::Trigger::Rising, Scope::Trigger::Falling, Scope::Trigger::Internal})
      for (bool dc : {false, true})
        for (double speed : {.005, .3, 1., 31.622776601683793}) {
            Scope a,b; Scope::Settings settings; settings.columns = 31;
            settings.trigger = trigger; settings.removeDc = dc; settings.columnsPerSample = speed;
            settings.internalCyclesPerSample = .01; settings.holdoffSamples = 103;
            check(a.prepare(settings) && b.prepare(settings), "partition cohort prepares");
            bool ok = a.processChecked(signal, 80);
            for (std::size_t offset=0; offset<signal.size();) {
                const auto count = std::min<std::size_t>(17, signal.size()-offset);
                processing = true;
                ok = b.processTrustedFinite(std::span(signal).subspan(offset,count),80+static_cast<std::int64_t>(offset)) && ok;
                processing = false;
                offset += count;
            }
            check(ok && same(a.current(),b.current()) && same(a.previous(),b.previous()), "sample chronology is independent of source callback partitions");
        }
    Scope s; Scope::Settings settings; settings.trigger = Scope::Trigger::Rising;
    settings.threshold=1; settings.columns=8192; settings.columnsPerSample=1; settings.removeDc=true;
    check(s.prepare(settings), "DC measurement prepares");
    std::array<float,2000> dc; dc.fill(.5f);
    check(s.processChecked(dc,0), "DC input accepts");
    check(s.current().columns.front().first == .5f && std::abs(s.current().columns.back().first) < .00002f,
          "DC state attenuates constant input without modifying input");
    check(std::all_of(dc.begin(),dc.end(),[](float x){return x==.5f;}), "scope never writes input");
}
void refusalAndLifecycle() {
    Scope s, reference; Scope::Settings settings;
    settings.trigger=Scope::Trigger::Rising; settings.columnsPerSample=1;
    const std::array<float,4> warm{-.5f,.3f,-.2f,.4f};
    check(s.prepare(settings) && reference.prepare(settings) && s.processChecked(warm,0) && reference.processChecked(warm,0), "refusal fixtures warm");
    for (int fault=0;fault<3;++fault) {
        failAfter=fault; const bool result=s.prepare(settings); failAfter=-1;
        check(!result && *s.settings()==settings, "each prepare allocation failure retains old configuration");
        const std::array<float,1> next{-.1f};
        check(s.processChecked(next,4+fault) && reference.processChecked(next,4+fault) &&
              same(s.current(),reference.current()) && same(s.previous(),reference.previous()), "failed preparation preserves exact continuation");
    }
    const auto old=s.current();
    const std::array<float,2> invalid{0,std::numeric_limits<float>::quiet_NaN()};
    check(!s.processChecked(invalid,7) && same(old,s.current()), "nonfinite last sample refuses entire span before mutation");
    const std::array<float,1> next{.2f};
    check(!s.processChecked(next,8) && !s.processChecked(next,-1) && !s.processChecked(next,std::numeric_limits<std::int64_t>::max()),
          "gap negative and overflowing sample sequence refuse");
    check(s.processChecked(next,7) && reference.processChecked(next,7) && same(s.current(),reference.current()), "corrected retry retains exact state");
    const auto epoch=s.current().epoch;
    processing=true; s.reset(); processing=false;
    check(s.current().epoch!=epoch && s.current().columns.empty() && s.previous().columns.empty() && s.previous().serial==0,
          "source reset invalidates incomplete captures and changes epoch without allocation");
    check(s.processChecked(next,800), "new epoch accepts new source sequence");
    auto bad=settings; bad.columns=0;
    check(!s.prepare(bad), "zero columns refuse");
    bad=settings; bad.columns=Scope::kMaximumColumns+1;
    check(!s.prepare(bad), "oversized capture refuses");
    bad=settings; bad.columnsPerSample=0;
    check(!s.prepare(bad), "nonadvancing capture speed refuses");
    bad=settings; bad.sampleRate=std::numeric_limits<double>::infinity();
    check(!s.prepare(bad), "invalid rate refuses");
    bad=settings; bad.trigger=static_cast<Scope::Trigger>(99);
    check(!s.prepare(bad), "invalid enum refuses");
    check(s.processChecked({},-1) && s.processTrustedFinite({},0), "empty prepared input is a no-op");
}
void mapping() {
    Scope::Settings settings;
    check(Scope::mapNormalized({},settings) && settings.threshold==0 && settings.gain==1 && settings.columnsPerSample==.1 &&
          settings.holdoffSamples==1, "normalized midpoint equations are explicit");
    const auto saved=settings;
    auto bad=Scope::Normalized{}; bad.amplitude=std::numeric_limits<float>::infinity();
    check(!Scope::mapNormalized(bad,settings) && saved==settings, "invalid normalized mapping retains caller output");
    for(float u:{0.f,.25f,.5f,.75f,1.f}) {
        Scope::Normalized n; n.level=n.amplitude=n.time=n.holdoff=n.speed=u;
        Scope s;
        check(Scope::mapNormalized(n,settings) && s.prepare(settings), "mapped endpoints are admitted without out-of-range conversion");
    }
}
void extremesAndFastBoundary() {
    Scope::Settings base; base.trigger=Scope::Trigger::Rising; base.threshold=1;
    base.columnsPerSample=1; base.columns=64;
    const std::array<float,16> data{0.f,-0.f,std::numeric_limits<float>::denorm_min(),
        -std::numeric_limits<float>::denorm_min(),1.f,1.f,-1.f,-1.f,
        std::numeric_limits<float>::max(),-std::numeric_limits<float>::max(),
        .2f,-.2f,0.f,0.f,1.f,-1.f};
    for(bool dc:{false,true}) {
        Scope slow,fast; auto s=base;s.removeDc=dc;
        check(slow.prepare(s)&&fast.prepare(s),"finite extreme and fast caller prepares");
        processing=true;
        const bool ok=slow.processChecked(data,0)&&scopeFastProcess(fast,data,0);
        processing=false;
        check(ok&&same(slow.current(),fast.current())&&same(slow.previous(),fast.previous()),
              "separately fast caller preserves precise capture under clipped extremes and signed/subnormal zeros");
        const auto current=slow.current();
        check(std::all_of(current.columns.begin(),current.columns.end(),[](const auto &c){
            return std::isfinite(c.first)&&std::isfinite(c.second)&&std::abs(c.first)<=1&&std::abs(c.second)<=1;
        }),"finite maximum input remains finite and clipped without a numerical floor on raw samples");
    }
    Scope s,reference;
    check(s.prepare(base)&&reference.prepare(base)&&s.processChecked(data,0)&&reference.processChecked(data,0),
          "physical guard continuation fixture prepares");
    std::vector<Scope::Settings> bad;
    for(float value:{std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity()}) {
        auto x=base;x.gain=value;bad.push_back(x);
        x=base;x.threshold=value;bad.push_back(x);
    }
    for(double value:{std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity(),-1.}) {
        auto x=base;x.columnsPerSample=value;bad.push_back(x);
        x=base;x.internalCyclesPerSample=value;bad.push_back(x);
    }
    auto x=base;x.holdoffSamples=0;bad.push_back(x);
    x=base;x.holdoffSamples=10001;bad.push_back(x);
    x=base;x.sampleRate=7999;bad.push_back(x);
    x=base;x.sampleRate=384001;bad.push_back(x);
    x=base;x.gain=0;bad.push_back(x);
    x=base;x.gain=1001;bad.push_back(x);
    x=base;x.threshold=-1.1f;bad.push_back(x);
    x=base;x.threshold=1.1f;bad.push_back(x);
    x=base;x.columnsPerSample=32.01;bad.push_back(x);
    x=base;x.internalCyclesPerSample=1.01;bad.push_back(x);
    for(const auto &invalid:bad) check(!s.prepare(invalid)&&*s.settings()==base,"physical invalid guard preserves complete stream");
    check(s.processChecked(data,16)&&reference.processChecked(data,16)&&same(s.current(),reference.current())&&
          same(s.previous(),reference.previous()),"all physical refusals preserve exact continuation");
    for(double rate:{8000.,384000.}) {
        auto a=base;a.sampleRate=rate;a.gain=.001f;a.columnsPerSample=.0001;a.internalCyclesPerSample=0;
        check(s.prepare(a),"lower physical acquisition endpoints admit");
        a.gain=1000;a.columnsPerSample=32;a.internalCyclesPerSample=1;a.holdoffSamples=10000;
        check(s.prepare(a),"upper physical acquisition endpoints admit");
    }
}
}
void *operator new(std::size_t size) {
    if (failAfter==0) throw std::bad_alloc{};
    if (failAfter>0) --failAfter;
    if (processing) ++allocations;
    if (auto *p=std::malloc(size?size:1)) return p;
    throw std::bad_alloc{};
}
void operator delete(void *p) noexcept { if(p&&processing)++frees; std::free(p); }
void *operator new[](std::size_t n) { return ::operator new(n); }
void operator delete[](void *p) noexcept { ::operator delete(p); }
void operator delete(void *p,std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void *p,std::size_t) noexcept { ::operator delete(p); }
int main() {
    basicAndOrdering(); triggersAndHoldoff(); partitionAndDc(); refusalAndLifecycle(); mapping(); extremesAndFastBoundary();
    check(allocations==0&&frees==0,"process/reset C++ allocation and free remain zero");
    std::printf("triggered scope: %d checks, %d failures; allocations %zu, frees %zu\n",checks,failures,allocations,frees);
    return failures?1:0;
}
