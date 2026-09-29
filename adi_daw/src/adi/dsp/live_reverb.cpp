/*
 * sst-effects - an open source library of audio effects
 * built by Surge Synth Team.
 *
 * Copyright 2018-2023, various authors, as described in the GitHub
 * transaction log.
 *
 * sst-effects is released under the GNU General Public Licence v3
 * or later (GPL-3.0-or-later). The license is found in the "LICENSE"
 * file in the root of this repository, or at
 * https://www.gnu.org/licenses/gpl-3.0.en.html
 *
 * The majority of these effects at initiation were factored from
 * Surge XT, and so git history prior to April 2023 is found in the
 * surge repo, https://github.com/surge-synthesizer/surge
 *
 * All source in sst-effects available at
 * https://github.com/surge-synthesizer/sst-effects
 */
// ADI scalar adaptation: preset-0 delays and Householder feedback from Surge
// Reverb1. Live controls, early taps, diffusion and modulation are ADI additions.
#include "adi/dsp/live_reverb.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>
namespace adi::dsp {
namespace {
// Verified in SST Reverb1::loadpreset(0); upstream uses 8 fractional bits.
constexpr std::array<int, 16> delays{1339934, 962710,  1004427, 1103966, 1198575, 1743348,
                                     1033425, 933313,  949407,  1402754, 1379894, 1225304,
                                     1135598, 1402107, 956152,  1137737};
constexpr double pi = std::numbers::pi;
} // namespace
LiveReverb::LiveReverb() {
    for (int i = 0; i < Count; ++i)
        p_[static_cast<std::size_t>(i)] = ranges[i].initial;
}
void LiveReverb::prepare(double sr) {
    rate_ = std::isfinite(sr) && sr >= 1000 ? sr : 48000;
    const auto length = static_cast<std::size_t>(rate_) + 8;
    for (auto &t : taps_) {
        t.ring.assign(length, 0);
        t.low = t.high = 0;
    }
    predelay_.assign(length, 0);
    for (auto &d : diffusers_)
        d.assign(length, 0);
    write_ = 0;
    size_ = p_[Size] / 100;
    spin_ = chorus_ = low_ = high_ = 0;
    started_ = false;
}
void LiveReverb::set(Param key, double v) noexcept {
    if (key >= 0 && key < Count && std::isfinite(v))
        p_[static_cast<std::size_t>(key)] = std::clamp(v, ranges[key].low, ranges[key].high);
}
double LiveReverb::read(const std::vector<float> &ring, double delay) const noexcept {
    delay = std::clamp(delay, 0., static_cast<double>(ring.size() - 2));
    const auto whole = static_cast<std::size_t>(delay);
    const double fraction = delay - static_cast<double>(whole);
    const auto at = (write_ + ring.size() - whole) % ring.size();
    return ring[at] * (1 - fraction) + ring[(at + ring.size() - 1) % ring.size()] * fraction;
}
void LiveReverb::process(const float *inL, const float *inR, float *outL, float *outR,
                         std::size_t n) noexcept {
    if (predelay_.empty()) {
        std::fill(outL, outL + n, 0.f);
        std::fill(outR, outR + n, 0.f);
        return;
    }
    auto &p = p_;
    const auto count = static_cast<std::size_t>(4 << static_cast<int>(std::round(p[Density])));
    const double lowCoefficient = 1 - std::exp(-2 * pi * p[LowCut] / rate_),
                 highCoefficient =
                     1 - std::exp(-2 * pi * std::min(p[HighCut], rate_ * 0.45) / rate_);
    const double lowShelf = 1 - std::exp(-2 * pi * p[LowShelfFreq] / rate_),
                 highShelf =
                     1 - std::exp(-2 * pi * std::min(p[HighShelfFreq], rate_ * 0.45) / rate_);
    const double reflectGain = std::pow(10., p[Reflect] / 20),
                 diffuseGain = std::pow(10., p[Diffuse] / 20);
    for (std::size_t frame = 0; frame < n; ++frame) {
        const double inputL = inL[frame], inputR = inR[frame];
        double input = 0.5 * (inputL + inputR);
        low_ += (input - low_) * lowCoefficient;
        if (p[LowCutOn] >= 0.5)
            input -= low_;
        high_ += (input - high_) * highCoefficient;
        if (p[HighCutOn] >= 0.5)
            input = high_;
        predelay_[write_] = static_cast<float>(input);
        const int smooth = static_cast<int>(std::round(p[Smooth]));
        const double target = p[Size] / 100;
        if (!started_ || smooth == 0)
            size_ = target;
        else
            size_ += (target - size_) * (1 - std::exp(-1 / (rate_ * (smooth == 1 ? 0.02 : 0.2))));
        started_ = true;
        spin_ += p[SpinRate] / rate_;
        spin_ -= std::floor(spin_);
        chorus_ += p[ChorusRate] / rate_;
        chorus_ -= std::floor(chorus_);
        const double predelay = p[Predelay] * rate_ / 1000;
        double earlyL = 0, earlyR = 0;
        for (int t = 0; t < 8; ++t) {
            const double shift = p[SpinOn] >= 0.5
                                     ? p[SpinAmount] * rate_ / 1000 *
                                           std::sin(2 * pi * (spin_ + static_cast<double>(t) / 8))
                                     : 0;
            const double time =
                predelay + size_ * rate_ * 0.005 * static_cast<double>(t) + std::max(0., shift);
            const double gain =
                std::exp(-static_cast<double>(t) * (0.1 + 0.5 * p[Shape] / 100)) / 4;
            const double echo = read(predelay_, time) * gain;
            earlyL += echo * (t % 2 == 0 ? 1 : 0.3);
            earlyR += echo * (t % 2 == 0 ? 0.3 : 1);
        }
        double injected = read(predelay_, predelay + rate_ * 0.04 * p[Shape] / 100);
        const double diffusion = 0.7 * p[Diffusion] / 100;
        for (std::size_t d = 0; d < 2; ++d) {
            const double delay =
                std::max(1., (d == 0 ? 149. : 211.) * size_ * p[Scale] * rate_ / 48000);
            const double old = read(diffusers_[d], delay);
            const double value = injected - diffusion * old;
            diffusers_[d][write_] = static_cast<float>(value);
            injected = old + diffusion * value;
        }
        if (p[Freeze] >= 0.5 && p[Cut] >= 0.5)
            injected = 0;
        std::array<double, 16> out{}, times{};
        double sum = 0;
        for (std::size_t t = 0; t < count; ++t) {
            auto &tap = taps_[t];
            // Round static lengths to integer samples: no unintended interpolation
            // damping when Chorus is off. Decay gain uses this actual length.
            times[t] = std::max(
                1., std::round(static_cast<double>(delays[t]) / 256 * size_ * rate_ / 48000));
            const double modulation =
                p[ChorusOn] >= 0.5
                    ? p[ChorusAmount] * rate_ / 1000 *
                          std::sin(2 * pi *
                                   (chorus_ + static_cast<double>(t) / static_cast<double>(count)))
                    : 0;
            double value = read(tap.ring, times[t] + modulation);
            const bool flat = p[Freeze] >= 0.5 && p[Flat] >= 0.5;
            if (!flat && p[HighShelfOn] >= 0.5) {
                tap.high += (value - tap.high) * highShelf;
                const double relative =
                    std::pow(0.001, times[t] / (rate_ * p[Decay]) * (1 / p[HighShelfDecay] - 1));
                value = tap.high + (value - tap.high) * relative;
            }
            if (!flat && p[LowShelfOn] >= 0.5) {
                tap.low += (value - tap.low) * lowShelf;
                const double relative =
                    std::pow(0.001, times[t] / (rate_ * p[Decay]) * (1 / p[LowShelfDecay] - 1));
                value -= tap.low * (1 - relative);
            }
            out[t] = value;
            sum += value;
        }
        // Surge's unitary Householder reflection, -2/N times the tap sum.
        const double feedback = -2 * sum / static_cast<double>(count) + injected * 0.25;
        double lateL = 0, lateR = 0;
        for (std::size_t t = 0; t < count; ++t) {
            const double gain =
                p[Freeze] >= 0.5 ? 1 : std::pow(0.001, times[t] / (rate_ * p[Decay]));
            taps_[t].ring[write_] = static_cast<float>((out[t] + feedback) * gain);
            const double pan = -1 + 2 * static_cast<double>(t) / static_cast<double>(count - 1);
            lateL += out[t] * std::sqrt(0.5 - 0.495 * pan);
            lateR += out[t] * std::sqrt(0.5 + 0.495 * pan);
        }
        double l =
            earlyL * reflectGain + lateL * diffuseGain / std::sqrt(static_cast<double>(count));
        double r =
            earlyR * reflectGain + lateR * diffuseGain / std::sqrt(static_cast<double>(count));
        const double mid = (l + r) * 0.5, side = (l - r) * 0.5 * p[Stereo] / 120;
        l = mid + side;
        r = mid - side;
        const double mix = p[Mix] / 100;
        outL[frame] = static_cast<float>(inputL * (1 - mix) + l * mix);
        outR[frame] = static_cast<float>(inputR * (1 - mix) + r * mix);
        write_ = (write_ + 1) % predelay_.size();
    }
}
} // namespace adi::dsp
