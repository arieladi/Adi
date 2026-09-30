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
/*
 * sst-filters - A header-only collection of SIMD filter
 * implementations by the Surge Synth Team
 *
 * Copyright 2019-2025, various authors, as described in the GitHub
 * transaction log.
 *
 * sst-filters is released under the Gnu General Public Licens
 * version 3 or later. Some of the filters in this package
 * originated in the version of Surge open sourced in 2018.
 *
 * All source in sst-filters available at
 * https://github.com/surge-synthesizer/sst-filters
 */
/*
 * sst-basic-blocks - an open source library of core audio utilities
 * built by Surge Synth Team.
 *
 * Provides a collection of tools useful on the audio thread for blocks,
 * modulation, etc... or useful for adapting code to multiple environments.
 *
 * Copyright 2023, various authors, as described in the GitHub
 * transaction log. Parts of this code are derived from similar
 * functions original in Surge or ShortCircuit.
 *
 * sst-basic-blocks is released under the GNU General Public Licence v3
 * or later (GPL-3.0-or-later). The license is found in the "LICENSE"
 * file in the root of this repository, or at
 * https://www.gnu.org/licenses/gpl-3.0.en.html.
 *
 * A very small number of explicitly chosen header files can also be
 * used in an MIT/BSD context. Please see the README.md file in this
 * repo or the comments in the individual files. Only headers with an
 * explicit mention that they are dual licensed may be copied and reused
 * outside the GPL3 terms.
 *
 * All source in sst-basic-blocks available at
 * https://github.com/surge-synthesizer/sst-basic-blocks
 */
