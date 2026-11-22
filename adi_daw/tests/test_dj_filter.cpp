// SPDX-License-Identifier: GPL-3.0-or-later
//
// The DJ Filter (DEV-22), on Auto Filter's cores. The reference is Auto
// Filter itself: at the matched settings (Type DJ, Control, Output, Mix) the
// DJ Filter's output equals Auto Filter's sample for sample, still and under
// automation, at every block size. Then the knob's meaning: 0 passes the
// signal, the left end low-passes and the right end high-passes. Parity with
// Live waits on a reading.
#include "adi/dsp/auto_filter.hpp"
#include "adi/dsp/dj_filter.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <numbers>
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
using D = adi::dsp::DjFilter;
using A = adi::dsp::AutoFilter;
constexpr double kRate = 48000;

std::vector<float> signal(std::size_t frames, double phase) {
    std::vector<float> x(frames);
    std::uint32_t s = 0x1234567u;
    for (std::size_t i = 0; i < frames; ++i) {
        s = s * 1664525u + 1013904223u;
        const double noise = static_cast<double>(s >> 8) / 16777216.0 - 0.5;
        x[i] = static_cast<float>(0.3 * std::sin(0.031 * static_cast<double>(i) + phase) + 0.2 * noise);
    }
    return x;
}

struct Setting {
    double filter, output, mix;
};

/// Both rendered in `block`-sized calls, with the settings moved at the
/// given frames (automation); the outputs are compared bit for bit.
bool matched(const std::vector<std::pair<std::size_t, Setting>>& moves, std::size_t block, std::size_t frames = 24000) {
    D dj;
    A af;
    af.set(A::Type, A::DJ);
    dj.prepare(kRate);
    af.prepare(kRate);
    const auto l = signal(frames, 0), r = signal(frames, 1.3);
    std::vector<float> dl(frames), dr(frames), al(frames), ar(frames);
    std::size_t next = 0;
    for (std::size_t at = 0; at < frames;) {
        while (next < moves.size() && moves[next].first <= at) {
            const auto& s = moves[next].second;
            dj.set(D::Filter, s.filter);
            dj.set(D::Output, s.output);
            dj.set(D::DryWet, s.mix);
            af.set(A::Control, s.filter);
            af.set(A::Output, s.output);
            af.set(A::Mix, s.mix);
            ++next;
        }
        std::size_t n = std::min(block, frames - at);
        if (next < moves.size()) {
            n = std::min(n, moves[next].first - at);
        }
        dj.process(l.data() + at, r.data() + at, dl.data() + at, dr.data() + at, n);
        af.process(l.data() + at, r.data() + at, al.data() + at, ar.data() + at, n);
        at += n;
    }
    return dl == al && dr == ar;
}

double level(const std::vector<float>& y, double hz) {
    std::complex<double> sum = 0;
    const std::size_t n = 24000, from = y.size() - n;
    for (std::size_t i = 0; i < n; ++i) {
        sum += static_cast<double>(y[from + i]) * std::polar(1.0, -2 * std::numbers::pi * hz * static_cast<double>(from + i) / kRate);
    }
    return 2 * std::abs(sum) / static_cast<double>(n);
}

void againstAutoFilter() {
    for (double f : {-100.0, -60.0, -12.5, 0.0, 7.0, 45.0, 100.0}) {
        check(matched({{0, {f, 0, 100}}}, 512), "Filter " + std::to_string(f) + ": the DJ Filter is Auto Filter's DJ type, sample for sample");
    }
    check(matched({{0, {-40, 6, 60}}}, 333), "with Output +6 dB and Dry/Wet 60 %, still equal");
    // Automation: the knob swept across the centre, Output and Mix moved.
    const std::vector<std::pair<std::size_t, Setting>> sweep = {{0, {-80, 0, 100}}, {3000, {-20, -3, 100}}, {6100, {0, -3, 80}},
                                                                {9000, {30, 0, 80}},  {15000, {95, 12, 40}}, {20000, {-95, 0, 100}}};
    bool all = true;
    for (std::size_t block : {1u, 64u, 333u, 4096u}) {
        all = all && matched(sweep, block);
    }
    check(all, "under automation across the centre, at every block size, still equal");
    // A plant: Auto Filter's LP type at the same Control is not the DJ Filter.
    D dj;
    A lp;
    lp.set(A::Type, A::LP);
    lp.set(A::Frequency, 2000);
    dj.set(D::Filter, -50);
    dj.prepare(kRate);
    lp.prepare(kRate);
    const auto x = signal(4800, 0);
    std::vector<float> a(4800), b(4800), c(4800), d(4800);
    dj.process(x.data(), x.data(), a.data(), b.data(), 4800);
    lp.process(x.data(), x.data(), c.data(), d.data(), 4800);
    check(a != c, "plant: another Auto Filter type does not match");
}

void knob() {
    std::vector<float> tone(48000), hi(48000);
    for (std::size_t i = 0; i < tone.size(); ++i) {
        const double t = static_cast<double>(i) / kRate;
        tone[i] = static_cast<float>(0.4 * std::sin(2 * std::numbers::pi * 100 * t) + 0.4 * std::sin(2 * std::numbers::pi * 8000 * t));
    }
    const auto through = [&](double f, std::vector<float>& out) {
        D dj;
        dj.set(D::Filter, f);
        dj.prepare(kRate);
        std::vector<float> r(tone.size());
        out.assign(tone.size(), 0.f);
        dj.process(tone.data(), tone.data(), out.data(), r.data(), tone.size());
    };
    std::vector<float> y;
    through(0, y);
    check(y == tone, "Filter 0 passes the signal untouched");
    through(-70, y);
    check(level(y, 8000) < 0.01 && level(y, 100) > 0.35, "the left side low-passes: 8 kHz gone, 100 Hz kept");
    through(70, y);
    check(level(y, 100) < 0.01 && level(y, 8000) > 0.35, "the right side high-passes: 100 Hz gone, 8 kHz kept");
    D d;
    d.set(D::Filter, 1000);
    d.set(D::Output, -99);
    check(d.value(D::Filter) == 100 && d.value(D::Output) == -24, "controls are clamped to their ranges");
}

void robustness() {
    D dj;
    dj.set(D::Filter, -30);
    std::vector<float> bad(512, std::numeric_limits<float>::quiet_NaN()), o(512), p(512);
    const auto x = signal(512, 0);
    counting = true;
    dj.process(bad.data(), bad.data(), o.data(), p.data(), bad.size());
    dj.set(D::Filter, 60);
    dj.set(D::DryWet, 30);
    dj.process(x.data(), x.data(), o.data(), p.data(), 512);
    counting = false;
    check(allocations == 0, "processing and moved controls allocate nothing");
    check(std::all_of(o.begin(), o.end(), [](float v) { return std::isfinite(v); }), "a NaN input leaves the output finite");
    D z;
    std::vector<float> left(4800, 0.25f), shared(4800, -0.5f), rightOut(4800);
    z.process(left.data(), shared.data(), shared.data(), rightOut.data(), 4800);
    check(rightOut.back() == -0.5f && shared.back() == 0.25f, "an output plane that is the other channel's input is read first");
}

}  // namespace

int main() {
    againstAutoFilter();
    knob();
    robustness();
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAILED" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
