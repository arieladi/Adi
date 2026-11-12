// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Dsp/MultirateKernels.h"

#if defined(ADIL_HAS_AVX1_KERNELS) && defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#endif

namespace adi::mix::limiter::multirate_detail
{
#if defined(ADIL_HAS_AVX1_KERNELS)
int upAvx(const float*, float*, int, const DoubleCoefficient*, std::size_t) noexcept;
int downAvx(const float*, float*, int, const DoubleCoefficient*, std::size_t) noexcept;
#endif

bool avxAvailable() noexcept
{
#if defined(ADIL_HAS_AVX1_KERNELS) && defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    int registers[4]{};
    __cpuid(registers, 0);
    if (registers[0] < 1) return false;
    __cpuidex(registers, 1, 0);
    constexpr auto required = (1 << 26) | (1 << 27) | (1 << 28);
    if ((registers[2] & required) != required) return false;
    return (_xgetbv(0) & 6) == 6;
#else
    return false;
#endif
}

Kernels selectKernels(bool enabled) noexcept
{
#if defined(ADIL_HAS_AVX1_KERNELS)
    static const auto available = avxAvailable();
    if (enabled && available) return {upAvx, downAvx};
#else
    static_cast<void>(enabled);
#endif
    return {};
}
}
