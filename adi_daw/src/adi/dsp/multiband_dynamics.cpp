// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/multiband_dynamics.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#if defined(__SSE__) || defined(_M_X64)
#include <xmmintrin.h>
#endif
// Bit-exactness against Live's float pipeline needs every rounding where Live
// has one: no fused multiply-adds (cmake/devices/multiband_dynamics.cmake also
// passes -ffp-contract=off, because GCC ignores this pragma).
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#endif
namespace adi::dsp {
namespace {
constexpr double kPi = 3.14159265358979323846;
// Live's Butterworth sections use 0.707, not 1/sqrt(2): the measured dip of
// -0.0026 dB at every crossover is exactly 20*log10(2*0.707^2).
constexpr double kQ = 0.707;
// The gain computer's constants, all in log2 units (see staticGainLog2):
// the dynamic gain never exceeds 64x = 2^6 exactly (+36.12 dB), measured with every
// way of asking for more (upward compression, upward expansion, both at once, with
// +-24 dB around it). A cap of 20*log10(64) dB converted like a threshold (6.0006)
// breaks 25 probes (cap_*, up_*_bm40, st_below-50_r*, st_both_peak, fine_b0_*,
// gap_up_*, loud_upx_cap, denorm_up, thr_cross_*, ...). Capping 2^x at 64 afterwards
// is the same thing; whether Live caps the sum of the two sides or each side, no
// probe can tell yet (they never push both at once). The soft knee spans
// 10 dB either side of the threshold, converted like a threshold (10 / 6.02), and
// its quadratic r*t^2/(4*half) is computed as r*(t*t*0.1505): 6.02/40 and
// 0.25/half are that same float.
constexpr float kGainCapLog2 = 6.0f;
constexpr float kOctavesPerDb = 1.0f / MultibandDynamics::kDbPerOctave;
constexpr float kKneeHalf = 10.0f * kOctavesPerDb; // the same float as 10 / 6.02f
constexpr float kKneeCurve = MultibandDynamics::kDbPerOctave / 40.0f;
// Live's log2 and 2^x approximations: the continuous least-squares cubics (endpoints
// pinned, p(0) = 0, p(1) = 1 and q(0) = 1, q(1) = 2) of log2(1 + t) and 2^f on [0, 1],
// rounded to float -- 1.4208645374, -0.5772506508, 0.1563861134 and 0.6959284704,
// 0.2249463111, 0.0791252185. Not the minimax cubics (log2 would be 1.42286524,
// -0.58208523, 0.15921999). Found by fitting Live's periodic static error (6.3 mdB
// humps per octave of level, 1.2 mdB per octave of gain) on the 0.02 dB sweeps
// fine_*, then confirmed to the last bit there.
constexpr float kLog1 = 0x1.6bbdc8p+0f, kLog2 = -0x1.278d66p-1f, kLog3 = 0x1.40475cp-3f;
constexpr float kExp1 = 0x1.6450bcp-1f, kExp2 = 0x1.ccb0a6p-3f, kExp3 = 0x1.4418cep-4f;
float floatFromBits(std::uint32_t bits) noexcept {
    float f;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}
std::uint32_t bitsOfFloat(float f) noexcept {
    std::uint32_t bits;
    std::memcpy(&bits, &f, sizeof bits);
    return bits;
}
// ARMv8's 8-bit reciprocal square root and reciprocal estimates (FRSQRTE and FRECPE),
// as the ARM Architecture Reference Manual defines them (RecipSqrtEstimate and
// RecipEstimate on a 9-bit fraction), tabulated at compile time. Live's RMS level
// goes through both (see rmsAmplitude); this emulation matches an Apple M1's
// instructions for every positive float.
constexpr int rsqrtEstimate9(int a) noexcept { // a in 128..511: [0.25, 1) in 1/512ths
    a = a < 256 ? a * 2 + 1 : ((a >> 1) << 1) * 2 + 2;
    int lo = 512, hi = 1448; // the largest b with a*b^2 < 2^28 (ARM counts up; same b)
    while (lo < hi) {
        const int mid = (lo + hi + 1) / 2;
        if (static_cast<long long>(a) * mid * mid < (1LL << 28))
            lo = mid;
        else
            hi = mid - 1;
    }
    return (lo + 1) / 2;
}
constexpr int recipEstimate9(int a) noexcept { // a in 256..511: [0.5, 1) in 1/512ths
    return ((1 << 19) / (a * 2 + 1) + 1) / 2;
}
constexpr auto kRsqrtEstimate = [] {
    std::array<std::uint8_t, 512> t{};
    for (int a = 128; a < 512; ++a)
        t[static_cast<std::size_t>(a)] = static_cast<std::uint8_t>(rsqrtEstimate9(a) & 255);
    return t;
}();
constexpr auto kRecipEstimate = [] {
    std::array<std::uint8_t, 256> t{};
    for (int a = 256; a < 512; ++a)
        t[static_cast<std::size_t>(a - 256)] = static_cast<std::uint8_t>(recipEstimate9(a) & 255);
    return t;
}();
float frsqrte(float x) noexcept { // x > 0 and finite
    const std::uint32_t bits = bitsOfFloat(x);
    int exponent = static_cast<int>((bits >> 23) & 255u);
    std::uint32_t fraction = bits & 0x7fffffu;
    if (exponent == 0) { // subnormal: normalise (exponent goes to 0, -1, ...)
        while ((fraction & 0x400000u) == 0) {
            fraction <<= 1;
            --exponent;
        }
        fraction = (fraction << 1) & 0x7fffffu;
    }
    const std::size_t scaled = (exponent & 1) == 0 ? 256u | (fraction >> 15) : 128u | (fraction >> 16);
    const auto resultExponent = static_cast<std::uint32_t>((380 - exponent) / 2);
    return floatFromBits((resultExponent << 23) | (std::uint32_t{kRsqrtEstimate[scaled]} << 15));
}
float frecpe(float x) noexcept { // normal x > 0 below 2^126 (here: about 1/sqrt of a level)
    const std::uint32_t bits = bitsOfFloat(x);
    const auto exponent = (bits >> 23) & 255u;
    const std::uint32_t estimate = kRecipEstimate[(bits & 0x7fffffu) >> 15];
    return floatFromBits(((253u - exponent) << 23) | (estimate << 15));
}
// Every detector starts at 0.01 in its own units (-40 dB peak, -20 dB RMS, since
// the RMS detector holds a mean square), not at silence: a 50 s attack shows it.
constexpr float kEnvelopeStart = 0.01f;
using P = MultibandDynamics::Param;
struct Range {
    double min, max;
};
constexpr std::array<Range, P::Count> kRange{{{30, 3000},     {300, 15000},  {0, 1},
                                             {0, 1},         {-24, 24},     {0, 100},
                                             {10, 1000},     {-24, 24},     {-24, 24},
                                             {-24, 24},      {-24, 24},     {-24, 24},
                                             {-24, 24},      {0, 1},        {0, 1},
                                             {0, 1},         {-80, 0},      {-80, 0},
                                             {-80, 0},       {-80, 0},      {-80, 0},
                                             {-80, 0},       {-1, 1},       {-1, 1},
                                             {-1, 1},        {-3, 1},       {-3, 1},
                                             {-3, 1},        {0.1, 5000},   {0.1, 5000},
                                             {0.1, 5000},    {0.1, 5000},   {0.1, 5000},
                                             {0.1, 5000},    {0, 1},        {-70, 24},
                                             {0, 100},       {0, 1},        {0, 1},
                                             {0, 1},         {0, 1},        {0, 1},
                                             {0, 1}}};
// Live's gain knobs (band input and output gains, master) are powf(10, dB*0.05f):
// the float dB times the float 0.05, rounded, then the platform's powf (macOS libm,
// which is not correctly rounded). Each half is pinned: all 15 input gains of battery
// mbd_rt (rt_in-24 .. rt_in24) and all 10 masters (rt_master-24 .. rt_master24) are
// bit-exact in Listen with the product dB*0.05f (dB/20f fails -18, 9 and 18 dB, one or
// two ulps), and with powf rather than the double pow for the plain gains of mbd_gain
// (output 5.87 dB = 0x1.f733p+0, master -2.13 dB = 0x1.90a788p-1: double pow lands one
// ulp high on both); input 11.42 dB = 0x1.dca954p+1 needs the product (dB/20f with
// either pow gives ...94e or ...950). The S/C gain is no such knob: Live stores it as
// a linear factor (the set's sidechain Volume, ranging from this formula's -70 dB to
// its 24 dB, 15.8489332) and multiplies by a float within two ulps of it, which
// dbToGain returns for our dB value. Exact for 0.25, 0.7, 1, 1.5, 3 and -69.5 .. -40 dB
// (lis_3b_g0.25 nulls at only -77 dB one ulp off, through the 120 Hz filters); not yet
// explained: 0.5 acts one ulp high, 2.0 two ulps low (sc_listen, sc_g2), 4 and 8 one
// low, 24 dB as 15.8489332 (sc_gp24). This formula cannot produce 0.25 or 8 at all, and
// no round trip through dB, ln, log2 or a normalised value matches (battery mbd_rt).
float knobGain(double db) noexcept { return std::pow(10.0f, static_cast<float>(db) * 0.05f); }
bool isSwitch(P p) noexcept {
    switch (p) {
    case P::SoftKnee:
    case P::PeakMode:
    case P::ActiveLow:
    case P::ActiveMid:
    case P::ActiveHigh:
    case P::SidechainOn:
    case P::LowBandOn:
    case P::HighBandOn:
    case P::SoloLow:
    case P::SoloMid:
    case P::SoloHigh:
    case P::SidechainListen:
        return true;
    default:
        return false;
    }
}
float dbToGain(double db) noexcept { return static_cast<float>(std::pow(10.0, db / 20.0)); }
// Live renders with denormals flushed to zero: only then do the impulse probes
// null with a maximum error of exactly 0 (x30_300_soloLow, x30_soloLow_imp1e-6,
// n_small ...); without it their decaying tails part from Live's near 1e-38.
class FlushDenormals {
#if defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
    std::uint64_t saved_;

