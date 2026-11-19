// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cmath>
// Original private experiment; never use under a non-nearest rounding mode.
// Polynomial estimates are accepted only inside the original float rounding
// cell; ambiguous cases retain the platform expf. Requires exhaustive tests.
inline float expandedNegativeExpNearest(float value) noexcept
{
    if (!(value >= -0.125f && value < -0.001f)) return std::exp(value);
    const double x=value, square=x*x;
    double minusOne{},bound{};
    if (value >= -0.005f)
    {
        minusOne=x+square*(0.5+x*(1.0/6.0));bound=2e-10;
    }
    else if (value >= -0.02f)
    {
        minusOne=x+square*(0.5+x*(1.0/6.0)+square*(1.0/24.0));bound=2e-10;
    }
    else if (value >= -0.04f)
    {
        minusOne=(x+square*(0.5+x*(1.0/6.0)))+(square*square)*((1.0/24.0)+x*(1.0/120.0));bound=2e-10;
    }
    else
    {
        minusOne=(x+square*(0.5+x*(1.0/6.0)))+(square*square)*((1.0/24.0)+x*(1.0/120.0)+square*(1.0/720.0));bound=2e-10;
    }
    const double estimate=1.0+minusOne;
    const float rounded=static_cast<float>(estimate);
    const double distance=std::abs(static_cast<double>(rounded)-estimate);
    if (distance < bound || distance > 0x1p-25-bound) return std::exp(value);
    return rounded;
}
