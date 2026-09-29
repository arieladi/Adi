// SPDX-License-Identifier: MIT
// Original ADI sample-and-hold decimator and companding quantizer.
#include "adi/dsp/redux.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace adi::dsp {
void Redux::prepare(double sr) noexcept {
    sampleRate_=std::isfinite(sr) && sr>0 ? sr : 48000;
    channels_={};
    channels_[0].random=0x12345678u;
    channels_[1].random=0x87654321u;
    coefficients();
}
void Redux::coefficients() noexcept {
    const double nyquist=std::min(rate_,sampleRate_)*0.5;
    const auto coefficient=[this](double hz) {
        return 1-std::exp(-2*std::numbers::pi*std::clamp(hz,0.01,sampleRate_*0.49)/sampleRate_);
    };
    preCoefficient_=coefficient(nyquist);
    postCoefficient_=coefficient(nyquist*std::exp2(octave_));
}
void Redux::setRate(double v) noexcept { if(std::isfinite(v)){rate_=std::clamp(v,20.,48000.);coefficients();} }
void Redux::setJitter(double v) noexcept { if(std::isfinite(v))jitter_=std::clamp(v,0.,100.)/100; }
void Redux::setBits(double v) noexcept { if(std::isfinite(v))bits_=std::clamp(v,1.,24.); }
void Redux::setShape(double v) noexcept { if(std::isfinite(v))shape_=std::clamp(v,0.,100.)/100; }
void Redux::setPre(double v) noexcept { if(std::isfinite(v))pre_=v>=0.5; }
void Redux::setPost(double v) noexcept { if(std::isfinite(v))post_=v>=0.5; }
void Redux::setOctave(double v) noexcept { if(std::isfinite(v)){octave_=std::clamp(v,-4.,4.);coefficients();} }
void Redux::setDcShift(double v) noexcept { if(std::isfinite(v))dc_=v>=0.5; }
void Redux::setMix(double v) noexcept { if(std::isfinite(v))mix_=std::clamp(v,0.,100.)/100; }
double Redux::sample(double input, Channel& c) noexcept {
    const double dry=input;
    c.pre1+=preCoefficient_*(input-c.pre1);
    c.pre2+=preCoefficient_*(c.pre1-c.pre2);
    if(pre_)input=c.pre2;
    // Carry fractional time across every sample and every host block. No
    // anti-aliasing here: the deliberately folded spectrum is the effect.
    if(c.remaining<=0){
        c.held=input;
        c.random^=c.random<<13; c.random^=c.random>>17; c.random^=c.random<<5;
        const double noise=2*static_cast<double>(c.random)/4294967295.-1;
        const double period=sampleRate_/std::min(rate_,sampleRate_);
        c.remaining+=std::max(1.,period*(1+0.95*jitter_*noise));
    }
    c.remaining-=1;
    const double exponent=1/(1+3*shape_);
    const double step=std::exp2(1-bits_);
    double compressed=std::copysign(std::pow(std::min(1.,std::abs(c.held)),exponent),c.held);
    if(dc_)compressed+=step*0.5;
    // Mid-rise gives two levels at one bit. Silence remains silent without
    // DC Shift; the exception avoids creating a DC tone from an empty input.
    double quantized=compressed==0 ? 0 : (std::floor(std::clamp(compressed,-1.,1.)/step)+0.5)*step;
    quantized=std::clamp(quantized,-1+step*0.5,1-step*0.5);
    double wet=std::copysign(std::pow(std::abs(quantized),1/exponent),quantized);
    c.post1+=postCoefficient_*(wet-c.post1);
    c.post2+=postCoefficient_*(c.post1-c.post2);
    if(post_)wet=c.post2;
    return dry+(wet-dry)*mix_;
}
void Redux::process(const float* left,const float* right,float* outLeft,float* outRight,std::size_t n) noexcept {
    for(std::size_t i=0;i<n;++i){
        outLeft[i]=static_cast<float>(sample(left[i],channels_[0]));
        outRight[i]=static_cast<float>(sample(right[i],channels_[1]));
    }
}
}
