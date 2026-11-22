// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live 12's Shaper modulator, the kernel (DEV-07's device side, ADR-0249 d8).
// Each property is checked against a number derived apart from the code: a
// straight segment's midpoint, a curved segment's value from its exponential
// law, a step, the period from the rate or the song's tempo. Parity with
// Live's Shaper waits on a Live 12.4 reading (reading 11).
#include "adi/dsp/shaper_modulator.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

namespace {
bool counting = false;
std::size_t allocations = 0;
int checks = 0, failures = 0;
void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}
}  // namespace
void* operator new(std::size_t n) {
    if (counting) {
        ++allocations;
    }
    if (void* p = std::malloc(n ? n : 1)) {
        return p;
    }
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {
using S = adi::dsp::ShaperModulator;
constexpr double kRate = 48000;

std::vector<double> render(S& shaper, std::size_t frames, std::size_t block, std::int64_t from = 0) {
    std::vector<double> out(frames);
    for (std::size_t at = 0; at < frames; at += block) {
        const auto n = std::min(block, frames - at);
        S::Position position;
        position.sample = from + static_cast<std::int64_t>(at);
        shaper.render(out.data() + at, n, position);
    }
    return out;
}

void breakpoints() {
    S s;
    const S::Point line[] = {{0, 0, 0}, {0.5, 1, 0}};
    check(s.setPoints(line, 2), "two points are a shape");
    check(std::abs(s.shapeAt(0.25) - 0.5) < 1e-15 && std::abs(s.shapeAt(0.75) - 0.5) < 1e-15,
          "a straight segment's midpoint, and the last point's segment running into the next cycle");
    // A curved segment: 8 x 0.5 = 4, so at u = 0.5 the value is expm1(2) / expm1(4).
    const S::Point bent[] = {{0, 0, 0.5}, {0.5, 1, 0}};
    s.setPoints(bent, 2);
    const double want = std::expm1(2.0) / std::expm1(4.0);
    check(std::abs(s.shapeAt(0.25) - want) < 1e-15, "a curve of 0.5 follows its exponential law");
    const S::Point steep[] = {{0, 0, -0.5}, {0.5, 1, 0}};
    s.setPoints(steep, 2);
    check(std::abs(s.shapeAt(0.25) - std::expm1(-2.0) / std::expm1(-4.0)) < 1e-15 && s.shapeAt(0.25) > 0.5,
          "a curve of -0.5 starts fast");
    // A step: two points at one time, the later given wins from that time on.
    const S::Point step[] = {{0, -1, 0}, {0.5, -1, 0}, {0.5, 1, 0}};
    s.setPoints(step, 3);
    check(s.shapeAt(0.4999) == -1 && s.shapeAt(0.5) == 1, "two points at one time make a step");
    // Unsorted points are sorted by time.
    const S::Point shuffled[] = {{0.5, 1, 0}, {0, 0, 0}};
    s.setPoints(shuffled, 2);
    check(std::abs(s.shapeAt(0.25) - 0.5) < 1e-15, "points are sorted by time");
    // One point is a constant; none keeps the shape; non-finite points are dropped.
    const S::Point one[] = {{0.3, 0.4, 0}};
    s.setPoints(one, 1);
    check(s.shapeAt(0.9) == 0.4 && !s.setPoints(nullptr, 0) && s.pointCount() == 1, "one point is a constant; none keeps it");
    const S::Point bad[] = {{0.1, std::nan(""), 0}, {0.2, 0.5, 0}};
    check(s.setPoints(bad, 2) && s.pointCount() == 1 && s.shapeAt(0.7) == 0.5, "a non-finite point is dropped");
    // The default: ADI's triangle through 0, 1, -1 until reading 11.
    S fresh;
    check(fresh.pointCount() == 3 && std::abs(fresh.shapeAt(0.25) - 1) < 1e-15 && std::abs(fresh.shapeAt(0.75) + 1) < 1e-15,
          "the default shape is a triangle through 0, 1 and -1");
    // A plant: a curve of 0.5 is not a straight line.
    s.setPoints(bent, 2);
    check(std::abs(s.shapeAt(0.25) - 0.5) > 0.1, "plant: a curved segment is not its straight line");
}

void ratesAndBlocks() {
    // Synced 1/4 at 120 BPM: a cycle of 24,000 samples; the default triangle
    // crests a quarter in.
    S s;
    s.set(S::Sync, 1);
    s.set(S::Division, 4);
    s.prepare(kRate);
    const auto out = render(s, 48000, 64);
    check(std::abs(out[6000] - 1) < 1e-12 && std::abs(out[30000] - 1) < 1e-12 && std::abs(out[18000] + 1) < 1e-12,
          "synced 1/4 at 120 BPM: a cycle of 24,000 samples");
    // Free at 4.8 Hz: a cycle of 10,000 samples.
    S f;
    f.set(S::Rate, 4.8);
    f.prepare(kRate);
    const auto free = render(f, 30000, 97);
    check(std::abs(free[2500] - 1) < 1e-9 && std::abs(free[12500] - 1) < 1e-9, "4.8 Hz: the crest every 10,000 samples");
    // The same bits at every block size, free and synced, curved, jittered, smoothed.
    const S::Point shape[] = {{0, 0, 0.3}, {0.2, 1, -0.6}, {0.5, -0.4, 0}, {0.5, 0.8, 0.9}, {0.9, -1, 0}};
    bool identical = true;
    for (bool sync : {false, true}) {
        S base;
        base.setPoints(shape, 5);
        base.set(S::Sync, sync ? 1 : 0);
        base.set(S::Rate, 3.3);
        base.set(S::Division, 2);
        base.set(S::Jitter, 0.5);
        base.set(S::Smooth, 0.3);
        base.prepare(kRate);
        const auto one = render(base, 30000, 1, 777);
        for (std::size_t block : {32u, 33u, 64u, 100u, 128u, 256u, 512u, 1000u, 1024u, 2048u, 4096u}) {
            S other;
            other.setPoints(shape, 5);
            other.set(S::Sync, sync ? 1 : 0);
            other.set(S::Rate, 3.3);
            other.set(S::Division, 2);
            other.set(S::Jitter, 0.5);
            other.set(S::Smooth, 0.3);
            other.prepare(kRate);
            identical = identical && render(other, 30000, block, 777) == one;
        }
    }
    check(identical, "free and synced, curved, stepped, jittered and smoothed: the same bits at block sizes 1 to 4,096");
    // A locate lands on the song's own phase.
    S a, b;
    a.set(S::Sync, 1);
    b.set(S::Sync, 1);
    a.prepare(kRate);
    b.prepare(kRate);
    const auto from = render(a, 4800, 64, 100000);
    const auto all = render(b, 104800, 64);
    check(std::equal(from.begin(), from.end(), all.begin() + 100000), "synced: a locate lands on the song's phase");
}

void allocation() {
    S s;
    const S::Point shape[] = {{0, 0, 0.3}, {0.2, 1, -0.6}, {0.9, -1, 0}};
    std::vector<double> buffer(512);
    S::Position position;
    counting = true;
    for (int i = 0; i < 100; ++i) {
        s.setPoints(shape, 3);
        position.sample = i * 512;
        s.render(buffer.data(), buffer.size(), position);
    }
    counting = false;
    check(allocations == 0, "setPoints and render allocate nothing");
}

}  // namespace

int main() {
    breakpoints();
    ratesAndBlocks();
    allocation();
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAILED" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
