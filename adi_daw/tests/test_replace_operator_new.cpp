// SPDX-License-Identifier: AGPL-3.0-or-later
// The shared allocation-counting header: every operator new form is counted
// exactly once and released by its matching delete (DF-E66).
#include "replace_operator_new.hpp"
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <new>
#include <vector>
namespace {
thread_local bool watch = false;
std::atomic<unsigned> allocations{0};
} // namespace
void adi::test::countReplacedNew() noexcept {
    if (watch) ++allocations;
}
namespace {
int checks = 0, failures = 0;
void check(bool ok, const char* label) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", label);
    }
}
struct alignas(64) Wide {
    float lanes[16];
};
constexpr auto page = std::align_val_t{4096};
// New-expressions whose result never escapes may be elided by the optimizer;
// every pointer goes through this sink first.
void* volatile sink = nullptr;
template <class T> T* keep(T* p) {
    sink = p;
    return p;
}
bool alignedTo(const void* p, std::size_t a) {
    return p != nullptr && reinterpret_cast<std::uintptr_t>(p) % a == 0;
}
// Runs one allocation under the watch and reports how many the hook counted.
unsigned counted(const std::function<void()>& allocate) {
    const unsigned before = allocations.load();
    watch = true;
    allocate();
    watch = false;
    return allocations.load() - before;
}
void everyForm() {
    check(counted([] { delete keep(new int(1)); }) == 1, "plain new counted once");
    check(counted([] { delete[] keep(new int[8]); }) == 1, "array new counted once");
    check(counted([] { delete keep(new (std::nothrow) int(1)); }) == 1, "nothrow new counted once");
    check(counted([] { delete[] keep(new (std::nothrow) int[8]); }) == 1, "nothrow array new counted once");
    check(counted([] {
              Wide* w = keep(new Wide{});
              check(alignedTo(w, alignof(Wide)), "aligned new honours the alignment");
              delete w;
          }) == 1,
          "aligned new counted once");
    check(counted([] {
              Wide* w = keep(new Wide[3]{});
              check(alignedTo(w, alignof(Wide)), "aligned array new honours the alignment");
              delete[] w;
          }) == 1,
          "aligned array new counted once");
    check(counted([] {
              Wide* w = keep(new (std::nothrow) Wide{});
              check(alignedTo(w, alignof(Wide)), "aligned nothrow new honours the alignment");
              delete w;
          }) == 1,
          "aligned nothrow new counted once");
    check(counted([] {
              Wide* w = keep(new (std::nothrow) Wide[3]{});
              check(alignedTo(w, alignof(Wide)), "aligned nothrow array new honours the alignment");
              delete[] w;
          }) == 1,
          "aligned nothrow array new counted once");
}
void explicitCalls() {
    // The operator functions themselves, each released through its own delete form.
    check(counted([] {
              void* p = ::operator new(0);
              check(p != nullptr, "zero-size new returns memory");
              ::operator delete(p, std::size_t{0});
          }) == 1,
          "zero-size new counted once");
    check(counted([] {
              void* p = ::operator new(24, std::nothrow);
              ::operator delete(p, std::nothrow);
          }) == 1,
          "nothrow new paired with nothrow delete");
    check(counted([] {
              void* p = ::operator new[](24, std::nothrow);
              ::operator delete[](p, std::nothrow);
          }) == 1,
          "nothrow array new paired with nothrow array delete");
    check(counted([] {
              void* p = ::operator new(100, page);
              check(alignedTo(p, 4096), "page-aligned new honours the alignment");
              ::operator delete(p, std::size_t{100}, page);
          }) == 1,
          "page-aligned new paired with sized aligned delete");
    check(counted([] {
              void* p = ::operator new[](0, page, std::nothrow);
              check(alignedTo(p, 4096), "zero-size aligned nothrow array new returns aligned memory");
              ::operator delete[](p, page, std::nothrow);
          }) == 1,
          "aligned nothrow array new paired with aligned nothrow array delete");
    check(counted([] {
              void* p = ::operator new(8, std::align_val_t{2});
              check(alignedTo(p, 2), "an alignment below a pointer's still returns memory");
              ::operator delete(p, std::align_val_t{2});
          }) == 1,
          "small explicit alignment counted once");
}
void libraryBuffer() {
    // The reported path: the standard library's temporary buffer for a stable
    // sort comes from a nothrow new and goes back through the library's delete.
    std::vector<int> values(1 << 16);
    for (std::size_t i = 0; i < values.size(); ++i) values[i] = static_cast<int>((i * 7919u) % 1021u);
    check(counted([&values] { std::stable_sort(values.begin(), values.end()); }) >= 1,
          "a stable sort's temporary buffer is counted");
    check(std::is_sorted(values.begin(), values.end()), "the stable sort sorted");
}
} // namespace
int main() {
    everyForm();
    explicitCalls();
    libraryBuffer();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
