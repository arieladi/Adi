// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#if defined(_M_X64) || defined(__SSE2__)
#include <xmmintrin.h>
#endif

namespace adi::mix::filters {
// Scope the existing x86 audio policy to the caller's thread and restore its
// original state. Other architectures retain their configured FP mode.
class FlushDenormals {
  public:
#if defined(_M_X64) || defined(__SSE2__)
    FlushDenormals() noexcept : previous_(_mm_getcsr()) { _mm_setcsr(previous_ | 0x8040U); }
    ~FlushDenormals() { _mm_setcsr(previous_); }
#else
    FlushDenormals() noexcept = default;
#endif
    FlushDenormals(const FlushDenormals &) = delete;
    FlushDenormals &operator=(const FlushDenormals &) = delete;

  private:
#if defined(_M_X64) || defined(__SSE2__)
    unsigned previous_;
#endif
};
} // namespace adi::mix::filters
