// SPDX-License-Identifier: GPL-3.0-or-later
// Multiband Dynamics against Ableton Live 11.2.7. Every reference number below was
// measured from Live's own renders of the same signals
// (collab/mac/2026-10-03-multiband-dynamics-pd.md, collab/mac/live-probe/).
#include "adi/dsp/multiband_dynamics.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <numbers>
#include <vector>
namespace {
thread_local bool countAllocations = false;
thread_local std::size_t allocations = 0;
} // namespace
void *operator new(std::size_t n) {
    if (countAllocations)
        ++allocations;
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
using MD = adi::dsp::MultibandDynamics;
int checks = 0, failures = 0;
void check(bool ok, const char *text) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", text);
    }
}
void near(double got, double want, double tol, const char *text) {
    check(std::abs(got - want) <= tol, text);
    if (std::abs(got - want) > tol)
        std::printf("     got %.6f want %.6f (tol %.4f)\n", got, want, tol);
}
// Live 11.2.7's single-band neutral response to a 0.5 impulse, first 64 samples.
const std::array<float, 64> kLiveImpulse{
    0x1.9791e6p-13f, 0x1.81108p-7f, 0x1.be1d54p-4f, 0x1.3bc95p-2f,
    0x1.a17cb4p-3f, -0x1.62f288p-3f, 0x1.df0c8p-9f, 0x1.9b5f12p-4f,
    -0x1.ebf88ep-4f, 0x1.6efec8p-4f, -0x1.5752eap-5f, -0x1.88311p-9f,
    0x1.297644p-5f, -0x1.cbde4p-5f, 0x1.062428p-4f, -0x1.00ec0ap-4f,
    0x1.c4d054p-5f, -0x1.6a41cep-5f, 0x1.029cb4p-5f, -0x1.336bfep-6f,
    0x1.bb62ep-8f, 0x1.f836f8p-9f, -0x1.a2c658p-7f, 0x1.46b7bcp-6f,
    -0x1.9fb3d8p-6f, 0x1.de6668p-6f, -0x1.02d19p-5f, 0x1.0c4aeep-5f,
    -0x1.0d3c78p-5f, 0x1.07330cp-5f, -0x1.f73a2ep-6f, 0x1.d78628p-6f,
    -0x1.b18964p-6f, 0x1.8731fp-6f, -0x1.5a2068p-6f, 0x1.2bae32p-6f,
    -0x1.f9e96cp-7f, 0x1.9da9ecp-7f, -0x1.43fe1ap-7f, 0x1.dbe4d4p-8f,
    -0x1.38990cp-8f, 0x1.3e5becp-9f, -0x1.056f4p-12f, -0x1.ce2158p-10f,
    0x1.d898ecp-9f, -0x1.5a04fep-8f, 0x1.bce93ep-8f, -0x1.0aa784p-7f,
    0x1.31d15p-7f, -0x1.54303p-7f, 0x1.7207bcp-7f, -0x1.8b9ec4p-7f,
    0x1.a13dap-7f, -0x1.b32cccp-7f, 0x1.c1b3e4p-7f, -0x1.cd18b8p-7f,
    0x1.d59ec8p-7f, -0x1.db86c4p-7f, 0x1.df0e42p-7f, -0x1.e06f88p-7f,
    0x1.dfe174p-7f, -0x1.dd9778p-7f, 0x1.d9c184p-7f, -0x1.d48c46p-7f,
};
// Live 11.2.7 at 44100 Hz (engine and export): the same response, 32 samples.
const std::array<float, 32> kLiveImpulse44k{
    0x1.9791e6p-13f, 0x1.81108p-7f, 0x1.be1d54p-4f, 0x1.3bc95p-2f,
    0x1.a17cb4p-3f, -0x1.62f288p-3f, 0x1.df0c8p-9f, 0x1.9b5f12p-4f,
    -0x1.ebf88ep-4f, 0x1.6efec8p-4f, -0x1.5752eap-5f, -0x1.88311p-9f,
    0x1.297644p-5f, -0x1.cbde4p-5f, 0x1.062428p-4f, -0x1.00ec0ap-4f,
    0x1.c4d054p-5f, -0x1.6a41cep-5f, 0x1.029cb4p-5f, -0x1.336bfep-6f,
    0x1.bb62ep-8f, 0x1.f836f8p-9f, -0x1.a2c658p-7f, 0x1.46b7bcp-6f,
    -0x1.9fb3d8p-6f, 0x1.de6668p-6f, -0x1.02d19p-5f, 0x1.0c4aeep-5f,
    -0x1.0d3c78p-5f, 0x1.07330cp-5f, -0x1.f73a2ep-6f, 0x1.d78628p-6f,
};
// Live 11.2.7 at 96000 Hz (engine and export): the same response, 32 samples.
const std::array<float, 32> kLiveImpulse96k{
    0x1.9791e6p-13f, 0x1.81108p-7f, 0x1.be1d54p-4f, 0x1.3bc95p-2f,
    0x1.a17cb4p-3f, -0x1.62f288p-3f, 0x1.df0c8p-9f, 0x1.9b5f12p-4f,
    -0x1.ebf88ep-4f, 0x1.6efec8p-4f, -0x1.5752eap-5f, -0x1.88311p-9f,
    0x1.297644p-5f, -0x1.cbde4p-5f, 0x1.062428p-4f, -0x1.00ec0ap-4f,
    0x1.c4d054p-5f, -0x1.6a41cep-5f, 0x1.029cb4p-5f, -0x1.336bfep-6f,
    0x1.bb62ep-8f, 0x1.f836f8p-9f, -0x1.a2c658p-7f, 0x1.46b7bcp-6f,
    -0x1.9fb3d8p-6f, 0x1.de6668p-6f, -0x1.02d19p-5f, 0x1.0c4aeep-5f,
    -0x1.0d3c78p-5f, 0x1.07330cp-5f, -0x1.f73a2ep-6f, 0x1.d78628p-6f,
};
// Band magnitudes (dB) of Live's solo impulse responses at kFreqs.
const std::array<double, 7> kFreqs{50, 120, 500, 1000, 2500, 5000, 10000};
const std::array<double, 7> k_n_soloLow{-0.272263, -6.038481, -49.636954, -73.676140, -105.484711, -130.379894, -151.100811};
const std::array<double, 7> k_n_soloMid{-30.746575, -6.034369, -0.043852, -0.220761, -6.023310, -24.831009, -49.389301};
const std::array<double, 7> k_n_soloHigh{-110.254159, -112.000271, -55.994760, -32.118547, -6.023221, -0.514196, -0.029831};
const std::array<double, 7> k_n_neutral{-0.015994, -0.015803, -0.001327, -0.000958, -0.002620, -0.001231, -0.000312};
const std::array<double, 7> k_x1000_8000_soloLow{-0.000317, -0.002122, -0.527485, -6.023348, -32.119376, -56.231647, -81.260439};
const std::array<double, 7> k_x1000_8000_soloMid{-104.004670, -73.658445, -24.620125, -6.027311, -0.297221, -1.187124, -11.072846};
const std::array<double, 7> k_x1000_8000_soloHigh{-128.411800, -127.717585, -97.110802, -73.056695, -41.217503, -17.995622, -2.851559};
// The probes' baseline: every band active and neutral, RMS, hard knee, 10/100 ms.
void neutral(MD &m) {
    m.set(MD::SoftKnee, 0);
    for (int b = 0; b < 3; ++b) {
        m.set(static_cast<MD::Param>(MD::AboveThresholdLow + b), 0);
        m.set(static_cast<MD::Param>(MD::BelowThresholdLow + b), -80);
        m.set(static_cast<MD::Param>(MD::AttackLow + b), 10);
        m.set(static_cast<MD::Param>(MD::ReleaseLow + b), 100);
    }
}
struct Stereo {
    std::vector<float> l, r;
};
Stereo render(MD &m, const Stereo &in, std::size_t block = 64, const Stereo *sc = nullptr) {
    Stereo out{std::vector<float>(in.l.size()), std::vector<float>(in.l.size())};
    for (std::size_t i = 0; i < in.l.size(); i += block) {
        const auto n = std::min(block, in.l.size() - i);
        m.process(in.l.data() + i, in.r.data() + i, out.l.data() + i, out.r.data() + i, n,
                  sc ? sc->l.data() + i : nullptr, sc ? sc->r.data() + i : nullptr);
    }
    return out;
}
Stereo mono(std::vector<float> x) { return {x, x}; }
std::vector<float> dc(std::size_t n, double db) {
    return std::vector<float>(n, static_cast<float>(std::pow(10.0, db / 20.0)));
}
double db(double v) { return 20.0 * std::log10(std::abs(v)); }
double magnitude(const std::vector<float> &x, std::size_t from, double hz) {
    std::complex<double> sum = 0;
    for (std::size_t k = from; k < x.size(); ++k)
        sum += static_cast<double>(x[k]) *
               std::polar(1.0, -2 * std::numbers::pi * hz * static_cast<double>(k - from) / 48000);
    return std::abs(sum);
}

