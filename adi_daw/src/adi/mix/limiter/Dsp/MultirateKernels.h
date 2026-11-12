// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstddef>

namespace adi::mix::limiter::multirate_detail
{
struct alignas(16) DoubleCoefficient
{
    double lanes[2];
};

using FirKernel = int (*)(const float*, float*, int, const DoubleCoefficient*, std::size_t) noexcept;

struct Kernels
{
    FirKernel up = nullptr;
    FirKernel down = nullptr;
};

bool avxAvailable() noexcept;
Kernels selectKernels(bool enabled = true) noexcept;
}
