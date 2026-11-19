// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/adi_shaper_reverb.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {
using Room = adi::dsp::AdiShaperReverb;
using Stereo = std::array<float,2>;
constexpr int rate = 48000, length = rate*2;
int errors = 0;
void check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "FAIL %s\n", message); ++errors; }
}

std::vector<Stereo> impulse(Room& room, float width, float delay = 0, double bpm = 120,
                            bool closeReturn = false, bool clear = false, bool closeSend = false,
                            float send = 1, float ret = 1) {
    room.reset();
    room.setWidth(width);
    room.setPredelay(delay, bpm);
    room.setClearTails(clear);
    std::vector<Stereo> result(length);
    for (int n=0; n<length; ++n) {
        float l = n == 0 ? .5f : 0.f, r = 0;
        room.processStereo(l,r,closeSend && n>=256 ? 0.f : send,
                           closeReturn && n>=12000 && n<18000 ? 0.f : ret);
        result[static_cast<std::size_t>(n)] = {l,r};
    }
    return result;
}
}

int main() {
    Room room;
    float l=1, r=1;
    room.processStereo(l,r,1,1);
    check(l==0 && r==0,"unprepared room is silent");
    check(room.prepare(rate),"prepare original stereo room");
    const auto base=impulse(room,.5f);
    std::size_t count=0;
    double energy=0, rightPeak=0;
    for (const auto& frame:base) {
        if(frame[0]!=0) ++count;
        energy+=frame[0]*frame[0];
        rightPeak=std::max(rightPeak,std::abs(static_cast<double>(frame[1])));
    }
    check(count>500 && energy>.001 && rightPeak==0,"authored FIR has a tail, independent L/R at neutral width");
    check(impulse(room,.5f,0,120,false,false,true)==base,"closing send preserves the existing tail");
    const auto mono=impulse(room,0);
    const auto atten=impulse(room,.5f,0,120,false,false,false,.25f,.5f);
    const auto mute=impulse(room,.5f,0,120,true,false);
    const auto cleared=impulse(room,.5f,0,120,true,true);
    bool width=true,gain=true,restore=true,blank=true;
    double laterEnergy=0;
    for (int i=0;i<length;++i) {
        const auto n=static_cast<std::size_t>(i);
        const auto ms=.5f*base[n][0];
        width &= mono[n][0]==ms && mono[n][1]==ms;
        gain &= std::abs(atten[n][0]-.125f*base[n][0])<1e-8;
        restore &= mute[n][0] == (i>=12000 && i<18000 ? 0 : base[n][0]);
        blank &= cleared[n][0] == (i>=12000 ? 0 : base[n][0]);
        if(i>=18000)laterEnergy+=base[n][0]*base[n][0];
    }
    check(width && gain,"independent linear send, return and mono width");
    check(restore && blank && laterEnergy>0,"mute/reopen restores or clears the tail");
    for(const auto [p,tempo,offset]:std::array<std::array<double,3>,5>{{
          {.125,120,450},{.25,90,1800},{.5,120,7200},{.75,120,12000},{1.,90,32000}}}) {
        const auto shifted=impulse(room,.5f,static_cast<float>(p),tempo);
        bool ok=true;
        for(int i=0;i<length;++i) {
            const int prior=i-static_cast<int>(offset);
            ok &= shifted[static_cast<std::size_t>(i)][0] == (prior<0?0:base[static_cast<std::size_t>(prior)][0]);
        }
        check(ok,"free and tempo-synced predelays give exact integer shifts");
    }
    room.setShape(.5f,0,true);
    const auto gated=impulse(room,.5f);
    check(std::all_of(gated.begin()+rate/4,gated.end(),[](const Stereo& x) {return x[0]==0 && x[1]==0;}),
          "shortest room gate fully decays");
    room.setShape(.5f,0,false);
    const auto shortDecay=impulse(room,.5f);
    double shortEnergy=0;
    for(const auto& frame:shortDecay) shortEnergy+=frame[0]*frame[0];
    check(shortEnergy<energy && shortEnergy>0,"decay control attenuates the authored room");
    return errors ? 1 : 0;
}
