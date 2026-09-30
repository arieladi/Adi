/*
 * Surge XT - a free and open source hybrid synthesizer,
 * built by Surge Synth Team
 *
 * Learn more at https://surge-synthesizer.github.io/
 *
 * Copyright 2018-2024, various authors, as described in the GitHub
 * transaction log.
 *
 * Surge XT is released under the GNU General Public Licence v3
 * or later (GPL-3.0-or-later). The license is found in the "LICENSE"
 * file in the root of this repository, or at
 * https://www.gnu.org/licenses/gpl-3.0.en.html
 *
 * Surge was a commercial product from 2004-2018, copyright and ownership
 * held by Claes Johanson at Vember Audio during that period.
 * Claes made Surge open source in September 2018.
 *
 * All source for Surge XT is available at
 * https://github.com/surge-synthesizer/surge
 */
// Adapted from Surge XT 58914e59: VectorizedSVFilter.{h,cpp} and
// effects/VocoderEffect.cpp. Live-style controls and carrier generators are ADI.
#include "vocoder.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>
namespace adi::dsp {
namespace {
constexpr double pi = std::numbers::pi;
double db(double x) noexcept { return std::pow(10., x / 20.); }
} // namespace
void VocoderBand::coefficients(double hz, double quality, double sampleRate) noexcept {
    quality = std::max(.707, quality);
    q = 1 / quality;
    const double spread = .4 / quality;
    hz = std::clamp(hz, 10., sampleRate * .2);
    f1 = 2 * std::sin(pi * hz * (1 - spread) / (2 * sampleRate));
    f2 = 2 * std::sin(pi * hz * (1 + spread) / (2 * sampleRate));
}
double VocoderBand::step(double input) noexcept {
    for (int i = 0; i < 2; ++i) {
        l1 += f1 * b1;
        const double h1 = input * q - l1 - q * b1;
        b1 += f1 * h1;
        l2 += f2 * b2;
        const double h2 = b1 * q - l2 - q * b2;
        b2 += f2 * h2;
    }
    return b2;
}
Vocoder::Vocoder() noexcept
    : p_{0, 48000, 100, 80, 1200, 0, 0, 0, 0, 50, 1, 4, 80, 12000, 100, 0, -96, 0, 100, 5, 100, 1,
         0, 100,   0,   0,  0,    0, 0, 0, 0, 0,  0, 0, 0,  0,     0,   0, 0,   0, 0,   0, 0,   0} {
}
void Vocoder::prepare(double sampleRate) noexcept {
    sr_ = std::isfinite(sampleRate) ? std::clamp(sampleRate, 8000., 384000.) : 48000.;
    mod_ = {};
    carrier_ = {};
    envelopes_ = {};
    carrierPower_ = {};
    pitchBuffer_ = {};
    pitchWrite_ = pitchCount_ = 0;
    decimation_ = static_cast<unsigned>(std::max(1., std::floor(sr_ / 12000.)));
    decimationPhase_ = trackerClock_ = 0;
    seed_ = 0x12345678u;
    pitchHz_ = 220;
    oscPhase_ = 0;
    noisePhase_ = 1;
    noise_ = pitchLP_ = 0;
    detectorLP_ = detectorPower_ = detectorHigh_ = unvoiced_ = 0;
    dirty_ = true;
    update();
}
void Vocoder::set(Param id, double value) noexcept {
    if (id < 0 || id >= Count || !std::isfinite(value))
        return;
    static constexpr std::array<double, Count> lo = {
        0,   100,  0,   40,  100, 0,   -48, 0,   0,   0,   0,   0,   40,  1000, 10,
        0,   -120, -60, 0,   0.1, 1,   0,   -24, 0,   -60, -60, -60, -60, -60,  -60,
        -60, -60,  -60, -60, -60, -60, -60, -60, -60, -60, -60, -60, -60, -60};
    static constexpr std::array<double, Count> hi = {
        3, 48000, 100, 1000, 4000, 3,    48, 1,  100, 100, 1, 4, 4000, 20000, 200,
        1, 0,     24,  200,  1000, 5000, 2,  24, 100, 0,   0, 0, 0,    0,     0,
        0, 0,     0,   0,    0,    0,    0,  0,  0,   0,   0, 0, 0,    0};
    value = std::clamp(value, lo[id], hi[id]);
    if (id == Carrier || id == Waveform || id == Bands || id == Channels)
        value = std::round(value);
    p_[id] = value;
    dirty_ = true;
}
void Vocoder::update() noexcept {
    if (!dirty_)
        return;
    dirty_ = false;
    bands_ = 4 + 4 * static_cast<int>(p_[Bands]);
    const double low = std::min(p_[Low], p_[High]), high = std::max(p_[Low], p_[High]);
    const double ratio = std::pow(high / low, 1. / (bands_ - 1));
    const double baseQ = std::clamp(
        1. / (std::sqrt(ratio) - 1 / std::sqrt(ratio)) * 100 / p_[Bandwidth], .707, 100.);
    for (int i = 0; i < bands_; ++i) {
        const auto b = static_cast<std::size_t>(i);
        const double hz = low * std::pow(ratio, i);
        const double tilt = p_[Retro] >= .5 ? std::sqrt(hz / low) : 1.;
        for (std::size_t c = 0; c < 2; ++c) {
            mod_[c][b].coefficients(hz, baseQ * tilt, sr_);
            carrier_[c][b].coefficients(hz * std::exp2(p_[Formant] / 12), baseQ * tilt, sr_);
        }
        gains_[b] = (p_[Band1 + b] <= -60 ? 0 : db(p_[Band1 + b])) * std::sqrt(tilt);
    }
    attack_ = std::exp(-1 / (sr_ * p_[Attack] * .001));
    release_ = std::exp(-1 / (sr_ * p_[Release] * .001));
    gate_ = db(p_[Gate]);
    gate_ *= gate_;
    level_ = db(p_[Level]);
    detectorRate_ = std::exp(-1 / (sr_ * (p_[Fast] >= .5 ? .005 : .05)));
}
double Vocoder::random() noexcept {
    seed_ ^= seed_ << 13;
    seed_ ^= seed_ >> 17;
    seed_ ^= seed_ << 5;
    return static_cast<double>(seed_) / 2147483648. - 1;
}
void Vocoder::track(double input) noexcept {
    pitchLP_ += (1 - std::exp(-2 * pi * std::min(p_[PitchHigh] * 1.5, sr_ * .2) / sr_)) *
                (input - pitchLP_);
    if (++decimationPhase_ < decimation_)
        return;
    decimationPhase_ = 0;
    pitchBuffer_[pitchWrite_] = pitchLP_;
    pitchWrite_ = (pitchWrite_ + 1) % pitchBuffer_.size();
    pitchCount_ = std::min(pitchCount_ + 1, pitchBuffer_.size());
    if (++trackerClock_ < 128 || pitchCount_ < 1024)
        return;
    trackerClock_ = 0;
    const double rate = sr_ / decimation_;
    const double low = std::min(p_[PitchLow], p_[PitchHigh]),
                 high = std::max(p_[PitchLow], p_[PitchHigh]);
    const auto first = static_cast<std::size_t>(std::max(2., std::floor(rate / high)));
    const auto last = static_cast<std::size_t>(std::clamp(std::ceil(rate / low), 3., 510.));
    auto corr = [&](std::size_t lag) {
        double xy = 0, xx = 0, yy = 0;
        for (std::size_t i = 0; i < 512; ++i) {
            const double x = pitchBuffer_[(pitchWrite_ + 1023 - i) % 1024],
                         y = pitchBuffer_[(pitchWrite_ + 1023 - i - lag) % 1024];
            xy += x * y;
            xx += x * x;
            yy += y * y;
        }
        return xx > 1e-8 && yy > 1e-8 ? xy / std::sqrt(xx * yy) : 0.;
    };
    double a = corr(first - 1), b = corr(first);
    for (std::size_t lag = first; lag <= last; ++lag) {
        const double c = corr(lag + 1);
        if (b > .85 && b > a && b >= c) {
            const double denominator = a - 2 * b + c;
            const double delta = std::abs(denominator) > 1e-12 ? .5 * (a - c) / denominator : 0.;
            const double f = rate / (static_cast<double>(lag) + std::clamp(delta, -.5, .5));
            if (f >= low && f <= high)
                pitchHz_ = f;
            break;
        }
        a = b;
        b = c;
    }
}
void Vocoder::process(const float *left, const float *right, float *outLeft, float *outRight,
                      std::size_t frames, const float *carrierLeft,
                      const float *carrierRight) noexcept {
    update();
    for (std::size_t n = 0; n < frames; ++n) {
        const double dry[2] = {left ? left[n] : 0, right ? right[n] : 0};
        const double mid = .5 * (dry[0] + dry[1]);
        const int mode = static_cast<int>(p_[Carrier]), channels = static_cast<int>(p_[Channels]);
        if (mode == 3)
            track(mid);
        noisePhase_ += std::min(p_[NoiseRate], sr_) / sr_;
        if (noisePhase_ >= 1) {
            noisePhase_ -= std::floor(noisePhase_);
            const double r = random();
            noise_ = (.5 * (random() + 1) < p_[NoiseDensity] * .01) ? r : 0;
        }
        oscPhase_ += pitchHz_ * std::exp2(p_[Pitch] / 12) / sr_;
        oscPhase_ -= std::floor(oscPhase_);
        const int wave = static_cast<int>(p_[Waveform]);
        const double pulse = wave == 1 ? .5 : wave == 2 ? .25 : .125;
        const double osc = wave == 0 ? 2 * oscPhase_ - 1 : (oscPhase_ < pulse ? 1. : -1.);
        double car[2] = {mode == 0   ? noise_
                         : mode == 1 ? (carrierLeft ? carrierLeft[n] : 0)
                         : mode == 2 ? dry[0]
                                     : osc,
                         mode == 0   ? noise_
                         : mode == 1 ? (carrierRight ? carrierRight[n] : 0)
                         : mode == 2 ? dry[1]
                                     : osc};
        detectorLP_ += (1 - std::exp(-2 * pi * 2000 / sr_)) * (mid - detectorLP_);
        const double hp = mid - detectorLP_;
        detectorPower_ = detectorRate_ * detectorPower_ + (1 - detectorRate_) * mid * mid;
        detectorHigh_ = detectorRate_ * detectorHigh_ + (1 - detectorRate_) * hp * hp;
        const double sens = p_[Sensitivity] * .01;
        const double uv =
            sens >= 1 ? 1
            : sens <= 0
                ? 0
                : (detectorHigh_ > (1 - sens) * detectorPower_ && detectorPower_ > 1e-10 ? 1. : 0.);
        unvoiced_ = detectorRate_ * unvoiced_ + (1 - detectorRate_) * uv;
        const double breath = random() * p_[Unvoiced] * .01 * unvoiced_;
        car[0] += breath;
        car[1] += breath;
        if (channels == 0)
            car[0] = car[1] = .5 * (car[0] + car[1]);
        double wet[2] = {};
        for (std::size_t c = 0; c < 2; ++c) {
            for (int i = 0; i < bands_; ++i) {
                const auto b = static_cast<std::size_t>(i);
                const double m = mod_[c][b].step(channels == 2 ? dry[c] : mid);
                double power = std::min(6., m * m);
                if (power < gate_)
                    power = 0;
                auto &env = envelopes_[c][b];
                const double rate = power > env ? attack_ : release_;
                env = rate * env + (1 - rate) * power;
                const double amp = p_[Depth] == 0 ? 1 : std::pow(std::sqrt(env), p_[Depth] * .01);
                // Surge applies each envelope before its corresponding carrier filter.
                double v = carrier_[c][b].step(car[c] * amp);
                auto &cp = carrierPower_[c][b];
                cp = .999 * cp + .001 * v * v;
                if (p_[Enhance] >= .5)
                    v *= std::min(8., 1 / std::sqrt(cp + 1e-6));
                wet[c] += gains_[b] * v;
            }
        }
        const double mix = p_[Mix] * .01;
        outLeft[n] = static_cast<float>(dry[0] * (1 - mix) + 4 * wet[0] * mix * level_);
        outRight[n] = static_cast<float>(dry[1] * (1 - mix) + 4 * wet[1] * mix * level_);
    }
}
} // namespace adi::dsp
