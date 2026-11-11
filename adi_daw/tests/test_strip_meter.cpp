// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/mixer.hpp"
#include <array>
#include <cstdio>
#include <limits>
#include <atomic>
#include <thread>
namespace {
using namespace adi::engine;
int checks=0,failures=0;
void check(bool good,const char* name){++checks;if(!good){++failures;std::printf("FAIL: %s\n",name);}}
bool near(double a,double b){return std::abs(a-b)<1e-6;}
template<class T> void run() {
    StripNode strip;strip.prepare(48000,8);
    check(strip.meter().peak.load()==0 && strip.meter().rms.load()==0,"new strip starts with zero readouts");
    std::array<T,8> left{1,1,0,0,0,0,0,0},right{},outL{},outR{};
    const T* in[]{left.data(),right.data()};T* out[]{outL.data(),outR.data()};
    NodeIo io;io.channels=2;io.frames=2;
    if constexpr(std::is_same_v<T,float>){io.in=in;io.out=out;}
    else {io.in64=in;io.out64=out;}
    strip.process(io);
    check(strip.meter().peak.load()==1 && near(strip.meter().rms.load(),std::sqrt(0.5)),"meter reads post-fader samples and averages both channels");
    io.blockOffset=2;io.frames=6;strip.process(io);
    check(strip.meter().peak.load()==1 && near(strip.meter().rms.load(),std::sqrt(0.125)),"split callback retains earlier peak and weights all frames in RMS");
    left.fill(T{0.5});right.fill(T{0.5});io.blockOffset=0;io.frames=8;strip.process(io);
    check(strip.meter().peak.load()==0.5f && strip.meter().rms.load()==0.5f,"next callback resets the accumulation without a tap");
    strip.setStatic(-6.020599913,1,PanLaw::Balance,true);strip.restart();strip.process(io);
    check(near(strip.meter().peak.load(),0.25) && near(strip.meter().rms.load(),0.25/std::sqrt(2.0)),"meter follows fader gain and pan");
    strip.setStatic(0,0,PanLaw::Live,false);strip.restart();strip.process(io);
    check(strip.meter().peak.load()==0 && strip.meter().rms.load()==0,"muted strip reads zero");
    strip.setStatic(0,0,PanLaw::Live,true);strip.restart();strip.process(io);
    io.frames=0;strip.process(io);
    check(strip.meter().peak.load()==0.5f,"empty segment preserves the most recent readout");
    io.frames=8;strip.silenceTaps(io);
    check(strip.meter().peak.load()==0 && strip.meter().rms.load()==0,"suspended strip clears stale levels");
    strip.process(io);strip.release();
    check(strip.meter().peak.load()==0 && strip.meter().rms.load()==0,"device stop clears the readout");
    // A graph re-prepares persistent strips while the old graph can play.
    strip.process(io);strip.prepare(96000,8);
    check(strip.meter().peak.load()==0.5f,"rebuild preparation preserves a live meter");
    io.channels=1;strip.process(io);
    check(strip.meter().peak.load()==0.5f && strip.meter().rms.load()==0.5f,"mono is averaged once");
    io.channels=2;left.fill(std::numeric_limits<T>::infinity());right.fill(std::numeric_limits<T>::quiet_NaN());strip.process(io);
    check(std::isfinite(strip.meter().peak.load()) && std::isfinite(strip.meter().rms.load()),"bad device output cannot poison UI meter values");
    left.fill(T{0.125});right.fill(T{0.125});strip.process(io);
    check(strip.meter().peak.load()==0.125f && strip.meter().rms.load()==0.125f,"meter recovers on the next finite block");
}
template<class T> void held() {
    StripNode strip;strip.prepare(48000,8);const StripMeter& meter=strip.meter();
    std::array<T,8> left{},right{},outL{},outR{};
    const T* in[]{left.data(),right.data()};T* out[]{outL.data(),outR.data()};
    NodeIo io;io.channels=2;io.frames=8;
    if constexpr(std::is_same_v<T,float>){io.in=in;io.out=out;}
    else {io.in64=in;io.out64=out;}
    const auto fill=[&](T value){left.fill(value);right.fill(value);};
    check(meter.takePeak()==0 && meter.takePeak()==0,"new held peak starts empty through const access");
    fill(T{1.25});strip.process(io);fill(T{0.25});strip.process(io);strip.process(io);
    check(meter.peak.load()==0.25f && meter.rms.load()==0.25f,"instantaneous meter still follows the latest callback");
    check(meter.takePeak()==1.25f,"slow reader retains an over-full-scale transient across lower callbacks");
    check(meter.takePeak()==0,"taking the peak clears only the held readout");
    check(meter.peak.load()==0.25f && meter.rms.load()==0.25f,"taking the hold does not reset callback peak or RMS");
    fill(T{0});left[0]=T{1.5};io.frames=2;strip.process(io);
    check(meter.takePeak()==1.5f,"first processing segment publishes before callback completion");
    io.blockOffset=2;io.frames=6;strip.process(io);
    check(meter.peak.load()==1.5f && meter.takePeak()==0,"later silence does not republish a consumed early segment");
    fill(T{0.125});io.blockOffset=0;io.frames=2;strip.process(io);
    check(meter.takePeak()==0.125f,"a new callback starts a fresh consumed interval");
    fill(T{0.75});io.blockOffset=2;io.frames=6;strip.process(io);
    check(meter.takePeak()==0.75f,"a later segment publishes its own new peak");
    fill(T{1.75});io.blockOffset=0;io.frames=8;strip.process(io);strip.silenceTaps(io);
    check(meter.peak.load()==0 && meter.rms.load()==0 && meter.takePeak()==1.75f,"sleeping clears callback levels while preserving an unread transient");
    strip.process(io);strip.setStatic(0,0,PanLaw::Live,false);strip.restart();strip.process(io);
    check(meter.peak.load()==0 && meter.takePeak()==1.75f,"muting cannot erase an unread transient");
    strip.setStatic(0,0,PanLaw::Live,true);strip.restart();strip.process(io);strip.release();
    check(meter.peak.load()==0 && meter.rms.load()==0 && meter.takePeak()==1.75f,"device stop preserves an unread peak until the reader clears it");
    strip.process(io);strip.prepare(96000,8);
    check(meter.takePeak()==1.75f,"graph preparation preserves an unread held peak");
    io.frames=0;strip.process(io);check(meter.takePeak()==0,"empty processing publishes no phantom peak");io.frames=8;
    fill(std::numeric_limits<T>::infinity());right.fill(std::numeric_limits<T>::quiet_NaN());strip.process(io);
    check(meter.takePeak()==std::numeric_limits<float>::max(),"held readout bounds infinity and ignores NaN without poisoning the reader");
    fill(T{0.125});strip.process(io);check(meter.takePeak()==0.125f,"the consumed hold recovers on finite audio");
    StripNode other;other.prepare(48000,8);fill(T{0.5});other.process(io);fill(T{0.25});strip.process(io);
    check(other.meter().takePeak()==0.5f && meter.takePeak()==0.25f,"strips retain and clear independent peaks");
}
template<class T> void concurrent() {
    StripNode strip;strip.prepare(48000,8);const StripMeter& meter=strip.meter();
    std::array<T,8> signal{},outL{},outR{};signal.fill(T{1.25});
    const T* in[]{signal.data(),signal.data()};T* out[]{outL.data(),outR.data()};
    NodeIo io;io.channels=2;io.frames=8;
    if constexpr(std::is_same_v<T,float>){io.in=in;io.out=out;}
    else {io.in64=in;io.out64=out;}
    std::atomic<bool> start{false},done{false};
    std::thread render([&]{while(!start.load(std::memory_order_acquire))std::this_thread::yield();
        for(unsigned block=0;block<50000;++block)strip.process(io);
        done.store(true,std::memory_order_release);});
    bool valid=true;float highest=0;start.store(true,std::memory_order_release);
    do {const float value=meter.takePeak();valid=valid && (value==0 || value==1.25f);highest=std::max(highest,value);}
    while(!done.load(std::memory_order_acquire));
    render.join();highest=std::max(highest,meter.takePeak());
    check(valid && highest==1.25f,"concurrent render publication and UI consumes retain a finite actual peak");
    check(meter.takePeak()==0 && meter.peak.load()==1.25f,"final consume empties the hold without disturbing the render meter");
}
void access() {
    adi::rows::Model model;adi::rows::Track track;track.id=9;track.kind="master";model.tracks.push_back(track);
    MixerStrips strips;strips.sync(model,nullptr,{});const auto& reader=strips;
    check(reader.stripFor(9)!=nullptr && reader.stripFor(42)==nullptr,"const Session strip access resolves by track id");
    const auto* strip=reader.stripFor(9);strips.sync(model,nullptr,{});
    check(reader.stripFor(9)==strip,"meter address survives an ordinary graph rebuild");
}
}
int main(){run<float>();run<double>();held<float>();held<double>();concurrent<float>();concurrent<double>();access();std::printf("%s -- %d checks, %d failure(s)\n",failures?"FAIL":"PASS",checks,failures);return failures?1:0;}
