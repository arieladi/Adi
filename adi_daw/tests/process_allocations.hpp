// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Include once in a standalone test executable.
#include <cstdlib>
#include <new>

namespace adi::test::process_allocations {
inline thread_local bool enabled = false;
inline thread_local int count = 0;

template<class Process>
int measure(Process process) noexcept {
    count = 0;
    enabled = true;
    process();
    enabled = false;
    return count;
}
}

void* operator new(std::size_t size) {
    if (adi::test::process_allocations::enabled) ++adi::test::process_allocations::count;
    if (void* result = std::malloc(size == 0 ? 1 : size)) return result;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return operator new(size); }
void operator delete(void* data) noexcept { std::free(data); }
void operator delete[](void* data) noexcept { std::free(data); }
void operator delete(void* data, std::size_t) noexcept { std::free(data); }
void operator delete[](void* data, std::size_t) noexcept { std::free(data); }