// HIIR's coefficient design, re-derived here so the constant in the core is checked.
std::array<double, 8> halfbandDesign() {
    const double tbw = 0.01;
    double k = std::tan((1 - tbw * 2) * std::numbers::pi / 4);
    k *= k;
    const double kk = std::pow(1 - k * k, 0.25), e = 0.5 * (1 - kk) / (1 + kk), e2 = e * e,
                 e4 = e2 * e2, q = e * (1 + e4 * (2 + e4 * (15 + 150 * e4)));
    std::array<double, 8> out{};
    const int order = 17;
    for (int i = 0; i < 8; ++i) {
        const int c = i + 1;
        double num = 0, den = 0;
        for (int j = 0, s = 1;; ++j, s = -s) {
            const double t = std::pow(q, j * (j + 1)) * std::sin((j * 2 + 1) * c * std::numbers::pi / order) * s;
            num += t;
            if (std::abs(t) <= 1e-100)
                break;
        }
        for (int j = 1, s = -1;; ++j, s = -s) {
            const double t = std::pow(q, j * j) * std::cos(j * 2 * c * std::numbers::pi / order) * s;
            den += t;
            if (std::abs(t) <= 1e-100)
                break;
        }
        const double ww = num * std::pow(q, 0.25) / (den + 0.5), w2 = ww * ww;
        const double x = std::sqrt((1 - w2 * k) * (1 - w2 / k)) / (1 + w2);
        out[static_cast<std::size_t>(i)] = (1 - x) / (1 + x);
    }
    return out;
}

