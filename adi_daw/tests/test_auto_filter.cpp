// SPDX-License-Identifier: MIT
#include "adi/dsp/auto_filter.hpp"
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
using Core = adi::dsp::AutoFilter;
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
void measurements() {
    auto x = tone(1000, .2);
    C dry;
    dry.set(C::Mix, 0);
    dry.prepare(48000);
    check(render(dry, x) == x, "zero wet is exact dry");
    auto magnitude = [&](int type, double hz, int slope = 1) {
        C c;
        c.set(C::Type, type);
        c.set(C::Slope, slope);
        c.set(C::Frequency, 1000);
        c.prepare(48000);
        return bin(render(c, tone(hz, .1)), hz);
    };
    check(magnitude(C::LP, 100) > magnitude(C::LP, 10000) * 50,
          "LP passes below and rejects above cutoff");
    check(magnitude(C::HP, 10000) > magnitude(C::HP, 100) * 50,
          "HP passes above and rejects below cutoff");
    check(magnitude(C::BP, 1000) > magnitude(C::BP, 100) * 5, "BP isolates cutoff region");
    check(magnitude(C::Notch, 1000) < 1e-6, "notch rejects cutoff");
    check(magnitude(C::LP, 8000, 2) < magnitude(C::LP, 8000, 1) * .1,
          "24 dB cascades attenuate more than 12");
    C dj;
    dj.set(C::Type, C::DJ);
    dj.prepare(48000);
    check(render(dj, x) == x, "DJ centre is exact pass-through");
    dj.set(C::Control, -100);
    dj.prepare(48000);
    check(bin(render(dj, x), 1000) < .001, "DJ negative end low-passes");
    C crush;
    crush.set(C::Type, C::Resampling);
    crush.set(C::Frequency, 8000);
    crush.prepare(48000);
    auto aliased = render(crush, tone(5000, .3));
    check(bin(aliased, 7011) < 1e-7, "resampling floor before folded peak");
    check(bin(aliased, 3000) > .1, "resampling folds 5 kHz across 8 kHz Nyquist to 3 kHz");
    C comb;
    comb.set(C::Type, C::Comb);
    comb.set(C::Frequency, 1000);
    comb.set(C::Resonance, 0);
    comb.prepare(48000);
    std::vector<float> impulse(128);
    impulse[0] = 1;
    auto delayed = render(comb, impulse);
    check(delayed[0] == .5f && std::abs(delayed[48] - .5f) < 1e-5,
          "comb delay is one cutoff period");
    comb.set(C::Morph, 100);
    comb.prepare(48000);
    check(render(comb, impulse)[48] < -.49f, "comb Morph flips delayed polarity");
    C lfo;
    lfo.set(C::LfoAmount, 50);
    lfo.set(C::LfoMode, 2);
    lfo.set(C::LfoBeats, 1);
    lfo.tempo(120);
    lfo.prepare(48000);
    auto mod = render(lfo, x);
    check(bin(mod, 7011) < 1e-7, "synced modulation far floor first");
    check(bin(mod, 998) > .001 && bin(mod, 1002) > .001,
          "one-beat 120 BPM LFO creates 2 Hz sidebands");
    lfo.tempo(60);
    lfo.prepare(48000);
    auto slower = render(lfo, x);
    check(bin(slower, 999) > .001 && bin(slower, 1001) > .001,
          "tempo change moves sidebands to 1 Hz");
    C env;
    env.set(C::Frequency, 300);
    env.set(C::Envelope, 100);
    env.set(C::External, 1);
    env.prepare(48000);
    std::vector<float> key(x.size(), 1), zero(x.size()), left(x.size()), right(x.size());
    env.process(x.data(), x.data(), left.data(), right.data(), x.size(), key.data(), key.data());
    const auto raised = bin(left, 1000);
    env.prepare(48000);
    env.process(x.data(), x.data(), left.data(), right.data(), x.size(), zero.data(), zero.data());
    check(raised > bin(left, 1000) * 5,
          "external envelope opens filter without mixing key into audio");
    env.set(C::SCListen, 1);
    env.prepare(48000);
    auto side = tone(250, .1);
    env.process(x.data(), x.data(), left.data(), right.data(), x.size(), side.data(), side.data());
    check(left == side, "sidechain Listen exposes trigger only");
    env.set(C::SCFilter, 1);
    env.set(C::SCType, 3);
    env.set(C::SCFrequency, 100);
    env.prepare(48000);
    env.process(x.data(), x.data(), left.data(), right.data(), x.size(), side.data(), side.data());
    check(bin(left, 250) < .03, "sidechain filter shapes trigger/listen bus");
    for (int type = 0; type < 10; ++type) {
        C base;
        base.set(C::Type, type);
        base.prepare(48000);
        auto expected = render(base, x, 32);
        check(
            std::all_of(expected.begin(), expected.end(), [](float v) { return std::isfinite(v); }),
            "each type stays finite");
        for (std::size_t block : {32u, 64u, 128u, 256u, 512u, 1024u, 2048u, 4096u}) {
            C other;
            other.set(C::Type, type);
            other.prepare(48000);
            check(render(other, x, block) == expected, "type output identical for all block sizes");
        }
    }
    for (int circuit = 0; circuit < 4; ++circuit)
        for (int type = 0; type < 5; ++type) {
            C c;
            c.set(C::Circuit, circuit);
            c.set(C::Type, type);
            c.set(C::Resonance, 95);
            c.set(C::Drive, 80);
            c.prepare(48000);
            auto result = render(c, x);
            check(
                std::all_of(result.begin(), result.end(), [](float v) { return std::isfinite(v); }),
                "all circuit/type combinations finite under drive/resonance");
        }
    for (int wave = 0; wave < 8; ++wave) {
        C a;
        a.set(C::LfoWave, wave);
        a.set(C::LfoAmount, 80);
        a.prepare(48000);
        const auto expected = render(a, x, 32);
        C other;
        other.set(C::LfoWave, wave);
        other.set(C::LfoAmount, 80);
        other.prepare(48000);
        check(render(other, x, 4096) == expected, "all LFO waveforms deterministic across blocks");
    }
    for (int circuit = 0; circuit < 4; ++circuit) {
        double last = 1;
        for (int slope = 0; slope < 4; ++slope) {
            C c;
            c.set(C::Type, C::MorphFilter);
            c.set(C::Circuit, circuit);
            c.set(C::Slope, slope);
            c.set(C::Frequency, 1000);
            c.set(C::Resonance, 0);
            c.prepare(48000);
            const double response = bin(render(c, tone(8000, .01)), 8000);
            check(response < last * .4, "each circuit's morph LP slopes get progressively steeper");
            last = response;
        }
    }
    C stereo;
    stereo.set(C::LfoAmount, 80);
    stereo.set(C::StereoMode, 1);
    stereo.set(C::Spin, 100);
    stereo.prepare(48000);
    stereo.process(x.data(), x.data(), left.data(), right.data(), x.size());
    check(left != right, "Spin detunes stereo modulation");
    stereo.set(C::StereoMode, 0);
    stereo.process(x.data(), x.data(), left.data(), right.data(), x.size());
    check(std::equal(left.begin() + 48000, left.end(), right.begin() + 48000),
          "returning to Phase zero reunites the LFOs after prior Spin");
    stereo.set(C::Phase, 180);
    stereo.process(x.data(), x.data(), left.data(), right.data(), x.size());
    check(left != right, "Phase 180 produces opposite stereo modulation");
    stereo.set(C::Phase, 0);
    stereo.set(C::StereoMode, 1);
    stereo.set(C::Spin, 0);
    stereo.process(x.data(), x.data(), left.data(), right.data(), x.size());
    check(std::equal(left.begin() + 48000, left.end(), right.begin() + 48000),
          "Spin zero reunites channels after automation");
    C quant;
    quant.set(C::LfoAmount, 100);
    quant.set(C::Quantize, 1);
    quant.set(C::Steps, 1); // Sine sampled only at phase zero: no frequency modulation.
    quant.prepare(48000);
    C constant;
    constant.prepare(48000);
    check(render(quant, x) == render(constant, x),
          "one-step sine quantization holds phase-zero value");
    C negative;
    negative.set(C::External, 1);
    negative.set(C::Envelope, -100);
    negative.prepare(48000);
    negative.process(x.data(), x.data(), left.data(), right.data(), x.size(), key.data(),
                     key.data());
    check(bin(left, 1000) < .001, "negative envelope closes cutoff");
    auto holdResponse = [&](bool hold) {
        C c;
        c.set(C::External, 1);
        c.set(C::Frequency, 300);
        c.set(C::Envelope, 100);
        c.set(C::Attack, 100);
        c.set(C::Release, 1);
        c.set(C::Hold, hold ? 1 : 0);
        c.prepare(48000);
        std::vector<float> pulse(x.size());
        std::fill_n(pulse.begin(), 48, 1.f);
        c.process(x.data(), x.data(), left.data(), right.data(), x.size(), pulse.data(),
                  pulse.data());
        double energy = 0;
        for (std::size_t i = 2400; i < 4800; ++i)
            energy += left[i] * left[i];
        return energy;
    };
    check(holdResponse(true) > holdResponse(false) * 3,
          "Hold completes attack on a short trigger instead of immediately releasing");
    for (int quantization = 0; quantization < 3; ++quantization) {
        C a, b;
        for (C *c : {&a, &b}) {
            c->set(C::LfoAmount, 80);
            c->set(C::Quantize, quantization);
            c->set(C::Envelope, 50);
            c->set(C::EnvSH, 1);
            c->prepare(48000);
        }
        check(render(a, x, 32) == render(b, x, 4096),
              "LFO and envelope quantization keep sample-accurate timing across blocks");
    }
    C realtime;
    realtime.set(C::Type, C::Comb);
    realtime.prepare(48000);
    std::vector<float> small(4096, .2f), out(4096), other(4096);
    allocations = 0;
    counting = true;
    realtime.process(small.data(), small.data(), out.data(), other.data(), small.size());
    counting = false;
    check(allocations == 0, "comb process allocates zero");
}
} // namespace
int main() {
    measurements();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
