// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/multiband_dynamics.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
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
// The dynamic gain never exceeds 64x: +36.12 dB, measured with every way of
// asking for more (upward compression, upward expansion, both at once).
const double kGainCap = 20.0 * std::log10(64.0);
constexpr double kKnee = 20.0;
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
// Live's gain knobs (band input and output gains, master) round the exponent dB/20
// to float before raising 10 to it: 12 dB is 3.98107195, one ulp above 10^0.6
// rounded (3.98107171); 6 dB is 1.99526238 and -2.9 dB 0.71614337. Bit-exact this
// way, not with a double exponent: input gains 12, -12, 6 dB in Listen
// (lis_single_inMid12, lis_3b_inLowm12_inHigh6), band output gain 6 dB
// (gainLow6_inMidm6_out3), master 6 and -2.9 dB (out_global_6,
// fine_out_global_-2.9, lis_3b_outMid12_master6). The S/C gain is no such knob:
// Live stores it as a linear factor and multiplies by that float (lis_3b_g0.25 is
// bit-exact with 0.25 and nulls at only -77 dB with 0.25000003, through the 120 Hz
// filters), which dbToGain returns for our dB value. Not yet explained there: a
// stored 2.0 acts as 1.99999976 (2 ulps low, sc_listen).
float knobGain(double db) noexcept {
    const float exponent = static_cast<float>(db) / 20.0f;
    return static_cast<float>(std::pow(10.0, static_cast<double>(exponent)));
}
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