void resampler() {
    const auto design = halfbandDesign();
    bool same = true;
    for (std::size_t i = 0; i < 8; ++i)
        same &= static_cast<float>(design[i]) == MD::kHalfbandCoefficients[i];
    check(same, "half-band coefficients are HIIR's 8-coefficient, 0.01-transition design");
    MD m;
    neutral(m);
    m.set(MD::LowBandOn, 0);
    m.set(MD::HighBandOn, 0);
    m.prepare(48000);
    std::vector<float> x(256);
    x[0] = 0.5f;
    const auto y = render(m, mono(x));
    bool exact = true;
    for (std::size_t i = 0; i < kLiveImpulse.size(); ++i)
        exact &= y.l[i] == kLiveImpulse[i] && y.r[i] == kLiveImpulse[i];
    check(exact, "single-band impulse response equals Live's render bit for bit");
    // 2x at any host rate: Live rendered at 44.1 and 96 kHz agrees just as exactly
    const auto atRate = [](double rate, const std::array<float, 32> &want) {
        MD r;
        neutral(r);
        r.set(MD::LowBandOn, 0);
        r.set(MD::HighBandOn, 0);
        r.prepare(rate);
        std::vector<float> in(64);
        in[0] = 0.5f;
        const auto got = render(r, mono(in));
        bool same = true;
        for (std::size_t i = 0; i < want.size(); ++i)
            same &= got.l[i] == want[i];
        return same;
    };
    check(atRate(44100, kLiveImpulse44k), "44.1 kHz single-band impulse equals Live's bit for bit");
    check(atRate(96000, kLiveImpulse96k), "96 kHz single-band impulse equals Live's bit for bit");
}

