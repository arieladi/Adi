// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/adi_shaper_noise.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace {
using Noise = adi::dsp::AdiShaperNoise;

void check(bool result, const char* message) {
    if (!result) throw std::runtime_error(message);
}

void near(double actual, double expected, double tolerance, const char* message) {
    if (!(std::abs(actual - expected) <= tolerance)) {
        std::fprintf(stderr, "%s: %.7g versus %.7g (tolerance %.7g)\n", message, actual, expected, tolerance);
        throw std::runtime_error(message);
    }
}

Noise make(double rate, Noise::Source source = Noise::Source::White) {
    Noise noise;
    noise.prepare(rate);
    noise.setFilters(0, 1);
    noise.setSource(source);
    noise.setMono(false);
    noise.setNoiseOnly(true);
    return noise;
}

struct Reading {
    double rms, lagOne, stereoCorrelation, kurtosis;
};

Reading meter(Noise& noise, float pitch = .5f) {
    constexpr int count = 192000;
    double l2 = 0, r2 = 0, l4 = 0, lr = 0, lag = 0, last = 0;
    for (int i = 0; i < count; ++i) {
        float left = 0, right = 0;
        noise.processStereo(left, right, 1, pitch);
        const double square = static_cast<double>(left)*left;
        l2 += square;
        r2 += static_cast<double>(right)*right;
        l4 += square*square;
        lr += static_cast<double>(left)*right;
        lag += last*left;
        last = left;
    }
    return {std::sqrt(l2/count), lag/l2, lr/std::sqrt(l2*r2), l4*count/(l2*l2)};
}

void white() {
    struct Reference { double hz, rms, lag; };
    constexpr Reference references[]{{44100,.024725,-.0012}, {48000,.022345,.2260}, {96000,.022329,.7298}};
    for (auto r : references) {
        auto n = make(r.hz);
        const auto m = meter(n);
        near(m.rms, r.rms, .00035, "white RMS");
        near(m.lagOne, r.lag, .015, "white sample rate and reconstruction");
        near(m.stereoCorrelation, 0, .015, "white stereo independence");
        near(m.kurtosis, r.hz == 44100 ? 3.0 : 3.09, .10, "white amplitude distribution");
    }
}

void routing() {
    auto a = make(48000), b = a;
    a.setSource(Noise::Source::Off);
    float l = .2f, r = -.3f;
    a.setNoiseOnly(false);
    a.processStereo(l,r,1,0);
    check(l == .2f && r == -.3f, "Off additive passes input");
    a.setNoiseOnly(true);
    a.processStereo(l,r,1,0);
    check(l == 0 && r == 0, "Off solo is silent");

    a = b;
    a.setMono(true);
    for (int i=0; i<256; ++i) {
        float ll=.2f, rr=-.3f, sl=0, sr=0;
        a.processStereo(ll,rr,1,0);
        b.processStereo(sl,sr,1,1);
        check(ll == rr && ll == sl, "Mono centers the left stream; white pitch is invariant");
    }
    a = b;
    a.setNoiseOnly(false);
    a.setTrim(.5f);
    for (int i=0; i<512; ++i) {
        float ll=.2f, rr=-.3f, sl=0, sr=0;
        a.processStereo(ll,rr,.25f,.5f);
        b.processStereo(sl,sr,1,.5f);
        const double gain = std::pow(10.0, -18.0/20.0)*.25;
        near(ll, .2f+gain*sl, 3e-8, "dry plus level and trim");
        near(rr,-.3f+gain*sr, 3e-8, "stereo additive routing");
    }
    a = b;
    for (int i=0; i<96; ++i) {
        float ll=1, rr=1, sl=0, sr=0;
        a.processStereo(ll,rr,0,.5f);
        b.processStereo(sl,sr,1,.5f);
        check(ll==0 && rr==0, "zero level is silent");
    }
    float one=0,two=0,three=0,four=0;
    a.processStereo(one,two,1,.5f);
    b.processStereo(three,four,1,.5f);
    check(one==three && two==four, "zero level preserves source continuity");
    auto fresh=make(48000);
    fresh.processStereo(three,four,1,.5f);
    a.reset();
    a.processStereo(one,two,1,.5f);
    check(one!=three || two!=four, "warm reset advances the random stream");
    a.prepare(48000);
    a.processStereo(one,two,1,.5f);
    check(one==three && two==four, "prepare with a fixed seed is reproducible");
    a.prepare(std::numeric_limits<double>::quiet_NaN());
    a.processStereo(one,two,1,.5f);
    check(one==0 && two==0, "invalid rate produces no noise");
}

// Independent difference equation for two identical first-order sections.
void filters() {
    for (bool high : {false,true}) {
        auto raw=make(48000), filtered=raw;
        filtered.setFilters(high ? .5f : 0, high ? 1 : .5f);
        const double k=std::tan(std::numbers::pi*(20.58*32)/48000);
        const double scale=(high ? 1 : k)/(1+k), pole=(1-k)/(1+k);
        std::array<double,2> input{},output{};
        for (int i=0; i<5000; ++i) {
            float l=0,r=0,fl=0,fr=0;
            raw.processStereo(l,r,1,.5f);
            filtered.processStereo(fl,fr,1,.5f);
            double x=l;
            for (std::size_t j=0; j<2; ++j) {
                const double y=scale*(x+(high ? -input[j] : input[j]))+pole*output[j];
                input[j]=x; output[j]=y; x=y;
            }
            near(fl,x,1.0e-7,"measured two-pole noise cutoff");
        }
    }
}

void pink() {
    auto noise=make(48000,Noise::Source::Pink);
    const auto m=meter(noise);
    near(m.rms,.02235,.0012,"independent pink calibration");
    near(m.stereoCorrelation,0,.045,"pink stereo independence");
    auto slow=make(48000,Noise::Source::Pink), fast=slow;
    check(meter(slow,.25f).lagOne > meter(fast,.75f).lagOne+.08, "pink pitch changes source speed");

    // Average equal-width frequency bins at two octave-spaced frequencies.
    constexpr int frames=1024, blocks=192;
    std::array<double,frames> window{};
    for (int i=0;i<frames;++i) window[i]=.5-.5*std::cos(2*std::numbers::pi*i/frames);
    std::array<double,2> power{};
    for (int block=0;block<blocks;++block) {
        std::array<double,frames> data{};
        for (int i=0;i<frames;++i) {
            float l=0,r=0;
            noise.processStereo(l,r,1,.5f);
            data[i]=window[i]*l;
        }
        for (std::size_t j=0;j<2;++j) {
            const int center=j==0 ? 16 : 128;
            for (int bin=center-1;bin<=center+1;++bin) {
                const double coefficient=2*std::cos(2*std::numbers::pi*bin/frames);
                double q1=0,q2=0;
                for (double x:data) {
                    const double q0=x+coefficient*q1-q2;
                    q2=q1;
                    q1=q0;
                }
                power[j]+=q1*q1+q2*q2-coefficient*q1*q2;
            }
        }
    }
    near(10*std::log10(power[1]/power[0])/3, -3.01, .40, "pink dB per octave");
}
} // namespace

int main() {
    try {
        white();
        routing();
        filters();
        pink();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
    std::puts("Noise source checks passed.");
    return 0;
}
