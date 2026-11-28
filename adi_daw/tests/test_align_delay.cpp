// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live 12's Align Delay, the core (DEV-22). Each delay is checked against a
// number derived apart from the code: milliseconds times the rate, samples as
// given, a distance over the speed of sound at its temperature. Parity with
// Live's Align Delay waits on a Live 12.4 reading.
#include "adi/dsp/align_delay.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
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
using A = adi::dsp::AlignDelay;

/// Where an impulse at sample 0 comes out on each channel, in blocks of `block`.
std::pair<long, long> arrival(A& a, std::size_t frames = 60000, std::size_t block = 97) {
    std::vector<float> l(frames), r(frames), ol(frames), orr(frames);
    l[0] = 1.f;
    r[0] = 0.5f;
    for (std::size_t at = 0; at < frames; at += block) {
        const auto n = std::min(block, frames - at);
        a.process(l.data() + at, r.data() + at, ol.data() + at, orr.data() + at, n);
    }
    long left = -1, right = -1;
    for (std::size_t i = 0; i < frames; ++i) {
        if (ol[i] == 1.f && left < 0) {
            left = static_cast<long>(i);
        }
        if (orr[i] == 0.5f && right < 0) {
            right = static_cast<long>(i);
        }
    }
    return {left, right};
}

void modes() {
    // Time: 10 ms and 2.5 ms are 480 and 120 samples at 48 kHz.
    A time;
    time.set(A::LeftMs, 10);
    time.set(A::RightMs, 2.5);
    time.prepare(48000);
    auto [l, r] = arrival(time);
    check(l == 480 && r == 120, "Time: 10 ms and 2.5 ms are 480 and 120 samples at 48 kHz");
    // At 44.1 kHz, 10 ms is 441.
    A slower;
    slower.set(A::LeftMs, 10);
    slower.prepare(44100);
    check(arrival(slower).first == 441, "Time at 44.1 kHz: 10 ms is 441 samples");
    // Samples as given.
    A samples;
    samples.set(A::Mode, A::Samples);
    samples.set(A::LeftSamples, 123);
    samples.set(A::RightSamples, 7);
    samples.prepare(48000);
    auto [ls, rs] = arrival(samples);
    check(ls == 123 && rs == 7, "Samples: 123 and 7 samples, exactly");
    // Distance: 3.432 m at 20 degrees C (331.3 sqrt(1 + 20 / 273.15) =
    // 343.215 m/s) is 10.0 ms, 480 samples.
    A distance;
    distance.set(A::Mode, A::Distance);
    distance.set(A::LeftDistance, 3.432);
    distance.prepare(48000);
    const double c = 331.3 * std::sqrt(1 + 20 / 273.15);
    check(std::abs(distance.speedOfSound() - c) < 1e-12 && arrival(distance).first == std::lround(3.432 / c * 48000),
          "Distance: 3.432 m at 20 degrees C is the delay over the speed of sound (480 samples)");
    // Feet: 10 ft is 3.048 m.
    A feet;
    feet.set(A::Mode, A::Distance);
    feet.set(A::Units, 1);
    feet.set(A::LeftDistance, 10);
    feet.prepare(48000);
    check(arrival(feet).first == std::lround(3.048 / c * 48000), "Distance in feet: 10 ft is 3.048 m");
    // Colder air is slower: -30 degrees C, the same 3.432 m takes longer.
    A cold;
    cold.set(A::Mode, A::Distance);
    cold.set(A::LeftDistance, 3.432);
    cold.set(A::Temperature, -30);
    cold.prepare(48000);
    const double cc = 331.3 * std::sqrt(1 - 30 / 273.15);
    check(arrival(cold).first == std::lround(3.432 / cc * 48000), "Temperature: at -30 degrees C sound is slower");
    // Link: the right channel takes the left's delay.
    A linked;
    linked.set(A::LeftMs, 5);
    linked.set(A::RightMs, 1);
    linked.set(A::Link, 1);
    linked.prepare(48000);
    auto [ll, lr] = arrival(linked);
    check(ll == 240 && lr == 240, "Link: the right channel takes the left's delay");
    // The longest delays fit the line: 1 s, 48,000 samples, 300 m at -30.
    A longest;
    longest.set(A::LeftMs, 1000);
    longest.prepare(48000);
    check(arrival(longest, 60000).first == 48000, "the longest time delay, 1 s, fits");
    A farthest;
    farthest.set(A::Mode, A::Distance);
    farthest.set(A::LeftDistance, 300);
    farthest.set(A::Temperature, -30);
    farthest.prepare(48000);
    check(arrival(farthest, 60000).first == std::lround(300 / cc * 48000), "the longest distance, 300 m at -30, fits");
    // A plant: the next sample over is not the delay.
    check(arrival(time).first != 481, "plant: 481 is not 10 ms");
}

void reportsAndRobustness() {
    A a;
    a.set(A::LeftMs, 20);
    a.prepare(48000);
    check(a.latency() == 0, "the delay is the effect: no latency reported");
    // Every block size gives the same samples.
    std::vector<float> in(9000), first(9000), out(9000), right(9000);
    for (std::size_t i = 0; i < in.size(); ++i) {
        in[i] = static_cast<float>(std::sin(0.01 * static_cast<double>(i)));
    }
    bool identical = true;
    for (std::size_t block : {1u, 64u, 333u, 4096u}) {
        A b;
        b.set(A::LeftMs, 20);
        b.prepare(48000);
        for (std::size_t at = 0; at < in.size(); at += block) {
            const auto n = std::min(block, in.size() - at);
            b.process(in.data() + at, in.data() + at, out.data() + at, right.data() + at, n);
        }
        if (block == 1) {
            first = out;
        } else {
            identical = identical && out == first;
        }
    }
    check(identical, "every block size gives the same samples");
    // A host may hand an output plane that is an input plane (Pd does): with
    // the left output written into the right input's buffer, the right still
    // reads its own input (a found defect).
    {
        A z;
        z.prepare(48000);
        std::vector<float> left(64, 0.25f), shared(64, -0.5f), rightOut(64);
        z.process(left.data(), shared.data(), shared.data(), rightOut.data(), 64);
        check(std::all_of(rightOut.begin(), rightOut.end(), [](float v) { return v == -0.5f; }) &&
                  std::all_of(shared.begin(), shared.end(), [](float v) { return v == 0.25f; }),
              "an output plane that is the other channel's input plane is read before it is written");
    }
    // A non-finite sample is silence; processing allocates nothing.
    std::vector<float> bad(512, std::numeric_limits<float>::quiet_NaN()), o(512), p(512);
    counting = true;
    a.process(bad.data(), bad.data(), o.data(), p.data(), bad.size());
    a.set(A::LeftMs, 5);
    a.process(bad.data(), bad.data(), o.data(), p.data(), bad.size());
    counting = false;
    check(allocations == 0 && std::all_of(o.begin(), o.end(), [](float v) { return v == 0.f; }),
          "a non-finite input is silence, and nothing allocates");
}

}  // namespace

int main() {
    modes();
    reportsAndRobustness();
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAILED" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
