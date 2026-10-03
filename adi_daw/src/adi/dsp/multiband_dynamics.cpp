// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/multiband_dynamics.hpp"
#include <algorithm>
#include <cmath>
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

float MultibandDynamics::Biquad::step(float v) noexcept {
    const float feed = b0 * v;
    const float y = feed + s1;
    const float f1 = b1 * v;
    const float r1 = a1 * y;
    const float d1 = f1 - r1;
    s1 = d1 + s2;
    const float f2 = b2 * v;
    const float r2 = a2 * y;
    s2 = f2 - r2;
    return y;
}

void MultibandDynamics::Lr4::design(double frequency, double rate, bool high) noexcept {
    const double k = std::tan(kPi * frequency / rate);
    const double norm = 1.0 / (1.0 + k / kQ + k * k);
    const double a1 = 2.0 * (k * k - 1.0) * norm;
    const double a2 = (1.0 - k / kQ + k * k) * norm;
    const double b0 = high ? norm : k * k * norm;
    const double b1 = high ? -2.0 * norm : 2.0 * k * k * norm;
    for (auto &s : stage) {
        s.b0 = static_cast<float>(b0);
        s.b1 = static_cast<float>(b1);
        s.b2 = static_cast<float>(b0);
        s.a1 = static_cast<float>(a1);
        s.a2 = static_cast<float>(a2);
    }
}

void MultibandDynamics::Lr4::clear() noexcept {
    for (auto &s : stage)
        s.s1 = s.s2 = 0;
}

void MultibandDynamics::BandSplit::design(double low, double high, double rate) noexcept {
    lowL.design(low, rate, false);
    highL.design(low, rate, true);
    for (Lr4 *f : {&lowOfLow, &lowOfHigh, &lowH})
        f->design(high, rate, false);
    for (Lr4 *f : {&highOfLow, &highOfHigh, &highH})
        f->design(high, rate, true);
}

void MultibandDynamics::BandSplit::clear() noexcept {
    for (Lr4 *f : {&lowL, &highL, &lowOfLow, &highOfLow, &lowOfHigh, &highOfHigh, &lowH, &highH})
        f->clear();
}

