// SPDX-License-Identifier: GPL-3.0-or-later
#include "auto_filter.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>
namespace adi::dsp {
void AutoFilter::prepare(double rate) {
    rate_ = std::isfinite(rate) ? std::clamp(rate, 8000., 384000.) : 48000.;
    p_ = target_;
    smooth_ = std::exp(-1. / (.005 * rate_));
    beats_ = 0;
    for (auto &s : lane_) {
        s = Lane{};
        s.comb.assign(static_cast<std::size_t>(std::ceil(rate_ / 20)) + 4, 0);
    }
}
void AutoFilter::tempo(double bpm) noexcept {
    if (std::isfinite(bpm))
        bpm_ = std::clamp(bpm, 20., 999.);
}
void AutoFilter::set(Param id, double v) noexcept {
    const auto n = static_cast<std::size_t>(id);
    if (n >= p_.size() || !std::isfinite(v))
        return;
    constexpr double lo[] = {0,       0,    20,      0, 0, -100, -24,     0,   0,   0, 0,
                             .01,     0,    .015625, 0, 0, 0,    0,       0,   0,   0, 1,
                             .015625, -100, .1,      0, 1, 0,    .015625, 0,   -24, 0, 0,
                             0,       -24,  0,       0, 0, 20,   .1,      -24, 0};
    constexpr double hi[] = {9,  3,   22000, 100, 100, 100, 24, 4,  3,  100, 100,   40, 3,    64,
                             7,  100, 1,     360, 100, 360, 2,  32, 16, 100, 1000,  1,  3000, 1,
                             16, 1,   24,    100, 1,   100, 24, 1,  1,  5,   22000, 18, 24,   1};
    v = std::clamp(v, lo[n], hi[n]);
    switch (id) {
    case Type:
    case Slope:
    case Circuit:
    case LfoMode:
    case LfoWave:
    case StereoMode:
    case Quantize:
    case Steps:
    case Hold:
    case EnvSH:
    case Clip:
    case External:
    case SCListen:
    case SCFilter:
    case SCType:
    case SCMono:
        v = std::round(v);
        p_[n] = v;
        break;
    default:
        break;
    }
    target_[n] = v;
}
double AutoFilter::random(std::uint64_t n) noexcept {
    n += 0x9e3779b97f4a7c15ULL;
    n = (n ^ (n >> 30)) * 0xbf58476d1ce4e5b9ULL;
    n = (n ^ (n >> 27)) * 0x94d049bb133111ebULL;
    n ^= n >> 31;
    return static_cast<double>(n >> 11) * (2. / 9007199254740992.) - 1;
}
double AutoFilter::lfo(Lane &s, std::size_t channel) noexcept {
    const int wave = static_cast<int>(p_[LfoWave]);
    double hz = p_[LfoMode] == 0   ? p_[LfoRate]
                : p_[LfoMode] == 1 ? 1 / p_[LfoRate]
                                   : bpm_ / 60 / (p_[LfoBeats] * (p_[LfoMode] == 3 ? .25 : 1));
    if (p_[StereoMode] >= .5 && channel == 1 && wave < 6)
        hz *= 1 + p_[Spin] / 100 * .25;
    double phase = s.phase + (p_[LfoMode] >= 2 ? p_[PhaseOffset] / 360 : 0) +
                   (p_[StereoMode] < .5 && channel == 1 ? p_[Phase] / 360 : 0);
    const auto cycle = static_cast<std::uint64_t>(std::floor(phase));
    double f = phase - std::floor(phase);
    s.phase += hz / rate_;
    if (p_[Quantize] == 1)
        f = std::floor(f * p_[Steps]) / p_[Steps];
    if (wave < 6)
        f = std::pow(f, std::pow(4., (p_[LfoShape] - 50) / 50));
    double value = 0;
    switch (wave) {
    case 0:
        value = std::sin(2 * std::numbers::pi * f);
        break;
    case 1:
        value = 1 - 4 * std::abs(f - .5);
        break;
    case 2:
        value = 2 * f - 1;
        break;
    case 3:
        value = f < .5 ? 1 : -1;
        break;
    case 4:
        value = f;
        break;
    case 5:
        value = 1 - f;
        break;
    case 6: {
        const double t = f * f * (3 - 2 * f);
        value = random(cycle) * (1 - t) + random(cycle + 1) * t;
        break;
    }
    default:
        value = random(cycle);
        break;
    }
    if (wave == 7) {
        const double a = std::exp(-1. / (rate_ * (.0001 + .2 * p_[LfoShape] / 100)));
        s.randomSmooth = value + a * (s.randomSmooth - value);
        value = s.randomSmooth;
    }
    if (p_[Quantize] == 2) {
        const double slot = std::floor(beats_ / p_[LfoSH]);
        if (slot != s.lastLfoSlot) {
            s.lastLfoSlot = slot;
            s.quantLfo = value;
        }
        value = s.quantLfo;
    }
    return value;
}
double AutoFilter::filter(Lane &s, double x, double freq) noexcept {
    const int type = static_cast<int>(p_[Type]), slope = static_cast<int>(p_[Slope]),
              circuit = static_cast<int>(p_[Circuit]);
    const double res = std::min(.98, p_[Resonance] / 100), m = p_[Morph] / 100,
                 drive = p_[Drive] / 100;
    if (type == DJ) {
        const double control = p_[Control] / 100;
        if (std::abs(control) < 1e-8)
            return x;
        freq = control < 0 ? 20000 * std::pow(.001, -control) : 20 * std::pow(1000., control);
        const auto out = s.svf[0].process(x, freq, .25 + std::abs(control) * .65, rate_);
        return control < 0 ? out.low : out.high;
    }
    if (type == Comb) {
        if (s.comb.empty())
            return x;
        const double delay = std::clamp(rate_ / freq, 1., static_cast<double>(s.comb.size() - 2));
        const auto whole = static_cast<std::size_t>(delay);
        const double fraction = delay - static_cast<double>(whole);
        const auto a = (s.write + s.comb.size() - whole) % s.comb.size(),
                   b = (a + s.comb.size() - 1) % s.comb.size();
        const double delayed = s.comb[a] * (1 - fraction) + s.comb[b] * fraction,
                     polarity = 1 - 2 * m;
        s.comb[s.write] = x + delayed * .95 * res * polarity;
        s.write = (s.write + 1) % s.comb.size();
        return .5 * (x + polarity * delayed);
    }
    if (type == Resampling) {
        s.holdPhase += freq / rate_;
        if (s.holdPhase >= 1) {
            s.holdPhase -= std::floor(s.holdPhase);
            s.held = x;
        }
        return s.held;
    }
    if (type == Vowel) {
        constexpr double centres[5][3] = {{800, 1150, 2900},
                                          {400, 1700, 2600},
                                          {350, 2000, 2800},
                                          {450, 800, 2830},
                                          {325, 700, 2530}};
        const auto a = static_cast<std::size_t>(p_[Formant]), b = std::min<std::size_t>(4, a + 1);
        const double blend = p_[Formant] - static_cast<double>(a),
                     pitch = std::pow(2., p_[Pitch] / 12);
        double sum = 0;
        for (std::size_t i = 0; i < 3; ++i) {
            const double f = (centres[a][i] * (1 - blend) + centres[b][i] * blend) * pitch;
            const double weight = i == 0 ? .55 : i == 1 ? .3 : .15;
            sum += s.svf[i].process(x, f, .9, rate_).band * (weight * (1 - m) + m / 3);
        }
        return sum;
    }
    if (type == NotchLP) {
        double y = s.svf[0].process(x, freq * std::pow(4., 1 - m), res, rate_).notch;
        y = s.svf[1].process(y, freq, res, rate_).low;
        if (slope >= 2)
            y = s.svf[2].process(y, freq, res, rate_).low;
        return y;
    }
    const double driven = x + drive * (std::tanh(x * std::pow(16., drive)) - x);
    double y = driven;
    if (circuit == 1 && drive > 0)
        y += drive * (std::tanh(y + s.feedback * drive * .5) - y);
    // A six-dB endpoint needs one pole; the two/four-pole circuit cores cannot supply it.
    if (type == MorphFilter && slope == 0) {
        const double pole = std::exp(-2 * std::numbers::pi * freq / rate_);
        s.onePole = (1 - pole) * y + pole * s.onePole;
        s.onePoleBand = (1 - pole) * (y - s.onePole) + pole * s.onePoleBand;
        s.feedback = m < .5 ? s.onePole * (1 - 2 * m) + s.onePoleBand * (2 * m)
                            : s.onePoleBand * (2 - 2 * m) + (y - s.onePole) * (2 * m - 1);
        return s.feedback;
    }
    auto select = [type, m](double low, double band, double high) {
        switch (type) {
        case LP:
            return low;
        case HP:
            return high;
        case BP:
            return band;
        case Notch:
            return low + high;
        default:
            return m < .5 ? low * (1 - 2 * m) + band * (2 * m)
                          : band * (2 - 2 * m) + high * (2 * m - 1);
        }
    };
    if (circuit == 2 || circuit == 3) {
        const int sections = circuit == 2 ? (type == MorphFilter && slope == 3 ? 4
                                             : slope >= 2                      ? 2
                                                                               : 1)
                                          : (type == MorphFilter && slope == 3 ? 2 : 1);
        for (int section = 0; section < sections; ++section) {
            const auto index = static_cast<std::size_t>(section);
            double low = 0, high = 0, band = 0;
            if (circuit == 2) {
                low = s.k35Low[index].process(y, freq, res, drive * 4, rate_, false);
                high = s.k35High[index].process(y, freq, res, drive * 4, rate_, true);
                band = y - low - high;
            } else {
                auto &ladder = s.ladder[index];
                low = ladder.process(y, freq, res, rate_, slope >= 2 ? 4 : 2, false);
                const auto &z = ladder.stage;
                high = slope >= 2 ? y - 4 * z[0] + 6 * z[1] - 4 * z[2] + z[3] : y - 2 * z[0] + z[1];
                band = slope >= 2 ? 4 * (z[1] - 2 * z[2] + z[3]) : 2 * (z[0] - z[1]);
            }
            y = select(low, band, high);
        }
        return y;
    }
    const int stages = type == MorphFilter ? (slope == 3   ? 4
                                              : slope == 2 ? 2
                                                           : 1)
                                           : (slope >= 2 ? 2 : 1);
    for (int stage = 0; stage < stages; ++stage) {
        const auto o = s.svf[static_cast<std::size_t>(stage)].process(y, freq, res, rate_);
        switch (type) {
        case LP:
            y = o.low;
            break;
        case HP:
            y = o.high;
            break;
        case BP:
            y = o.band * (2 - 2 * res);
            break;
        case Notch:
            y = o.notch;
            break;
        default:
            y = m < .5 ? o.low * (1 - 2 * m) + o.band * (2 * m) * (2 - 2 * res)
                       : o.band * (2 - 2 * m) * (2 - 2 * res) + o.high * (2 * m - 1);
            break;
        }
    }
    s.feedback = y;
    return y;
}
void AutoFilter::process(const float *l, const float *r, float *ol, float *orr, std::size_t n,
                         const float *scLeft, const float *scRight) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t p = 0; p < p_.size(); ++p)
            p_[p] = target_[p] + smooth_ * (p_[p] - target_[p]);
        double input[] = {l && std::isfinite(l[i]) ? l[i] : 0, r && std::isfinite(r[i]) ? r[i] : 0};
        double key[] = {scLeft && std::isfinite(scLeft[i]) ? scLeft[i] : 0,
                        scRight && std::isfinite(scRight[i]) ? scRight[i] : 0};
        const double external = p_[External] >= .5 ? p_[SCMix] / 100 : 0,
                     scGain = std::pow(10., p_[SCGain] / 20);
        for (std::size_t c = 0; c < 2; ++c)
            key[c] = input[c] * (1 - external) + key[c] * scGain * external;
        if (p_[SCMono] >= .5)
            key[0] = key[1] = (key[0] + key[1]) * .5;
        surge_eq::Coeff sc;
        const double w = 2 * std::numbers::pi * std::min(p_[SCFrequency], rate_ * .475) / rate_,
                     q = p_[SCQ];
        if (p_[SCFilter] >= .5) {
            switch (static_cast<int>(p_[SCType])) {
            case 0:
            case 2:
                sc = surge_eq::shelf(w, q, p_[SCFilterGain], p_[SCType] == 2);
                break;
            case 1:
                sc = surge_eq::peak(w, 2 * std::asinh(1 / (2 * q)) / std::log(2.),
                                    std::pow(10., p_[SCFilterGain] / 20),
                                    std::pow(10., p_[SCFilterGain] / 40));
                break;
            case 3:
            case 5:
                sc = surge_eq::cut(w, q, p_[SCType] == 5);
                break;
            default: {
                const double alpha = std::sin(w) / (2 * q);
                sc = surge_eq::normalized(1 + alpha, -2 * std::cos(w), 1 - alpha, alpha, 0, -alpha);
                break;
            }
            }
        }
        // Restore a common clock after leaving Spin, including automation back to zero.
        // Random waves ignore Spin. Align before advancing either lane for this sample.
        if (p_[StereoMode] < .5 || p_[LfoWave] >= 6 || target_[Spin] == 0)
            lane_[1].phase = lane_[0].phase;
        float *out[] = {ol, orr};
        for (std::size_t c = 0; c < 2; ++c) {
            auto &s = lane_[c];
            double trigger = key[c];
            if (p_[SCFilter] >= .5) {
                trigger = key[c] * sc.b0 + s.sc1;
                s.sc1 = key[c] * sc.b1 - sc.a1 * trigger + s.sc2;
                s.sc2 = key[c] * sc.b2 - sc.a2 * trigger;
            }
            const double power = trigger * trigger;
            double target = power;
            if (p_[Hold] >= .5) {
                if (power > s.holdPeak) {
                    s.holdPeak = power;
                    s.holdSamples = p_[Attack] * .001 * rate_;
                }
                if (s.holdSamples > 0) {
                    target = s.holdPeak;
                    --s.holdSamples;
                } else
                    s.holdPeak = power;
            }
            const double ms = target > s.env ? p_[Attack] : p_[Release],
                         a = std::exp(-1. / (.001 * ms * rate_));
            s.env = target + a * (s.env - target);
            double env = std::sqrt(std::max(0., s.env));
            if (p_[EnvSH] >= .5) {
                const double slot = std::floor(beats_ / p_[EnvBeats]);
                if (slot != s.lastEnvSlot) {
                    s.lastEnvSlot = slot;
                    s.envHeld = env;
                }
                env = s.envHeld;
            }
            const double modulation =
                lfo(s, c) * p_[LfoAmount] / 25 + std::clamp(env, 0., 1.) * p_[Envelope] / 25;
            const double frequency =
                std::clamp(p_[Frequency] * std::pow(2., modulation), 20., rate_ * .475);
            double wet = filter(s, input[c], frequency);
            if (p_[Clip] >= .5)
                wet = std::tanh(wet);
            wet *= std::pow(10., p_[Output] / 20);
            out[c][i] = static_cast<float>(
                p_[SCListen] >= .5 ? trigger : input[c] + p_[Mix] / 100 * (wet - input[c]));
        }
        beats_ += bpm_ / 60 / rate_;
    }
}
} // namespace adi::dsp
