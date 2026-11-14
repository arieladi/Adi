// SPDX-License-Identifier: AGPL-3.0-or-later
// Shared authored ADI-Clip processing. Preserve numerical evaluation and bank history.
#pragma once
#include <algorithm>
#include <cmath>
namespace adi::mix::clip::detail
{
// Saturation curves use float evaluation; fixed level trims retain double precision.
inline float bounded(float x, float lo, float hi) noexcept
{
    x = x < hi ? x : hi;
    return x > lo ? x : lo;
}
inline float rationalTanh(float x) noexcept
{
    const float q = x * x;
    return (((q + 378.f) * q + 17325.f) * q + 135135.f) * x /
           (((28.f * q + 3150.f) * q + 62370.f) * q + 135135.f);
}
inline float knee(float x, float k, float limit) noexcept
{
    x = bounded(x, -5, 5);
    float a = std::abs(x);
    if (a > k)
    {
        if (k == 1.f)
            k = .98888886f;
        const float width = limit - k;
        a = k + width * rationalTanh((a - k) / width);
    }
    return x < 0 ? -a : a;
}
inline float expand(float x) noexcept
{
    x = bounded(x, -5, 5);
    const float q = x * x;
    return -x * (((479249.f * q + 52785430.f) * q + 1640635900.f) * q + 11511340000.f) /
           (((18361.f * q - 3177720.f) * q + 277920700.f) * q - 11511340000.f);
}
struct CurveConstants
{
    const double up8 = std::pow(10., .4), up12 = std::pow(10., .6000000000000001),
                 down6 = std::pow(10., -.30000000000000004);
    const double siliconUp = std::pow(10., 1.2000000000000002), geTrim = std::pow(10., -.65),
                 siTrim = std::pow(10., -.86);
    const double tubeTrim = std::pow(10., .010000000000000002), tapeTrim = std::pow(10., .07500000000000001);
    const double speakerUp = std::pow(10., .30000000000000004), speakerDown = std::pow(10., -.2),
                 speakerTrim = std::pow(10., .05), guitarTrim = std::pow(10., .48);
};
inline const CurveConstants curveConstants;
template <int M> inline float shape(float x, float k, float limit) noexcept
{
    const auto &c = curveConstants;
    auto stage = [&](float v) { return bounded(knee(v, k, (float)c.up8), -(float)c.up8, 2.42f); };
    float y = 0;
    if constexpr (M == 0)
        y = knee(x, k, limit);
    if constexpr (M == 1)
    {
        x = bounded(x, -5, 5);
        const double xx = (double)x * x;
        float z = expand(expand(x)) + (float)(xx * xx) * std::abs(x) * .0014224f;
        y = rationalTanh(bounded(z, -5, 5));
    }
    if constexpr (M == 2)
        y = (float)((double)knee(stage(x), k, limit) * c.tubeTrim);
    if constexpr (M == 3)
        y = (float)((double)knee(stage(stage(x)), k, limit) * c.tapeTrim);
    if constexpr (M == 4 || M == 5)
    {
        float z = bounded((float)((double)x * c.up12), -1, 1);
        const float b = z + 2.42f;
        const float biased = (float)((double)b * b) * .33333334f - 1.72f;
        z = knee((biased + (M == 4 ? .088f : .14f)) - (M == 4 ? .3201335f : .3721335f), M == 4 ? .5f : 1.f,
                 1.f);
        z = bounded(z, -.8f, .99995f);
        z = (float)((double)z * c.down6);
        z = (float)((double)z * (M == 4 ? c.up12 : c.siliconUp));
        y = (float)((double)knee(stage(z), k, limit) * (M == 4 ? c.geTrim : c.siTrim));
    }
    if constexpr (M == 6)
    {
        float z = stage(x);
        z = stage((float)((double)z * c.speakerUp));
        y = (float)((double)knee((float)((double)z * c.speakerDown), k, limit) * c.speakerTrim);
    }
    if constexpr (M == 7)
    {
        const float z = bounded(knee(x, k, limit), -5, 5);
        y = (float)((double)(-((std::abs(z) * .07648f - z * .1135f) +
                               (rationalTanh(z) * .20716f - z * .4135f))) *
                    c.guitarTrim);
    }
    return bounded(y, -limit, limit);
}
} // namespace adi::mix::clip::detail
