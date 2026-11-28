// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/ui/spectrum_slope.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <numbers>
#include <span>

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

namespace {
double hannAt(double fraction) {
    const auto sine = std::sin(std::numbers::pi * fraction);
    return sine * sine;
}

double directNarrow(std::span<const float> db, double low, double width) {
    const auto limited = [](double a, double b) {
        return ((a > 0 ? 1. : -1.) + (b > 0 ? 1. : -1.)) *
            std::min({std::abs(a), std::abs(b), .25 * std::abs(a + b)});
    };
    double weighted = 0., weights = 1e-300;
    for (int j = 0; j < 12; ++j) {
        const double u = j / 12.;
        const auto position = low + u * width;
        const auto bin = static_cast<std::size_t>(position);
        if (bin >= db.size()) break;
        const double a = db[bin == 0 ? 0 : bin - 1], b = db[bin];
        const double c = db[std::min(bin + 1, db.size() - 1)], d = db[std::min(bin + 2, db.size() - 1)];
        const double p = b - a, q = c - b, r = d - c;
        const auto startTangent = .5 * (limited(p, q) + .3 * p + .5 * q);
        const auto endTangent = .5 * (limited(q, r) + .5 * q + .3 * r);
        const auto t = position - static_cast<double>(bin), tt = t * t, ttt = tt * t;
        const auto cubic = (2. * ttt - 3. * tt + 1.) * b + (ttt - 2. * tt + t) * startTangent +
            (-2. * ttt + 3. * tt) * c + (ttt - tt) * endTangent;
        const auto weight = hannAt(u);
        weighted += weight * cubic;
        weights += weight;
    }
    return weighted / weights;
}

double directBroadMean(std::span<const float> db, double low, double high) {
    const auto magnitude = [&](std::size_t bin) { return std::pow(10., db[bin] / 20.); };
    double tail = 0., weighted = 0., weights = 0.;
    if (high >= static_cast<double>(db.size())) {
        for (std::size_t i = 0; i < 4; ++i)
            tail += .5 * magnitude(db.size() - 1 - i) - .25 * magnitude(db.size() - 5 - i);
    }
    const auto first = static_cast<std::size_t>(std::ceil(low));
    const auto last = static_cast<std::size_t>(std::floor(high));
    for (auto bin = first; bin <= last; ++bin) {
        const auto weight = hannAt((static_cast<double>(bin) - low) / (high - low));
        weighted += weight * (bin < db.size() ? magnitude(bin) : tail);
        weights += weight;
    }
    return weighted / weights;
}

std::array<double, 2> smoothingBounds(double bin, double octaves, double upperCorrection) {
    const auto low = bin * std::exp2(-1.5 * octaves);
    return {low, bin + (bin - low) * upperCorrection};
}

void smoothingNumerics() {
    using namespace adi::ui;
    const auto choiceFor = [](double octaves) {
        const auto found = std::ranges::find_if(kSpectrumSmoothingChoices, [=](const auto &choice) {
            return std::abs(choice.octaves - octaves) < 1e-6;
        });
        return static_cast<std::size_t>(found - kSpectrumSmoothingChoices.begin());
    };
    const auto off = choiceFor(0.), narrow = choiceFor(1. / 6.), broad = choiceFor(2.5);
    check(off < kSpectrumSmoothingChoices.size() && narrow < kSpectrumSmoothingChoices.size() &&
              broad < kSpectrumSmoothingChoices.size(), "smoothing exposes the numerical widths used by the projection");
    if (off >= kSpectrumSmoothingChoices.size() || narrow >= kSpectrumSmoothingChoices.size() ||
        broad >= kSpectrumSmoothingChoices.size()) return;

    constexpr int fft = 16384;
    constexpr double rate = 48000., minHz = 20., maxHz = rate / 2.;
    constexpr double toleranceDb = .0007;
    std::array<float, fft / 2 + 1> raw{};
    for (std::size_t i = 0; i < raw.size(); ++i) raw[i] = -22.f - .75f * static_cast<float>((i * 19) % 53);
    SpectrumSmoothing projection;
    check(!projection.prepare(off, minHz, maxHz, rate, fft, raw.size()) && projection.project(raw).empty(),
          "Off bypasses the smoothing projection entirely");
    check(projection.prepare(narrow, minHz, maxHz, rate, fft, raw.size()) &&
              projection.project(std::span<const float>(raw).first(raw.size() - 1)).empty(),
          "the selected smoothing rejects data that does not match its prepared bin count");
    const auto narrowPoints = projection.project(raw);
    const auto gridSize = static_cast<std::size_t>(std::ceil(16. * std::log2(maxHz / minHz) / std::sqrt(1. / 6.) + 1.));
    const auto spacing = std::pow(maxHz / minHz, 1. / static_cast<double>(gridSize - 1));
    check(narrowPoints.size() == gridSize && narrowPoints.front().frequencyHz == minHz &&
              narrowPoints.back().frequencyHz == maxHz &&
              std::abs(narrowPoints[1].frequencyHz / minHz - spacing) < 1e-12,
          "smoothing projects onto the width-dependent logarithmic frequency grid");
    bool narrowMatches = true, blendMatches = true;
    int narrowChecks = 0, blendChecks = 0;
    for (const auto &point : narrowPoints) {
        const auto [low, high] = smoothingBounds(point.frequencyHz * fft / rate, 1. / 6., 1.);
        const auto width = high - low;
        if (width >= 10.) continue;
        const auto narrowDb = directNarrow(raw, low, width);
        if (width <= 5.) {
            ++narrowChecks;
            narrowMatches &= std::abs(point.db - narrowDb) <= toleranceDb;
        } else {
            ++blendChecks;
            const auto broadDb = 20. * std::log10(std::abs(directBroadMean(raw, low, high)) + 1e-15);
            blendMatches &= std::abs(point.db - std::lerp(narrowDb, broadDb, (width - 5.) / 5.)) <= toleranceDb;
        }
    }
    check(narrowChecks > 5 && narrowMatches, "narrow smoothing matches a direct Hann-weighted cubic in dB");
    check(blendChecks > 5 && blendMatches, "intermediate widths blend cubic and magnitude smoothing in dB");

    raw.fill(-24.f);
    std::fill(raw.begin() + 880, raw.begin() + 2150, -11.f);
    std::fill(raw.begin() + 2500, raw.begin() + 4112, -55.f);
    std::fill(raw.end() - 8, raw.end() - 4, -1.f);
    std::fill(raw.end() - 4, raw.end(), -90.f);
    check(projection.prepare(broad, minHz, maxHz, rate, fft, raw.size()), "the widest valid smoothing prepares");
    const auto broadPoints = projection.project(raw);
    bool broadMatches = true, edgeMatches = true;
    int broadChecks = 0, signedEdgeChecks = 0;
    for (const auto &point : broadPoints) {
        if (point.frequencyHz < 6000.) continue;
        const auto [low, high] = smoothingBounds(point.frequencyHz * fft / rate, 2.5, 1.27);
        const auto mean = directBroadMean(raw, low, high);
        const auto reference = 20. * std::log10(std::abs(mean) + 1e-15);
        if (high < static_cast<double>(raw.size())) {
            ++broadChecks;
            broadMatches &= std::abs(point.db - reference) <= toleranceDb;
        } else {
            signedEdgeChecks += mean < 0. ? 1 : 0;
            edgeMatches &= std::abs(point.db - reference) <= toleranceDb;
        }
    }
    check(broadChecks > 3 && broadMatches, "large interior windows match the exact weighted magnitude mean");
    check(signedEdgeChecks > 3 && edgeMatches, "large edge windows keep signed continuation before converting to dB");
    const auto *storage = broadPoints.data();
    allocations = frees = 0; audit = true;
    const bool prepared = projection.prepare(broad, minHz, maxHz, rate, fft, raw.size());
    const auto repeated = projection.project(raw);
    audit = false;
    check(prepared && allocations == 0 && frees == 0 && repeated.data() == storage,
          "steady smoothing geometry and projection reuse existing storage");
    check(!projection.prepare(broad, 0., maxHz, rate, fft, raw.size()) && projection.project(raw).empty() &&
              !projection.prepare(kSpectrumSmoothingChoices.size(), minHz, maxHz, rate, fft, raw.size()) &&
              !projection.prepare(broad, minHz, maxHz, rate, fft, raw.size() - 1),
          "invalid smoothing metadata clears the prepared projection");
}
}

int main() {
    using namespace adi::ui;
    constexpr std::array<std::array<double, 2>, 5> points{{{250., -9.}, {500., -4.5}, {1000., 0.}, {2000., 4.5}, {4000., 9.}}};
    for (const auto &point : points)
        check(std::abs(spectrumDisplayOffset(SpectrumSlope::Tilt45, point[0]) - point[1]) < 1.e-12 &&
                  std::abs(spectrumDisplayOffset(SpectrumSlope::Flat, point[0])) < 1.e-12,
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
    smoothingNumerics();
    std::printf("SpectrumSlope: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
