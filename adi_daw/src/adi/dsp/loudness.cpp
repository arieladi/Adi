// SPDX-License-Identifier: GPL-3.0-or-later
// See loudness.hpp.

#include "adi/dsp/loudness.hpp"

#include "adi/dsp/auto_gain.hpp"

#include <algorithm>
#include <cmath>

namespace adi::dsp {
namespace {

constexpr int kBins = 9000;              // -70 .. +20 LUFS at 0.01 LU
constexpr double kBinWidth = 0.01;
constexpr double kAbsoluteGate = -70.0;
constexpr int kMomentary = 40, kShortTerm = 300, kHop = 10;

/// MATLAB's round(): halves away from zero (Tech 3342's reference uses it).
std::int64_t roundHalfAway(double x) noexcept {
    return static_cast<std::int64_t>(x < 0 ? std::ceil(x - 0.5) : std::floor(x + 0.5));
}

}  // namespace

double LoudnessMeter::loudness(double energy) noexcept {
    return energy > 0 ? -0.691 + 10.0 * std::log10(energy) : -kInfinity;
}

int LoudnessMeter::binOf(double lufs) noexcept {
    if (!(lufs > kAbsoluteGate)) return 0;
    const double index = std::floor((lufs - kAbsoluteGate) / kBinWidth);
    return static_cast<int>(std::min<double>(index, kBins - 1));
}

double LoudnessMeter::valueOf(int bin) noexcept {
    return kAbsoluteGate + (static_cast<double>(bin) + 0.5) * kBinWidth;
}

// --- the Fenwick histogram -----------------------------------------------------

void LoudnessMeter::Histogram::prepare(int bins) {
    count.assign(static_cast<std::size_t>(bins) + 1, 0);
    energy.assign(static_cast<std::size_t>(bins) + 1, 0.0);
    total = 0;
    totalEnergy = 0.0;
}

void LoudnessMeter::Histogram::clear() noexcept {
    std::fill(count.begin(), count.end(), 0);
    std::fill(energy.begin(), energy.end(), 0.0);
    total = 0;
    totalEnergy = 0.0;
}

void LoudnessMeter::Histogram::add(int bin, double e) noexcept {
    const auto n = static_cast<int>(count.size()) - 1;
    for (int i = bin + 1; i <= n; i += i & -i) {
        count[static_cast<std::size_t>(i)] += 1;
        energy[static_cast<std::size_t>(i)] += e;
    }
    total += 1;
    totalEnergy += e;
}

std::int64_t LoudnessMeter::Histogram::countBelow(int bin) const noexcept {
    std::int64_t sum = 0;
    for (int i = bin; i > 0; i -= i & -i) sum += count[static_cast<std::size_t>(i)];
    return sum;
}

double LoudnessMeter::Histogram::energyBelow(int bin) const noexcept {
    double sum = 0.0;
    for (int i = bin; i > 0; i -= i & -i) sum += energy[static_cast<std::size_t>(i)];
    return sum;
}

int LoudnessMeter::Histogram::find(std::int64_t k) const noexcept {
    const auto n = static_cast<int>(count.size()) - 1;
    int step = 1;
    while (step * 2 <= n) step *= 2;
    int position = 0;
    for (; step > 0; step /= 2) {
        const int next = position + step;
        if (next <= n && count[static_cast<std::size_t>(next)] < k) {
            position = next;
            k -= count[static_cast<std::size_t>(next)];
        }
    }
    return std::min(position, n - 1);   // 0-based bin
}

// --- the meter -------------------------------------------------------------

void LoudnessMeter::prepare(double sampleRate, std::span<const double> channelWeights) {
    rate_ = std::isfinite(sampleRate) && sampleRate >= 1000.0 ? sampleRate : 48000.0;
    weights_.assign(channelWeights.begin(), channelWeights.end());
    const auto n = weights_.size();
    shelf_.assign(n, AutoGain::kShelf(rate_));
    highPass_.assign(n, AutoGain::kHighPass(rate_));
    shelfState_.assign(n, State{});
    highPassState_.assign(n, State{});
    integrated_.prepare(kBins);
    range_.prepare(kBins);
    truePeakOn_ = n > 0;
    truePeak_.prepare(rate_, static_cast<int>(n));
    tailZeros_.assign(static_cast<std::size_t>(TruePeakMeter::kTapsPerPhase), 0.0);
    tailChannels_.assign(n, tailZeros_.data());
    reset();
}

void LoudnessMeter::finish() noexcept {
    // The interpolator answers kDelay samples late; a phase's whole length of
    // silence drains it. Only the true peak sees it.
    if (truePeakOn_ && !tailChannels_.empty()) {
        truePeak_.process(tailChannels_.data(), channels(), TruePeakMeter::kTapsPerPhase);
    }
}

void LoudnessMeter::reset() noexcept {
    std::fill(shelfState_.begin(), shelfState_.end(), State{});
    std::fill(highPassState_.begin(), highPassState_.end(), State{});
    subBlocks_ = 0;
    samplesInSubBlock_ = 0;
    subBlockLength_ = std::llround(rate_ / 100.0);
    subBlockEnergy_ = 0.0;
    std::fill(std::begin(ringEnergy_), std::end(ringEnergy_), 0.0);
    std::fill(std::begin(ringSamples_), std::end(ringSamples_), 0);
    integrated_.clear();
    range_.clear();
    momentary_ = shortTerm_ = maxMomentary_ = maxShortTerm_ = integratedValue_ = -kInfinity;
    rangeValue_ = 0.0;
    rangeValid_ = false;
    runningSince_ = 0;
    if (truePeakOn_) truePeak_.reset();
}

void LoudnessMeter::setRunning(bool running) noexcept {
    if (running && !running_)   // the sub-block under way holds paused audio
        runningSince_ = subBlocks_ + (samplesInSubBlock_ > 0 ? 1 : 0);
    running_ = running;
}

void LoudnessMeter::process(const float* const* channels, int frames) noexcept {
    run(channels, frames);
    if (truePeakOn_ && frames > 0) truePeak_.process(channels, this->channels(), frames);
}

void LoudnessMeter::process(const double* const* channels, int frames) noexcept {
    run(channels, frames);
    if (truePeakOn_ && frames > 0) truePeak_.process(channels, this->channels(), frames);
}

template <class Sample>
void LoudnessMeter::run(const Sample* const* channels, int frames) noexcept {
    const auto n = weights_.size();
    for (int f = 0; f < frames; ++f) {
        double weighted = 0.0;
        for (std::size_t c = 0; c < n; ++c) {
            // A non-finite sample reads as silence and a huge one is held
            // to +120 dBFS, so neither can poison the filters or overflow
            // the energy (N13 guards these too). The true peak guards its own.
            double x = static_cast<double>(channels[c][f]);
            if (!std::isfinite(x)) {
                x = 0.0;
            } else {
                x = std::clamp(x, -1.0e6, 1.0e6);
            }
            const Biquad& a = shelf_[c];
            State& s = shelfState_[c];
            const double y = a.b0 * x + a.b1 * s.x1 + a.b2 * s.x2 - a.a1 * s.y1 - a.a2 * s.y2;
            s.x2 = s.x1; s.x1 = x; s.y2 = s.y1; s.y1 = y;
            const Biquad& b = highPass_[c];
            State& h = highPassState_[c];
            const double z = b.b0 * y + b.b1 * h.x1 + b.b2 * h.x2 - b.a1 * h.y1 - b.a2 * h.y2;
            h.x2 = h.x1; h.x1 = y; h.y2 = h.y1; h.y1 = z;
            weighted += weights_[c] * z * z;
        }
        subBlockEnergy_ += weighted;
        if (++samplesInSubBlock_ >= subBlockLength_) endSubBlock();
    }
}

bool LoudnessMeter::counts(int window) const noexcept {
    return running_ && subBlocks_ >= window && subBlocks_ - runningSince_ >= window;
}

void LoudnessMeter::endSubBlock() noexcept {
    const auto slot = static_cast<std::size_t>(subBlocks_ % kRing);
    ringEnergy_[slot] = subBlockEnergy_;
    ringSamples_[slot] = samplesInSubBlock_;
    ++subBlocks_;
    subBlockEnergy_ = 0.0;
    samplesInSubBlock_ = 0;
    // The next boundary, so every rate gets exactly 100 sub-blocks a second.
    subBlockLength_ = std::llround(static_cast<double>(subBlocks_ + 1) * rate_ / 100.0) -
                      std::llround(static_cast<double>(subBlocks_) * rate_ / 100.0);
    auto window = [&](int length) {
        double energy = 0.0;
        std::int64_t samples = 0;
        for (int i = 1; i <= length; ++i) {
            const auto at = static_cast<std::size_t>((subBlocks_ - i) % kRing);
            energy += ringEnergy_[at];
            samples += ringSamples_[at];
        }
        return samples > 0 ? energy / static_cast<double>(samples) : 0.0;
    };
    if (subBlocks_ >= kMomentary) {
        momentary_ = loudness(window(kMomentary));
        if (counts(kMomentary)) maxMomentary_ = std::max(maxMomentary_, momentary_);
    }
    if (subBlocks_ >= kShortTerm) {
        shortTerm_ = loudness(window(kShortTerm));
        if (counts(kShortTerm)) maxShortTerm_ = std::max(maxShortTerm_, shortTerm_);
    }
    if (subBlocks_ % kHop == 0) gatingStep();
}

void LoudnessMeter::gatingStep() noexcept {
    if (!running_) return;
    if (counts(kMomentary) && momentary_ > kAbsoluteGate) {
        // The block's mean energy, recovered from its loudness exactly.
        integrated_.add(binOf(momentary_), std::pow(10.0, (momentary_ + 0.691) / 10.0));
    }
    if (counts(kShortTerm) && shortTerm_ > kAbsoluteGate) {
        range_.add(binOf(shortTerm_), std::pow(10.0, (shortTerm_ + 0.691) / 10.0));
    }
    // Integrated: the relative gate 10 LU under the absolute-gated loudness.
    if (integrated_.total > 0) {
        const double gate = loudness(integrated_.totalEnergy / static_cast<double>(integrated_.total)) - 10.0;
        const int bin = gate > kAbsoluteGate ? binOf(gate) : 0;
        const auto kept = integrated_.total - integrated_.countBelow(bin);
        const double energy = integrated_.totalEnergy - integrated_.energyBelow(bin);
        integratedValue_ = kept > 0 ? loudness(energy / static_cast<double>(kept)) : -kInfinity;
    }
    // Range (Tech 3342): relative gate 20 LU under the absolute-gated short-term
    // power mean; the 10th and 95th percentiles of what passes.
    if (range_.total > 0) {
        const double gate = loudness(range_.totalEnergy / static_cast<double>(range_.total)) - 20.0;
        const int bin = gate > kAbsoluteGate ? binOf(gate) : 0;
        const auto below = range_.countBelow(bin);
        const auto kept = range_.total - below;
        if (kept > 0) {
            const auto low = roundHalfAway(static_cast<double>(kept - 1) * 0.10 + 1.0);
            const auto high = roundHalfAway(static_cast<double>(kept - 1) * 0.95 + 1.0);
            rangeValue_ = valueOf(range_.find(below + high)) - valueOf(range_.find(below + low));
            rangeValid_ = true;
        }
    }
}

LoudnessMeter::Readings LoudnessMeter::readings() const noexcept {
    Readings r;
    r.momentary = momentary_;
    r.shortTerm = shortTerm_;
    r.integrated = integratedValue_;
    r.range = rangeValue_;
    r.rangeValid = rangeValid_;
    r.maxMomentary = maxMomentary_;
    r.maxShortTerm = maxShortTerm_;
    r.truePeak = truePeakOn_ ? truePeak_.peakDb() : -kInfinity;
    r.running = running_;
    return r;
}

}  // namespace adi::dsp