void crossovers() {
    struct Case {
        double low, high;
        int solo;
        const std::array<double, 7> *want;
        const char *what;
    } cases[] = {{120, 2500, 0, &k_n_soloLow, "low band 120 Hz matches Live"},
                 {120, 2500, 1, &k_n_soloMid, "mid band 120/2500 matches Live"},
                 {120, 2500, 2, &k_n_soloHigh, "high band 2500 Hz matches Live"},
                 {120, 2500, -1, &k_n_neutral, "neutral three-band sum matches Live"},
                 {1000, 8000, 0, &k_x1000_8000_soloLow, "low band 1 kHz matches Live"},
                 {1000, 8000, 1, &k_x1000_8000_soloMid, "mid band 1k/8k matches Live"},
                 {1000, 8000, 2, &k_x1000_8000_soloHigh, "high band 8 kHz matches Live"}};
    for (const auto &c : cases) {
        MD m;
        neutral(m);
        m.set(MD::LowMidCrossover, c.low);
        m.set(MD::MidHighCrossover, c.high);
        if (c.solo >= 0)
            m.set(static_cast<MD::Param>(MD::SoloLow + c.solo), 1);
        m.prepare(48000);
        std::vector<float> x(16384);
        x[0] = 0.5f;
        const auto y = render(m, mono(x));
        bool ok = true;
        for (std::size_t i = 0; i < kFreqs.size(); ++i) {
            const double got = db(magnitude(y.l, 0, kFreqs[i]) / 0.5), want = (*c.want)[i];
            // to a few thousandths of a dB in the passband; below -90 dB both are float noise
            const double tol = want > -60 ? 0.02 : 1.0;
            const bool bad = want > -90 ? std::abs(got - want) > tol : got > -80;
            if (bad) {
                ok = false;
                std::printf("     %s %.0f Hz: got %.4f want %.4f\n", c.what, kFreqs[i], got, want);
            }
        }
        check(ok, c.what);
    }
}

// Static curves: DC at a level, peak detection, 0.1 ms times, read once settled.
double staticGain(double level, void (*setup)(MD &)) {
    MD m;
    neutral(m);
    m.set(MD::LowBandOn, 0);
    m.set(MD::HighBandOn, 0);
    m.set(MD::PeakMode, 1);
    m.set(MD::AttackMid, 0.1);
    m.set(MD::ReleaseMid, 0.1);
    setup(m);
    m.prepare(48000);
    const auto y = render(m, mono(dc(4800, level)));
    return db(y.l.back()) - level;
}
void statics() {
    struct Case {
        double level, want;
        void (*setup)(MD &);
        const char *what;
    };
    const auto comp = [](MD &m) { m.set(MD::AboveThresholdMid, -20), m.set(MD::AboveRatioMid, -0.75); };
    const auto knee = [](MD &m) {
        m.set(MD::AboveThresholdMid, -20), m.set(MD::AboveRatioMid, -0.75), m.set(MD::SoftKnee, 1);
    };
    const auto expand = [](MD &m) { m.set(MD::BelowThresholdMid, -50), m.set(MD::BelowRatioMid, -3); };
    const auto upward = [](MD &m) {
        m.set(MD::BelowThresholdMid, -50), m.set(MD::BelowRatioMid, 0.5), m.set(MD::SoftKnee, 1);
    };
    const auto upx = [](MD &m) { m.set(MD::AboveThresholdMid, -20), m.set(MD::AboveRatioMid, 1); };
    const auto crossed = [](MD &m) {
        m.set(MD::AboveThresholdMid, -40), m.set(MD::AboveRatioMid, -0.75);
        m.set(MD::BelowThresholdMid, -20), m.set(MD::BelowRatioMid, 0.5);
    };
    const auto amount = [](MD &m) {
        m.set(MD::AboveThresholdMid, -20), m.set(MD::AboveRatioMid, -0.75), m.set(MD::Amount, 50);
    };
    const Case cases[] = {
        {-24, 0.0000, comp, "4:1 above -20 dB at -24 dB (Live)"},
        {-16, -3.0013, comp, "4:1 above -20 dB at -16 dB (Live)"},
        {-8, -9.0043, comp, "4:1 above -20 dB at -8 dB (Live)"},
        {0, -15.0022, comp, "4:1 above -20 dB at 0 dB (Live)"},
        {6, -19.5008, comp, "4:1 above -20 dB at 6 dB (Live)"},
        {-30, 0.0000, knee, "soft knee, 4:1 at -20 at -30 dB (Live)"},
        {-24, -0.6746, knee, "soft knee, 4:1 at -20 at -24 dB (Live)"},
        {-20, -1.8769, knee, "soft knee, 4:1 at -20 at -20 dB (Live)"},
        {-16, -3.6760, knee, "soft knee, 4:1 at -20 at -16 dB (Live)"},
        {-8, -9.0043, knee, "soft knee, 4:1 at -20 at -8 dB (Live)"},
        {-80, -89.9802, expand, "1:4 expansion below -50 at -80 dB (Live)"},
        {-60, -29.9927, expand, "1:4 expansion below -50 at -60 dB (Live)"},
        {-52, -5.9864, expand, "1:4 expansion below -50 at -52 dB (Live)"},
        {-62, 5.9967, upward, "upward compression with knee at -62 dB (Live)"},
        {-56, 3.1966, upward, "upward compression with knee at -56 dB (Live)"},
        {-50, 1.2492, upward, "upward compression with knee at -50 dB (Live)"},
        {-44, 0.2000, upward, "upward compression with knee at -44 dB (Live)"},
        {-10, 10.0004, upx, "1:2 upward expansion at -10 dB (Live)"},
        {0, 20.0019, upx, "1:2 upward expansion at 0 dB (Live)"},
        {-60, 20.0003, crossed, "crossed thresholds add at -60 dB (Live)"},
        {-30, -2.5027, crossed, "crossed thresholds add at -30 dB (Live)"},
        {-10, -22.5015, crossed, "crossed thresholds add at -10 dB (Live)"},
        {-10, -3.7506, amount, "Amount 50% halves r at -10 dB (Live)"},
        {0, -7.5004, amount, "Amount 50% halves r at 0 dB (Live)"},
    };
    for (const auto &c : cases)
        near(staticGain(c.level, c.setup), c.want, 0.01, c.what);
    // the cap: upward compression from far below asks for +60 dB and gets 64x
    near(staticGain(-140, [](MD &m) { m.set(MD::BelowThresholdMid, -40), m.set(MD::BelowRatioMid, 1); }),
         20 * std::log10(64.0), 0.002, "upward gain stops at 64x (+36.12 dB)");
    near(staticGain(-10, [](MD &m) { m.set(MD::AboveThresholdMid, -80), m.set(MD::AboveRatioMid, 1); }),
         20 * std::log10(64.0), 0.002, "upward expansion stops at 64x too");
    near(staticGain(-140, [](MD &m) {
             m.set(MD::BelowThresholdMid, -40), m.set(MD::BelowRatioMid, 1), m.set(MD::InputGainMid, 24);
         }),
         24 + 20 * std::log10(64.0), 0.002, "input gain sits outside the cap");
    near(staticGain(0, [](MD &m) { m.set(MD::AboveThresholdMid, -80), m.set(MD::AboveRatioMid, -1); }),
         -80, 0.01, "no floor on downward gain");
    near(staticGain(-10, [](MD &m) { m.set(MD::AboveThresholdMid, -20), m.set(MD::AboveRatioMid, -1), m.set(MD::Amount, 0); }),
         0, 1e-4, "Amount 0 is a ratio of 1");
}