// Measured against Live, by phase as well as magnitude:
//   low  = LP(fL) * AP(fH)        both LR4, the allpass as LP + HP of the fH split
//   high = HP(fH) * AP(fL)
//   mid  = HP(fL)*LP(fH) - LP(fL)*HP(fH), so the three always sum to AP(fL)*AP(fH)
// With fL at or above fH both splits sit at fH and the mid band is silent.
void MultibandDynamics::BandSplit::step(float v, bool lowOn, bool highOn, bool crossed,
                                        float &low, float &mid, float &high) noexcept {
    const float a = lowL.step(v);
    const float h = highL.step(v);
    const float ll = lowOfLow.step(a);
    const float lh = highOfLow.step(a);
    const float hl = lowOfHigh.step(h);
    const float hh = highOfHigh.step(h);
    const float xl = lowH.step(v);
    const float xh = highH.step(v);
    if (lowOn && highOn) {
        low = ll + lh;
        mid = crossed ? 0.0f : hl - lh;
        high = lh + hh;
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

// Live's attack and release reach -80 dB (1e-4) of the way in the set time.
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
    for (auto *d : {&down_, &downListen_})
        for (auto &h : *d)
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
    for (auto *d : {&down_, &downListen_})
        for (auto &h : *d) {
            h.even.x.fill(0), h.even.y.fill(0), h.odd.x.fill(0), h.odd.y.fill(0);
            h.held = 0;
        }
    for (auto *s : {&split_, &splitSc_})
        for (auto &b : *s)
            b.clear();
    for (auto &d : detector_)
        d.env = 0;
}

void MultibandDynamics::set(Param p, double v) noexcept {
    if (p < 0 || p >= Count || !std::isfinite(v))
        return;
    v = std::clamp(v, kRange[p].min, kRange[p].max);
    if (isSwitch(p))
        v = v >= 0.5 ? 1.0 : 0.0;
    if (p == PeakMode)
        v = v >= 0.5 ? 1.0 : 0.0;
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
        inGain_[i] = dbToGain(at(InputGainLow + b));
        outGain_[i] = dbToGain(at(OutputGainLow + b));
        aboveT_[i] = at(AboveThresholdLow + b);
        belowT_[i] = at(BelowThresholdLow + b);
        aboveR_[i] = at(AboveRatioLow + b) * amount;
        belowR_[i] = at(BelowRatioLow + b) * amount;
        attack_[i] = static_cast<float>(envelopeCoefficient(value_[i + AttackLow] * time, os));
        release_[i] = static_cast<float>(envelopeCoefficient(value_[i + ReleaseLow] * time, os));
    }
    master_ = dbToGain(at(MasterOutput));
    scGain_ = dbToGain(at(SidechainGain));
    const double mix = at(SidechainMix) / 100.0 * kPi / 2;
    scDry_ = static_cast<float>(std::cos(mix));
    scWet_ = static_cast<float>(std::sin(mix));
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
    const bool lowOn = value_[LowBandOn] >= 0.5, highOn = value_[HighBandOn] >= 0.5;
    const std::array<bool, 3> exists{lowOn, true, highOn};
    const std::array<bool, 3> active{value_[ActiveLow] >= 0.5, value_[ActiveMid] >= 0.5,
                                     value_[ActiveHigh] >= 0.5};
    std::array<bool, 3> solo{value_[SoloLow] >= 0.5, value_[SoloMid] >= 0.5,
                             value_[SoloHigh] >= 0.5};
    const bool anySolo = solo[0] || solo[1] || solo[2];
    const bool peak = value_[PeakMode] >= 0.5, knee = value_[SoftKnee] >= 0.5;
    const bool sidechain = value_[SidechainOn] >= 0.5;
    const bool listen = sidechain && value_[SidechainListen] >= 0.5;
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
        if (sidechain) {
            const float s[2] = {scL ? scL[i] : 0.0f, scR ? scR[i] : 0.0f};
            for (int c = 0; c < 2; ++c) {
                const float dry = scDry_ * x[c];
                const float boosted = scGain_ * s[c];
                const float wet = scWet_ * boosted;
                const float mixed = dry + wet;
                upSc_[static_cast<std::size_t>(c)].step(mixed, sc[c][0], sc[c][1]);
            }
        }
        float out[2][2];
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
                float d;
                if (peak) {
                    const float s = std::fabs(dl) + std::fabs(dr);
                    d = 0.5f * s;
                } else {
                    const float s = dl * dl + dr * dr;
                    d = 0.5f * s;
                }
                float &env = detector_[bi].env;
                const float coef = d > env ? attack_[bi] : release_[bi];
                env = d + coef * (env - d);
                const double level = env > 0 ? (peak ? 20.0 : 10.0) * std::log10(env) : -1000.0;
                double g = staticGain(level, aboveT_[bi], aboveR_[bi], belowT_[bi], belowR_[bi], knee);
                g = std::min(g, kGainCap);
                const float gain = dbToGain(g) * outGain_[bi];
                sum[0] += l * gain;
                sum[1] += r * gain;
            }
            for (int c = 0; c < 2; ++c)
                out[c][k] = sum[c] * master_;
        }
        if (listen) {
            for (int c = 0; c < 2; ++c) {
                const auto ci = static_cast<std::size_t>(c);
                const float heard = downListen_[ci].step(sc[c][0], sc[c][1]);
                down_[ci].step(out[c][0], out[c][1]);
                (c == 0 ? outL : outR)[i] = heard;
            }
        } else {
            outL[i] = down_[0].step(out[0][0], out[0][1]);
            outR[i] = down_[1].step(out[1][0], out[1][1]);
        }
    }
}
} // namespace adi::dsp
