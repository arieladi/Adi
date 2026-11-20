// SPDX-License-Identifier: MIT
#include "adi/dsp/eq_eight.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <new>
#include <numbers>
#include <vector>
namespace {
bool counting = false;
std::size_t allocations = 0;
} // namespace
void *operator new(std::size_t n) {
    if (counting)
        ++allocations;
    if (auto p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t n) { return operator new(n); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }

namespace {
using Core = adi::dsp::EqEight;
using C = Core;
int checks = 0, failures = 0;
void check(bool ok, const char *what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", what);
    }
}
constexpr double pi = std::numbers::pi;
std::vector<float> tone(double hz, double amplitude = 0.4) {
    std::vector<float> x(96000);
    for (std::size_t i = 0; i < x.size(); ++i)
        x[i] =
            static_cast<float>(amplitude * std::sin(2 * pi * hz * static_cast<double>(i) / 48000));
    return x;
}
std::vector<float> render(Core &c, const std::vector<float> &in, std::size_t block = 64) {
    std::vector<float> out(in.size()), right(in.size());
    for (std::size_t i = 0; i < in.size(); i += block)
        c.process(in.data() + i, in.data() + i, out.data() + i, right.data() + i,
                  std::min(block, in.size() - i));
    return out;
}
double bin(const std::vector<float> &x, double hz) {
    std::complex<double> sum = 0, phase = 1, step = std::polar(1., -2 * pi * hz / 48000);
    for (std::size_t i = 48000; i < x.size(); ++i) {
        sum += static_cast<double>(x[i]) * phase;
        phase *= step;
    }
    return 2 * std::abs(sum) / 48000;
}
void band(C &c, int which, int type, double freq, double db = 0, double q = .70710678,
          int channel = 0) {
    const int i = channel * 40 + which * 5;
    c.set(static_cast<C::Param>(i), 1);
    c.set(static_cast<C::Param>(i + 1), type);
    c.set(static_cast<C::Param>(i + 2), freq);
    c.set(static_cast<C::Param>(i + 3), db);
    c.set(static_cast<C::Param>(i + 4), q);
}
// DEV-22: Live 12.4's EQ Eight browser default, per curve (A, B) and band:
// on, type (the menu's order, Live's own), frequency. Every gain 0 dB, every
// Q 0.7071; Stereo, Edit A, Adaptive Q on, Scale 100 %, Output 0 dB, HQ off.
struct LiveBand { int on, type; double hz; };
constexpr LiveBand kLiveDefault[2][8] = {
    {{1, 2, 30}, {1, 3, 200}, {1, 3, 1000}, {1, 5, 5000},
     {0, 3, 100}, {0, 3, 10000}, {0, 3, 5000}, {0, 6, 18000}},
    {{0, 1, 40}, {0, 2, 200}, {1, 3, 100}, {1, 3, 500},
     {1, 3, 2000}, {1, 3, 10000}, {0, 5, 5000}, {0, 6, 18000}},
};
constexpr double kLiveGlobal[7] = {0, 0, 1, 100, 0, 0, 0};
double liveDefault(int id) {
    if (id >= 80) return kLiveGlobal[id - 80];
    const auto& b = kLiveDefault[id / 40][(id % 40) / 5];
    switch (id % 5) {
    case 0: return b.on;
    case 1: return b.type;
    case 2: return b.hz;
    case 3: return 0;
    default: return .70710678;
    }
}
void liveDefaults() {
    C eq;
    int wrong = 0;
    for (int id = 0; id < C::Count; ++id)
        if (std::abs(eq.value(static_cast<std::size_t>(id)) - liveDefault(id)) > 1e-6) {
            std::printf("  parameter %d: %g, Live %g\n", id, eq.value(static_cast<std::size_t>(id)), liveDefault(id));
            ++wrong;
        }
    check(wrong == 0, "every native default is Live 12.4's (DEV-22)");
    auto x = tone(1000, .1);
    C fresh;
    fresh.prepare(48000);
    check(render(fresh, x) == x, "Live's default curve (all gains 0 dB) is bit-exact unity");
}
void measurements() {
    auto x = tone(1000, .1);
    C flat;
    flat.prepare(48000);
    check(render(flat, x) == x, "zero-gain EQ is bit-exact unity");
    C peak;
    peak.set(C::AdaptiveQ, 0);
    band(peak, 0, C::Peak, 1000, 6);
    peak.prepare(48000);
    auto boosted = render(peak, x);
    check(bin(boosted, 7011) < 1e-7, "EQ far floor first");
    check(std::abs(bin(boosted, 1000) - .1 * std::pow(10., 6. / 20)) < 1e-5,
          "Surge peak reaches specified centre gain");
    band(peak, 1, C::Peak, 1000, 6);
    peak.prepare(48000);
    check(std::abs(bin(render(peak, x), 1000) - .1 * std::pow(10., 12. / 20)) < 1e-5,
          "identical bands sum in dB");
    peak.set(C::Scale, 0);
    peak.prepare(48000);
    check(render(peak, x) == x, "Scale zero neutralises gain bands");
    auto magnitude = [&](int type, double hz) {
        C c;
        c.set(C::AdaptiveQ, 0);
        band(c, 0, type, 1000, 6);
        c.prepare(48000);
        return bin(render(c, tone(hz, .1)), hz);
    };
    check(magnitude(C::LowShelf, 50) > .19 && magnitude(C::LowShelf, 10000) < .101,
          "low shelf boosts low end only");
    check(magnitude(C::HighShelf, 10000) > .19 && magnitude(C::HighShelf, 50) < .101,
          "high shelf boosts high end only");
    check(magnitude(C::Notch, 1000) < 1e-6, "notch rejects centre");
    const auto low12 = 20 * std::log10(magnitude(C::LowCut12, 200) / magnitude(C::LowCut12, 100));
    const auto low48 = 20 * std::log10(magnitude(C::LowCut48, 200) / magnitude(C::LowCut48, 100));
    const auto high12 =
        20 * std::log10(magnitude(C::HighCut12, 2000) / magnitude(C::HighCut12, 4000));
    const auto high48 =
        20 * std::log10(magnitude(C::HighCut48, 2000) / magnitude(C::HighCut48, 4000));
    std::printf("METRIC slopes low %.6f %.6f high %.6f %.6f\n", low12, low48, high12, high48);
    check(std::abs(low12 - 12) < .3 && std::abs(low48 - 48) < .4,
          "12/48 dB low-cut slopes measured one octave");
    check(high12 > 11 && high12 < 13 && high48 > 47 && high48 < 51,
          "12/48 dB high-cut slopes measured one octave");
    C stereo;
    stereo.set(C::Mode, 1);
    stereo.set(C::AdaptiveQ, 0);
    band(stereo, 0, C::Peak, 1000, 6);
    stereo.prepare(48000);
    std::vector<float> l(x.size()), r(x.size());
    stereo.process(x.data(), x.data(), l.data(), r.data(), x.size());
    check(bin(l, 1000) > bin(r, 1000) * 1.99, "LR curve affects only left");
    stereo.set(C::Mode, 2);
    stereo.prepare(48000);
    stereo.process(x.data(), x.data(), l.data(), r.data(), x.size());
    check(l == r && bin(l, 1000) > .199, "MS mid curve boosts correlated stereo");
    std::vector<float> anti = x;
    for (auto &v : anti)
        v = -v;
    stereo.prepare(48000);
    stereo.process(x.data(), anti.data(), l.data(), r.data(), x.size());
    check(std::abs(bin(l, 1000) - .1) < 1e-6, "MS side bypasses mid-only boost");
    C adapt;
    band(adapt, 0, C::Peak, 1000, 15, 1);
    adapt.set(C::AdaptiveQ, 0);
    adapt.prepare(48000);
    const auto wide = bin(render(adapt, tone(1500, .1)), 1500);
    adapt.set(C::AdaptiveQ, 1);
    adapt.prepare(48000);
    check(bin(render(adapt, tone(1500, .1)), 1500) < wide, "Adaptive Q tightens boosted peak");
    C controls;controls.set(C::AdaptiveQ,0);band(controls,0,C::Peak,1000,6);band(controls,1,C::Peak,1000,6);controls.set(C::Audition,1);controls.prepare(48000);
    check(std::abs(bin(render(controls,x),1000)-.1*std::pow(10.,6./20))<1e-5,"audition excludes other bands");
    controls.set(static_cast<C::Param>(0),0);controls.prepare(48000);check(render(controls,x)==x,"disabled audition band leaves dry signal");
    controls.set(C::Output,-6);controls.prepare(48000);check(std::abs(bin(render(controls,x),1000)-.1*std::pow(10.,-6./20))<1e-5,"global output gain applies independently");
    C hq;
    hq.set(C::HiQuality, 1);
    hq.prepare(48000);
    std::vector<float> impulse(128);
    impulse[0] = 1;
    auto delayed = render(hq, impulse);
    const auto at = std::max_element(delayed.begin(), delayed.end()) - delayed.begin();
    check(at == hq.latency() && at == 16, "2x FIR latency matches actual impulse peak");
    for (int quality : {0, 1}) {
        C base;
        band(base, 0, C::Peak, 1200, 9);
        base.set(C::HiQuality, quality);
        base.prepare(48000);
        const auto expected = render(base, x, 32);
        for (std::size_t block : {32u, 64u, 128u, 256u, 512u, 1024u, 2048u, 4096u}) {
            C same;
            band(same, 0, C::Peak, 1200, 9);
            same.set(C::HiQuality, quality);
            same.prepare(48000);
            check(render(same, x, block) == expected, "EQ stream identical across all block sizes");
        }
    }
    std::vector<float> input(4096, .1f), out(4096), right(4096);
    allocations = 0;
    counting = true;
    hq.process(input.data(), input.data(), out.data(), right.data(), input.size());
    counting = false;
    check(allocations == 0, "HQ EQ process allocates zero");
    for (int type = 0; type < 8; ++type) {
        C edge;
        band(edge, 0, type, 22000, 15, 18);
        edge.prepare(48000);
        const auto v = render(edge, x);
        check(std::all_of(v.begin(), v.end(), [](float f) { return std::isfinite(f); }),
              "extreme response remains finite");
    }
}
} // namespace
int main() {
    liveDefaults();
    measurements();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
