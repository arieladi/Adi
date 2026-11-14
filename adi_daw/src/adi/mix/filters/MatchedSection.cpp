// SPDX-License-Identifier: AGPL-3.0-or-later
#include "MatchedSection.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>

namespace adi::mix::filters {
namespace {
struct Magnitude {
    std::array<double, 3> n{}, d{};
    bool power(double omega, double &value) const noexcept {
        const double x2 = omega * omega;
        const double x4 = x2 * x2;
        const double numerator = (x4 * n[0] + x2 * n[1]) + n[2];
        const double denominator = (x4 * d[0] + x2 * d[1]) + d[2];
        if (!(denominator > 0) || !std::isfinite(numerator) || !std::isfinite(denominator))
            return false;
        value = std::max(0.0, numerator / denominator);
        return std::isfinite(value);
    }
};
bool finite(const std::array<double, 3> &a) noexcept {
    return std::all_of(a.begin(), a.end(), [](double x) { return std::isfinite(x); });
}
Magnitude magnitude(const MatchRequest &r) noexcept {
    const double w2 = r.angularFrequency * r.angularFrequency;
    const bool quadratic =
        std::abs(static_cast<float>(r.numerator[0])) > std::numeric_limits<float>::epsilon() ||
        std::abs(static_cast<float>(r.denominator[0])) > std::numeric_limits<float>::epsilon();
    const auto cache = [w2, quadratic](const std::array<double, 3> &p) {
        if (!quadratic)
            return std::array<double, 3>{0, p[1] * p[1], p[2] * p[2] * w2};
        const double constant = p[2] * w2;
        return std::array<double, 3>{p[0] * p[0], (p[1] * p[1] - (p[0] + p[0]) * p[2]) * w2,
                                     constant * constant};
    };
    return {cache(r.numerator), cache(r.denominator)};
}
} // namespace

bool validCoefficients(const Coefficients &c) noexcept {
    return std::isfinite(c.b0) && std::isfinite(c.b1) && std::isfinite(c.b2) && std::isfinite(c.a1) &&
           std::isfinite(c.a2) && std::abs(c.a2) < 1 && 1 + c.a1 + c.a2 > 0 && 1 - c.a1 + c.a2 > 0;
}

Status design(const MatchRequest &r, Coefficients &output) noexcept {
    if (!finite(r.numerator) || !finite(r.denominator) || !finite(r.points) ||
        !std::isfinite(r.angularFrequency) || r.angularFrequency <= 0 || !std::isfinite(r.highReference) ||
        r.highReference <= 0 || r.highReference > std::numbers::pi ||
        std::any_of(r.points.begin(), r.points.end(),
                    [](double x) { return x <= 0 || x >= std::numbers::pi; }))
        return Status::InvalidArgument;
    if (r.points[0] == r.points[1] || r.points[0] == r.points[2] || r.points[1] == r.points[2])
        return Status::UnsupportedConfiguration;
    const auto mag = magnitude(r);
    double g0sq = 0, ginfsq = 0;
    std::array<double, 3> g{};
    if (!mag.power(0, g0sq) || !mag.power(r.highReference, ginfsq))
        return Status::UnsupportedConfiguration;
    for (std::size_t i = 0; i < g.size(); ++i)
        if (!mag.power(r.points[i], g[i]))
            return Status::UnsupportedConfiguration;
    Coefficients candidate{};
    const double tolerance = g[0] * 1e-4;
    if (std::abs(g[0] - g0sq) < tolerance && std::abs(g[0] - ginfsq) < tolerance &&
        std::abs(g[0] - g[1]) < tolerance && std::abs(g[0] - g[2]) < tolerance) {
        candidate.b0 = std::sqrt(g[0]);
    } else {
        const double t0 = std::tan(r.points[0] * .5), t1 = std::tan(r.points[1] * .5),
                     t2 = std::tan(r.points[2] * .5);
        const double U = t0 * t0, V = t1 * t1, W = t2 * t2;
        const double g1 = g[0], g2 = g[1], g3 = g[2];
        const double g0 = std::sqrt(g0sq);
        double gi = std::sqrt(ginfsq);
        g0sq = g0 * g0;
        ginfsq = gi * gi;
        const double D =
            ((g2 - g3) * (g0sq - g1) * V - (g1 - g3) * (g0sq - g2) * U) * W + (g0sq - g3) * (g1 - g2) * U * V;
        if (!std::isfinite(D) || D == 0)
            return Status::UnsupportedConfiguration;
        double C = (((V - W) * ginfsq + (U - V) * g2 + (W - U) * g3) * g1 +
                    ((W - U) * g2 + (U - V) * g3) * ginfsq + (V - W) * g3 * g2) /
                   D;
        const double floor = .0025 / (std::sqrt(V) * std::sqrt(W));
        if (C < floor) {
            C = floor;
            const double dividend = (V - W) * g1 + (U - V) * g3 + (W - U) * g2;
            if (dividend == 0 || !std::isfinite(dividend))
                return Status::UnsupportedConfiguration;
            ginfsq = std::max(0.0, ((U * g3 - U * g2 + V * g2 - W * g3) * g1 + (W - V) * g3 * g2 + C * D) /
                                       dividend);
            gi = std::sqrt(ginfsq);
        }
        const double b0 = std::sqrt(C) * std::sqrt(U) * std::sqrt(V) * std::sqrt(W);
        const double v = V * gi - b0 * g0, u = U * gi - b0 * g0;
        const double den = (g1 - g2) * U * V;
        if (den == 0 || U * g1 == 0 || !std::isfinite(den))
            return Status::UnsupportedConfiguration;
        const double a1sq =
            std::max(0.0, (u * u * V * g2 - ((U * V - b0 * b0) * (U - V) * g2 + v * v * U) * g1) / den);
        const double b1sq = std::max(0.0, (a1sq * U - (U - b0) * (U - b0) * g1 + u * u) / (U * g1));
        const double a1 = std::sqrt(a1sq), b1 = std::sqrt(b1sq), divisor = 1 + b0 + b1;
        candidate = {(g0 * b0 + gi + a1) / divisor, -2 * (gi - g0 * b0) / divisor,
                     (gi - a1 + g0 * b0) / divisor, -2 * (1 - b0) / divisor, (1 + b0 - b1) / divisor};
    }
    if (!validCoefficients(candidate))
        return Status::UnsupportedConfiguration;
    output = candidate;
    return Status::Ok;
}

Update Designer::update(const MatchRequest &request) noexcept {
    if (prepared_ && request == request_)
        return Update::Unchanged;
    Coefficients candidate{};
    const auto status = design(request, candidate);
    if (status == Status::InvalidArgument)
        return Update::InvalidArgument;
    if (status != Status::Ok)
        return Update::UnsupportedConfiguration;
    request_ = request;
    coefficients_ = candidate;
    prepared_ = true;
    return Update::Changed;
}

bool DfiSection::setCoefficients(const Coefficients &coefficients) noexcept {
    if (!validCoefficients(coefficients))
        return false;
    coefficients_ = coefficients;
    return true;
}

float DfiSection::process(float input) noexcept {
    const double x = static_cast<double>(input);
    const double y = (((x * coefficients_.b0 + x1_ * coefficients_.b1) + x2_ * coefficients_.b2) -
                      y1_ * coefficients_.a1) -
                     y2_ * coefficients_.a2;
    x2_ = x1_;
    x1_ = x;
    y2_ = y1_;
    y1_ = y;
    return static_cast<float>(y);
}

void DfiSection::processInPlace(std::span<float> samples) noexcept {
    for (auto &sample : samples)
        sample = process(sample);
}

} // namespace adi::mix::filters
