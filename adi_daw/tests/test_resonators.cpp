// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/resonators.hpp"
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
using adi::dsp::Resonators;
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
std::vector<float> render(Resonators &c, const std::vector<float> &in, std::size_t block = 64) {
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
using R = Resonators;
void setup(R &r) {
    r = R{};
    r.set(R::On2, 0);
    r.set(R::On3, 0);
    r.set(R::On4, 0);
    r.set(R::On5, 0);
    r.set(R::Note, 69);
    r.set(R::Color, 100);
    r.set(R::Const, 1);
    r.set(R::Mix, 100);
    r.prepare(48000);
}
double energy(const std::vector<float> &x, std::size_t from = 0, std::size_t count = 0) {
    if (!count)
        count = x.size() - from;
    double e = 0;
    for (std::size_t i = from; i < from + count; ++i)
        e += static_cast<double>(x[i]) * x[i];
    return e / static_cast<double>(count);
}
void measurements() {
    R r;
    setup(r);
    auto impulse = tone(440, 0);
    impulse[0] = .001f;
    auto out = render(r, impulse);
    check(bin(out, 7011) < 1e-6, "far bin floor precedes resonant peak");
    double best = 0, pitch = 0;
    for (int i = -20; i <= 20; ++i) {
        double f = 440 + static_cast<double>(i) * .1;
        double mag = bin(out, f);
        if (mag > best) {
            best = mag;
            pitch = f;
        }
    }
    const double cents = 1200 * std::log2(pitch / 440);
    check(std::abs(cents) < 1, "impulse tail spectral peak tuned within one cent");
    check(best > 1e-8, "resonator tail actually sounds");
    setup(r);
    r.tuning({480, 480, 480, 480, 480});
    r.set(R::Decay, 1);
    out = render(r, impulse);
    const double ratio = std::abs(out[48100] / out[100]);
    check(std::abs(20 * std::log10(ratio) + 60) < .1,
          "one-second RT60 at small signal within 0.1 dB");
    for (bool constant : {false, true})
        for (double hz : {240., 480.}) {
            setup(r);
            r.tuning({hz, hz, hz, hz, hz});
            r.set(R::Decay, 1);
            r.set(R::Const, constant ? 1 : 0);
            auto tail = render(r, impulse);
            const auto period = static_cast<std::size_t>(48000 / hz);
            const double measured = 20 * std::log10(std::abs(tail[period + 48000] / tail[period]));
            const double expected = -60 / (constant ? 1 : std::sqrt(440 / hz));
            check(std::abs(measured - expected) < .2,
                  "rendered pitch-dependent and constant RT60 within 0.2 dB");
        }
    setup(r);
    r.set(R::Const, 0);
    std::vector<float> scratch(64);
    r.process(impulse.data(), impulse.data(), scratch.data(), scratch.data(), 64);
    check(std::abs(r.decaySeconds(0) - 2) < 1e-9, "pitch-dependent decay referenced to 440 Hz");
    r.set(R::Note, 57);
    r.process(impulse.data(), impulse.data(), scratch.data(), scratch.data(), 64);
    check(std::abs(r.decaySeconds(0) - 2 * std::sqrt(2.)) < 1e-9, "lower pitch rings longer");
    r.set(R::Const, 1);
    r.process(impulse.data(), impulse.data(), scratch.data(), scratch.data(), 64);
    check(r.decaySeconds(0) == 2, "Const keeps decay independent of pitch");
    setup(r);
    r.set(R::Fine1, 50);
    r.set(R::On2, 1);
    r.set(R::Pitch2, 12);
    r.set(R::Fine2, -25);
    r.process(impulse.data(), impulse.data(), scratch.data(), scratch.data(), 64);
    check(std::abs(1200 * std::log2(r.frequency(0) / 440) - 50) < 1e-8,
          "root fine tuning in cents");
    check(std::abs(1200 * std::log2(r.frequency(1) / 440) - 1175) < 1e-8,
          "relative voice pitch plus fine tuning");
    setup(r);
    check(r.tuning({432, 540, 648, 756, 864}), "explicit non-equal tuning accepted");
    r.process(impulse.data(), impulse.data(), scratch.data(), scratch.data(), 64);
    check(r.frequency(0) == 432 && r.frequency(4) == 864,
          "alternate tuning applied independently per voice");
    check(!r.tuning({0, 540, 648, 756, 864}), "invalid tuning rejected atomically");
    r.clearTuning();
    r.process(impulse.data(), impulse.data(), scratch.data(), scratch.data(), 64);
    check(std::abs(r.frequency(0) - 440) < 1e-9, "clear tuning restores note controls");
    setup(r);
    r.tuning({480, 480, 480, 480, 480});
    r.set(R::Mode, 1);
    auto negative = render(r, impulse);
    check(negative[50] > .0009 && negative[100] < 0, "Mode B alternates feedback at half-period");
    check(bin(negative, 480) > bin(negative, 960) * 10,
          "Mode B has odd-mode resonance, not octave-up tuning");
    setup(r);
    auto bright = render(r, impulse);
    r.set(R::Color, 0);
    r.prepare(48000);
    auto dark = render(r, impulse);
    check(bin(dark, 440 * 8) < bin(bright, 440 * 8) * .05, "Color damps high resonances");
    setup(r);
    auto steady = tone(440, .0001);
    auto base = render(r, steady);
    r.set(R::Gain, 6);
    r.prepare(48000);
    auto gain = render(r, steady);
    check(std::abs(bin(gain, 440) / bin(base, 440) - std::pow(10., .3)) < 1e-4,
          "wet output gain in dB");
    r.set(R::Mix, 0);
    check(render(r, steady) == steady, "dry endpoint ignores wet gain");
    for (int type = 0; type < 4; ++type) {
        setup(r);
        r.set(R::FilterOn, 1);
        r.set(R::FilterType, type);
        r.set(R::Frequency, 440);
        auto filtered = render(r, steady);
        const double relative = bin(filtered, 440) / bin(base, 440);
        check(type == 3 ? relative < .001 : relative > .4 && relative < 1.2,
              "input filter response at its cutoff matches type");
    }
    std::printf("METRIC Resonators pitch %.4f Hz (%.6f cents), one-second decay %.5f dB\n", pitch,
                cents, 20 * std::log10(ratio));
}
void routing() {
    R r;
    setup(r);
    r.set(R::On1, 0);
    r.set(R::On2, 1);
    r.set(R::Gain2, 0);
    r.set(R::Pitch2, 0);
    auto in = tone(440, .001), zero = tone(440, 0);
    std::vector<float> l(in.size()), rr(in.size());
    r.process(in.data(), zero.data(), l.data(), rr.data(), in.size());
    check(energy(l) > 1e-5 && energy(rr) == 0, "II takes left; I off does not disable II");
    r.set(R::Width, 0);
    r.prepare(48000);
    r.process(in.data(), zero.data(), l.data(), rr.data(), in.size());
    check(l == rr && energy(l) > 0, "Width zero sums wet II-V to mono");
    setup(r);
    r.set(R::On1, 0);
    r.set(R::On3, 1);
    r.set(R::Gain3, 0);
    r.set(R::Pitch3, 0);
    r.process(zero.data(), in.data(), l.data(), rr.data(), in.size());
    check(energy(l) == 0 && energy(rr) > 1e-5, "III takes right");
    setup(r);
    r.process(zero.data(), in.data(), l.data(), rr.data(), in.size());
    check(energy(l) == 0 && energy(rr) > 1e-5, "I accepts right independently");
    r.set(R::On1, 0);
    r.process(in.data(), in.data(), l.data(), rr.data(), in.size());
    check(energy(l) == 0 && energy(rr) == 0, "disabled voices produce no output");
}
void partitions() {
    auto in = tone(173);
    in.resize(16384);
    R r;
    r.prepare(48000);
    auto baseline = render(r, in, 32);
    for (std::size_t n : {64u, 128u, 256u, 512u, 1024u, 2048u, 4096u}) {
        r.prepare(48000);
        check(render(r, in, n) == baseline, "exact output at block sizes 32 to 4096");
    }
    std::vector<float> l(in.size()), rr(in.size());
    allocations = 0;
    counting = true;
    r.process(in.data(), in.data(), l.data(), rr.data(), in.size());
    counting = false;
    check(allocations == 0, "zero allocations in process");
    for (double sr : {8000., 44100., 96000., 192000.}) {
        r.set(R::Note, 0);
        r.set(R::Pitch2, -24);
        r.set(R::Decay, 20);
        r.prepare(sr);
        auto result = render(r, in);
        check(std::all_of(result.begin(), result.end(), [](float x) { return std::isfinite(x); }),
              "extreme low pitch/decay stable across sample rates");
    }
}
void upstream() {
    for (double x : {-10., -1.5, -.7, 0., .3, 1.5, 10.}) {
        double c = std::clamp(x, -1.5, 1.5);
        double expected = c + (-4. / 27.) * c * c * c;
        check(std::abs(R::softclip(x) - expected) < 1e-14,
              "Surge softclip scalar polynomial matches");
    }
    R r;
    setup(r);
    r.tuning({480, 480, 480, 480, 480});
    r.set(R::Decay, 1);
    auto impulse = tone(0, 0);
    impulse[0] = .1f;
    auto output = render(r, impulse);
    double y = R::softclip(.1f);
    const double g = std::pow(.001, 100. / 48000);
    bool match = true;
    for (std::size_t i = 100; i < 48000; i += 100) {
        match &= std::abs(output[i] - y) < 1e-7;
        y = R::softclip(g * y);
    }
    check(match, "integer comb echoes match Surge read-feedback-softclip-write recurrence");
}
} // namespace
int main() {
    measurements();
    routing();
    partitions();
    upstream();
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
