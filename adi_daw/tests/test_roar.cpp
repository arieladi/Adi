// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live 12's Roar, the core (DEV-22). The reference for small signals is each
// routing's documented structure computed apart from the code's signal path:
// the stages' filters (biquad.hpp's designs, the band pass a constant-peak
// RBJ), their levels and 5 Hz DC blockers, the Linkwitz-Riley bands, and the
// feedback loop's closed form, Y/X = DC / (1 - fb LP DC z^-D), all on the
// unit circle; a -100 dBFS sine must match within 0.05 dB. Driven, a bias
// makes even harmonics; a change of routing does not replay an idle stage.
// Parity with Live waits on a reading.
#include "adi/dsp/biquad.hpp"
#include "adi/dsp/roar.hpp"

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
using R = adi::dsp::Roar;
using C = std::complex<double>;
constexpr double kRate = 48000;

C at(const adi::dsp::Biquad& b, double hz) {
    const C z1 = std::polar(1.0, -2 * std::numbers::pi * hz / kRate), z2 = z1 * z1;
    return (b.b0 + b.b1 * z1 + b.b2 * z2) / (1.0 + b.a1 * z1 + b.a2 * z2);
}
C dc(double hz) {
    const double pole = std::exp(-2 * std::numbers::pi * 5 / kRate);
    const C z1 = std::polar(1.0, -2 * std::numbers::pi * hz / kRate);
    return (1.0 - z1) / (1.0 - pole * z1);
}
adi::dsp::Biquad bandPass(double hz, double q) {
    const double w = 2 * std::numbers::pi * hz / kRate, alpha = std::sin(w) / (2 * q), a0 = 1 + alpha;
    return {alpha / a0, 0, -alpha / a0, -2 * std::cos(w) / a0, (1 - alpha) / a0};
}
/// The three stages used below: a 2 kHz low pass at -3 dB, a 300 Hz high
/// pass, a 1 kHz band pass (Q 2) at +3 dB; every shape quiet.
void stages(R& r) {
    const double filters[3] = {R::LowPass, R::HighPass, R::BandPass}, freqs[3] = {2000, 300, 1000}, qs[3] = {0.7071, 1, 2}, levels[3] = {-3, 0, 3};
    for (std::size_t s = 0; s < 3; ++s) {
        r.set(R::stage(s, R::StageShape), R::Tube);
        r.set(R::stage(s, R::StageAmount), 0);
        r.set(R::stage(s, R::StageFilter), filters[s]);
        r.set(R::stage(s, R::StageFreq), freqs[s]);
        r.set(R::stage(s, R::StageQ), qs[s]);
        r.set(R::stage(s, R::StageLevel), levels[s]);
    }
}
C stageResponse(std::size_t s, double hz) {
    using namespace adi::dsp;
    const C f = s == 0 ? at(lowPass(kRate, 2000, 0.7071), hz) : s == 1 ? at(highPass(kRate, 300, 1), hz) : at(bandPass(1000, 2), hz);
    const double g = s == 0 ? std::pow(10.0, -3 / 20.0) : s == 1 ? 1.0 : std::pow(10.0, 3 / 20.0);
    return f * g * dc(hz);
}
C lr4(bool high, double f0, double hz) {
    const auto b = high ? adi::dsp::highPass(kRate, f0, 1 / std::sqrt(2.0)) : adi::dsp::lowPass(kRate, f0, 1 / std::sqrt(2.0));
    return at(b, hz) * at(b, hz);
}

std::vector<float> sine(double hz, std::size_t frames, double amplitude) {
    std::vector<float> x(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        x[i] = static_cast<float>(amplitude * std::sin(2 * std::numbers::pi * hz * static_cast<double>(i) / kRate));
    }
    return x;
}
double component(const std::vector<float>& y, double hz) {
    std::complex<double> sum = 0;
    const std::size_t n = 48000, from = y.size() - n;
    for (std::size_t i = 0; i < n; ++i) {
        sum += static_cast<double>(y[from + i]) * std::polar(1.0, -2 * std::numbers::pi * hz * static_cast<double>(from + i) / kRate);
    }
    return 2 * std::abs(sum) / static_cast<double>(n);
}

