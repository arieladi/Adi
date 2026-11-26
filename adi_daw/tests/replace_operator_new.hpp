// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
// Replaces every global operator new and delete form with malloc and free (the
// platform's aligned allocator for the aligned forms) so a suite can count the
// allocations it cares about. Every allocating form calls the suite's hook once,
// so a library allocation through the nothrow or aligned form is counted, and it
// is freed by the delete that matches the new (a suite that replaced only some
// forms paired the runtime's nothrow new with its own free: a sanitizer's
// alloc-dealloc mismatch).
//
// Include it in exactly one translation unit of a test executable (the
// replacement functions are not inline) and define the hook there:
//     void adi::test::countReplacedNew() noexcept { if(adi::audio::onAudioThread) ++allocations; }
// The hook runs on every allocation from every thread, before the memory is taken.
//
// A suite that also counts releases defines ADI_TEST_COUNT_REPLACED_DELETE
// before the include and defines a second hook, which every delete form calls
// once, with the pointer, before the memory is freed (nullptr included):
//     void adi::test::countReplacedDelete(void* p) noexcept { if(audit && p) ++deallocations; }
#include <cstddef>
#include <cstdlib>
#include <new>
#if defined(_WIN32)
#include <malloc.h>
#else
#include <stdlib.h>
#endif

namespace adi::test {
void countReplacedNew() noexcept;
#if defined(ADI_TEST_COUNT_REPLACED_DELETE)
void countReplacedDelete(void* p) noexcept;
#endif

#if defined(__GNUC__) && !defined(__clang__)
#define ADI_TEST_NOINLINE __attribute__((noinline))
#else
#define ADI_TEST_NOINLINE
#endif

namespace replaced_new {
inline void counted([[maybe_unused]] void* p) noexcept {
#if defined(ADI_TEST_COUNT_REPLACED_DELETE)
    countReplacedDelete(p);
#endif
}
ADI_TEST_NOINLINE inline void release(void* p) noexcept {
    counted(p);
    std::free(p);
}
ADI_TEST_NOINLINE inline void* plain(std::size_t n) noexcept { return std::malloc(n == 0 ? 1 : n); }
ADI_TEST_NOINLINE inline void* aligned(std::size_t n, std::align_val_t alignment) noexcept {
    const std::size_t size = n == 0 ? 1 : n;
    std::size_t a = static_cast<std::size_t>(alignment);
    if (a < sizeof(void*)) a = sizeof(void*);
#if defined(_WIN32)
    return _aligned_malloc(size, a);
#else
    void* p = nullptr;
    return posix_memalign(&p, a, size) == 0 ? p : nullptr;
#endif
}
ADI_TEST_NOINLINE inline void releaseAligned(void* p) noexcept {
    counted(p);
#if defined(_WIN32)
    _aligned_free(p);
#else
    std::free(p);
#endif
}
} // namespace replaced_new
#undef ADI_TEST_NOINLINE
} // namespace adi::test

void* operator new(std::size_t n) {
    adi::test::countReplacedNew();
    if (void* p = adi::test::replaced_new::plain(n)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return operator new(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    adi::test::countReplacedNew();
    return adi::test::replaced_new::plain(n);
}
void* operator new[](std::size_t n, const std::nothrow_t& tag) noexcept { return operator new(n, tag); }
void operator delete(void* p) noexcept { adi::test::replaced_new::release(p); }
void operator delete[](void* p) noexcept { adi::test::replaced_new::release(p); }
void operator delete(void* p, std::size_t) noexcept { adi::test::replaced_new::release(p); }
void operator delete[](void* p, std::size_t) noexcept { adi::test::replaced_new::release(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { adi::test::replaced_new::release(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { adi::test::replaced_new::release(p); }

void* operator new(std::size_t n, std::align_val_t a) {
    adi::test::countReplacedNew();
    if (void* p = adi::test::replaced_new::aligned(n, a)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n, std::align_val_t a) { return operator new(n, a); }
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    adi::test::countReplacedNew();
    return adi::test::replaced_new::aligned(n, a);
}
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t& tag) noexcept {
    return operator new(n, a, tag);
}
void operator delete(void* p, std::align_val_t) noexcept { adi::test::replaced_new::releaseAligned(p); }
void operator delete[](void* p, std::align_val_t) noexcept { adi::test::replaced_new::releaseAligned(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { adi::test::replaced_new::releaseAligned(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { adi::test::replaced_new::releaseAligned(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept {
    adi::test::replaced_new::releaseAligned(p);
}
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept {
    adi::test::replaced_new::releaseAligned(p);
}
