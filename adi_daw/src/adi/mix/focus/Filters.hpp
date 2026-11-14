// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cmath>

namespace adi::mix::focus::detail {
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    double tick(double input) noexcept {
        const double output = (((x1*b1 + input*b0 + b2*x2) - y1*a1) - a2*y2) * (1./a0);
        x2=x1; x1=input; y2=y1; y1=output; return output;
    }
    void bandpass(double omega, double q) noexcept {
        const double alpha=std::sin(omega)/(2*q);
        b0=alpha; b1=0; b2=-alpha; a0=1+alpha; a1=-2*std::cos(omega); a2=1-alpha;
    }
    void peak(double alpha, double negativeTwoCosine, double totalGainDb) noexcept {
        const double a=std::sqrt(std::pow(10., (totalGainDb*.5)/20.));
        b0=a*alpha+1; b1=negativeTwoCosine; b2=1-a*alpha;
        a0=alpha/a+1; a1=negativeTwoCosine; a2=1-alpha/a;
    }
    void coefficientsFrom(const Biquad &other) noexcept {
        b0=other.b0; b1=other.b1; b2=other.b2;
        a0=other.a0; a1=other.a1; a2=other.a2;
    }
};
inline double knee(double value, double threshold) noexcept {
    const double upper=threshold+2.5, denominator=5.-6.*threshold;
    if (value < threshold-2.5) return value;
    if (std::abs(value-threshold) <= 2.5) {
        const double a=-1./denominator, b=8.*threshold/denominator+1.;
        return threshold-(a*upper*upper+b*upper)+(a*value*value+b*value);
    }
    if (value > upper) return threshold;
    return 0.;
}
struct Envelope {
    double rms=0., mainRms=0., gain=0.;
    static double level(double input, double &state) noexcept {
        const double power=input*input*(1.-.95)+state*state*.95;
        const float root=std::sqrt(static_cast<float>(power));
        state=static_cast<double>(root);
        return std::max(-120., static_cast<double>(20.f*std::log10(root)));
    }
    double tick(double main, double key, double threshold, double attack,
                double release, bool difference, bool keyValid) noexcept {
        const double keyLevel=level(key,rms);
        const double delta=difference ? level(main,mainRms)-keyLevel : 0.;
        const double target=keyValid ? (difference ? knee(delta,threshold)-delta : keyLevel-knee(keyLevel,threshold)) : 0.;
        const double a=!keyValid ? release : (gain <= target ? attack : release);
        gain=(1.-a)*target+a*gain;
        return static_cast<double>(difference ? static_cast<float>(gain) : -static_cast<float>(gain));
    }
};
} // namespace adi::mix::focus::detail
