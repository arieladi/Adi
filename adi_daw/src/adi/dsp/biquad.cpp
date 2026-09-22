// SPDX-License-Identifier: GPL-3.0-or-later

#include "adi/dsp/biquad.hpp"

#include <cmath>
#include <complex>
#include <cstdio>

namespace adi::dsp {

namespace {

constexpr double kPi = 3.14159265358979323846;

Biquad normalise(double b0, double b1, double b2, double a0, double a1, double a2) noexcept {
    Biquad c;
    c.b0 = b0 / a0;
    c.b1 = b1 / a0;
    c.b2 = b2 / a0;
    c.a1 = a1 / a0;
    c.a2 = a2 / a0;
    return c;
}

}  // namespace

Biquad peaking(double fs, double f0, double q, double gainDb) noexcept {
    const double A = std::pow(10.0, gainDb / 40.0);
    const double w0 = 2.0 * kPi * f0 / fs;
    const double alpha = std::sin(w0) / (2.0 * q);
    const double cw = std::cos(w0);
    return normalise(1.0 + alpha * A, -2.0 * cw, 1.0 - alpha * A,
                     1.0 + alpha / A, -2.0 * cw, 1.0 - alpha / A);
}

Biquad lowShelf(double fs, double f0, double gainDb) noexcept {
    const double A = std::pow(10.0, gainDb / 40.0);
    const double w0 = 2.0 * kPi * f0 / fs;
    const double cw = std::cos(w0);
    // Shelf slope S = 1: alpha = sin(w0)/2 * sqrt((A + 1/A)(1/S - 1) + 2).
    const double alpha = std::sin(w0) / 2.0 * std::sqrt(2.0);
    const double sa = 2.0 * std::sqrt(A) * alpha;
    return normalise(A * ((A + 1.0) - (A - 1.0) * cw + sa),
                     2.0 * A * ((A - 1.0) - (A + 1.0) * cw),
                     A * ((A + 1.0) - (A - 1.0) * cw - sa),
                     (A + 1.0) + (A - 1.0) * cw + sa,
                     -2.0 * ((A - 1.0) + (A + 1.0) * cw),
                     (A + 1.0) + (A - 1.0) * cw - sa);
}

Biquad highShelf(double fs, double f0, double gainDb) noexcept {
    const double A = std::pow(10.0, gainDb / 40.0);
    const double w0 = 2.0 * kPi * f0 / fs;
    const double cw = std::cos(w0);
    const double alpha = std::sin(w0) / 2.0 * std::sqrt(2.0);
    const double sa = 2.0 * std::sqrt(A) * alpha;
    return normalise(A * ((A + 1.0) + (A - 1.0) * cw + sa),
                     -2.0 * A * ((A - 1.0) + (A + 1.0) * cw),
                     A * ((A + 1.0) + (A - 1.0) * cw - sa),
                     (A + 1.0) - (A - 1.0) * cw + sa,
                     2.0 * ((A - 1.0) - (A + 1.0) * cw),
                     (A + 1.0) - (A - 1.0) * cw - sa);
}

Biquad lowPass(double fs, double f0, double q) noexcept {
    const double w0 = 2.0 * kPi * f0 / fs;
    const double alpha = std::sin(w0) / (2.0 * q);
    const double cw = std::cos(w0);
    return normalise((1.0 - cw) / 2.0, 1.0 - cw, (1.0 - cw) / 2.0,
                     1.0 + alpha, -2.0 * cw, 1.0 - alpha);
}

Biquad highPass(double fs, double f0, double q) noexcept {
    const double w0 = 2.0 * kPi * f0 / fs;
    const double alpha = std::sin(w0) / (2.0 * q);
    const double cw = std::cos(w0);
    return normalise((1.0 + cw) / 2.0, -(1.0 + cw), (1.0 + cw) / 2.0,
                     1.0 + alpha, -2.0 * cw, 1.0 - alpha);
}

double butterworthQ(int order, int k) noexcept {
    // The poles of an order-n Butterworth sit at angles (2k-1) pi / (2n) from
    // the imaginary axis; each conjugate pair is one biquad with this Q.
    return 1.0 / (2.0 * std::sin((2.0 * k - 1.0) * kPi / (2.0 * order)));
}

std::vector<Biquad> cut(CutKind kind, double fs, double f0, int slopeDbPerOct) {
    int sections = slopeDbPerOct / 12;
    if (sections < 1) sections = 1;
    const int order = 2 * sections;
    std::vector<Biquad> out;
    out.reserve(static_cast<std::size_t>(sections));
    for (int k = 1; k <= sections; ++k) {
        const double q = butterworthQ(order, k);
        out.push_back(kind == CutKind::LowCut ? highPass(fs, f0, q) : lowPass(fs, f0, q));
    }
    return out;
}

bool isStable(const Biquad& c) noexcept {
    return std::fabs(c.a2) < 1.0 && std::fabs(c.a1) < 1.0 + c.a2;
}

double magnitudeDb(const Biquad& c, double fs, double f) noexcept {
    const std::complex<double> z1 = std::polar(1.0, -2.0 * kPi * f / fs);   // z^-1
    const std::complex<double> z2 = z1 * z1;
    const std::complex<double> h = (c.b0 + c.b1 * z1 + c.b2 * z2) /
                                   (1.0 + c.a1 * z1 + c.a2 * z2);
    return 20.0 * std::log10(std::abs(h));
}

double magnitudeDb(const std::vector<Biquad>& cascade, double fs, double f) noexcept {
    double db = 0.0;
    for (const Biquad& c : cascade) db += magnitudeDb(c, fs, f);
    return db;
}

PdBiquad toPd(const Biquad& c) noexcept {
    PdBiquad p;
    p.fb1 = -c.a1;     // NEGATED: biquad~ adds its feedback terms
    p.fb2 = -c.a2;
    p.ff1 = c.b0;
    p.ff2 = c.b1;
    p.ff3 = c.b2;
    return p;
}

std::string pdMessage(const PdBiquad& p) {
    // %.17g: enough digits that the patch receives exactly the double the
    // host computed, not a neighbour of it.
    char buf[160];
    std::snprintf(buf, sizeof buf, "%.17g %.17g %.17g %.17g %.17g",
                  p.fb1, p.fb2, p.ff1, p.ff2, p.ff3);
    return buf;
}

}  // namespace adi::dsp