void routings() {
    struct Case {
        int routing;
        const char* name;
    };
    for (const Case c : {Case{R::Single, "Single"}, Case{R::Serial, "Serial"}, Case{R::Parallel, "Parallel"}, Case{R::Multiband, "Multiband"}}) {
        double worst = 0;
        for (double hz : {80.0, 400.0, 1000.0, 2500.0, 7000.0}) {
            R r;
            stages(r);
            r.set(R::RoutingParam, c.routing);
            r.prepare(kRate);
            const auto x = sine(hz, 96000, 1e-5);
            std::vector<float> l(x.size()), rr(x.size());
            r.process(x.data(), x.data(), l.data(), rr.data(), x.size());
            C want = 0;
            switch (c.routing) {
            case R::Single:
                want = stageResponse(0, hz);
                break;
            case R::Serial:
                want = stageResponse(0, hz) * stageResponse(1, hz) * stageResponse(2, hz);
                break;
            case R::Parallel:
                want = (stageResponse(0, hz) + stageResponse(1, hz) + stageResponse(2, hz)) / 3.0;
                break;
            default:
                want = lr4(false, 200, hz) * (lr4(false, 2000, hz) + lr4(true, 2000, hz)) * stageResponse(0, hz) +
                       lr4(true, 200, hz) * lr4(false, 2000, hz) * stageResponse(1, hz) + lr4(true, 200, hz) * lr4(true, 2000, hz) * stageResponse(2, hz);
                break;
            }
            worst = std::max(worst, std::abs(20 * std::log10(component(l, hz) / 1e-5) - 20 * std::log10(std::abs(want))));
        }
        check(worst < 0.05, std::string(c.name) + ": a quiet sine follows the routing's documented structure within 0.05 dB (worst " + std::to_string(worst) + ")");
    }
    // Mid Side: the mid through stage 1, the side through stage 2.
    R ms;
    stages(ms);
    ms.set(R::RoutingParam, R::MidSide);
    ms.prepare(kRate);
    const auto x = sine(400, 96000, 1e-5);
    std::vector<float> minus(x.size()), l(x.size()), r(x.size());
    for (std::size_t i = 0; i < x.size(); ++i) {
        minus[i] = -x[i];
    }
    ms.process(x.data(), minus.data(), l.data(), r.data(), x.size());
    const double sideDb = 20 * std::log10(component(l, 400) / 1e-5), wantSide = 20 * std::log10(std::abs(stageResponse(1, 400)));
    check(std::abs(sideDb - wantSide) < 0.05, "Mid Side: a pure side signal goes through stage 2");
    // Multiband with every stage flat: the bands sum to an allpass.
    double worst = 0;
    for (double hz : {60.0, 200.0, 700.0, 2000.0, 9000.0}) {
        R flat;
        flat.set(R::RoutingParam, R::Multiband);
        flat.set(R::stage(0, R::StageAmount), 0);
        flat.prepare(kRate);
        const auto s = sine(hz, 96000, 1e-5);
        std::vector<float> a(s.size()), b(s.size());
        flat.process(s.data(), s.data(), a.data(), b.data(), s.size());
        worst = std::max(worst, std::abs(20 * std::log10(component(a, hz) / 1e-5) - 20 * std::log10(std::abs(dc(hz)))));
    }
    check(worst < 0.05, "Multiband with every stage flat: the bands sum flat (worst " + std::to_string(worst) + " dB)");
}

void feedback() {
    for (double hz : {200.0, 700.0}) {
        R r;
        r.set(R::stage(0, R::StageAmount), 0);
        r.set(R::Feedback, 50);
        r.set(R::FeedbackTime, 10);
        r.set(R::FeedbackFreq, 5000);
        r.prepare(kRate);
        const auto x = sine(hz, 96000, 1e-5);
        std::vector<float> l(x.size()), rr(x.size());
        r.process(x.data(), x.data(), l.data(), rr.data(), x.size());
        const C z = std::polar(1.0, -2 * std::numbers::pi * hz / kRate * 480);
        const C want = dc(hz) / (1.0 - 0.5 * at(adi::dsp::lowPass(kRate, 5000, 1 / std::sqrt(2.0)), hz) * dc(hz) * z);
        const double got = 20 * std::log10(component(l, hz) / 1e-5);
        check(std::abs(got - 20 * std::log10(std::abs(want))) < 0.05, "Feedback 50 %, 10 ms, at " + std::to_string(hz) + " Hz: the loop's closed form (" +
                                                                         std::to_string(got) + " dB)");
    }
}

void shapes() {
    double worst = 0;
    for (int s : {R::SoftSine, R::Diode, R::Tube, R::Polynomial, R::Fold}) {
        const double h = 1e-6;
        worst = std::max(worst, std::abs((R::shape(s, h, 0) - R::shape(s, -h, 0)) / (2 * h) - 1));
    }
    check(worst < 1e-6, "every shape but the crusher has slope 1 at 0");
    check(std::abs(R::shape(R::Diode, 1, 0) + R::shape(R::Diode, -1, 0)) > 0.1, "Diode is asymmetric");
    check(R::shape(R::BitCrusher, 0.3, 1) == 0.25 && R::shape(R::BitCrusher, 0.3, 0) != 0.25, "the crusher's steps: 2^-2 at Amount 100");
    // A bias makes even harmonics; none without one.
    const auto second = [](double bias) {
        R r;
        r.set(R::stage(0, R::StageShape), R::Tube);
        r.set(R::stage(0, R::StageAmount), 40);
        r.set(R::stage(0, R::StageBias), bias);
        r.prepare(kRate);
        const auto x = sine(220, 96000, 0.3);
        std::vector<float> l(x.size()), rr(x.size());
        r.process(x.data(), x.data(), l.data(), rr.data(), x.size());
        return component(l, 440) / component(l, 220);
    };
    check(second(0) < 1e-5 && second(60) > 0.01, "a bias makes a second harmonic, none without");
}