// The RBJ cookbook section, all in float: w = 2*pi*f/fs evaluated left to right in
// float (a double w is off by an ulp at 3000 Hz and 2999 Hz), cosf/sinf,
// alpha = sin/(2Q), and every coefficient multiplied by 1/a0 rather than divided by
// a0 (dividing nulls only to -54 dB at 120 Hz). The allpass is the same section's
// denominator turned round: b = (a2, a1, 1).
void MultibandDynamics::Biquad::design(float frequency, float rate, Kind kind) noexcept {
    const float q = static_cast<float>(kQ);
    const float w = 2.0f * static_cast<float>(kPi) * frequency / rate;
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
    // Live scales the mid band by 1 - 10^(-24 dB per octave of split distance / 20).
    // This float formula nulls all five measured pairs to the last bit (120/2500,
    // 500/2000, 1000/8000, 30/300, 3000/15000); other float spellings of it agree
    // there too but not everywhere (see the filters agent's battery_next.py). It is
    // 0 when the splits meet, which is why crossed splits leave the mid band silent.
    midGain = 1.0f - std::pow(10.0f, -24.0f * std::log2(fh / fl) / 20.0f);
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

double MultibandDynamics::staticGain(double level, double aboveThreshold, double aboveRatio,
                                     double belowThreshold, double belowRatio,
                                     bool knee) noexcept {
    // Each side is r times the distance past its threshold; a soft knee is a
    // quadratic over 20 dB centred on the threshold. The two sides simply add,
    // even when the thresholds cross or the knees overlap (all measured).
    const auto side = [knee](double over, double ratio) {
        if (knee && std::abs(over) < kKnee / 2) {
            const double t = over + kKnee / 2;
            return ratio * t * t / (2 * kKnee);
        }
        return over > 0 ? ratio * over : 0.0;
    };
    return side(level - aboveThreshold, aboveRatio) + side(belowThreshold - level, belowRatio);
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
    sidechainRamp_ = kSidechainRamp;
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
    const double amount = at(Amount) / 100.0;
    const double time = value_[TimeScaling] / 100.0;
    const double os = 2.0 * rate_;
    for (int b = 0; b < 3; ++b) {
        const auto i = static_cast<std::size_t>(b);
        inGain_[i] = knobGain(at(InputGainLow + b));
        outGain_[i] = knobGain(at(OutputGainLow + b));
        aboveT_[i] = at(AboveThresholdLow + b);
        belowT_[i] = at(BelowThresholdLow + b);
        aboveR_[i] = at(AboveRatioLow + b) * amount;
        belowR_[i] = at(BelowRatioLow + b) * amount;
        attack_[i] = static_cast<float>(envelopeCoefficient(value_[i + AttackLow] * time, os));
        release_[i] = static_cast<float>(envelopeCoefficient(value_[i + ReleaseLow] * time, os));
    }
    master_ = knobGain(at(MasterOutput));
    // The S/C gain's minimum, -70 dB, is off (-inf, like Live's mixer volume): sc_gm70
    // shows no gain reduction at all while a -76 dB trigger should take 2 dB off at a
    // -80 dB threshold.
    scGain_ = at(SidechainGain) <= kRange[SidechainGain].min ? 0.0f : dbToGain(at(SidechainGain));
    // Equal-power mix with both ends exact: at 100 % the main is exactly 0 in the trigger
    // (cos(pi/2) in double would leak 6e-17 of it; Live's Listen output is exactly 0
    // wherever the sidechain is silent, lis_single_inMid12), at 50 % both gains are
    // float(sqrt(0.5)) (sc_listen_dw0.5 bit-exact).
    const double mix = at(SidechainMix) / 100.0;
    scDry_ = static_cast<float>(std::sin((1.0 - mix) * kPi / 2));
    scWet_ = static_cast<float>(std::sin(mix * kPi / 2));
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
    if ((value_[SidechainOn] >= 0.5) != sidechainOn_) {
        sidechainOn_ = !sidechainOn_;
        // a switch back before the ramp has finished turns it around where it is
        // (smoothstep is point-symmetric; Live's behaviour here is not measured)
        sidechainRamp_ = kSidechainRamp - std::min(sidechainRamp_, kSidechainRamp);
    }
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
        // The sidechain path (its upsampler and crossovers) only runs while S/C is on
        // or its ramp is moving; while off it is not processed at all, so the first
        // switch-on starts it from silence (lis_scon_1_2s: -123 dB over the switch;
        // feeding it the main input while off: -12 dB).
        const bool ramping = sidechainRamp_ < kSidechainRamp;
        const bool sidechain = sidechainOn_ || ramping;
        // Listen plays only while that path runs (lis_scoff: S/C off = normal output).
        const bool listen = sidechain && listenOn;
        if (sidechain) {
            const float s[2] = {scL ? scL[i] : 0.0f, scR ? scR[i] : 0.0f};
            float toward = 1.0f;
            if (ramping) {
                const double t = static_cast<double>(sidechainRamp_) / kSidechainRamp;
                const double w = t * t * (3.0 - 2.0 * t);
                toward = static_cast<float>(sidechainOn_ ? w : 1.0 - w);
                ++sidechainRamp_;
            }
            for (int c = 0; c < 2; ++c) {
                const float dry = scDry_ * x[c];
                const float boosted = scGain_ * s[c];
                const float wet = scWet_ * boosted;
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
                // While Listen plays, the detectors hold still: after S/C goes off from
                // Listen, Live's high band is uncompressed (0.0000 dB) although the
                // trigger had driven it to -8.4 dB (lis_scon_1_2s).
                if (!listen)
                    env = kept + taken;
                const double level = env > 0 ? (peak ? 20.0 : 10.0) * std::log10(env) : -1000.0;
                double g = staticGain(level, aboveT_[bi], aboveR_[bi], belowT_[bi], belowR_[bi], knee);
                g = std::min(g, kGainCap);
                const float gain = dbToGain(g) * outGain_[bi];
                sum[0] += l * gain;
                sum[1] += r * gain;
            }
            for (int c = 0; c < 2; ++c)
                out[c][k] = sum[c];
        }
        // Listen and the normal output share one downsampler: Live switches what it
        // is fed at the oversampled rate, so leaving Listen continues smoothly from
        // the listened signal (two downsamplers: a -24 dB click, lis_scon_1_2s). The
        // master gain multiplies at the host rate, after the downsampler, in Listen as
        // well: bit-exact in out_global_6, fine_out_global_-2.9 and
        // lis_3b_outMid12_master6; applied before downsampling, 47 000 to 320 000
        // samples differ by an ulp.
        const auto &fed = listen ? heard : out;
        outL[i] = down_[0].step(fed[0][0], fed[0][1]) * master_;
        outR[i] = down_[1].step(fed[1][0], fed[1][1]) * master_;
    }
}
} // namespace adi::dsp