// DC from -40 to -6 dB at 0.5 s and back at 1.5 s; threshold -20, 4:1, single band.
std::vector<double> ballistics(bool peak, double attack, double release, std::size_t t0,
                               std::initializer_list<double> ms) {
    std::vector<float> x(100000, static_cast<float>(std::pow(10.0, -40 / 20.0)));
    std::fill(x.begin() + 24000, x.begin() + 72000, static_cast<float>(std::pow(10.0, -6 / 20.0)));
    auto make = [&](bool compress) {
        MD m;
        neutral(m);
        m.set(MD::LowBandOn, 0);
        m.set(MD::HighBandOn, 0);
        m.set(MD::PeakMode, peak ? 1 : 0);
        m.set(MD::AttackMid, attack);
        m.set(MD::ReleaseMid, release);
        if (compress)
            m.set(MD::AboveThresholdMid, -20), m.set(MD::AboveRatioMid, -0.75);
        m.prepare(48000);
        return render(m, mono(x)).l;
    };
    const auto y = make(true), ref = make(false);
    std::vector<double> out;
    for (double t : ms) {
        const auto i = t0 + static_cast<std::size_t>(std::lround(t * 48));
        out.push_back(db(y[i] / ref[i]));
    }
    return out;
}
void envelopes() {
    struct Case {
        bool peak;
        double attack, release;
        std::size_t t0;
        std::initializer_list<double> ms;
        std::vector<double> want;
        const char *what;
    };
    const Case cases[] = {
        {true, 10, 1, 24000, {1, 2, 5, 10}, {-7.0889, -9.3302, -10.4331, -10.5001}, "peak attack 10 ms (Live)"},
        {false, 10, 1, 24000, {1, 2, 5}, {-8.7779, -9.9173, -10.4679}, "RMS attack 10 ms (Live)"},
        {true, 100, 1, 24000, {5, 10, 20, 50}, {-4.1949, -7.2612, -9.3976, -10.4357}, "peak attack 100 ms (Live)"},
        {true, 0.1, 100, 72000, {5, 10, 15}, {-7.6938, -4.7339, -1.9064}, "peak release 100 ms (Live)"},
        {true, 0.1, 1000, 72000, {50, 100, 150}, {-7.5830, -4.7003, -1.8769}, "peak release 1000 ms (Live)"},
    };
    for (const auto &c : cases) {
        const auto got = ballistics(c.peak, c.attack, c.release, c.t0, c.ms);
        bool ok = true;
        for (std::size_t i = 0; i < got.size(); ++i)
            if (std::abs(got[i] - c.want[i]) > 0.03) {
                ok = false;
                std::printf("     %s #%zu: got %.4f want %.4f\n", c.what, i, got[i], c.want[i]);
            }
        check(ok, c.what);
    }
}