  public:
    FlushDenormals() noexcept {
        asm volatile("mrs %0, fpcr" : "=r"(saved_));
        asm volatile("msr fpcr, %0" ::"r"(saved_ | (std::uint64_t{1} << 24))); // FZ
    }
    ~FlushDenormals() { asm volatile("msr fpcr, %0" ::"r"(saved_)); }
#elif defined(__SSE__) || defined(_M_X64)
    unsigned saved_;

  public:
    FlushDenormals() noexcept : saved_(_mm_getcsr()) { _mm_setcsr(saved_ | 0x8040u); } // FTZ | DAZ
    ~FlushDenormals() { _mm_setcsr(saved_); }
#endif
};
} // namespace

// HIIR's design (de Soras) for 8 coefficients and a transition band of 0.01,
// as float. Live's 2x resampler reproduces with these to the last bit.
const std::array<float, 8> MultibandDynamics::kHalfbandCoefficients{
    0x1.3bdd06p-4f, 0x1.105a0ep-2f, 0x1.eda3ecp-2f, 0x1.548888p-1f,
    0x1.97f8dap-1f, 0x1.c4a8f4p-1f, 0x1.e1ebb6p-1f, 0x1.f6c96ap-1f};

float MultibandDynamics::AllpassChain::step(float v) noexcept {
    for (std::size_t i = 0; i < c.size(); ++i) {
        const float difference = v - y[i];
        const float scaled = difference * c[i];
        const float out = scaled + x[i];
        x[i] = v;
        y[i] = out;
        v = out;
    }
    return v;
}

void MultibandDynamics::HalfbandUp::step(float in, float &first, float &second) noexcept {
    first = even.step(in);
    second = odd.step(in);
}

// The downsampler pairs each first sample with the PREVIOUS second one.
float MultibandDynamics::HalfbandDown::step(float first, float second) noexcept {
    const float a = even.step(first);
    const float b = odd.step(held);
    held = second;
    const float sum = a + b;
    return 0.5f * sum;
}

// Live's crossover filters, held against Live 11.2.7 renders of exact impulses
// (probe sets mbd_a..mbd_c, every split pair and both one-split modes): with the
// arithmetic below, in this order, each band nulls to the last bit.
float MultibandDynamics::Biquad::step(float v) noexcept {
    float y = b0 * v;
    y = y + b1 * x1;
    y = y + b2 * x2;
    y = y - a1 * y1;
    y = y - a2 * y2;
    x2 = x1;
    x1 = v;
    y2 = y1;
    y1 = y;
    return y;
}

// The RBJ cookbook section, all in float: w = 2*pi*f times the float reciprocal of
// the (oversampled) rate, left to right (a double w is off by an ulp at 3000 Hz and
// 2999 Hz), cosf/sinf, alpha = sin/(2Q), and every coefficient multiplied by 1/a0
// rather than divided by a0 (dividing nulls only to -54 dB at 120 Hz). The allpass
// is the same section's denominator turned round: b = (a2, a1, 1).
// Multiplying by 1/fs is not dividing by fs: at a 96 kHz host (192 kHz inside) the
// division puts w for 2500 Hz an ulp high and a2 an ulp low, and the 120/2500 probes
// null only to -97..-107 dB (sr96_neutral, sr96_solo*, sr96_lis_3b); with 1/fs they
// are exact. At 44.1 and 48 kHz both spellings give the same coefficients at every
// measured split. A float w taken from the ratio f/fs (2*pi in double) is exact at
// 96 kHz too, but moves the 200 Hz section: mb_x200_5000_dyn -84.9 -> -72.4 dB.
void MultibandDynamics::Biquad::design(float frequency, float rate, Kind kind) noexcept {
    const float q = static_cast<float>(kQ);
    const float w = 2.0f * static_cast<float>(kPi) * frequency * (1.0f / rate);
    const float cs = std::cos(w);
    const float sn = std::sin(w);
    const float alpha = sn / (2.0f * q);
    const float inverse = 1.0f / (1.0f + alpha);
    a1 = -2.0f * cs * inverse;
    a2 = (1.0f - alpha) * inverse;
    switch (kind) {
    case Kind::LowPass:
        b0 = (1.0f - cs) * 0.5f * inverse;
        b1 = (1.0f - cs) * inverse;
        b2 = b0;
        break;
    case Kind::HighPass:
        b0 = (1.0f + cs) * 0.5f * inverse;
        b1 = -(1.0f + cs) * inverse;
        b2 = b0;
        break;
    case Kind::AllPass:
        b0 = a2;
        b1 = a1;
        b2 = 1.0f;
        break;
    }
}

void MultibandDynamics::Lr4::design(float frequency, float rate, bool high) noexcept {
    for (auto &s : stage)
        s.design(frequency, rate, high ? Biquad::Kind::HighPass : Biquad::Kind::LowPass);
}

void MultibandDynamics::Lr4::clear() noexcept {
    for (auto &s : stage)
        s.clear();
}

// The XML's 2000 Hz filters at 1999.9996 (10^log10f(2000)), 15000 at 14999.998:
// without this step 500/2000 and 3000/15000 null only to -110 dB, with it exactly.
// The value is rounded to float before the log, and the power is taken in float:
// log10 of the double value (dec_*_131.6/160.3/306.2/312.9, -50..-92 dB) and
// (float)pow(10.0, ...) (pow_*_1602/2491/4122/5293, -92..-122 dB) each move these
// one-split probes off the last bit, which this spelling hits (probe set mbd_f).
float MultibandDynamics::BandSplit::liveFrequency(double hz) noexcept {
    return std::pow(10.0f, std::log10(static_cast<float>(hz)));
}

void MultibandDynamics::BandSplit::design(double low, double high, double rate) noexcept {
    const float fl = liveFrequency(low), fh = liveFrequency(high);
    const auto fs = static_cast<float>(rate);
    lowL.design(fl, fs, false);
    highL.design(fl, fs, true);
    allpassL.design(fl, fs, Biquad::Kind::AllPass);
    allpassOfLow.design(fh, fs, Biquad::Kind::AllPass);
    for (Lr4 *f : {&lowOfHigh, &lowH})
        f->design(fh, fs, false);
    for (Lr4 *f : {&highOfAllpass, &highH})
        f->design(fh, fs, true);
    // Live scales the mid band by 1 minus the amplitude of -24 dB per octave of split
    // distance, in float: octaves = log2 of the ratio of the two filter frequencies
    // above, and the dB go to amplitude as 10^(dB * 0.05f). Multiplying by 0.05f is
    // not dividing by 20 (0.05f is not 1/20): at 2161/2848 Hz the exponent differs by
    // an ulp and so does the gain, and only the * 0.05f gain nulls g_2161_2848_soloMid
    // (+1 ulp of the / 20 one; a scan of the gain proves one value per probe). All
    // eleven measured pairs null to the last bit (120/2500, 500/2000, 1000/8000,
    // 30/300, 3000/15000 and mbd_f's g_* probes); octaves from the raw XML values, from
    // a difference of logs, or -24 * (octaves / 20) each miss at least one. The gain is
    // 0 when the splits meet, which is why crossed splits leave the mid band silent.
    const float decibels = -24.0f * std::log2(fh / fl);
    midGain = 1.0f - std::pow(10.0f, decibels * 0.05f);
}

void MultibandDynamics::BandSplit::clear() noexcept {
    for (Lr4 *f : {&lowL, &highL, &lowOfHigh, &highOfAllpass, &lowH, &highH})
        f->clear();
    allpassOfLow.clear();
    allpassL.clear();
}

// Measured against Live to the last bit, by band:
//   low  = AP(fH) after LP4(fL)     AP = one second-order allpass section
//   high = HP4(fH) after AP(fL)
//   mid  = midGain * LP4(fH) after HP4(fL), the gain applied after both filters
// (LP4/HP4: two identical Butterworth sections, Q 0.707). The three no longer sum
// to an exact allpass. With one split off, the other split's LP4/HP4 alone.
void MultibandDynamics::BandSplit::step(float v, bool lowOn, bool highOn, bool crossed,
                                        float &low, float &mid, float &high) noexcept {
    const float a = lowL.step(v);
    const float h = highL.step(v);
    const float lowBand = allpassOfLow.step(a);
    const float m = lowOfHigh.step(h);
    const float highBand = highOfAllpass.step(allpassL.step(v));
    const float xl = lowH.step(v);
    const float xh = highH.step(v);
    if (lowOn && highOn) {
        low = lowBand;
        mid = crossed ? 0.0f : midGain * m;
        high = highBand;
    } else if (lowOn) {
        low = a;
        mid = h;
        high = 0;
    } else if (highOn) {
        low = 0;
        mid = xl;
        high = xh;
    } else {
        low = 0;
        mid = v;
        high = 0;
    }
}

// log2 of a level the way Live takes it: the exponent field plus a cubic in the
// mantissa m = 1 + t, evaluated (c1*t + c2*t^2) + c3*t^3 in float. Exact at powers of
// two, up to 1.05e-3 (6.3 mdB) off in between; zero (and anything subnormal) reads
// as -127 plus the cubic, never -inf. The 0.02 dB DC sweeps of battery C
// (fine_a-60_r-1, fine_a-60_r-0.5, fine_a-31_r1, fine_b0_r1, fine_b0_r0.3,
// fine_a-60.3_r-0.77) null to the last bit with it; the exact log2 leaves 7 mdB.
float MultibandDynamics::fastLog2(float v) noexcept {
    const std::uint32_t bits = bitsOfFloat(v);
    const auto exponent = static_cast<float>(static_cast<int>((bits >> 23) & 255u) - 127);
    const float t = floatFromBits((bits & 0x7fffffu) | 0x3f800000u) - 1.0f;
    const float t2 = t * t;
    const float t3 = t2 * t;
    const float linear = kLog1 * t;
    const float square = kLog2 * t2;
    const float cube = kLog3 * t3;
    const float sum = linear + square;
    return exponent + (sum + cube);
}

// 2^x the way Live takes it: x + 127 is split into the exponent field (truncated)
// and a fraction f, so x is first rounded to the float grid of 127 + x (which
// shows: splitting x itself is up to 2e-6 off on the same sweeps), and 2^f is a
// cubic evaluated ((1 + q1*f) + q2*f^2) + q3*f^3. Integers come out exact, so the
// 2^6 cap is exactly 64. Below 2^-126 (an expander on near silence) the gain is 0,
// which is what flush-to-zero arithmetic makes of a denormal gain anyway; this way
// a platform without FlushDenormals gives the same output.
float MultibandDynamics::fastExp2(float x) noexcept {
    const float biased = x + 127.0f;
    if (!(biased >= 1.0f))
        return 0.0f;
    const auto whole = static_cast<int>(biased);
    const float f = biased - static_cast<float>(whole);
    const float f2 = f * f;
    const float f3 = f2 * f;
    const float linear = kExp1 * f;
    const float square = kExp2 * f2;
    const float cube = kExp3 * f3;
    const float q = ((1.0f + linear) + square) + cube;
    return std::ldexp(q, whole - 127);
}

// Live's RMS level is not sqrtf of the mean square but the reciprocal of its
// reciprocal square root, each an ARMv8 8-bit estimate refined by one fused Newton
// step, as an SSE-style rcp(rsqrt(x)) built on NEON would compute it:
//   e0 = FRSQRTE(ms), e1 = e0 * FRSQRTS(e0*e0, ms)   FRSQRTS(a, b) = (3 - a*b) / 2
//   r0 = FRECPE(e1),  r1 = r0 * FRECPS(r0, e1)       FRECPS(a, b)  = 2 - a*b
// with a*b unrounded inside both steps. Up to 2e-5 off sqrt, and it shows: the RMS
// sweep fine_a-60_r-1_rms nulls on 16 % of its samples with sqrtf and on all of
// them with this (FRSQRTS(ms*e0, e0) instead: 91 %); 62 probes in all need it. The
// emulation is plain integer, float and double arithmetic, so any compiler and CPU
// give the same bits: it matches this M1's instructions for every positive float.
float MultibandDynamics::rmsAmplitude(float meanSquare) noexcept {
    if (!(meanSquare > 0.0f))
        return 0.0f; // rcp(rsqrt(0)) = rcp(inf) = 0
    const float e0 = frsqrte(meanSquare);
    const float square = e0 * e0;
    // Below 2^-128 the square overflows and the hardware chain ends in -0 (-inf
    // through FRSQRTS, FRECPE(-inf) = -0); log2 reads both zeros alike.
    if (square > std::numeric_limits<float>::max())
        return 0.0f;
    // Each step's a*b is near 1 and exact in double (two 24-bit mantissas); 3 - a*b
    // and 2 - a*b are then exact too (at most 50 significant bits), so rounding them
    // to float rounds once, like the fused hardware step, without needing an fma.
    const double rsqrtStep = 3.0 - static_cast<double>(square) * static_cast<double>(meanSquare);
    const float e1 = e0 * (static_cast<float>(rsqrtStep) * 0.5f);
    const float r0 = frecpe(e1);
    const double recipStep = 2.0 - static_cast<double>(r0) * static_cast<double>(e1);
    return r0 * static_cast<float>(recipStep);
}

// Live's gain computer, in log2 units with every step in float. Each side is its
// ratio times the distance past its threshold, r*(L - Ta) above and r*(Tb - L)
// below; the two simply add, even when the thresholds cross or the knees overlap
// (thr_cross_*, thr_close_*, thr_equal_*). With the soft knee, within 10 dB of a
// threshold (|L - T| < half, half = 10/6.02) a side is r*t^2/(4*half), t measured
// from where the knee starts: t = L - (Ta - half) above, (Tb + half) - L below. That
// spelling is bit-exact on all 13 knee probes (knee_*, *_knee, *_knee_rms, mb_knee_rms,
// amt0.5_knee); t = (L - T) + half breaks 11 of them. The sum is capped at 6 (64x);
// there is no lower limit.
float MultibandDynamics::staticGainLog2(float level, float aboveThreshold, float aboveRatio,
                                        float belowThreshold, float belowRatio,
                                        bool knee) noexcept {
    const auto side = [knee](float over, float t, float ratio) {
        if (knee && std::fabs(over) < kKneeHalf) {
            const float square = t * t;
            return ratio * (square * kKneeCurve);
        }
        return over > 0.0f ? ratio * over : 0.0f;
    };
    const float above = side(level - aboveThreshold, level - (aboveThreshold - kKneeHalf), aboveRatio);
    const float below = side(belowThreshold - level, (belowThreshold + kKneeHalf) - level, belowRatio);
    return std::min(above + below, kGainCapLog2);
}

// Live's attack and release reach -80 dB (1e-4) of the way in the set time, at the
// oversampled rate, with the time already multiplied by Time scaling and no clamp
// at either end (0.01 ms and 50 s both measured). Rounded to float, this is Live's
// coefficient to the last bit: one ulp either way shows in the 1000 ms probes.
double MultibandDynamics::envelopeCoefficient(double ms, double rate) noexcept {
    return std::exp(std::log(1e-4) / (ms * 0.001 * rate));
}

void MultibandDynamics::Smoother::resize(std::size_t length) {
    ring1.assign(length, target);
    ring2.assign(length, target);
    jump(target);
}

void MultibandDynamics::Smoother::jump(double v) noexcept {
    target = out = v;
    std::fill(ring1.begin(), ring1.end(), v);
    std::fill(ring2.begin(), ring2.end(), v);
    sum1 = sum2 = v * static_cast<double>(ring1.size());
    pos = quiet = 0;
    active = false;
}

void MultibandDynamics::Smoother::retarget(double v) noexcept {
    target = v;
    quiet = 0;
    active = true;
}

double MultibandDynamics::Smoother::step() noexcept {
    const auto length = static_cast<double>(ring1.size());
    sum1 += target - ring1[pos];
    ring1[pos] = target;
    const double first = sum1 / length;
    sum2 += first - ring2[pos];
    ring2[pos] = first;
    out = sum2 / length;
    if (++pos == ring1.size())
        pos = 0;
    if (++quiet >= 2 * ring1.size())
        jump(target);
    return out;
}

MultibandDynamics::MultibandDynamics() {
    for (std::size_t i = 0; i < Count; ++i) {
        value_[i] = kDefaults[i];
        smooth_[i].target = smooth_[i].out = kDefaults[i];
    }
    for (auto *u : {&up_, &upSc_})
        for (auto &h : *u)
            for (std::size_t i = 0; i < 4; ++i) {
                h.even.c[i] = kHalfbandCoefficients[2 * i];
                h.odd.c[i] = kHalfbandCoefficients[2 * i + 1];
            }
    for (auto &h : down_)
        for (std::size_t i = 0; i < 4; ++i) {
            h.even.c[i] = kHalfbandCoefficients[2 * i];
            h.odd.c[i] = kHalfbandCoefficients[2 * i + 1];
        }
    derive();
    designFilters();
}

bool MultibandDynamics::smoothed(Param p) const noexcept {
    if (isSwitch(p))
        return false;
    switch (p) {
    case AttackLow:
    case AttackMid:
    case AttackHigh:
    case ReleaseLow:
    case ReleaseMid:
    case ReleaseHigh:
    case TimeScaling:
        return false; // they move coefficients, not levels: nothing to soften
    default:
        return true;
    }
}

void MultibandDynamics::prepare(double sampleRate) {
    rate_ = sampleRate > 0 ? sampleRate : 48000.0;
    smoothing_ = std::max<std::size_t>(1, static_cast<std::size_t>(std::lround(rate_ * 94.0 / 48000.0)));
    // The S/C On ramp lasts 1.5 ms in whole samples (66, 72, 144 at 44.1, 48, 96 kHz:
    // sr44_scsw_dc_listen, scsw_dc_listen and sr96_scsw_dc_listen are bit-exact with
    // these and no other lengths; 66.15 at 44.1 kHz does not say whether Live rounds or
    // truncates). Its step is the float reciprocal of that length.
    sidechainRampLength_ = std::max(1, static_cast<int>(std::lround(rate_ * 0.0015)));
    sidechainStep_ = 1.0f / static_cast<float>(sidechainRampLength_);
    for (std::size_t i = 0; i < Count; ++i) {
        smooth_[i].target = value_[i];
        smooth_[i].resize(smoothing_);
    }
    activeSmoothers_ = 0;
    designedLow_ = designedHigh_ = -1;
    derive();
    designFilters();
    reset();
}

void MultibandDynamics::reset() noexcept {
    for (auto *u : {&up_, &upSc_})
        for (auto &h : *u) {
            h.even.x.fill(0), h.even.y.fill(0), h.odd.x.fill(0), h.odd.y.fill(0);
        }
    for (auto &h : down_) {
        h.even.x.fill(0), h.even.y.fill(0), h.odd.x.fill(0), h.odd.y.fill(0);
        h.held = 0;
    }
    resumeAfterBypass();
    sidechainOn_ = value_[SidechainOn] >= 0.5;
    sidechainRamp_ = sidechainRampLength_;
    sidechainFade_ = sidechainOn_ ? 1.0f : 0.0f;
}

void MultibandDynamics::resumeAfterBypass() noexcept {
    for (auto *s : {&split_, &splitSc_})
        for (auto &b : *s)
            b.clear();
    for (auto &d : detector_)
        d.env = kEnvelopeStart;
}

void MultibandDynamics::set(Param p, double v) noexcept {
    if (p < 0 || p >= Count || !std::isfinite(v))
        return;
    v = std::clamp(v, kRange[p].min, kRange[p].max);
    if (isSwitch(p))
        v = v >= 0.5 ? 1.0 : 0.0;
    if (p == PeakMode)
        v = v >= 0.5 ? 1.0 : 0.0;
    // Flipping Peak/RMS mid-stream carries each envelope over into the other unit
    // (amplitude <-> mean square), so level and gain do not move: with a steady
    // -12 dB DC Live's output stays bit-identical through both switches
    // (auto_rms_to_peak, auto_peak_to_rms), where an unconverted envelope jumps
    // 12 dB and two detectors running side by side would land one ulp away.
    if (p == PeakMode && v != value_[p])
        for (auto &d : detector_)
            d.env = v >= 0.5 ? std::sqrt(d.env) : d.env * d.env;
    value_[p] = v;
    if (smoothed(p) && !smooth_[p].ring1.empty()) {
        if (!smooth_[p].active)
            ++activeSmoothers_;
        smooth_[p].retarget(v);
        return;
    }
    smooth_[p].jump(v);
    derive();
}

void MultibandDynamics::derive() noexcept {
    const auto at = [this](int p) { return smooth_[static_cast<std::size_t>(p)].out; };
    // Live's parameters are floats. A threshold goes to log2 units as float(T) times
    // the float 1/6.02 (the measured -T*1e-4 dB offset of every threshold against
    // 20*log10(2)); dividing by 6.02f gives another float for 1 threshold in 6, and of
    // the measured ones for -45 dB: sc_3b_rms_below (BelowThresholdMid -45) is
    // bit-exact only with the multiplication. Amount scales each ratio as float(r) *
    // float(amount), the one spelling the probes cannot pin down: every measured
    // Amount is 0, 0.25, 0.5 or 1.
    const auto amount = static_cast<float>(at(Amount) / 100.0);
    const double time = value_[TimeScaling] / 100.0;
    const double os = 2.0 * rate_;
    for (int b = 0; b < 3; ++b) {
        const auto i = static_cast<std::size_t>(b);
        inGain_[i] = knobGain(at(InputGainLow + b));
        outGain_[i] = knobGain(at(OutputGainLow + b));
        aboveT_[i] = static_cast<float>(at(AboveThresholdLow + b)) * kOctavesPerDb;
        belowT_[i] = static_cast<float>(at(BelowThresholdLow + b)) * kOctavesPerDb;
        aboveR_[i] = static_cast<float>(at(AboveRatioLow + b)) * amount;
        belowR_[i] = static_cast<float>(at(BelowRatioLow + b)) * amount;
        attack_[i] = static_cast<float>(envelopeCoefficient(value_[i + AttackLow] * time, os));
        release_[i] = static_cast<float>(envelopeCoefficient(value_[i + ReleaseLow] * time, os));
    }
    master_ = knobGain(at(MasterOutput));
    // The bottom of the S/C gain is off (-inf, like Live's mixer volume), and not only
    // its -70 dB minimum: sc_gm70 shows no gain reduction at all while a -76 dB trigger
    // should take 2 dB off at a -80 dB threshold, and Listen plays exactly 0 at -69.99
    // and -69.9 dB (rt_g_db-69.99, rt_g_db-69.9), while -69.5 dB acts as its linear
    // value (rt_g_db-69.5 bit-exact). The probes bound the cut to (-69.9, -69.5] dB; it
    // sits midway until one pins it.
    constexpr double kSidechainGainOffDb = -69.7;
    scGain_ = at(SidechainGain) < kSidechainGainOffDb ? 0.0f : dbToGain(at(SidechainGain));
    // Equal-power mix, the dry gain DERIVED from the wet one: wet = sin(m*pi/2) of the
    // float mix value, in double, rounded to float; dry = sqrt(1 - wet*wet) in float
    // (wet*wet and 1 - that each rounded). Bit-exact for all 15 mix values of battery
    // mbd_rt (rt_mix0.001 .. rt_mix0.999) in Listen. Near 100 % the float subtraction
    // shows: at 99.9 % dry is 0.00158221 where cos(0.999*pi/2) = 0.00157080 (+0.7 %,
    // rt_mix0.999 nulled at only -92 dB with the cosine). Both ends are exact: at 100 %
    // the main is exactly 0 in the trigger (Listen output is exactly 0 wherever the
    // sidechain is silent) and at 50 % both gains are float(sqrt(0.5)).
    const auto mix = static_cast<float>(at(SidechainMix) / 100.0);
    scWet_ = static_cast<float>(std::sin(static_cast<double>(mix) * kPi / 2));
    const float wetSquared = scWet_ * scWet_;
    scDry_ = std::sqrt(1.0f - wetSquared);
    // The S/C gain multiplies the wet gain, not the sidechain signal: the trigger is
    // dry*main + (wet*gain)*sidechain with wet*gain rounded to float first. Only this
    // order is bit-exact in rt_g1.5_mix0.3, rt_g0.7_mix0.8, rt_g3.0_mix0.5,
    // rt_g0.5_mix0.25, rt_g1.25_mix0.6 and rt_in6_g1.5_mix0.3; wet*(gain*sc) or
    // gain*(wet*sc) leave about 75 000 samples an ulp apart in each.
    scWetGain_ = scWet_ * scGain_;
}

void MultibandDynamics::designFilters() noexcept {
    const double high = smooth_[MidHighCrossover].out;
    const double low = std::min(smooth_[LowMidCrossover].out, high);
    if (low == designedLow_ && high == designedHigh_)
        return;
    designedLow_ = low;
    designedHigh_ = high;
    for (auto *s : {&split_, &splitSc_})
        for (auto &b : *s)
            b.design(low, high, 2.0 * rate_);
}

void MultibandDynamics::process(const float *inL, const float *inR, float *outL, float *outR,
                                std::size_t n, const float *scL, const float *scR) noexcept {
    [[maybe_unused]] const FlushDenormals flush;
    const bool lowOn = value_[LowBandOn] >= 0.5, highOn = value_[HighBandOn] >= 0.5;
    const std::array<bool, 3> exists{lowOn, true, highOn};
    const std::array<bool, 3> active{value_[ActiveLow] >= 0.5, value_[ActiveMid] >= 0.5,
                                     value_[ActiveHigh] >= 0.5};
    std::array<bool, 3> solo{value_[SoloLow] >= 0.5, value_[SoloMid] >= 0.5,
                             value_[SoloHigh] >= 0.5};
    const bool anySolo = solo[0] || solo[1] || solo[2];
    const bool peak = value_[PeakMode] >= 0.5, knee = value_[SoftKnee] >= 0.5;
    const bool sidechainWanted = value_[SidechainOn] >= 0.5;
    const bool listenOn = value_[SidechainListen] >= 0.5;
    for (std::size_t i = 0; i < n; ++i) {
        if (activeSmoothers_ > 0) {
            std::size_t moving = 0;
            for (auto &s : smooth_)
                if (s.active) {
                    s.step();
                    moving += s.active ? 1 : 0;
                }
            activeSmoothers_ = moving;
            derive();
            if (--redesignCountdown_ <= 0 || moving == 0) {
                designFilters();
                redesignCountdown_ = 4;
            }
        }
        const float x[2] = {inL[i], inR[i]};
        float os[2][2];
        for (int c = 0; c < 2; ++c)
            up_[static_cast<std::size_t>(c)].step(x[c], os[c][0], os[c][1]);
        float sc[2][2] = {{0, 0}, {0, 0}};
        // S/C On acts at its own sample (no block grid: scsw_dc_listen switches at
        // +7, +13 and +1 samples into a 32-sample block). A ramp, once started, runs to
        // its end: a switch back 40 samples into it (scsw_dc_listen at 48041) waits for
        // it and then ramps back from the far end, as if it arrived when the ramp ended
        // (turning the ramp round where it is: 0.23 off).
        if (sidechainRamp_ >= sidechainRampLength_ && sidechainWanted != sidechainOn_) {
            sidechainOn_ = sidechainWanted;
            sidechainRamp_ = 0;
            sidechainFade_ = sidechainOn_ ? 0.0f : 1.0f;
        }
        // The sidechain path (its upsampler and crossovers) only runs while S/C is on
        // or its ramp is moving; while off it is not processed at all, so the first
        // switch-on starts it from silence (scsw_dc_listen at 24007: the cold upsampler
        // rings, bit-exact; running it on the main input while off: -12 dB in
        // lis_scon_1_2s), and a later switch-on resumes the state it stopped in
        // (scsw_dc_listen at 48001 and 60000 bit-exact; a DC main cannot tell this
        // from a path that keeps running while off, but a cold restart would ring as
        // at 24007).
        const bool ramping = sidechainRamp_ < sidechainRampLength_;
        const bool sidechain = sidechainOn_ || ramping;
        // Listen plays whenever it is on, S/C on or off: with S/C off the detectors hear
        // the main input, so Listen plays the main's bands (times input gain, summed,
        // master), not the normal output. Bit-exact with compressing settings in
        // rt_scoff_listen_single and rt_scoff_listen_3b_out12 (normal output there:
        // -0.6 and -24 dB). lis_scoff could not tell: nothing compressed it.
        const bool listen = listenOn;
        if (sidechain) {
            const float s[2] = {scL ? scL[i] : 0.0f, scR ? scR[i] : 0.0f};
            float toward = 1.0f;
            if (ramping) {
                // The trigger's share of the sidechain mix is a smoothstep of a float
                // position that steps by 1/length per sample, accumulated (0 up for On,
                // 1 down for Off), and is spelled t*t*3 - t*t*t*2 in float. Exactly this
                // makes every edge of scsw_dc_listen (Listen plays the trigger, single
                // band) bit-exact; a position k/length computed afresh each sample is
                // up to 30 ulps off mid-ramp (a ~3.6e-7 bump: the accumulated rounding
                // of the step), t*t*(3-2t) or double arithmetic 1..2 ulps.
                const float t = sidechainFade_;
                toward = t * t * 3.0f - t * t * t * 2.0f;
                sidechainFade_ = sidechainOn_ ? t + sidechainStep_ : t - sidechainStep_;
                ++sidechainRamp_;
            }
            for (int c = 0; c < 2; ++c) {
                const float dry = scDry_ * x[c];
                const float wet = scWetGain_ * s[c];
                float mixed = dry + wet;
                if (ramping) {
                    const float main = (1.0f - toward) * x[c];
                    const float side = toward * mixed;
                    mixed = main + side;
                }
                upSc_[static_cast<std::size_t>(c)].step(mixed, sc[c][0], sc[c][1]);
            }
        }
        float out[2][2], heard[2][2] = {{0, 0}, {0, 0}};
        const bool crossed = smooth_[LowMidCrossover].out >= smooth_[MidHighCrossover].out;
        for (int k = 0; k < 2; ++k) { // the two oversampled samples
            float band[2][3], trig[2][3];
            for (int c = 0; c < 2; ++c) {
                const auto ci = static_cast<std::size_t>(c);
                split_[ci].step(os[c][k], lowOn, highOn, crossed, band[c][0], band[c][1],
                                band[c][2]);
                if (sidechain)
                    splitSc_[ci].step(sc[c][k], lowOn, highOn, crossed, trig[c][0], trig[c][1],
                                      trig[c][2]);
            }
            float sum[2] = {0, 0};
            for (int b = 0; b < 3; ++b) {
                const auto bi = static_cast<std::size_t>(b);
                if (!exists[bi] || (anySolo && !solo[bi]))
                    continue;
                if (!active[bi]) {
                    sum[0] += band[0][b];
                    sum[1] += band[1][b];
                    continue;
                }
                const float l = band[0][b] * inGain_[bi], r = band[1][b] * inGain_[bi];
                const float dl = sidechain ? trig[0][b] * inGain_[bi] : l;
                const float dr = sidechain ? trig[1][b] * inGain_[bi] : r;
                // Listen plays what the detectors hear: each band's split trigger times its
                // input gain, summed over the bands that compress (solo applies; an inactive
                // band is silent here although it passes raw audio), then the master. Not
                // the band output gains. Least-squares fit of Live's output onto the split
                // trigger bands, battery D lis_*: input gains +12.000/-12.014/+6.000 dB,
                // master +6.000, output gain 0, inactive or unsoloed bands below -120 dB,
                // all bands inactive exactly 0; with Live's crossovers every lis_* probe of
                // battery D is then bit-exact.
                if (listen) {
                    heard[0][k] += dl;
                    heard[1][k] += dr;
                }
                float d;
                if (peak) {
                    const float s = std::fabs(dl) + std::fabs(dr);
                    d = 0.5f * s;
                } else {
                    const float s = dl * dl + dr * dr;
                    d = 0.5f * s;
                }
                float &env = detector_[bi].env;
                // c*env + (1-c)*d with the product c*env rounded to float on its own. At
                // long times each step is only a few ulps of env, so the update stalls
                // short of d and the rounding decides where: d + c*(env-d) (or a fused
                // multiply-add) lands up to 0.02 dB away from Live at 5 s and 50 s.
                const float coef = d > env ? attack_[bi] : release_[bi];
                const float kept = coef * env;
                const float taken = (1.0f - coef) * d;
                // The detectors run on while Listen plays; no render can show otherwise,
                // since Live cannot automate Listen (SideListen has no automation target),
                // so a render listens from start to end or not at all. (lis_scon_1_2s,
                // once read as detectors frozen in Listen, is Listen playing the main
                // input after S/C Off.)
                env = kept + taken;
                // The level in log2 units, of the amplitude in both modes (an RMS
                // detector holds a mean square; sqrtf instead breaks 62 probes), the gain
                // back through 2^x. The band output gain multiplies last,
                // ((band * in) * G) * out: band * (G * out) breaks st_ingain6_out-3_peak,
                // cap_out24_up and mb_gains, (band * (in * G)) * out ten others (sc_in12*,
                // cap_in24_up, cap_in-24_down, out_global_m24_in24, ...).
                const float level = fastLog2(peak ? env : rmsAmplitude(env));
                const float g = staticGainLog2(level, aboveT_[bi], aboveR_[bi], belowT_[bi],
                                               belowR_[bi], knee);
                const float gain = fastExp2(g);
                const float yl = l * gain, yr = r * gain;
                sum[0] += yl * outGain_[bi];
                sum[1] += yr * outGain_[bi];
            }
            for (int c = 0; c < 2; ++c)
                out[c][k] = sum[c];
        }
        // One downsampler: what it is fed switches at the oversampled rate, in Listen
        // between the main input's bands and the sidechain path's (scsw_dc_listen is
        // bit-exact across every S/C edge only so). Listen and the normal output share
        // it too, which no render can tell from two (Listen is never switched in one,
        // see above). The master gain multiplies at the host rate, after the
        // downsampler, in Listen as well: bit-exact in out_global_6, fine_out_global_-2.9
        // and lis_3b_outMid12_master6; applied before downsampling, 47 000 to 320 000
        // samples differ by an ulp.
        const auto &fed = listen ? heard : out;
        outL[i] = down_[0].step(fed[0][0], fed[0][1]) * master_;
        outR[i] = down_[1].step(fed[1][0], fed[1][1]) * master_;
    }
}
} // namespace adi::dsp
