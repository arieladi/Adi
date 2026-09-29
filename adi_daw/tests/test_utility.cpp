// SPDX-License-Identifier: MIT
#include "adi/dsp/utility.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <numbers>
#include <vector>
namespace {
bool counting = false;
std::size_t allocs = 0;
int checks = 0, failures = 0;
} // namespace
void *operator new(std::size_t n) {
    if (counting)
        ++allocs;
    if (auto *p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t n) { return operator new(n); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
namespace {
using U = adi::dsp::Utility;
void check(bool ok, const char *what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", what);
    }
}
std::array<float, 2> point(U &u, float l = 0.6f, float r = 0.2f) {
    std::array<float, 2> o{};
    u.process(&l, &r, &o[0], &o[1], 1);
    return o;
}
bool near(double a, double b) { return std::abs(a - b) < 1e-6; }
void routing() {
    U u;
    u.prepare(48000);
    auto o = point(u);
    check(near(o[0], .6) && near(o[1], .2), "default exact stereo unity");
    u.set(U::PhaseL, 1);
    o = point(u);
    check(near(o[0], -.6) && near(o[1], .2), "independent left polarity");
    u.set(U::PhaseL, 0);
    u.set(U::PhaseR, 1);
    o = point(u);
    check(near(o[0], .6) && near(o[1], -.2), "independent right polarity");
    u.set(U::PhaseR, 0);
    u.set(U::Channel, 0);
    o = point(u);
    check(near(o[0], .6) && o[0] == o[1], "Left copies left to both");
    u.set(U::Width, 0);
    u.set(U::MidSideMode, 1);
    u.set(U::MidSide, 100);
    o = point(u);
    check(near(o[0], .6) && o[0] == o[1], "Left bypasses Width and MidSide");
    u.set(U::Channel, 1);
    o = point(u);
    check(near(o[0], .2) && o[0] == o[1], "Right bypasses Width and MidSide");
    u.set(U::MidSideMode, 0);
    u.set(U::Width, 100);
    u.set(U::Channel, 2);
    o = point(u);
    check(near(o[0], .2) && near(o[1], .6), "Swap exchanges channels");
    u.set(U::Channel, 3);
    u.set(U::Width, 0);
    o = point(u);
    check(near(o[0], .4) && o[0] == o[1], "zero width sums mono");
    u.set(U::Width, 200);
    o = point(u);
    check(near(o[0], .8) && near(o[1], 0), "200 percent doubles side only");
    u.set(U::MidSideMode, 1);
    u.set(U::MidSide, -100);
    o = point(u);
    check(near(o[0], .4) && o[0] == o[1], "100M retains mid only");
    u.set(U::MidSide, 100);
    o = point(u);
    check(near(o[0], .2) && near(o[1], -.2), "100S retains opposite-polarity side only");
    u.set(U::Mono, 1);
    o = point(u);
    check(o[0] == o[1], "Mono overrides stereo side");
    U gain;
    gain.set(U::Gain, 35);
    gain.prepare(48000);
    o = point(gain, .01f, .01f);
    check(near(o[0], .01 * std::pow(10., 35. / 20)), "gain reaches +35 dB");
    gain.set(U::Gain, -120);
    gain.prepare(48000);
    o = point(gain);
    check(o[0] == 0 && o[1] == 0, "negative infinity endpoint is exact silence");
    gain.set(U::Gain, 0);
    gain.set(U::Balance, 100);
    gain.prepare(48000);
    o = point(gain);
    check(o[0] == 0 && near(o[1], .2), "full right balance preserves right and mutes left");
    gain.set(U::Balance, -100);
    gain.prepare(48000);
    o = point(gain);
    check(near(o[0], .6) && o[1] == 0, "full left balance");
    gain.set(U::Mute, 1);
    o = point(gain);
    check(o[0] == 0 && o[1] == 0, "Mute silences both outputs");
}
void filters() {
    for (double rate : {48000., 96000.}) {
        U u;
        u.prepare(rate);
        u.set(U::DC, 1);
        std::array<float, 2> o{};
        for (int i = 0; i < static_cast<int>(rate); ++i)
            o = point(u, .3f, .3f);
        check(std::abs(o[0]) < 1e-10, "DC offset decays at both sample rates");
        u.prepare(rate);
        u.set(U::DC, 0);
        u.set(U::BassMono, 1);
        u.set(U::BassFreq, 500);
        double low = 0, high = 0;
        for (int i = 0; i < static_cast<int>(rate); ++i) {
            const float x = static_cast<float>(.3 * std::sin(2 * std::numbers::pi * 5 * i / rate));
            o = point(u, x, -x);
            if (i > static_cast<int>(rate / 2))
                low += o[0] * o[0];
        }
        u.prepare(rate);
        for (int i = 0; i < static_cast<int>(rate); ++i) {
            const float x =
                static_cast<float>(.3 * std::sin(2 * std::numbers::pi * 8000 * i / rate));
            o = point(u, x, -x);
            if (i > static_cast<int>(rate / 2))
                high += o[0] * o[0];
        }
        check(low < high * .002, "Bass Mono rejects sub-bass side while preserving treble side");
        u.prepare(rate);
        bool exact = true;
        for (int i = 0; i < 10000; ++i) {
            float x = static_cast<float>(std::sin(i * .02));
            o = point(u, x, x);
            exact &= near(o[0], x) && near(o[1], x);
        }
        check(exact, "complementary split preserves the entire mid signal");
        u.set(U::BassAudition, 1);
        u.prepare(rate);
        double energy = 0;
        for (int i = 0; i < static_cast<int>(rate); ++i) {
            float x = static_cast<float>(.3 * std::sin(2 * std::numbers::pi * 8000 * i / rate));
            o = point(u, x, x);
            if (i > 1000)
                energy += o[0] * o[0];
        }
        check(energy / rate < 1e-5, "Bass audition isolates low band");
    }
}
void partitions() {
    std::vector<float> l(16384), r(l.size()), ol(l.size()), orr(l.size()), expected;
    for (std::size_t i = 0; i < l.size(); ++i) {
        l[i] = static_cast<float>(.3 * std::sin(i * .017));
        r[i] = static_cast<float>(.2 * std::cos(i * .037));
    }
    for (std::size_t block : {32u, 64u, 128u, 256u, 512u, 1024u, 2048u, 4096u}) {
        U u;
        u.prepare(48000);
        u.set(U::BassMono, 1);
        u.set(U::DC, 1);
        u.set(U::Width, 170);
        allocs = 0;
        counting = true;
        for (std::size_t i = 0; i < l.size(); i += block)
            u.process(l.data() + i, r.data() + i, ol.data() + i, orr.data() + i,
                      std::min(block, l.size() - i));
        counting = false;
        check(allocs == 0, "zero process allocations");
        if (expected.empty())
            expected = ol;
        check(ol == expected, "identical blocks 32 through 4096");
    }
    U u;
    u.prepare(48000);
    u.set(U::Mute, 1);
    u.process(l.data(), r.data(), l.data(), r.data(), l.size());
    check(std::all_of(l.begin(), l.end(), [](float v) { return v == 0; }),
          "in-place processing handles Pd buffer reuse");
}
} // namespace
int main() {
    routing();
    filters();
    partitions();
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
