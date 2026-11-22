// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

#if defined(_MSC_VER) && (defined(_M_ARM64) || defined(_M_ARM64EC))
#include <float.h>
#elif defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <xmmintrin.h>
#endif

namespace adi::audio {

struct FloatingPointControl {
    using Word = std::uint64_t;

#if defined(_MSC_VER) && (defined(_M_ARM64) || defined(_M_ARM64EC))
    static constexpr Word denormalMask = _MCW_DN;
    static constexpr Word flushDenormals = _DN_FLUSH;
    static Word read() noexcept { return _control87(0, 0) & _MCW_DN; }
    static void write(Word word) noexcept { _control87(static_cast<unsigned>(word), _MCW_DN); }
#elif defined(__aarch64__) || defined(__arm64__)
    // AArch64 FZ covers float and double inputs and results in the normal FP mode.
    static constexpr Word denormalMask = Word{1} << 24;
    static constexpr Word flushDenormals = denormalMask;
    static Word read() noexcept {
        Word word;
        asm volatile("mrs %0, fpcr" : "=r"(word));
        return word;
    }
    static void write(Word word) noexcept { asm volatile("msr fpcr, %0" : : "r"(word)); }
#elif defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
    static constexpr Word denormalMask = 0x8040U; // FTZ | DAZ
    static constexpr Word flushDenormals = denormalMask;
    static Word read() noexcept { return _mm_getcsr(); }
    static void write(Word word) noexcept { _mm_setcsr(static_cast<unsigned>(word)); }
#else
    static constexpr Word denormalMask = 0;
    static constexpr Word flushDenormals = 0;
    static Word read() noexcept { return 0; }
    static void write(Word) noexcept {}
#endif
};

class ScopedFlushDenormals {
public:
    ScopedFlushDenormals() noexcept {
        FloatingPointControl::write((saved_ & ~FloatingPointControl::denormalMask) |
                                    FloatingPointControl::flushDenormals);
    }
    ~ScopedFlushDenormals() { FloatingPointControl::write(saved_); }
    ScopedFlushDenormals(const ScopedFlushDenormals&) = delete;
    ScopedFlushDenormals& operator=(const ScopedFlushDenormals&) = delete;

private:
    const FloatingPointControl::Word saved_ = FloatingPointControl::read();
};

} // namespace adi::audio