void stereoLink() {
    // Live: opposite-polarity channels compress as fully as in-phase ones (mean of |L|,|R|)
    for (int peak = 0; peak < 2; ++peak) {
        MD m;
        neutral(m);
        m.set(MD::LowBandOn, 0), m.set(MD::HighBandOn, 0), m.set(MD::PeakMode, peak);
        m.set(MD::AboveThresholdMid, -20), m.set(MD::AboveRatioMid, -0.75);
        m.prepare(48000);
        auto l = dc(48000, -6), r = l;
        for (auto &v : r)
            v = -v;
        const auto y = render(m, {l, r});
        near(db(y.l.back() / l.back()), -10.5, 0.01, peak ? "peak link: polarity does not matter" : "RMS link: polarity does not matter");
        check(std::abs(y.l.back() + y.r.back()) < 1e-7f, "one gain for both channels");
    }
    MD m;
    neutral(m);
    m.set(MD::LowBandOn, 0), m.set(MD::HighBandOn, 0), m.set(MD::PeakMode, 1);
    m.set(MD::AboveThresholdMid, -20), m.set(MD::AboveRatioMid, -0.75);
    m.prepare(48000);
    const auto y = render(m, {dc(48000, -6), dc(48000, -40)});
    near(db(y.l.back()) + 6, -6.113, 0.01, "peak link averages |L| and |R| (Live: -6.113 dB)");
    near(db(y.r.back()) + 40, -6.113, 0.01, "the quiet channel gets the same gain");
}

void sidechain() {
    // the trigger is cos/sin of Mix between the input and the sidechain, before the resampler
    MD m;
    neutral(m);
    m.set(MD::LowBandOn, 0), m.set(MD::HighBandOn, 0), m.set(MD::PeakMode, 1);
    m.set(MD::AboveThresholdMid, -20), m.set(MD::AboveRatioMid, -0.75);
    m.set(MD::SidechainOn, 1), m.set(MD::SidechainMix, 50);
    m.prepare(48000);
    const auto in = mono(dc(48000, -30)), sc = mono(dc(48000, -6));
    const auto y = render(m, in, 64, &sc);
    near(db(y.l.back()) + 30, -8.646, 0.01, "Mix 50% is an equal-power blend (Live: -8.646 dB)");
    MD q;
    neutral(q);
    q.set(MD::LowBandOn, 0), q.set(MD::HighBandOn, 0), q.set(MD::SidechainOn, 1),
        q.set(MD::SidechainListen, 1), q.set(MD::SidechainGain, 20 * std::log10(2.0));
    q.prepare(48000);
    const auto heard = render(q, in, 64, &sc);
    near(heard.l.back(), 2 * std::pow(10.0, -6 / 20.0), 1e-5, "Listen plays the trigger, gain included");
    MD off;
    neutral(off);
    off.set(MD::AboveThresholdMid, -20), off.set(MD::AboveRatioMid, -0.75);
    off.set(MD::LowBandOn, 0), off.set(MD::HighBandOn, 0), off.set(MD::SidechainOn, 1),
        off.set(MD::SidechainMix, 0);
    off.prepare(48000);
    near(db(render(off, in, 64, &sc).l.back()) + 30, 0, 1e-4, "Mix 0% ignores the sidechain");
}

