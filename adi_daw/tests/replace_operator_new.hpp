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

namespace replaced_new {
inline void* plain(std::size_t n) noexcept { return std::malloc(n == 0 ? 1 : n); }
inline void* aligned(std::size_t n, std::align_val_t alignment) noexcept {
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
inline void releaseAligned(void* p) noexcept {
#if defined(_WIN32)
    _aligned_free(p);
#else
    std::free(p);
#endif
}
} // namespace replaced_new
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
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }

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
