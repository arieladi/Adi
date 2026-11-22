// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live 12's Cabinet, the core (DEV-22). The reference is the documented
// cascade computed apart from the code's signal path: each cabinet's low
// cut, box resonance, cone peak and two-section high cut, the microphone's
// shelves, the mic type's shelf and Output, from the header's own values
// through biquad.hpp's designs, evaluated on the unit circle. A sine's level
// must match within 0.01 dB. Parity with Live waits on a reading.
#include "adi/dsp/biquad.hpp"
#include "adi/dsp/live_cabinet.hpp"

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
using K = adi::dsp::LiveCabinet;
using C = std::complex<double>;
constexpr double kRate = 48000;

C at(const adi::dsp::Biquad& b, double hz) {
    const C z1 = std::polar(1.0, -2 * std::numbers::pi * hz / kRate), z2 = z1 * z1;
    return (b.b0 + b.b1 * z1 + b.b2 * z2) / (1.0 + b.a1 * z1 + b.a2 * z2);
}
double expectedDb(int box, int mic, int type, double outputDb, double hz) {
    using namespace adi::dsp;
    const auto& b = K::kCabinets[box];
    const double q = 1 / std::sqrt(2.0), cut = mic == K::Far ? b.highCutHz / 2 : b.highCutHz;
    C h = at(highPass(kRate, b.lowCutHz, q), hz) * at(peaking(kRate, b.resonanceHz, 2, b.resonanceDb), hz) *
          at(peaking(kRate, b.coneHz, 1.5, b.coneDb), hz) * at(lowPass(kRate, cut, q), hz) * at(lowPass(kRate, cut, q), hz);
    if (mic == K::NearOff) {
        h *= at(highShelf(kRate, 3000, -6), hz);
    }
    if (mic == K::Far) {
        h *= at(lowShelf(kRate, 200, -3), hz);
    }
    if (type == 0) {
        h *= at(highShelf(kRate, 8000, 2), hz);
    }
    return 20 * std::log10(std::abs(h)) + outputDb;
}
double measuredDb(K& k, double hz) {
    const std::size_t frames = 96000;
    std::vector<float> in(frames), l(frames), r(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        in[i] = static_cast<float>(0.25 * std::sin(2 * std::numbers::pi * hz * static_cast<double>(i) / kRate));
    }
    k.prepare(kRate);
    k.process(in.data(), in.data(), l.data(), r.data(), frames);
    double a = 0, b = 0;
    for (std::size_t i = 48000; i < frames; ++i) {
        a += static_cast<double>(in[i]) * in[i];
        b += static_cast<double>(l[i]) * l[i];
    }
    return 10 * std::log10(b / a);
}

void responses() {
    for (int box = 0; box < K::Boxes; ++box) {
        for (int mic : {K::NearOn, K::NearOff, K::Far}) {
            for (int type : {0, 1}) {
                K k;
                k.set(K::Cabinet, box);
                k.set(K::Microphone, mic);
                k.set(K::MicType, type);
                k.set(K::Output, box == 2 ? -6 : 0);
                double worst = 0;
                for (double hz : {60.0, 120.0, 500.0, 2000.0, 4000.0, 9000.0}) {
                    worst = std::max(worst, std::abs(measuredDb(k, hz) - expectedDb(box, mic, type, box == 2 ? -6 : 0, hz)));
                }
                check(worst < 0.01, "cabinet " + std::to_string(box) + ", mic " + std::to_string(mic) + ", type " + std::to_string(type) +
                                        ": the documented response within 0.01 dB (worst " + std::to_string(worst) + ")");
            }
        }
    }
    // The microphone's meaning: off axis and far are duller than on axis.
    K on, off, far;
    off.set(K::Microphone, K::NearOff);
    far.set(K::Microphone, K::Far);
    check(measuredDb(off, 6000) < measuredDb(on, 6000) - 3 && measuredDb(far, 4000) < measuredDb(on, 4000) - 3,
          "off axis and far are duller than on axis");
    // A plant: a high cut 10 % off is far outside the tolerance.
    const auto& b = K::kCabinets[0];
    const double q = 1 / std::sqrt(2.0);
    const double shifted = 20 * std::log10(std::abs(at(adi::dsp::lowPass(kRate, b.highCutHz * 1.1, q), 4000) / at(adi::dsp::lowPass(kRate, b.highCutHz, q), 4000)));
    check(std::abs(shifted) > 0.1, "plant: a high cut 10 % off is outside the tolerance");
}

void routing() {
    std::vector<float> x(20000), y(20000), l(20000), r(20000);
    for (std::size_t i = 0; i < x.size(); ++i) {
        x[i] = static_cast<float>(0.3 * std::sin(0.013 * static_cast<double>(i)) + 0.1 * std::sin(0.9 * static_cast<double>(i)));
        y[i] = static_cast<float>(0.2 * std::sin(0.11 * static_cast<double>(i)));
    }
    K mono;
    mono.process(x.data(), y.data(), l.data(), r.data(), x.size());
    check(l == r, "Mono: one cabinet, the same to both channels");
    K dual, single;
    dual.set(K::Dual, 1);
    single.set(K::Dual, 1);
    dual.process(x.data(), y.data(), l.data(), r.data(), x.size());
    std::vector<float> l2(x.size()), r2(x.size());
    single.process(x.data(), x.data(), l2.data(), r2.data(), x.size());
    check(l != r && l == l2, "Dual: a cabinet for each channel, the left unaffected by the right");
    K dry;
    dry.set(K::DryWet, 0);
    dry.process(x.data(), y.data(), l.data(), r.data(), x.size());
    check(l == x && r == y, "Dry/Wet 0 is the input itself");
    std::vector<float> first;
    bool identical = true;
    for (std::size_t block : {1u, 64u, 333u, 4096u}) {
        K k;
        k.set(K::Cabinet, K::FourByTenBass);
        k.set(K::Dual, 1);
        k.prepare(kRate);
        for (std::size_t at0 = 0; at0 < x.size(); at0 += block) {
            const auto n = std::min(block, x.size() - at0);
            k.process(x.data() + at0, y.data() + at0, l.data() + at0, r.data() + at0, n);
        }
        if (first.empty()) {
            first = l;
        } else {
            identical = identical && l == first;
        }
    }
    check(identical, "every block size gives the same samples");
    K k;
    std::vector<float> bad(512, std::numeric_limits<float>::quiet_NaN()), o(512), p(512);
    counting = true;
    k.process(bad.data(), bad.data(), o.data(), p.data(), bad.size());
    const bool silent = std::all_of(o.begin(), o.end(), [](float v) { return v == 0.f; });
    k.set(K::Cabinet, K::FourByTwelve);
    k.set(K::Microphone, K::Far);
    k.process(x.data(), x.data(), o.data(), p.data(), 512);
    counting = false;
    check(allocations == 0, "processing and moved controls allocate nothing");
    check(silent, "a NaN input is silence");
    K z;
    z.set(K::DryWet, 0);
    std::vector<float> left(4800, 0.25f), shared(4800, -0.5f), rightOut(4800);
    z.process(left.data(), shared.data(), shared.data(), rightOut.data(), 4800);
    check(shared.back() == 0.25f && rightOut.back() == -0.5f, "an output plane that is the other channel's input is read first");
}

}  // namespace

int main() {
    responses();
    routing();
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAILED" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
