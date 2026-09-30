/*
 * sst-filters - A header-only collection of SIMD filter
 * implementations by the Surge Synth Team
 *
 * Copyright 2019-2025, various authors, as described in the GitHub
 * transaction log.
 *
 * sst-filters is released under the Gnu General Public Licens
 * version 3 or later. Some of the filters in this package
 * originated in the version of Surge open sourced in 2018.
 *
 * All source in sst-filters available at
 * https://github.com/surge-synthesizer/sst-filters
 */
// Adapted from sst-filters BiquadFilter::coeff_orfanidisEQ at
// e92d93a92beabde03fa4ab767b285fa21c6608d6, used by Surge ParametricEQ3BandEffect.
// Provider/SIMD storage removed; normalized values returned to ADI's sample smoother.
// powf becomes double pow; shelf extension below is ADI's cookbook implementation.
#pragma once
#include <algorithm>
#include <cmath>
#include <numbers>
namespace adi::dsp::surge_eq {
struct Coeff {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
};
inline Coeff normalized(double a0, double a1, double a2, double b0, double b1, double b2) noexcept {
    Coeff c{b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
    return std::isfinite(c.b0) && std::isfinite(c.b1) && std::isfinite(c.b2) &&
                   std::isfinite(c.a1) && std::isfinite(c.a2)
               ? c
               : Coeff{};
}
inline double square(double x) noexcept { return x * x; }
inline Coeff peak(double omega, double BW, double G, double GB, double G0 = 1) noexcept {
    using std::abs;
    using std::atan;
    using std::cos;
    using std::log;
    using std::sin;
    using std::sinh;
    using std::sqrt;
    using std::tan;
    // For the curious http://eceweb1.rutgers.edu/~orfanidi/ece521/hpeq.pdf appears to be the source
    // of this
    // double limit = 0.95;
    double w0 = omega; // min(std::numbers::pi-0.000001,omega);
    BW = std::max(0.0001, BW);
    double Dww = 2 * w0 * sinh((log(2.0) / 2.0) * BW); // sinh = (e^x - e^-x)/2

    // double gainscale = 1;
    // if(omega>std::numbers::pi) gainscale = 1 / (1 + (omega-std::numbers::pi)*(4/Dw));

    if (abs(G - G0) > 0.00001) {
        double F = abs(G * G - GB * GB);
        double G00 = abs(G * G - G0 * G0);
        double F00 = abs(GB * GB - G0 * G0);
        double num = G0 * G0 * square(w0 * w0 - (std::numbers::pi * std::numbers::pi)) +
                     G * G * F00 * (std::numbers::pi * std::numbers::pi) * Dww * Dww / F;
        double den = square(w0 * w0 - std::numbers::pi * std::numbers::pi) +
                     F00 * std::numbers::pi * std::numbers::pi * Dww * Dww / F;
        double G1 = sqrt(num / den);

        if (omega > std::numbers::pi) {
            G = G1 * 0.9999;
            w0 = std::numbers::pi - 0.00001;
            G00 = abs(G * G - G0 * G0);
            F00 = abs(GB * GB - G0 * G0);
        }

        double G01 = abs(G * G - G0 * G1);
        double G11 = abs(G * G - G1 * G1);
        double F01 = abs(GB * GB - G0 * G1);
        double F11 = abs(GB * GB - G1 * G1); // goes crazy (?)
        double W2 = sqrt(G11 / G00) * square(tan(w0 / 2));
        double w_lower = w0 * std::pow(2., -0.5 * BW);
        double w_upper =
            2 * atan(sqrt(F00 / F11) * sqrt(G11 / G00) * square(tan(w0 / 2)) / tan(w_lower / 2));
        double Dw = abs(w_upper - w_lower);
        double DW = (1 + sqrt(F00 / F11) * W2) * tan(Dw / 2);

        double C = F11 * DW * DW - 2 * W2 * (F01 - sqrt(F00 * F11));
        double D = 2 * W2 * (G01 - sqrt(G00 * G11));
        double A = sqrt((C + D) / F);
        double B = sqrt((G * G * C + GB * GB * D) / F);
        double a0 = (1 + W2 + A), a1 = -2 * (1 - W2), a2 = (1 + W2 - A), b0 = (G1 + G0 * W2 + B),
               b1 = -2 * (G1 - G0 * W2), b2 = (G1 - B + G0 * W2);

        return normalized(a0, a1, a2, b0, b1, b2);
    } else {
        return {};
    }
}
inline Coeff cut(double w, double q, bool high) noexcept {
    const double co = std::cos(w), alpha = std::sin(w) / (2 * q), b0 = (1 + (high ? co : -co)) * .5;
    return normalized(1 + alpha, -2 * co, 1 - alpha, b0, (high ? -2 : 2) * b0, b0);
}
inline Coeff notch(double w, double q) noexcept {
    const double co = std::cos(w), alpha = std::sin(w) / (2 * q);
    return normalized(1 + alpha, -2 * co, 1 - alpha, 1, -2 * co, 1);
}
inline Coeff shelf(double w, double q, double db, bool high) noexcept {
    const double A = std::pow(10., db / 40), co = std::cos(w), alpha = std::sin(w) / (2 * q),
                 term = 2 * std::sqrt(A) * alpha;
    if (high)
        return normalized((A + 1) - (A - 1) * co + term, 2 * ((A - 1) - (A + 1) * co),
                          (A + 1) - (A - 1) * co - term, A * ((A + 1) + (A - 1) * co + term),
                          -2 * A * ((A - 1) + (A + 1) * co), A * ((A + 1) + (A - 1) * co - term));
    return normalized((A + 1) + (A - 1) * co + term, -2 * ((A - 1) + (A + 1) * co),
                      (A + 1) + (A - 1) * co - term, A * ((A + 1) - (A - 1) * co + term),
                      2 * A * ((A - 1) - (A + 1) * co), A * ((A + 1) - (A - 1) * co - term));
}
} // namespace adi::dsp::surge_eq