void routing() {
    // A routing change does not replay an idle stage's old state.
    R r;
    stages(r);
    r.set(R::RoutingParam, R::Serial);
    r.prepare(kRate);
    const auto loud = sine(300, 24000, 0.8);
    std::vector<float> l(48000), rr(48000);
    r.process(loud.data(), loud.data(), l.data(), rr.data(), loud.size());
    r.set(R::RoutingParam, R::Single);
    const std::vector<float> quiet(48000, 0.f);
    r.process(quiet.data(), quiet.data(), l.data(), rr.data(), quiet.size());
    r.set(R::RoutingParam, R::Serial);
    r.process(quiet.data(), quiet.data(), l.data(), rr.data(), quiet.size());
    double peak = 0;
    for (float v : l) {
        peak = std::max(peak, static_cast<double>(std::abs(v)));
    }
    check(peak < 1e-9, "Serial, Single, Serial again over silence: nothing (" + std::to_string(peak) + ")");
    std::vector<float> x(20000), y(20000), a, b;
    for (std::size_t i = 0; i < x.size(); ++i) {
        x[i] = static_cast<float>(0.3 * std::sin(0.013 * static_cast<double>(i)));
        y[i] = static_cast<float>(0.2 * std::sin(0.11 * static_cast<double>(i)));
    }
    a.assign(x.size(), 0.f);
    b.assign(x.size(), 0.f);
    R dry;
    dry.set(R::DryWet, 0);
    dry.process(x.data(), y.data(), a.data(), b.data(), x.size());
    check(a == x && b == y, "Dry/Wet 0 is the input itself");
    std::vector<float> first;
    bool identical = true;
    for (std::size_t block : {1u, 64u, 333u, 4096u}) {
        R q;
        q.set(R::RoutingParam, R::Multiband);
        q.set(R::stage(1, R::StageAmount), 70);
        q.set(R::Feedback, 40);
        q.prepare(kRate);
        for (std::size_t at0 = 0; at0 < x.size(); at0 += block) {
            const auto n = std::min(block, x.size() - at0);
            q.process(x.data() + at0, y.data() + at0, a.data() + at0, b.data() + at0, n);
        }
        if (first.empty()) {
            first = a;
        } else {
            identical = identical && a == first;
        }
    }
    check(identical, "every block size gives the same samples");
    R q;
    std::vector<float> bad(512, std::numeric_limits<float>::quiet_NaN()), o(512), p(512);
    counting = true;
    q.process(bad.data(), bad.data(), o.data(), p.data(), bad.size());
    const bool silent = std::all_of(o.begin(), o.end(), [](float v) { return v == 0.f; });
    q.set(R::RoutingParam, R::Parallel);
    q.set(R::stage(2, R::StageShape), R::BitCrusher);
    q.set(R::Feedback, 95);
    q.process(x.data(), x.data(), o.data(), p.data(), 512);
    counting = false;
    check(allocations == 0, "processing and moved controls allocate nothing");
    check(silent, "a NaN input is silence");
    // Feedback 95 % on a loud input stays bounded.
    R hot;
    hot.set(R::Feedback, 95);
    hot.set(R::FeedbackTime, 5);
    hot.set(R::Input, 24);
    hot.prepare(kRate);
    const auto big = sine(150, 96000, 1.0);
    std::vector<float> hl(big.size()), hr(big.size());
    hot.process(big.data(), big.data(), hl.data(), hr.data(), big.size());
    check(std::all_of(hl.begin(), hl.end(), [](float v) { return std::isfinite(v) && std::abs(v) < 40; }), "Feedback 95 % on a hot input stays bounded");
    R z;
    z.set(R::DryWet, 0);
    std::vector<float> left(4800, 0.25f), shared(4800, -0.5f), rightOut(4800);
    z.process(left.data(), shared.data(), shared.data(), rightOut.data(), 4800);
    check(shared.back() == 0.25f && rightOut.back() == -0.5f, "an output plane that is the other channel's input is read first");
}

}  // namespace

int main() {
    routings();
    feedback();
    shapes();
    routing();
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAILED" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
