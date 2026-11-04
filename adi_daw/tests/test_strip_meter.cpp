// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/mixer.hpp"
#include <array>
#include <cstdio>
#include <limits>
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
void access() {
    adi::rows::Model model;adi::rows::Track track;track.id=9;track.kind="master";model.tracks.push_back(track);
    MixerStrips strips;strips.sync(model,nullptr,{});const auto& reader=strips;
    check(reader.stripFor(9)!=nullptr && reader.stripFor(42)==nullptr,"const Session strip access resolves by track id");
    const auto* strip=reader.stripFor(9);strips.sync(model,nullptr,{});
    check(reader.stripFor(9)==strip,"meter address survives an ordinary graph rebuild");
}
}
int main(){run<float>();run<double>();access();std::printf("%s -- %d checks, %d failure(s)\n",failures?"FAIL":"PASS",checks,failures);return failures?1:0;}