// Scalar adaptation of COMBquad_SSE2's delayed feedback/write/output and
// softclip_ps, arranged as Combulator's parallel tuned bank (five Live voices).
// Fractional reads use cubic interpolation rather than Surge's SIMD sinc table.
#include "resonators.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>
namespace adi::dsp {
namespace {
constexpr double pi = std::numbers::pi;
double db(double x) { return x <= -60 ? 0 : std::pow(10., x / 20.); }
} // namespace
Resonators::Resonators() noexcept
    : p_{0, 0,  10000, 0,  2, 0,  75, 48, 0, 1,  0, 1,  7,   0, -6,
         1, 12, 0,     -6, 1, 19, 0,  -6, 1, 24, 0, -6, 100, 0, 50} {}
double Resonators::softclip(double x) noexcept {
    x = std::clamp(x, -1.5, 1.5);
    return x - (4. / 27.) * x * x * x;
}
double Resonators::Biquad::tick(double x) noexcept {
    const double y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
}
void Resonators::prepare(double sampleRate) {
    sr_ = std::isfinite(sampleRate) ? std::clamp(sampleRate, 8000., 384000.) : 48000.;
    const auto size = static_cast<std::size_t>(std::ceil(sr_ / 2)) + 8;
    for (auto &v : voices_)
        for (auto &l : v.lanes) {
            l.line.assign(size, 0);
            l.write = 0;
            l.lowpass = 0;
        }
    input_ = {};
    prepared_ = true;
    dirty_ = true;
    update();
}
void Resonators::set(Param id, double value) noexcept {
    if (id < 0 || id >= Count || !std::isfinite(value))
        return;
    static constexpr std::array<double, Count> lo = {
        0, 0,   20,   0,   0.05, 0,   0,    0,   -100, 0,   -60,  0,   -24, -100, -60,
        0, -24, -100, -60, 0,    -24, -100, -60, 0,    -24, -100, -60, 0,   -60,  0};
    static constexpr std::array<double, Count> hi = {
        1, 3,  20000, 1,  20, 1,  100, 72, 100, 1,  12,  1,  24,  100, 12,
        1, 24, 100,   12, 1,  24, 100, 12, 1,   24, 100, 12, 200, 24,  100};
    p_[id] = std::clamp(value, lo[id], hi[id]);
    dirty_ = true;
}
bool Resonators::tuning(const std::array<double, 5> &hz) noexcept {
    for (double f : hz)
        if (!std::isfinite(f) || f < 2 || f > 20000)
            return false;
    tuningHz_ = hz;
    tuned_ = true;
    dirty_ = true;
    return true;
}
void Resonators::clearTuning() noexcept {
    tuned_ = false;
    dirty_ = true;
}
double Resonators::frequency(std::size_t voice) const noexcept {
    return voice < 5 ? voices_[voice].hz : 0;
}
double Resonators::decaySeconds(std::size_t voice) const noexcept {
    return voice < 5 ? voices_[voice].seconds : 0;
}
void Resonators::update() noexcept {
    if (!dirty_)
        return;
    dirty_ = false;
    const double cutoff = 200 * std::pow(100., p_[Color] * .01);
    const double alpha =
        p_[Color] >= 100 ? 1 : 1 - std::exp(-2 * pi * std::min(cutoff, sr_ * .45) / sr_);
    const std::array<Param, 5> on = {On1, On2, On3, On4, On5},
                               gain = {Gain1, Gain2, Gain3, Gain4, Gain5},
                               fine = {Fine1, Fine2, Fine3, Fine4, Fine5};
    const std::array<double, 5> transpose = {0, p_[Pitch2], p_[Pitch3], p_[Pitch4], p_[Pitch5]};
    for (std::size_t i = 0; i < 5; ++i) {
        auto &v = voices_[i];
        const double base =
            tuned_ ? tuningHz_[i] : 440 * std::exp2((p_[Note] + transpose[i] - 69) / 12.);
        v.hz = std::clamp(base * std::exp2(p_[fine[i]] / 1200.), 2., std::min(20000., sr_ * .2));
        v.seconds = p_[Decay] * (p_[Const] >= .5 ? 1 : std::sqrt(440 / v.hz));
        const double period = sr_ / v.hz * (p_[Mode] >= .5 ? .5 : 1.);
        const double omega = 2 * pi * v.hz / sr_;
        const double phaseDelay =
            std::atan2((1 - alpha) * std::sin(omega), 1 - (1 - alpha) * std::cos(omega)) / omega;
        v.delay = std::clamp(period - phaseDelay, 2., sr_ / 2);
        v.g = (p_[Mode] >= .5 ? -1 : 1) * std::pow(.001, period / (sr_ * v.seconds));
        v.alpha = alpha;
        v.gain = db(p_[gain[i]]);
        v.enabled = p_[on[i]] >= .5;
    }
    const double omega = 2 * pi * std::min(p_[Frequency], sr_ * .45) / sr_, cs = std::cos(omega),
                 sn = std::sin(omega), a = sn / (2 * .7071067811865476), den = 1 + a;
    double b0 = 1, b1 = 0, b2 = 0;
    switch (static_cast<int>(std::round(p_[FilterType]))) {
    case 0:
        b0 = (1 - cs) / 2;
        b1 = 1 - cs;
        b2 = b0;
        break;
    case 1:
        b0 = a;
        b1 = 0;
        b2 = -a;
        break;
    case 2:
        b0 = (1 + cs) / 2;
        b1 = -(1 + cs);
        b2 = b0;
        break;
    default:
        b0 = 1;
        b1 = -2 * cs;
        b2 = 1;
        break;
    }
    for (auto &f : input_) {
        f.b0 = b0 / den;
        f.b1 = b1 / den;
        f.b2 = b2 / den;
        f.a1 = -2 * cs / den;
        f.a2 = (1 - a) / den;
    }
}
double Resonators::sample(Voice &v, Lane &l, double input) noexcept {
    const auto size = l.line.size();
    double read = static_cast<double>(l.write) - v.delay;
    if (read < 0)
        read += static_cast<double>(size);
    const auto index = static_cast<std::size_t>(read);
    const double fraction = read - static_cast<double>(index);
    const double a = l.line[(index + size - 1) % size], b = l.line[index],
                 c = l.line[(index + 1) % size], d = l.line[(index + 2) % size];
    const double delayed =
        b + .5 * fraction *
                (c - a + fraction * (2 * a - 5 * b + 4 * c - d + fraction * (3 * (b - c) + d - a)));
    l.lowpass += v.alpha * (delayed - l.lowpass);
    // COMBquad: d=softclip(in+DBRead*feedback), write d, return wet DBRead.
    double value = softclip(input + v.g * l.lowpass);
    if (std::abs(value) < 1e-30)
        value = 0;
    l.line[l.write] = value;
    l.write = (l.write + 1) % size;
    return delayed * v.gain;
}
void Resonators::process(const float *left, const float *right, float *outLeft, float *outRight,
                         std::size_t frames) noexcept {
    update();
    if (!prepared_) {
        for (std::size_t n = 0; n < frames; ++n) {
            outLeft[n] = left ? left[n] : 0;
            outRight[n] = right ? right[n] : 0;
        }
        return;
    }
    const double gain = db(p_[Gain]), mix = p_[Mix] * .01, width = p_[Width] * .01;
    for (std::size_t n = 0; n < frames; ++n) {
        const double dry[2] = {left ? left[n] : 0, right ? right[n] : 0};
        const double in[2] = {p_[FilterOn] >= .5 ? input_[0].tick(dry[0]) : dry[0],
                              p_[FilterOn] >= .5 ? input_[1].tick(dry[1]) : dry[1]};
        double root[2] = {}, sides[2] = {};
        if (voices_[0].enabled) {
            root[0] = sample(voices_[0], voices_[0].lanes[0], in[0]);
            root[1] = sample(voices_[0], voices_[0].lanes[1], in[1]);
        }
        for (std::size_t i = 1; i < 5; ++i) {
            if (!voices_[i].enabled)
                continue;
            const std::size_t c = (i - 1) % 2;
            sides[c] += sample(voices_[i], voices_[i].lanes[c], in[c]);
        }
        const double mid = .5 * (sides[0] + sides[1]), side = .5 * (sides[0] - sides[1]) * width;
        outLeft[n] = static_cast<float>(dry[0] * (1 - mix) + gain * mix * (root[0] + mid + side));
        outRight[n] = static_cast<float>(dry[1] * (1 - mix) + gain * mix * (root[1] + mid - side));
    }
}
} // namespace adi::dsp
