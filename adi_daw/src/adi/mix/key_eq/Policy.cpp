// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Policy.hpp"
#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <numbers>

namespace adi::mix::key_eq {
namespace {
constexpr double pi = std::numbers::pi;
constexpr float epsilon = std::numeric_limits<float>::epsilon();
constexpr float rootHalf = .7071067690849304f;
constexpr float qFloor = rootHalf * .05f;
using Complex = std::complex<double>;
using Polynomial = std::array<double, 3>;
float f(double value) noexcept { return static_cast<float>(value); }
double clamp(double x, double lo, double hi) noexcept { return std::min(hi, std::max(lo, x)); }
struct Root {
    Complex value{};
    bool infinite = false;
};
struct Roots {
    std::array<Root, 4> values{};
    std::size_t count = 0;
};
struct Analog {
    Polynomial n{}, d{};
    int kind = -1;
    float damping = 0;
    double scale = 1;
};
struct AnalogCascade {
    std::array<Analog, 16> sections{};
    std::size_t count = 0;
    bool append(const Analog &section) noexcept {
        if (count == sections.size())
            return false;
        sections[count++] = section;
        return true;
    }
};
Complex reciprocal(Complex z) noexcept {
    if (std::abs(z.real()) >= std::abs(z.imag())) {
        const double ratio = z.imag() / z.real(), den = z.real() + z.imag() * ratio;
        return {1 / den, -ratio / den};
    }
    const double ratio = z.real() / z.imag(), den = z.real() * ratio + z.imag();
    return {ratio / den, -1 / den};
}
Polynomial polynomial(const Roots &roots, std::size_t begin, std::size_t count) noexcept {
    std::array<Complex, 2> finite{};
    std::size_t n = 0;
    for (std::size_t i = begin; i < begin + count; ++i)
        if (!roots.values[i].infinite)
            finite[n++] = roots.values[i].value;
    if (n == 0)
        return {0, 0, 1};
    if (n == 1)
        return {0, 1, -finite[0].real()};
    return {1, -(finite[0].real() + finite[1].real()),
            finite[0].real() * finite[1].real() - finite[0].imag() * finite[1].imag()};
}
std::array<Root, 2> split(Root r, double bandwidth) noexcept {
    if (r.infinite)
        return {Root{}, Root{{}, true}};
    double xr = -(bandwidth * r.value.real()), xi = -(bandwidth * r.value.imag());
    const double dr = xr * xr - xi * xi - 4, di = (xi + xi) * xr;
    const double magnitude = std::sqrt(dr * dr + di * di);
    double sr = std::sqrt((magnitude + dr) * .5), si = std::sqrt((magnitude - dr) * .5);
    if (di < 0)
        si = -si;
    xr *= -.5;
    xi *= -.5;
    sr *= .5;
    si *= .5;
    return {Root{{xr + sr, xi + si}, false}, Root{{xr - sr, xi - si}, false}};
}
Roots bandRoots(const Roots &source, double bandwidth, float &norm) noexcept {
    Roots out{};
    norm = 1;
    if (source.count == 2 && std::abs(f(source.values[0].value.imag())) > epsilon) {
        const auto pair = split(source.values[0], bandwidth);
        out.count = 4;
        out.values = {pair[0], Root{std::conj(pair[0].value), false}, pair[1],
                      Root{std::conj(pair[1].value), false}};
        return out;
    }
    for (std::size_t i = 0; i < source.count; ++i) {
        const auto pair = split(source.values[i], bandwidth);
        out.values[out.count++] = pair[0];
        out.values[out.count++] = pair[1];
        if (source.count == 2 && pair[1].infinite)
            norm = f(static_cast<double>(norm) * bandwidth);
    }
    return out;
}
double powerAtOne(const Polynomial &n, const Polynomial &d) noexcept {
    const double den = (d[2] - d[0]) * (d[2] - d[0]) + d[1] * d[1];
    return std::abs(f(den)) > epsilon ? std::sqrt(((n[2] - n[0]) * (n[2] - n[0]) + n[1] * n[1]) / den) : 1;
}
bool lowBandPrototype(double order, float mappedQ, AnalogCascade &out) noexcept {
    const double g = f(std::exp(static_cast<double>(f(f(order) * -15.f)) * .1151292546497023));
    const double bandwidth = f(1.f / mappedQ);
    for (int j = 0; j < 3; ++j) {
        const double scale = std::pow(32.0, j), p1 = -scale, p2 = p1 * 5.656854249492381;
        Roots poles{}, zeros{};
        poles.count = zeros.count = 2;
        poles.values[0].value = {p1, 0};
        poles.values[1].value = {p2, 0};
        zeros.values[0].value = {p1 / g, 0};
        zeros.values[1].value = {p2 / g, 0};
        float unused = 1, gain = 1;
        poles = bandRoots(poles, bandwidth, unused);
        zeros = bandRoots(zeros, bandwidth, gain);
        const float rootGain = f(std::pow((g * g) * gain, static_cast<double>(.25f)));
        const double norm = f(rootGain * rootGain);
        for (std::size_t k = 0; k < 4; k += 2) {
            auto n = polynomial(zeros, k, 2);
            const auto d = polynomial(poles, k, 2);
            for (auto &v : n)
                v *= norm;
            const double normalized = f(1 / powerAtOne(n, d));
            for (auto &v : n)
                v *= normalized;
            if (!out.append({n, d, static_cast<int>(k / 2), f(scale), 1}))
                return false;
        }
    }
    return true;
}
bool prototypes(double order, int shape, float gainDb, float mappedQ, AnalogCascade &out) noexcept {
    if (shape >= 7 && shape <= 10)
        order *= .5;
    if (order < 1)
        return shape == 8 && lowBandPrototype(order, mappedQ, out);
    if (order > 16)
        return false;
    const int ceiling = static_cast<int>(std::ceil(order));
    const double fraction = static_cast<double>(f(f(order) - f(ceiling))) + 1;
    const double g = f(std::exp(static_cast<double>(gainDb) * .1151292546497023));
    const bool gainShape = shape >= 1 && shape <= 7 && shape != 3;
    const double baseGain = shape == 6 ? g : (shape == 2 || shape == 5 || shape == 7) ? std::sqrt(g) : g;
    const double factor = std::pow(baseGain, 1 / order);
    const double bandwidth = f(1.f / mappedQ);
    const float qFactor = f(rootHalf / mappedQ);
    double runningNorm = 1;
    constexpr std::array<int, 13> conversion{0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 3, 4, 0};
    for (int j = 0; j < (ceiling + 1) / 2; ++j) {
        const double angle = (j * (2 * pi) + pi) * (.5 / order);
        const Complex pole{-std::sin(angle), std::cos(angle)};
        Roots roots{}, zeros{};
        roots.count = std::abs(f(pole.imag())) > epsilon ? 2U : 1U;
        roots.values[0].value = pole;
        if (roots.count == 2)
            roots.values[1].value = std::conj(pole);
        zeros.count = roots.count;
        for (std::size_t i = 0; i < zeros.count; ++i)
            zeros.values[i].infinite = true;
        float damping = f(-pole.real());
        double norm = 1;
        const bool lastFraction = order != std::floor(order) && j == (ceiling + 1) / 2 - 1;
        if (lastFraction && gainShape) {
            const double remaining = std::pow(factor, -(ceiling - (ceiling % 2 != 0 ? 1 : 2))) * baseGain;
            if (ceiling % 2 != 0) {
                roots.count = zeros.count = 1;
                roots.values[0] = {{-1, 0}, false};
                zeros.values[0] = {{-remaining, 0}, false};
                if (shape == 2 || (shape >= 5 && shape <= 7))
                    roots.values[0].value = {-1 / remaining, 0};
                if (shape == 6)
                    norm = 1 / remaining;
                damping = 1;
            } else {
                const double a = 1 / std::sqrt(remaining);
                const double loga = std::log2(std::sin((.5 / ceiling) * (j * (2 * pi) + pi)) * (a + a));
                const double inverseRemaining = 1 / remaining, logb = std::log2(inverseRemaining + 1);
                const double linear = std::pow(2.0, f(fraction) * (loga - logb) + logb);
                const double disc = linear * linear - inverseRemaining * 4;
                const double real = std::sqrt((std::abs(disc) + disc) * .5) * .5;
                const double imag = std::sqrt((std::abs(disc) - disc) * .5) * .5;
                const Complex first{linear * -.5 + real, imag}, second{linear * -.5 - real, -imag};
                const auto zFirst = reciprocal(first), zSecond = reciprocal(second);
                roots.count = zeros.count = 2;
                if (imag > epsilon) {
                    roots.values[0] = {first, false};
                    roots.values[1] = {std::conj(first), false};
                    zeros.values[0] = {zFirst, std::abs(f(zFirst.imag())) <= epsilon};
                    zeros.values[1] = std::abs(f(zFirst.imag())) > epsilon ? Root{std::conj(zFirst), false}
                                                                           : Root{zFirst, false};
                } else {
                    roots.values[0] = {factor > 1 ? first : second, false};
                    roots.values[1] = {factor > 1 ? second : first, false};
                    zeros.values[0] = {factor > 1 ? zFirst : zSecond, false};
                    zeros.values[1] = {factor > 1 ? zSecond : zFirst, false};
                }
                norm = f(shape == 6 ? inverseRemaining : 1);
            }
        } else if (lastFraction) {
            const double value = ceiling % 2 != 0 ? 1 / runningNorm
                                                  : (pole.real() * pole.real() + pole.real() * pole.real()) *
                                                        (1 / runningNorm);
            const double extra = value - 1 >= 0 ? std::sqrt(value - 1) : 0;
            norm = std::abs(f(extra)) > epsilon ? extra : 1;
            const Root zero{{std::abs(f(extra)) > epsilon ? -1 / extra : 0, 0},
                            std::abs(f(extra)) <= epsilon};
            if (ceiling % 2 != 0) {
                roots.count = zeros.count = 1;
                roots.values[0] = {{-1, 0}, false};
                zeros.values[0] = zero;
                damping = 1;
            } else {
                zeros.values[0] = {{}, true};
                zeros.values[1] = zero;
            }
        } else if (gainShape) {
            for (std::size_t i = 0; i < roots.count; ++i) {
                zeros.values[i] = {roots.values[i].value * factor, false};
                if (shape == 2 || (shape >= 5 && shape <= 7))
                    roots.values[i].value *= 1 / factor;
            }
            if (shape == 6)
                norm = 1 / (std::abs(f(roots.values[0].value.imag())) > epsilon ? factor * factor : factor);
        } else {
            runningNorm *= .25 / (pole.real() * pole.real());
        }
        const int conv = conversion[static_cast<std::size_t>(shape)];
        if (conv == 1 || conv == 3) {
            Complex numProduct{1, 0}, denProduct{1, 0};
            for (std::size_t i = 0; i < roots.count; ++i) {
                const auto p = roots.values[i].value;
                auto &z = zeros.values[i];
                if (z.infinite) {
                    z = {};
                } else if (z.value != Complex{}) {
                    const auto v = z.value;
                    numProduct = {-v.real() * numProduct.real() + v.imag() * numProduct.imag(),
                                  -v.imag() * numProduct.real() - v.real() * numProduct.imag()};
                    z.value = reciprocal(v);
                }
                denProduct = {-p.real() * denProduct.real() + p.imag() * denProduct.imag(),
                              -p.imag() * denProduct.real() - p.real() * denProduct.imag()};
                roots.values[i].value = reciprocal(p);
            }
            norm = (numProduct.real() / denProduct.real()) * norm;
        }
        if (conv == 2 || conv == 3) {
            float gn = 1, unused = 1;
            zeros = bandRoots(zeros, bandwidth, gn);
            norm = gn * norm;
            roots = bandRoots(roots, bandwidth, unused);
        }
        if (conv == 4) {
            for (std::size_t i = 0; i < roots.count; ++i) {
                const auto p = roots.values[i].value;
                zeros.values[i] = {{-p.real(), p.imag()}, false};
            }
            norm = 1;
        }
        const float rootGain =
            f(std::pow(norm, static_cast<double>(f(1.f / f(static_cast<double>(roots.count))))));
        for (std::size_t k = 0; k < roots.count; k += 2) {
            const auto count = std::min<std::size_t>(2, roots.count - k);
            auto n = polynomial(zeros, k, count);
            auto d = polynomial(roots, k, count);
            const double ng = count == 2 ? static_cast<double>(f(rootGain * rootGain)) : rootGain;
            for (auto &v : n)
                v *= ng;
            if (shape == 8) {
                const double normalized = f(1 / powerAtOne(n, d));
                for (auto &v : n)
                    v *= normalized;
            }
            float sectionDamping = damping;
            if (j == 0 && k == 0 && !(shape >= 7 && shape <= 10) &&
                (std::abs(f(n[0])) > epsilon || std::abs(f(d[0])) > epsilon)) {
                sectionDamping = std::max(f(damping * qFactor), qFloor);
                if (std::abs(f(n[1])) > epsilon)
                    n[1] = static_cast<double>(std::max(f(std::abs(n[1]) * qFactor), qFloor)) *
                           std::copysign(1.0, n[1]);
                d[1] = static_cast<double>(std::max(f(std::abs(d[1]) * qFactor), qFloor)) *
                       std::copysign(1.0, d[1]);
            }
            if (!out.append({n, d, roots.count <= 2 ? -1 : static_cast<int>(k / 2), sectionDamping, 1}))
                return false;
        }
    }
    return true;
}
struct Magnitude {
    Polynomial n{}, d{};
    bool quadratic = true;
    double power(double omega) const noexcept {
        const double x2 = omega * omega, x4 = x2 * x2;
        return std::max(0.0, ((x4 * n[0] + x2 * n[1]) + n[2]) / ((x4 * d[0] + x2 * d[1]) + d[2]));
    }
};
Magnitude magnitude(const Analog &s, double w) noexcept {
    const bool quadratic = std::abs(f(s.n[0])) > epsilon || std::abs(f(s.d[0])) > epsilon;
    const double w2 = w * w;
    const auto cache = [w2, quadratic](const Polynomial &p) {
        if (!quadratic)
            return Polynomial{0, p[1] * p[1], p[2] * p[2] * w2};
        const double constant = p[2] * w2;
        return Polynomial{p[0] * p[0], (p[1] * p[1] - (p[0] + p[0]) * p[2]) * w2, constant * constant};
    };
    return {cache(s.n), cache(s.d), quadratic};
}
struct Extrema {
    std::array<double, 2> values{};
    std::size_t count = 0;
};
Extrema extrema(const Magnitude &m) noexcept {
    Extrema result{};
    if (!m.quadratic)
        return result;
    double a = m.n[0] * m.d[1] - m.n[1] * m.d[0];
    double b = m.d[2] * m.n[0] - m.n[2] * m.d[0];
    b = b + b;
    const bool wasQuadratic = a != 0;
    if (!wasQuadratic) {
        a = b;
        b = 0;
    }
    const double c = m.d[2] * m.n[1] - m.n[2] * m.d[1];
    const double discriminant = b * b - a * 4 * c;
    if (discriminant < 0 || a == 0)
        return result;
    const double halfInverse = .5 / a, rootTerm = std::sqrt(discriminant) * halfInverse;
    const double center = -(halfInverse * b);
    for (double candidate : {center + rootTerm, center - rootTerm}) {
        if (wasQuadratic && candidate > 0)
            candidate = std::sqrt(candidate);
        if (candidate > 0)
            result.values[result.count++] = candidate;
    }
    if (result.count == 2 && result.values[0] > result.values[1])
        std::swap(result.values[0], result.values[1]);
    return result;
}
bool anchors(const Analog &s, double w, int shape, float qinv, filters::MatchRequest &r) noexcept {
    const auto m = magnitude(s, w);
    const double dc = m.n[2] / m.d[2];
    const auto highIndex = m.quadratic ? 0U : 1U;
    const double hf = m.n[highIndex] / m.d[highIndex];
    auto retained = extrema(m);
    const double W = s.scale * w;
    const double tolerance = shape == 7 && s.kind == 0 ? (W < .5 ? 1e-4 : 1e-5) : 1e-6;
    for (std::size_t i = retained.count; i != 0; --i) {
        const auto index = i - 1;
        const double value = m.power(retained.values[index]);
        if (!(value * tolerance < std::min(std::abs(value - dc), std::abs(value - hf)))) {
            if (index == 0 && retained.count == 2)
                retained.values[0] = retained.values[1];
            --retained.count;
        }
    }
    for (std::size_t i = 0; i < retained.count; ++i)
        retained.values[i] = std::min(retained.values[i], pi);
    const bool shelf = shape == 1 || shape == 2 || shape == 4 || shape == 5 || shape == 6;
    if (retained.count == 2 && shelf)
        std::swap(retained.values[0], retained.values[1]);
    const double E = retained.count != 0 ? retained.values[0] : std::min(W, pi);
    const double bias0 = clamp(E * (1 / pi) - .8, 0, .2);
    const float bias = f(bias0 * bias0 * 25);
    double cap1 = 0x1.8a14d57b373dfp+1, cap2 = 0x1.69e9565708efcp+1, cap3 = 0x1.881c68e4d6f74p+1;
    double nyq = 0, a = E, b = retained.count == 2 ? retained.values[1] : 0, c = 0;
    float adjustedDamping = s.damping;
    if (shape >= 7 && shape <= 10)
        adjustedDamping = f(((static_cast<double>(qinv) * .5) / rootHalf) * adjustedDamping);
    if (!m.quadratic) {
        a = 2.827433388230814;
        b = .01413716694115407;
        c = .2827433388230814;
    } else if (shape == 7) {
        const float ratioBase = f(1.4142135381698608 / qinv);
        float ratio = std::min(1.f, ratioBase);
        if (s.kind == -1) {
            nyq = clamp(E * 1.2, 2.827433388230814, pi);
            const double blend = clamp(f(static_cast<double>(bias) * bias * .25 + adjustedDamping), .1f, .8f);
            b = (1 - blend) * E;
            c = (1 - blend * .05) * E;
        } else {
            nyq = clamp(E * 1.8, 2.607521902479528, pi);
            const double power = std::max(std::pow(.5, qinv * .5), .05);
            const double proposed = s.kind == 0 ? W * power : W / power;
            const bool low = ratio < .3f;
            ratio = std::max(ratio, .3f);
            if (low || retained.count == 0)
                a = std::min(proposed, 1.5707963267948966);
            else if (retained.count == 2)
                a = std::max(W * (s.kind == 1 ? .5 : .05), E);
            else if (retained.count == 1)
                a = std::min(W, E) * ratio;
            c = static_cast<double>(f(ratio * ratio)) * .999 * a;
            const float inverseRatio = std::min(1.f, f(1.f / ratioBase));
            b = (.995 - inverseRatio * .5) * c;
            if (s.kind == 1) {
                if ((!low && retained.count != 0) || ratioBase >= .1)
                    b *= 1 - static_cast<double>(f(bias * bias)) * .1;
                else {
                    const float t = f(ratioBase * 10.f);
                    c = (c - a * .8) * t + a * .8;
                    b = (b - a * .1) * t + a * .1;
                }
                cap1 = 3.1353094682826135;
            } else {
                cap1 = 3.1101767270538954;
            }
            cap3 = cap1 * .9995;
            cap2 = cap1 * .99;
        }
    } else if (shape == 8 || shape == 10) {
        const float ratioBase = f(1.4142135381698608 / qinv);
        const float ratio = std::min(1.f, ratioBase);
        if (shape == 10) {
            if (s.kind != -1) {
                nyq = clamp(E * 1.8, 2.607521902479528, pi);
                cap1 = 0x1.921569e860335p+1;
                cap2 = 0x1.8e10145e5f3d1p+1;
                cap3 = 0x1.91ae7af42ce78p+1;
                double proposed = E;
                if (s.kind == 1)
                    proposed = std::min((.9998 - static_cast<double>(f(bias * bias)) * .0005) * E,
                                        3.141278494324434);
                else if (s.kind == 0 && retained.count == 1)
                    proposed = std::max(std::pow(.5, qinv * .5), .1) * W;
                a = std::max(ratio * proposed, W * .5);
                c = std::max(static_cast<double>(f(ratio * ratio)) * .999 * a, W * .02);
                const float inverseRatio = std::min(1.f, f(1.f / ratioBase));
                b = std::max((.995 - static_cast<double>(f(inverseRatio * inverseRatio)) * .9) * c, W * .001);
            } else {
                a = std::min((.9998 - static_cast<double>(f(bias * bias)) * .001) * E, cap1);
                nyq = clamp(a * 1.2, 2.9845130209103035, pi);
                cap3 = cap1 * .999;
                c = a * .999;
                b = a * .01;
            }
        } else {
            nyq = s.kind == -1 ? clamp(E * 1.25, 2.670353755551324, pi)
                               : clamp(E * 1.5, 2.827433388230814, 3.1101767270538954);
            c = (.999 - (1 - static_cast<double>(ratio)) * (1 - static_cast<double>(ratio)) * .5) * E;
            b = E * .01;
        }
    } else if (shelf) {
        const double blend = clamp(f(static_cast<double>(bias) * bias * .25 + adjustedDamping), .1f, .8f);
        if (retained.count == 2 && E < 2.9845130209103035 && shape != 6) {
            const double anchor = std::max(E, .7);
            nyq = std::min(
                pi, std::max((pi - E) * .1 + E, anchor * anchor * .06157521601035995 + 2.419026343264141));
            const double separation = clamp(f(f(E - b) - .15f), 0, 1.f);
            cap1 -= static_cast<double>(f(f(separation) * .15f)) * pi;
            cap2 = cap1 * .95;
            cap3 = cap2 * .995;
            c = (1 - blend * .05) * b;
        } else if (retained.count != 2 && shape == 6) {
            a = std::min(W, 1.8849555921538759);
            b = a * .05;
            c = a * .5;
        } else {
            a = retained.count == 2 ? b : W * .5;
            b = (1 - blend) * a;
            c = (1 - blend * .05) * a;
        }
    } else if (shape == 0) {
        c = retained.count == 1 ? std::max(W * .5, E) : W * .5;
        a = W;
        b = c * .5;
    } else if (shape == 3) {
        if (retained.count != 0) {
            float z = f(E * .44209706414415373 - .41666666666666663);
            z = f(static_cast<double>(f(z * z)) * .2 + .785);
            if (z > .96)
                z = f((nyq - .96) + nyq);
            nyq = std::min(pi, std::max(std::min(E, pi), z * pi));
        }
        const float dampingTerm = f(f(f(adjustedDamping * adjustedDamping) * 4.f) - 2.f);
        const double poleScale = dampingTerm < 0 ? std::max(.5, std::sqrt(dampingTerm * -.5)) : .5;
        const double mix = clamp(f(dampingTerm * -.5f), 0, 1.f);
        const double proposed = W / ((poleScale - 1) * mix + 1);
        const double biasFrequency = clamp(proposed - pi, 0, .9424777960769379);
        a = std::min({proposed, pi - biasFrequency, mix * .9424777960769379 + 2.199114857512855});
        double factor = .2;
        if (dampingTerm >= 6.f)
            factor -= (std::min(dampingTerm, 20.f) - 6.0) * .012857142857142859;
        c = a * factor;
        b = a * .001;
    } else {
        return false;
    }
    r = {s.n,
         s.d,
         w,
         {std::min(a, cap1), std::min(b, cap2), std::min(c, cap3)},
         std::abs(f(nyq)) <= epsilon ? pi : nyq};
    return true;
}
bool bilinear(const Analog &s, double w, filters::Coefficients &output) noexcept {
    const float capped = f(std::min(w, static_cast<double>(3.1315927505493164f)));
    const double t = f(std::tan(static_cast<double>(f(capped * .5f))));
    filters::Coefficients candidate{};
    if (std::abs(f(s.n[0])) <= epsilon && std::abs(f(s.d[0])) <= epsilon) {
        const double inverse = 1 / (t * s.d[2] + s.d[1]);
        candidate = {(t * s.n[2] + s.n[1]) * inverse, (t * s.n[2] - s.n[1]) * inverse, 0,
                     (t * s.d[2] - s.d[1]) * inverse, 0};
    } else {
        const double dt = t * s.d[1], nt = t * s.n[1];
        const double dt2 = t * t * s.d[2], nt2 = t * t * s.n[2];
        const double db = dt2 + s.d[0], nb = s.n[0] + nt2;
        const double inverse = 1 / (db + dt);
        candidate = {(nb + nt) * inverse, (nt2 - s.n[0]) * (inverse + inverse), (nb - nt) * inverse,
                     (dt2 - s.d[0]) * (inverse + inverse), (db - dt) * inverse};
    }
    if (!filters::validCoefficients(candidate))
        return false;
    output = candidate;
    return true;
}
} // namespace

bool validBand(const Band &b) noexcept {
    return std::isfinite(b.frequencyHz) && b.frequencyHz >= 10 && b.frequencyHz <= 30000 &&
           std::isfinite(b.gainDb) && b.gainDb >= -30 && b.gainDb <= 30 && std::isfinite(b.q) &&
           b.q >= .025f && b.q <= 40 && std::isfinite(b.slope) && b.slope >= 0 && b.slope <= 9 &&
           static_cast<unsigned>(b.shape) <= 9 && static_cast<unsigned>(b.placement) <= 2;
}

Status build(double sampleRate, const Band &band, Cascade &output) noexcept {
    if (!std::isfinite(sampleRate) || sampleRate < 8000 || sampleRate > 384000 || !validBand(band))
        return Status::InvalidArgument;
    constexpr std::array<int, 10> shapes{7, 2, 3, 5, 0, 10, 8, 6, 12, 11};
    const int shape = shapes[static_cast<std::size_t>(band.shape)];
    if (shape == 12)
        return Status::UnsupportedConfiguration;
    int selected = static_cast<int>(f(band.slope + .5f));
    if (selected == 9 && (shape == 0 || shape == 3))
        return Status::UnsupportedConfiguration;
    selected = std::min(8, selected);
    constexpr std::array<int, 9> orders{1, 2, 3, 4, 5, 6, 8, 12, 16};
    int order = orders[static_cast<std::size_t>(selected)];
    if (shape == 7 || shape == 10)
        order = std::max(2, order);
    float mappedQ = 0;
    if (shape == 2 || shape == 5 || shape == 6) {
        const float logq = f(std::log(static_cast<double>(band.q)));
        const float exponent = f(f(logq * .13554251194000244f) + .5f);
        mappedQ = f(f(std::pow(32.0, exponent)) * .125f);
    } else {
        mappedQ = f(band.q * rootHalf);
    }
    const float hz = f(std::exp(static_cast<double>(f(std::log2(static_cast<double>(band.frequencyHz)))) *
                                std::numbers::ln2));
    const double w = f(static_cast<double>(f(hz + hz)) * pi / f(sampleRate));
    const float inverseQ = f(1.f / mappedQ);
    AnalogCascade analog{};
    if (!prototypes(order, shape, band.gainDb, mappedQ, analog))
        return Status::UnsupportedConfiguration;
    Cascade candidate{};
    for (std::size_t i = 0; i < analog.count; ++i) {
        if (shape == 11) {
            if (!bilinear(analog.sections[i], w, candidate.sections[i]))
                return Status::UnsupportedConfiguration;
        } else {
            filters::MatchRequest request{};
            if (!anchors(analog.sections[i], w, shape, inverseQ, request) ||
                filters::design(request, candidate.sections[i]) != filters::Status::Ok)
                return Status::UnsupportedConfiguration;
        }
        ++candidate.count;
    }
    output = candidate;
    return Status::Ok;
}
} // namespace adi::mix::key_eq
