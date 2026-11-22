// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live 12's Spectral Resonator, the core (DEV-22). The references are the
// header's laws: partial n at Frequency x n^(1 + 0.1 Stretch/100); after a
// burst of noise the output rings at the first partials (within a bin);
// the ring falls 60 dB in Decay (within 15 %, the fall being per hop); a
// tone between partials is not heard; Dry/Wet 0 is the input one window
// late, exactly. Parity with Live waits on a reading.
#include "adi/dsp/spectral_resonator.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
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
using R = adi::dsp::SpectralResonator;
constexpr double kRate = 48000;

void run(R& s, const std::vector<float>& x, std::vector<float>& l, std::vector<float>& r, std::size_t block = 500) {
    l.assign(x.size(), 0.f);
    r.assign(x.size(), 0.f);
    for (std::size_t at = 0; at < x.size(); at += block) {
        const auto n = std::min(block, x.size() - at);
        s.process(x.data() + at, x.data() + at, l.data() + at, r.data() + at, n);
    }
}
std::vector<float> noiseBurst(std::size_t total, std::size_t burst) {
    std::vector<float> x(total, 0.f);
    std::uint32_t s = 4242;
    for (std::size_t i = 0; i < burst; ++i) {
        s = s * 1664525u + 1013904223u;
        x[i] = static_cast<float>(0.5 * (static_cast<double>(s >> 8) / 16777216.0 - 0.5));
    }
    return x;
}
double magnitude(const std::vector<float>& y, std::size_t from, std::size_t n, double hz) {
    std::complex<double> sum = 0;
    for (std::size_t i = from; i < from + n; ++i) {
        sum += static_cast<double>(y[i]) * std::polar(1.0, -2 * std::numbers::pi * hz * static_cast<double>(i) / kRate);
    }
    return std::abs(sum);
}

void partials() {
    R r;
    check(std::abs(r.partial(3) - 660) < 1e-9, "harmonic: partial 3 of 220 Hz is 660 Hz");
    r.set(R::Stretch, 100);
    check(std::abs(r.partial(3) - 220 * std::pow(3.0, 1.1)) < 1e-9, "Stretch +100: partial n at n^1.1");
    R h;
    h.set(R::Harmonics, 4);
    const double binHz = kRate / R::kSize;
    check(h.weight(static_cast<std::size_t>(std::lround(880 / binHz))) > 0 && h.weight(static_cast<std::size_t>(std::lround(1100 / binHz))) == 0 &&
              h.weight(static_cast<std::size_t>(std::lround(330 / binHz))) == 0,
          "Harmonics 4: the fourth partial's bin rings, the fifth's and those between are silent");
}

void rings() {
    // A 50 ms burst of noise, then silence: a second on, the output's
    // strongest lines sit at the first partials, within 1 Hz.
    R r;
    r.set(R::Decay, 8000);
    r.set(R::DryWet, 100);
    r.prepare(kRate);
    const auto x = noiseBurst(48000 * 3, 2400);
    std::vector<float> l, rr;
    run(r, x, l, rr);
    const std::size_t from = 48000, n = 48000;
    double worst = 0;
    for (int k = 1; k <= 4; ++k) {
        const double want = 220.0 * k;
        double best = 0, top = -1;
        for (double hz = want - 60; hz <= want + 60; hz += 0.25) {
            const double m = magnitude(l, from, n, hz);
            if (m > top) {
                top = m;
                best = hz;
            }
        }
        worst = std::max(worst, std::abs(best - want));
    }
    check(worst <= 1, "after a burst of noise the output rings at the first four partials, within 1 Hz (worst " + std::to_string(worst) + " Hz)");
    // A tone between partials (330 Hz, half way from 220 to 440) is not heard.
    R t;
    t.set(R::DryWet, 100);
    t.prepare(kRate);
    std::vector<float> tone(96000), tl, tr;
    for (std::size_t i = 0; i < tone.size(); ++i) {
        tone[i] = static_cast<float>(0.4 * std::sin(2 * std::numbers::pi * 330 * static_cast<double>(i) / kRate));
    }
    run(t, tone, tl, tr);
    double e = 0;
    for (std::size_t i = 48000; i < 96000; ++i) {
        e += static_cast<double>(tl[i]) * tl[i];
    }
    const double level = 10 * std::log10(e / 48000 / (0.4 * 0.4 / 2));
    check(level < -40, "a tone between partials is not heard (" + std::to_string(level) + " dB)");
}

void decay() {
    // Decay 2 s: from 0.5 s to 1.5 s after the burst the ring falls 30 dB,
    // within 15 % (the fall is per hop).
    R r;
    r.set(R::Decay, 2000);
    r.set(R::Harmonics, 1);
    r.set(R::DryWet, 100);
    r.prepare(kRate);
    const auto x = noiseBurst(48000 * 3, 2400);
    std::vector<float> l, rr;
    run(r, x, l, rr);
    const auto rms = [&](double t) {
        double e = 0;
        const auto from = static_cast<std::size_t>(t * kRate);
        for (std::size_t i = from; i < from + 4800; ++i) {
            e += static_cast<double>(l[i]) * l[i];
        }
        return std::sqrt(e / 4800);
    };
    const double fallen = 20 * std::log10(rms(0.6) / rms(1.6));
    check(std::abs(fallen - 30) < 4.5, "Decay 2 s: the ring falls 30 dB in a second, within 15 % (" + std::to_string(fallen) + " dB)");
}

void routing() {
    std::vector<float> x(20000, 0.f), l, r;
    x[5000] = 1.f;
    R dry;
    dry.set(R::DryWet, 0);
    dry.prepare(kRate);
    run(dry, x, l, r);
    check(l[5000 + R::kSize] == 1.f && std::count_if(l.begin(), l.end(), [](float v) { return v != 0.f; }) == 1 && dry.latency() == 2048,
          "Dry/Wet 0: the input, one window late; the latency is reported");
    std::vector<float> s(30000), first;
    for (std::size_t i = 0; i < s.size(); ++i) {
        s[i] = static_cast<float>(0.3 * std::sin(0.013 * static_cast<double>(i)) + 0.1 * std::sin(0.9 * static_cast<double>(i)));
    }
    bool identical = true;
    for (std::size_t block : {1u, 64u, 333u, 4096u}) {
        R q;
        q.set(R::Stretch, 30);
        q.prepare(kRate);
        run(q, s, l, r, block);
        if (first.empty()) {
            first = l;
        } else {
            identical = identical && l == first;
        }
    }
    check(identical, "every block size gives the same samples");
    R q;
    std::vector<float> bad(4096, std::numeric_limits<float>::quiet_NaN()), o(4096), p(4096);
    counting = true;
    q.process(bad.data(), bad.data(), o.data(), p.data(), bad.size());
    q.set(R::Frequency, 80);
    q.set(R::Harmonics, 64);
    q.set(R::Stretch, -100);
    q.set(R::Decay, 20000);
    q.process(s.data(), s.data(), o.data(), p.data(), 4096);
    counting = false;
    check(allocations == 0, "processing and moved controls allocate nothing");
    check(std::all_of(o.begin(), o.end(), [](float v) { return std::isfinite(v); }), "a NaN input leaves the output finite");
}

}  // namespace

int main() {
    partials();
    rings();
    decay();
    routing();
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAILED" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
