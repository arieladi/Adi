// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/ui/spectrum_slope.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <new>

namespace {
thread_local bool audit = false, denyAllocation = false;
thread_local std::size_t allocations = 0, frees = 0;
int checks = 0, failures = 0;
void check(bool value, const char *label) {
    ++checks;
    if (!value) { ++failures; std::printf("FAIL %s\n", label); }
}
}
void *operator new(std::size_t size) {
    if (audit) {
        ++allocations;
        if (denyAllocation) throw std::bad_alloc();
    }
    if (auto *p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *p) noexcept { if (audit && p) ++frees; std::free(p); }
void operator delete[](void *p) noexcept { ::operator delete(p); }
void operator delete(void *p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void *p, std::size_t) noexcept { ::operator delete(p); }

int main() {
    using namespace adi::ui;
    constexpr std::array<std::array<double, 2>, 5> points{{{250., -9.}, {500., -4.5}, {1000., 0.}, {2000., 4.5}, {4000., 9.}}};
    for (const auto &point : points)
        check(spectrumDisplayOffset(SpectrumSlope::Tilt45, point[0]) == point[1] &&
                  spectrumDisplayOffset(SpectrumSlope::Flat, point[0]) == 0,
              "declared octave/pivot points are exact without changing Flat");
    check(std::isfinite(spectrumDisplayOffset(SpectrumSlope::Tilt45, std::numeric_limits<double>::denorm_min())) &&
              std::isfinite(spectrumDisplayOffset(SpectrumSlope::Tilt45, std::numeric_limits<double>::min())) &&
              std::isfinite(spectrumDisplayOffset(SpectrumSlope::Tilt45, std::numeric_limits<double>::max())),
          "positive extreme frequencies cannot underflow the logarithm argument or produce infinity");
    SpectrumSlopeCache cache;
    audit = true;
    for (int frame = 0; frame < 256; ++frame) cache.prepare(SpectrumSlope::Flat, 48000, 1024, 513);
    cache.reset();
    audit = false;
    check(allocations == 0 && frees == 0 && cache.offsets().empty(),
          "default Flat owner frames and empty reset perform zero C++ allocation/free even in MSVC Debug");
    allocations = frees = 0; audit = true;
    const bool changed = cache.prepare(SpectrumSlope::Tilt45, 64000, 1024, 513);
    audit = false;
    check(changed && allocations == 1 && frees == 0 && cache.offsets().size() == 513,
          "a real slope configuration prepares one bounded float allocation");
    const auto *storage = cache.offsets().data(); const auto old = cache.offsets()[32];
    allocations = frees = 0; audit = true;
    for (int frame = 0; frame < 256; ++frame) cache.prepare(SpectrumSlope::Tilt45, 64000, 1024, 513);
    audit = false;
    check(allocations == 0 && frees == 0 && cache.offsets().data() == storage,
          "unchanged tilted frames do not rebuild, allocate or free the prepared projection");
    bool rejected = false;
    allocations = frees = 0; audit = denyAllocation = true;
    try { cache.prepare(SpectrumSlope::Tilt45, 96000, 2048, 1025); }
    catch (const std::bad_alloc &) { rejected = true; }
    audit = denyAllocation = false;
    check(rejected && allocations == 1 && frees == 0 && cache.offsets().data() == storage &&
              cache.offsets().size() == 513 && cache.offsets()[32] == old &&
              !cache.prepare(SpectrumSlope::Tilt45, 64000, 1024, 513),
          "failed prepare retains the complete old offsets and metadata until a successful replacement");
    allocations = frees = 0; audit = true;
    cache.prepare(SpectrumSlope::Flat, 64000, 1024, 513);
    cache.reset();
    audit = false;
    check(allocations == 0 && frees == 1 && cache.offsets().empty(),
          "Flat/reset releases prepared storage without constructing an allocating container");
    check(cache.prepare(SpectrumSlope::Tilt45, std::numeric_limits<double>::max(), 1024, 513) &&
              std::isfinite(cache.offsets()[512]) && std::abs(static_cast<double>(cache.offsets()[512]) -
                  spectrumDisplayOffset(SpectrumSlope::Tilt45, std::numeric_limits<double>::max() / 2.)) < .001,
          "finite extreme sample-rate metadata does not overflow the per-bin frequency product");
    check(cache.prepare(SpectrumSlope::Flat, 48000, 1024, 513) && cache.offsets().empty(),
          "returning Flat drops extreme-metadata storage too");
    std::printf("SpectrumSlope: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
