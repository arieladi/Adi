// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/vocoder.hpp"
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
using adi::dsp::Vocoder;
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
std::vector<float> render(Vocoder &c, const std::vector<float> &in, std::size_t block = 64) {
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
double error(const std::vector<float> &a, const std::vector<float> &b) {
    double sum = 0;
    for (std::size_t i = 48000; i < a.size(); ++i)
        sum += std::pow(static_cast<double>(a[i]) - b[i], 2);
    return sum / 48000;
}
using V = Vocoder;
void setup(V &v) {
    v = V{};
    v.set(V::Carrier, 1);
    v.set(V::Bands, 0);
    v.set(V::Low, 1000);
    v.set(V::High, 8000);
    v.set(V::Attack, 20);
    v.set(V::Release, 50);
    for (int i = 1; i < 20; ++i)
        v.set(static_cast<V::Param>(V::Band1 + i), -60);
    v.prepare(48000);
}
std::vector<float> external(V &v, const std::vector<float> &mod, const std::vector<float> &car,
                            std::size_t block = 64) {
    std::vector<float> out(mod.size()), right(mod.size());
    for (std::size_t i = 0; i < mod.size(); i += block)
        v.process(mod.data() + i, mod.data() + i, out.data() + i, right.data() + i,
                  std::min(block, mod.size() - i), car.data() + i, car.data() + i);
    return out;
}
void measurements() {
    auto input = tone(1000), high = tone(4000), oct = tone(2000);
    V v;
    setup(v);
    auto output = external(v, input, input);
    check(bin(output, 7011) < 1e-7, "far bin at floor before vocoded peak");
    const double peak = bin(output, 1000);
    check(peak > .03, "matching modulator and carrier band makes sound");
    v.prepare(48000);
    auto rejected = external(v, high, input);
    check(bin(rejected, 1000) < peak * .2, "out-of-band modulator suppressed");
    v.prepare(48000);
    auto unshifted = external(v, input, oct);
    v.set(V::Formant, 12);
    v.prepare(48000);
    auto shifted = external(v, input, oct);
    check(bin(shifted, 2000) > bin(unshifted, 2000) * 3, "formant moves carrier bank one octave");
    setup(v);
    v.set(V::Gate, 0);
    auto gated = external(v, input, input);
    check(bin(gated, 1000) < 1e-9, "per-band gate closes below threshold");
    v.set(V::Depth, 0);
    v.prepare(48000);
    auto zero = input;
    std::fill(zero.begin(), zero.end(), 0.f);
    auto independent = external(v, zero, input);
    check(bin(independent, 1000) > .5, "Depth zero discards modulator envelope");
    setup(v);
    v.set(V::Depth, 200);
    auto deep = external(v, input, input);
    check(bin(deep, 1000) < peak * .5, "Depth 200 emphasizes louder envelopes");
    setup(v);
    v.set(V::Band1, -60);
    auto muted = external(v, input, input);
    check(bin(muted, 1000) == 0, "all band attenuators silence bank");
    setup(v);
    v.set(V::Carrier, 2);
    auto self = render(v, input);
    check(bin(self, 1000) > .03, "Modulator carrier resynthesizes input");
    setup(v);
    v.set(V::Carrier, 0);
    auto noise = render(v, input);
    check(error(noise, zero) > 1e-5, "Noise carrier excites analysed band");
    v.set(V::NoiseDensity, 0);
    v.prepare(48000);
    check(error(render(v, input), zero) == 0, "zero noise density is silent");
    setup(v);
    v.set(V::Carrier, 0);
    v.set(V::NoiseRate, 500);
    auto held = render(v, input);
    check(error(noise, held) > 1e-5, "noise downsampling changes spectrum");
    setup(v);
    v.set(V::Enhance, 1);
    auto enhanced = external(v, input, input);
    check(bin(enhanced, 1000) > peak * 2, "Enhance lifts weak carrier spectrum");
    setup(v);
    v.set(V::Unvoiced, 100);
    v.set(V::Sensitivity, 0);
    auto noBreath = external(v, input, zero);
    check(error(noBreath, zero) == 0, "zero sensitivity disables unvoiced noise");
    v.set(V::Sensitivity, 100);
    v.prepare(48000);
    check(error(external(v, input, zero), zero) > 1e-5, "100 sensitivity enables unvoiced noise");
    setup(v);
    v.set(V::Retro, 1);
    v.set(V::Band1, -60);
    v.set(V::Band4, 0);
    auto retro = external(v, tone(8000), tone(8000));
    check(std::all_of(retro.begin(), retro.end(), [](float x) { return std::isfinite(x); }),
          "Retro high narrow bands stable");
    // Envelope timing: compare attack energy at 10 ms, then release after a steady tone.
    V fast, slow;
    setup(fast);
    setup(slow);
    fast.set(V::Attack, .1);
    slow.set(V::Attack, 1000);
    std::vector<float> a(480), b(480);
    fast.process(input.data(), input.data(), a.data(), b.data(), 480, input.data(), input.data());
    slow.process(input.data(), input.data(), a.data(), b.data(), 480, input.data(), input.data());
    check(fast.envelope(0) > slow.envelope(0) * 10, "Attack controls envelope rise");
    setup(fast);
    setup(slow);
    fast.set(V::Release, 1);
    slow.set(V::Release, 1000);
    external(fast, input, input);
    external(slow, input, input);
    a.resize(4800);
    b.resize(4800);
    fast.process(zero.data(), zero.data(), a.data(), b.data(), 4800, input.data(), input.data());
    slow.process(zero.data(), zero.data(), a.data(), b.data(), 4800, input.data(), input.data());
    check(slow.envelope(0) > fast.envelope(0) * 100, "Release controls envelope decay");
    V pitch;
    pitch.set(V::Carrier, 3);
    pitch.prepare(48000);
    render(pitch, tone(220));
    check(std::abs(pitch.trackedHz() - 220) < 1, "pitch tracker finds 220 Hz");
    render(pitch, zero);
    check(std::abs(pitch.trackedHz() - 220) < 1, "pitch holds through silence");
    render(pitch, tone(440));
    check(std::abs(pitch.trackedHz() - 440) < 2, "pitch follows a new clear note");
    setup(v);
    v.set(V::Level, 6);
    auto louder = external(v, input, input);
    check(std::abs(bin(louder, 1000) / peak - std::pow(10., .3)) < 1e-5, "Level uses dB gain");
    setup(v);
    v.set(V::Bandwidth, 10);
    auto narrow = external(v, input, tone(1400));
    v.set(V::Bandwidth, 200);
    v.prepare(48000);
    auto broad = external(v, input, tone(1400));
    check(bin(broad, 1400) > bin(narrow, 1400) * 5, "Bandwidth widens carrier passband");
    for (int wave = 0; wave < 4; ++wave) {
        pitch.set(V::Waveform, wave);
        pitch.set(V::Pitch, 12);
        pitch.prepare(48000);
        auto pitched = render(pitch, tone(220));
        check(bin(pitched, 440) > .001,
              "tracked oscillator coarse pitch applies to every waveform");
    }
    for (double rate : {8000., 44100., 96000., 192000.}) {
        V edge;
        edge.set(V::Bandwidth, 200);
        edge.set(V::High, 20000);
        edge.set(V::Retro, 1);
        edge.prepare(rate);
        auto data = render(edge, input);
        check(std::all_of(data.begin(), data.end(), [](float x) { return std::isfinite(x); }),
              "bank stays finite at supported sample rates");
    }
    std::printf("METRIC Vocoder peak %.9f shifted %.9f pitch %.5f\n", peak, bin(shifted, 2000),
                pitch.trackedHz());
}
void routing() {
    auto left = tone(1000), right = tone(2000), silent = left;
    std::fill(silent.begin(), silent.end(), 0.f);
    std::vector<float> l(left.size()), r(left.size());
    V v;
    setup(v);
    v.set(V::Channels, 2);
    v.process(left.data(), silent.data(), l.data(), r.data(), l.size(), left.data(), right.data());
    check(bin(l, 1000) > .03 && bin(r, 2000) == 0, "LR has independent modulator envelopes");
    v.set(V::Channels, 1);
    v.prepare(48000);
    v.process(left.data(), silent.data(), l.data(), r.data(), l.size(), left.data(), left.data());
    check(l == r, "Stereo sums modulator but retains carrier channels");
    v.set(V::Channels, 0);
    v.prepare(48000);
    v.process(left.data(), right.data(), l.data(), r.data(), l.size(), left.data(), right.data());
    check(l == r, "Mono sums both sources");
    v.set(V::Mix, 0);
    v.process(left.data(), right.data(), l.data(), r.data(), l.size());
    check(l == left && r == right, "dry endpoint exact stereo");
}
void partitions() {
    auto input = tone(217);
    input.resize(16384);
    V v;
    v.set(V::Carrier, 0);
    v.set(V::Unvoiced, 33);
    v.set(V::Retro, 1);
    v.prepare(48000);
    auto reference = render(v, input, 32);
    for (std::size_t block : {64u, 128u, 256u, 512u, 1024u, 2048u, 4096u}) {
        v.prepare(48000);
        check(render(v, input, block) == reference, "block-size invariant 32 through 4096");
    }
    std::vector<float> l(input.size()), r(input.size());
    allocations = 0;
    counting = true;
    v.process(input.data(), input.data(), l.data(), r.data(), input.size());
    counting = false;
    check(allocations == 0, "zero allocations in process");
}
// Independent scalar transcription of the pinned Surge recurrence, including ADI's
// two substeps. A coefficient or recurrence regression changes the impulse response.
void upstream() {
    adi::dsp::VocoderBand f;
    f.coefficients(1000, 20, 48000);
    double l1 = 0, b1 = 0, l2 = 0, b2 = 0;
    const double q = 1. / 20, f1 = 2 * std::sin(pi * 1000 * (1 - .4 / 20) / 96000),
                 f2 = 2 * std::sin(pi * 1000 * (1 + .4 / 20) / 96000);
    double worst = 0;
    for (int n = 0; n < 4096; ++n) {
        double x = n == 0 ? 1 : 0;
        for (int k = 0; k < 2; ++k) {
            l1 = f1 * b1 + l1;
            double h1 = (x * q - l1) - q * b1;
            b1 = f1 * h1 + b1;
            l2 = f2 * b2 + l2;
            double h2 = (b1 * q - l2) - q * b2;
            b2 = f2 * h2 + b2;
        }
        worst = std::max(worst, std::abs(f.step(x) - b2));
    }
    check(worst < 1e-12, "Surge SVF recurrence impulse matches scalar reference");
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
