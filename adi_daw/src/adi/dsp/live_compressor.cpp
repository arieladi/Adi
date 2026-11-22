// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/live_compressor.hpp"

#include <algorithm>
#include <cmath>

namespace adi::dsp {
namespace {
constexpr double kFloorDb = -150;
double coefficient(double ms, double rate) noexcept { return 1 - std::exp(-1000 / (ms * rate)); }
double toDb(double x) noexcept { return x > 1e-7 ? 20 * std::log10(x) : kFloorDb; }
double toGain(double db) noexcept { return db == 0 ? 1.0 : std::pow(10.0, db / 20); }
}  // namespace

LiveCompressor::LiveCompressor() noexcept {
    for (int i = 0; i < Count; ++i) p_[static_cast<std::size_t>(i)] = ranges[i].initial;
    update();
}

void LiveCompressor::prepare(double sampleRate) noexcept {
    rate_ = std::isfinite(sampleRate) ? std::clamp(sampleRate, 8000.0, 384000.0) : 48000.0;
    for (auto& line : line_) line.fill(0);
    write_ = 0;
    power_ = level_ = reductionDb_ = 0;
    smooth_ = coefficient(10, rate_);
    update();
    output_ = outputTarget_;
    wet_ = wetTarget_;
}

void LiveCompressor::set(Param id, double v) noexcept {
    const int n = static_cast<int>(id);
    if (n < 0 || n >= Count || !std::isfinite(v)) return;
    v = std::clamp(v, ranges[n].low, ranges[n].high);
    if (id == Lookahead || id == Mode || id == Envelope || id == Makeup) v = std::round(v);
    p_[static_cast<std::size_t>(n)] = v;
    update();
}

double LiveCompressor::staticGainDb(double levelDb) const noexcept {
    const double threshold = p_[Threshold], knee = p_[Knee];
    const double ratio = p_[Ratio];
    const bool expand = static_cast<int>(p_[Mode]) == Expand;
    // Slope of the gain change past the threshold: 1/R - 1 above it when
    // compressing (-1 at infinity), R - 1 below it when expanding.
    const double slope = expand ? ratio - 1 : (ratio >= 100 ? -1.0 : 1 / ratio - 1);
    const double over = levelDb - threshold;
    if (expand) {
        if (2 * over >= knee) return 0;
        if (2 * over > -knee) return -slope * (over - knee / 2) * (over - knee / 2) / (2 * knee);
        return slope * over;
    }
    if (2 * over <= -knee) return 0;
    if (2 * over < knee) return slope * (over + knee / 2) * (over + knee / 2) / (2 * knee);
    return slope * over;
}

void LiveCompressor::update() noexcept {
    attack_ = coefficient(p_[Attack], rate_);
    release_ = coefficient(p_[Release], rate_);
    rms_ = coefficient(10, rate_);
    constexpr double ms[] = {0, 1, 10};
    lookahead_ = std::min(kMaxLookahead - 1,
                          static_cast<int>(std::lround(ms[static_cast<std::size_t>(p_[Lookahead])] * rate_ / 1000)));
    makeupDb_ = p_[Makeup] >= .5 && static_cast<int>(p_[Mode]) != Expand ? -staticGainDb(0) : 0;
    outputTarget_ = toGain(p_[Output]);
    wetTarget_ = p_[DryWet] / 100;
}

void LiveCompressor::process(const float* inL, const float* inR, float* outL, float* outR, std::size_t frames) noexcept {
    const int mode = static_cast<int>(p_[Mode]);
    const bool linear = static_cast<int>(p_[Envelope]) == Lin;
    const std::size_t size = kMaxLookahead;
    for (std::size_t i = 0; i < frames; ++i) {
        const double l = std::isfinite(inL[i]) ? inL[i] : 0.0, r = std::isfinite(inR[i]) ? inR[i] : 0.0;
        // The detector, linked on the louder channel.
        double detected = std::max(std::abs(l), std::abs(r));
        if (mode == Rms) {
            power_ += rms_ * (detected * detected - power_);
            detected = std::sqrt(std::max(power_, 0.0));
        }
        double target;
        if (linear) {
            // Lin: the ballistics act on the level; the curve reads the result.
            level_ += (detected > level_ ? attack_ : release_) * (detected - level_);
            target = staticGainDb(toDb(level_));
            reductionDb_ = target;
        } else {
            // Log: the curve reads the level; the ballistics act on the dB.
            target = staticGainDb(toDb(detected));
            reductionDb_ += (target < reductionDb_ ? attack_ : release_) * (target - reductionDb_);
        }
        // The audio waits the lookahead; the detector did not.
        line_[0][write_] = l;
        line_[1][write_] = r;
        const std::size_t read = (write_ + size - static_cast<std::size_t>(lookahead_)) % size;
        write_ = (write_ + 1) % size;
        const double dryL = line_[0][read], dryR = line_[1][read];
        output_ += smooth_ * (outputTarget_ - output_);
        wet_ += smooth_ * (wetTarget_ - wet_);
        const double gain = toGain(reductionDb_ + makeupDb_) * output_;
        outL[i] = static_cast<float>(dryL + wet_ * (dryL * gain - dryL));
        outR[i] = static_cast<float>(dryR + wet_ * (dryR * gain - dryR));
    }
}

}  // namespace adi::dsp