void routing() {
    std::vector<float> x(16384);
    x[0] = 0.5f;
    auto energy = [](const std::vector<float> &v) {
        double e = 0;
        for (float s : v)
            e += static_cast<double>(s) * s;
        return e;
    };
    for (double f : {2000.0, 500.0}) {
        MD m;
        neutral(m);
        m.set(MD::LowMidCrossover, f), m.set(MD::MidHighCrossover, 500), m.set(MD::SoloMid, 1);
        m.prepare(48000);
        check(energy(render(m, mono(x)).l) == 0, "low split at or above the high split: mid band silent");
    }
    MD crossed;
    neutral(crossed);
    crossed.set(MD::LowMidCrossover, 2000), crossed.set(MD::MidHighCrossover, 1000), crossed.set(MD::SoloLow, 1);
    crossed.prepare(48000);
    near(db(magnitude(render(crossed, mono(x)).l, 0, 1000) / 0.5), -6.02, 0.05, "crossed splits both sit at the high split");
    MD inactive;
    neutral(inactive);
    inactive.set(MD::OutputGainHigh, 6), inactive.set(MD::InputGainHigh, 6), inactive.set(MD::ActiveHigh, 0);
    inactive.prepare(48000);
    MD plain;
    neutral(plain);
    plain.prepare(48000);
    check(render(inactive, mono(x)).l == render(plain, mono(x)).l, "an inactive band ignores its gains");
    MD gone;
    neutral(gone);
    gone.set(MD::LowBandOn, 0), gone.set(MD::SoloLow, 1);
    gone.prepare(48000);
    check(energy(render(gone, mono(x)).l) == 0, "soloing a band that does not exist is silence");
    MD soloInactive;
    neutral(soloInactive);
    soloInactive.set(MD::SoloMid, 1), soloInactive.set(MD::ActiveMid, 0), soloInactive.set(MD::OutputGainMid, 6);
    soloInactive.prepare(48000);
    near(db(magnitude(render(soloInactive, mono(x)).l, 0, 1000) / 0.5), k_n_soloMid[3], 0.02, "a soloed inactive band plays raw");
}

void engineering() {
    // block size never changes a sample
    std::vector<float> x(48000);
    for (std::size_t i = 0; i < x.size(); ++i)
        x[i] = static_cast<float>(0.5 * std::sin(2 * std::numbers::pi * 997 * static_cast<double>(i) / 48000) *
                                  (i % 9000 < 4500 ? 1.0 : 0.05));
    auto run = [&](std::size_t block) {
        MD m;
        m.set(MD::AboveRatioLow, -0.75), m.set(MD::AboveRatioMid, -0.5), m.set(MD::BelowRatioHigh, 0.5);
        m.prepare(48000);
        return render(m, mono(x), block);
    };
    const auto ref = run(64);
    bool same = true;
    for (std::size_t b : {1u, 32u, 333u, 4096u})
        same &= run(b).l == ref.l;
    check(same, "every block size renders the same samples");
    MD m;
    m.prepare(48000);
    std::vector<float> l(4096), r(4096), sl(4096), sr(4096), ol(4096), orr(4096);
    m.set(MD::SidechainOn, 1);
    allocations = 0;
    countAllocations = true;
    m.set(MD::MasterOutput, -6);
    m.process(l.data(), r.data(), ol.data(), orr.data(), l.size(), sl.data(), sr.data());
    countAllocations = false;
    check(allocations == 0, "set and process allocate nothing");
    // a parameter jump is Live's S-curve, two 94-sample boxcars: half way at 93, done at 187
    MD s;
    neutral(s);
    s.set(MD::LowBandOn, 0), s.set(MD::HighBandOn, 0);
    s.prepare(48000);
    std::vector<float> one(6000, 1.0f), out(6000), outR(6000);
    s.process(one.data(), one.data(), out.data(), outR.data(), 4800);
    s.set(MD::MasterOutput, -20);
    s.process(one.data() + 4800, one.data() + 4800, out.data() + 4800, outR.data() + 4800, 1200);
    near(db(out[4799]), 0, 1e-4, "before the jump");
    near(db(out[4800 + 93]), -10, 0.25, "half way after ~2 ms");
    near(db(out[4800 + 187]), -20, 1e-3, "settled after ~4 ms");
    MD clamp;
    clamp.set(MD::AttackLow, 1e9);
    clamp.set(MD::LowMidCrossover, std::nan(""));
    check(clamp.get(MD::AttackLow) == 5000 && clamp.get(MD::LowMidCrossover) == 120,
          "values clamp to Live's ranges and NaN is ignored");
}
} // namespace
int main() {
    resampler();
    crossovers();
    statics();
    envelopes();
    stereoLink();
    sidechain();
    routing();
    engineering();
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
