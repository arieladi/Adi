// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/live_reverb.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
using R = adi::dsp::LiveReverb;
int checks = 0, failures = 0;
void check(bool v, const char *s) {
    ++checks;
    if (!v) {
        ++failures;
        std::printf("FAIL %s\n", s);
    }
}
std::vector<float> render(R &r, const std::vector<float> &in, std::size_t block = 64) {
    std::vector<float> l(in.size()), right(in.size());
    for (std::size_t i = 0; i < in.size(); i += block)
        r.process(in.data() + i, in.data() + i, l.data() + i, right.data() + i,
                  std::min(block, in.size() - i));
    return l;
}
double energy(const std::vector<float> &a, std::size_t first = 0) {
    double e = 0;
    for (std::size_t i = first; i < a.size(); ++i)
        e += static_cast<double>(a[i]) * a[i];
    return e;
}
double rt60(const std::vector<float> &x, double sr) {
    std::vector<double> decay(x.size());
    double e = 0;
    for (std::size_t i = x.size(); i > 0; --i) {
        e += static_cast<double>(x[i - 1]) * x[i - 1];
        decay[i - 1] = e;
    }
    double sx = 0, sy = 0, sxx = 0, sxy = 0, n = 0;
    for (std::size_t i = 0; i < x.size(); i += 64) {
        const double db = 10 * std::log10(std::max(decay[i] / decay[0], 1e-30));
        if (db > -5 || db < -35)
            continue;
        const double t = static_cast<double>(i) / sr;
        sx += t;
        sy += db;
        sxx += t * t;
        sxy += t * db;
        ++n;
    }
    return -60 / ((n * sxy - sx * sy) / (n * sxx - sx * sx));
}
void decay() {
    for (double sr : {48000., 96000.})
        for (double seconds : {0.5, 1.5, 3.}) {
            R r;
            r.prepare(sr);
            r.set(R::Mix, 100);
            r.set(R::Reflect, -60);
            r.set(R::Decay, seconds);
            std::vector<float> in(static_cast<std::size_t>(sr * (seconds * 3 + 1)));
            in[0] = 1;
            const auto out = render(r, in);
            const double measured = rt60(out, sr);
            std::printf("METRIC RT60 %.0f Hz requested %.2f measured %.5f s\n", sr, seconds,
                        measured);
            check(std::abs(measured / seconds - 1) < 0.15,
                  "Schroeder -5 to -35 dB RT60 within 15 percent");
        }
}
void controls() {
    std::vector<float> in(96000);
    in[0] = 1;
    R r;
    r.prepare(48000);
    r.set(R::Mix, 100);
    r.set(R::Predelay, 50);
    auto out = render(r, in);
    check(std::all_of(out.begin(), out.begin() + 2400, [](float v) { return v == 0; }),
          "Predelay keeps first 50 ms silent");
    check(out[2400] != 0, "first reflection starts at Predelay");
    r.prepare(48000);
    r.set(R::HighShelfOn, 1);
    r.set(R::HighShelfDecay, 0.1);
    auto damp = render(r, in);
    check(energy(damp, 24000) < energy(out, 24000), "high shelf reduces late energy");
    r.prepare(48000);
    r.set(R::HighShelfOn, 0);
    r.set(R::Stereo, 0);
    std::vector<float> l(in.size()), right(in.size());
    r.process(in.data(), in.data(), l.data(), right.data(), in.size());
    check(l == right, "zero Stereo folds wet image to mono");
    for (int density = 0; density < 3; ++density) {
        r.prepare(48000);
        r.set(R::Density, density);
        auto tail = render(r, in);
        check(energy(tail, 4800) > 1e-5, "every Density has a late tail");
    }
    r.prepare(48000);
    r.set(R::Reflect, -60);
    r.set(R::Freeze, 0);
    r.process(in.data(), in.data(), l.data(), right.data(), 4800);
    r.set(R::Freeze, 1);
    r.set(R::Cut, 1);
    r.set(R::Flat, 1);
    std::vector<float> silence(192000);
    auto frozen = render(r, silence);
    const double e1 = energy(std::vector<float>(frozen.begin() + 48000, frozen.begin() + 96000)),
                 e2 = energy(std::vector<float>(frozen.begin() + 144000, frozen.end()));
    check(e2 > e1 * 0.8 && e2 < e1 * 1.2, "Freeze Flat sustains late energy");
}
void freezeAndFilters() {
    std::vector<float> impulse(4800);
    impulse[0] = 1;
    std::vector<float> silence(96000), ones(96000, 0.25f);
    R a, b;
    a.prepare(48000);
    b.prepare(48000);
    for (auto *r : {&a, &b}) {
        r->set(R::Mix, 100);
        r->set(R::Reflect, -60);
        (void)render(*r, impulse);
        r->set(R::Freeze, 1);
        r->set(R::Cut, 1);
    }
    const auto quiet = render(a, silence), cut = render(b, ones);
    double difference = 0;
    for (std::size_t i = 0; i < quiet.size(); ++i)
        difference = std::max(difference, std::abs(static_cast<double>(quiet[i]) - cut[i]));
    check(difference < 0.001,
          "Freeze Cut rejects new diffuse input (early path remains at -60 dB)");
    b.set(R::Cut, 0);
    auto fed = render(b, ones);
    check(energy(fed) > energy(cut) * 2, "Freeze without Cut admits new diffuse input");
    a.set(R::Flat, 0);
    a.set(R::HighShelfOn, 1);
    a.set(R::HighShelfDecay, 0.1);
    a.set(R::LowShelfOn, 1);
    a.set(R::LowShelfDecay, 0.1);
    auto absorbed = render(a, silence);
    check(energy(absorbed, 48000) < energy(quiet, 48000) * 0.9,
          "Flat off applies decay shelves while frozen");
    std::vector<float> input(48000);
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] = static_cast<float>(
            0.2 * std::sin(2 * std::numbers::pi * 10000 * static_cast<double>(i) / 48000));
    R r;
    r.prepare(48000);
    r.set(R::Mix, 100);
    auto unfiltered = render(r, input);
    r.prepare(48000);
    r.set(R::HighCutOn, 1);
    r.set(R::HighCut, 1000);
    auto filtered = render(r, input);
    check(energy(filtered) < energy(unfiltered) * 0.03,
          "input high cut rejects high frequency before wet paths");
}
void partition() {
    std::vector<float> in(32768);
    for (std::size_t i = 0; i < in.size(); ++i)
        in[i] = static_cast<float>(
            0.2 * std::sin(2 * std::numbers::pi * 733 * static_cast<double>(i) / 48000));
    std::vector<float> expected;
    for (std::size_t block : {32u, 64u, 128u, 256u, 512u, 1024u, 2048u, 4096u}) {
        R r;
        r.prepare(48000);
        r.set(R::SpinOn, 1);
        r.set(R::ChorusOn, 1);
        r.set(R::HighShelfOn, 1);
        r.set(R::LowShelfOn, 1);
        auto out = render(r, in, block);
        if (expected.empty())
            expected = out;
        check(out == expected, "modulated reverb identical for all block sizes");
    }
    R r;
    r.prepare(48000);
    std::vector<float> l(in.size()), right(in.size());
    allocations = 0;
    counting = true;
    r.process(in.data(), in.data(), l.data(), right.data(), in.size());
    counting = false;
    check(allocations == 0, "reverb process allocates nothing");
}
} // namespace
int main() {
    decay();
    controls();
    freezeAndFilters();
    partition();
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
