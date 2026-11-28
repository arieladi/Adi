// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/auto_shift.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace adi::dsp {

namespace {

/// The longest period the detector can read: its lowest pitch less the 5 %
/// it accepts below it.
double longestAt(double rate) noexcept { return std::ceil(rate / (0.95 * PitchDetector::kLowestHz)); }

}  // namespace

AutoShift::AutoShift() {
    for (std::size_t i = 0; i < Count; ++i) {
        p_[i] = ranges[i].initial;
    }
    prepare(48000);
}

std::size_t AutoShift::latencyFor(double sampleRate, std::size_t window) noexcept {
    return window / 2 + window / 8 + kChunk + static_cast<std::size_t>(std::ceil(1.625 * longestAt(sampleRate))) + 8;
}

void AutoShift::prepare(double sampleRate) noexcept {
    rate_ = std::isfinite(sampleRate) && sampleRate > 0 ? sampleRate : 48000;
    ready_ = false;
    size_ = 0;
    if (rate_ > kHighestRate) {   // past any host's: inert, its input out
        return;
    }
    decimation_ = std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(rate_ / 48000 - 1e-9)));
    detector_.prepare(rate_ / static_cast<double>(decimation_));
    if (detector_.window() == 0) {
        return;
    }
    longest_ = longestAt(rate_);
    latency_ = latencyFor(rate_, window());
    std::size_t size = 1;
    while (size < 2 * latency_ + 16) {
        size *= 2;
    }
    try {
        for (std::size_t c = 0; c < 2; ++c) {
            in_[c].assign(size, 0.f);
            for (auto& copy : low_) {
                copy[c].assign(size, 0.f);
            }
        }
        size_ = size;
        ready_ = true;
    } catch (...) {
        size_ = 0;
    }
    mix_.prepare(rate_);
    mix_.set(p_[DryWet] / 100);
    target_ = current_ = sum_ = 0;
    period_ = rate_ / 200;
    clock_ = pushed_ = head_ = count_ = fade_ = fadeLength_ = gathered_ = filled_ = 0;
    tap_ = old_ = -static_cast<double>(latency_);
    shortest_ = farthest_ = static_cast<double>(latency_);
    filter_ = {};
    design();
    switches_ = 0;
    retune();
    speed_ = ratio_;
    band_ = oldBand_ = -1;
    band_ = oldBand_ = band();
}

void AutoShift::set(Param p, double v) noexcept {
    const int index = static_cast<int>(p);
    if (index < 0 || index >= static_cast<int>(Count) || !std::isfinite(v)) {
        return;
    }
    const auto& r = ranges[static_cast<std::size_t>(index)];
    const bool whole = index == Correction || index == Root || index == Scale;
    p_[static_cast<std::size_t>(index)] = std::clamp(whole ? std::round(v) : v, r.low, r.high);
    if (index == DryWet) {
        mix_.set(p_[DryWet] / 100);
    }
    retune();
}

double AutoShift::nearest(double note, int root, int scale) noexcept {
    static constexpr std::array<std::array<bool, 12>, 3> kIn = {{
        {true, true, true, true, true, true, true, true, true, true, true, true},
        {true, false, true, false, true, true, false, true, false, true, false, true},
        {true, false, true, true, false, true, false, true, true, false, true, false},
    }};
    const auto& in = kIn[static_cast<std::size_t>(std::clamp(scale, 0, 2))];
    const double centre = std::round(note);
    double best = centre, distance = 1e9;
    for (int k = -6; k <= 6; ++k) {
        const double candidate = centre + k;
        const auto degree = static_cast<std::size_t>(((static_cast<long long>(candidate) - root) % 12 + 12) % 12);
        const double d = std::abs(candidate - note);
        if (in[degree] && d < distance) {   // the lower of two equally near notes wins
            best = candidate;
            distance = d;
        }
    }
    return best;
}

double AutoShift::shift() const noexcept { return p_[Shift] + (p_[Detune] + current_) / 100; }

void AutoShift::retune() noexcept {
    const double semitones = shift();
    ratio_ = std::abs(semitones) < 0.002 ? 1.0 : std::clamp(std::exp2(semitones / 12), 1 / kHighestRatio, kHighestRatio);
}

void AutoShift::design() noexcept {
    // Each band's eighth-order Butterworth (four sections) at 0.45 of the
    // rate over the band's top ratio: what a tap reading that fast can hold.
    // Fixed: designed once here, never retuned while it runs.
    constexpr std::array<double, 4> kQ = {0.50979557910415918, 0.60134488693504529, 0.89997622313641570, 2.5629154477415055};
    for (std::size_t k = 0; k < kBands; ++k) {
        const double w = 2 * std::numbers::pi * 0.45 / kTops[k], cs = std::cos(w), sn = std::sin(w);
        for (std::size_t s = 0; s < kQ.size(); ++s) {
            const double alpha = sn / (2 * kQ[s]), a0 = 1 + alpha;
            for (std::size_t c = 0; c < 2; ++c) {
                Biquad& f = filter_[k][c][s];
                f.b0 = f.b2 = (1 - cs) / 2 / a0;
                f.b1 = (1 - cs) / a0;
                f.a1 = -2 * cs / a0;
                f.a2 = (1 - alpha) / a0;
            }
        }
    }
}

int AutoShift::band() const noexcept {
    int want = -1;
    if (ratio_ > 1.05) {
        want = 0;
        while (want < static_cast<int>(kBands) - 1 && kTops[static_cast<std::size_t>(want)] < ratio_) {
            ++want;
        }
    }
    if (want < band_) {   // down only once 2 % under the current band's floor
        const double floor = band_ == 0 ? 1.05 : kTops[static_cast<std::size_t>(band_ - 1)];
        if (ratio_ > 0.98 * floor) {
            return band_;
        }
    }
    return want;
}

double AutoShift::read(const std::vector<float>& ring, double at) const noexcept {
    // Four-point Hermite, at an absolute (fractional) sample position; a
    // negative one wraps on the power-of-two ring.
    const double whole = std::floor(at), t = at - whole;
    const auto i = static_cast<long long>(whole);
    const auto get = [&](long long k) { return static_cast<double>(ring[static_cast<std::size_t>(k) & (size_ - 1)]); };
    const double y0 = get(i - 1), y1 = get(i), y2 = get(i + 1), y3 = get(i + 2);
    const double c1 = 0.5 * (y2 - y0), c2 = y0 - 2.5 * y1 + 2 * y2 - 0.5 * y3, c3 = 0.5 * (y3 - y0) + 1.5 * (y1 - y2);
    return ((c3 * t + c2) * t + c1) * t + y1;
}

void AutoShift::adopt(const Reading& r) noexcept {
    if (!r.clear) {
        return;   // an unclear reading holds the last period and correction
    }
    period_ = std::clamp(rate_ / r.hz, rate_ / PitchDetector::kHighestHz, longest_);
    if (p_[Correction] >= 0.5) {
        const double note = 69 + 12 * std::log2(r.hz / p_[Reference]);
        const double goal = nearest(note, static_cast<int>(p_[Root]), static_cast<int>(p_[Scale]));
        target_ = p_[Strength] / 100 * 100 * (goal - note);
    }
}

void AutoShift::process(const float* inL, const float* inR, float* outL, float* outR, std::size_t frames) noexcept {
    const std::size_t window = detector_.window(), hop = detector_.hop(), wide = window * decimation_;
    const double reach = static_cast<double>(hop * decimation_) / 2, centre = static_cast<double>(latency_);
    for (std::size_t i = 0; i < frames; ++i) {
        // Both inputs are read, and made finite and bounded, before anything
        // is written (a host may hand an output plane that is an input plane).
        const auto bounded = [&](const float* in) { return in && std::isfinite(in[i]) ? std::clamp(in[i], -1e6f, 1e6f) : 0.f; };
        const float x[2] = {bounded(inL), bounded(inR)};
        if (!ready_) {
            if (outL) {
                outL[i] = x[0];
            }
            if (outR) {
                outR[i] = x[1];
            }
            continue;
        }
        // Every 64th sample: the correction glides, the shift is set again.
        if (clock_ % kChunk == 0) {
            if (p_[Correction] < 0.5) {
                target_ = 0;
            }
            const double glide = p_[Smooth] <= 0 ? 1.0 : 1 - std::exp(-static_cast<double>(kChunk) / (p_[Smooth] / 1000 * rate_));
            current_ += (target_ - current_) * glide;
            if (std::abs(target_ - current_) < 1e-9) {
                current_ = target_;
            }
            retune();
        }
        const std::size_t slot = clock_ & (size_ - 1);
        for (std::size_t c = 0; c < 2; ++c) {
            in_[c][slot] = x[c];
            for (std::size_t k = 0; k < kBands; ++k) {
                double v = x[c];
                for (auto& section : filter_[k][c]) {
                    v = section.run(v);
                }
                low_[k][c][slot] = static_cast<float>(v);
            }
        }
        // The detector, at the rate over the decimation, a chunk at a time;
        // each new reading is queued with the middle of its window.
        sum_ += 0.5 * (static_cast<double>(x[0]) + x[1]);
        if (++gathered_ == decimation_) {
            mono_[filled_] = static_cast<float>(sum_ / static_cast<double>(decimation_));
            sum_ = 0;
            gathered_ = 0;
            if (++filled_ == kChunk) {
                filled_ = 0;
                detector_.push(mono_.data(), kChunk);
                pushed_ += kChunk;
                if (pushed_ >= window && pushed_ % hop == 0) {
                    Reading r;
                    r.centre = static_cast<double>(clock_) - static_cast<double>(wide - 1) / 2;
                    double clarity = 0;
                    r.clear = detector_.estimate(r.hz, clarity) && clarity >= kClarity;
                    if (count_ == kQueue) {   // never at this latency; bounded all the same
                        adopt(queue_[head_]);
                        head_ = (head_ + 1) % kQueue;
                        --count_;
                    }
                    queue_[(head_ + count_) % kQueue] = r;
                    ++count_;
                }
            }
        }
        // The readings the tap has reached.
        while (count_ > 0 && queue_[head_].centre <= tap_ + reach) {
            adopt(queue_[head_]);
            head_ = (head_ + 1) % kQueue;
            --count_;
        }
        // Between crossfades: the ratio is taken up; the tap splices a whole
        // number of periods back toward the centre once it has drifted half
        // a period, far enough that the next is not due before this one's
        // fade ends, and moves to the low-passed copy (or off it) when the
        // ratio crosses 1, in the same fade (a splice due before such a fade
        // ends is made with it). At a ratio of exactly 1, what is left of a
        // period is glided away at 0.1 % speed (1.7 cents, under hearing).
        if (fade_ == 0) {
            speed_ = ratio_;
            const double p = period_, delay = static_cast<double>(clock_) - tap_;
            const double length = std::clamp(std::round(p / 4), 16.0, std::ceil(longest_ / 4));
            const int want = band();
            const bool switching = want != band_;
            const double ahead = switching ? std::abs(speed_ - 1) * length : 0.0;
            double by = 0;
            if (speed_ > 1 && delay - ahead < centre - p / 2) {
                by = -std::ceil((centre - p / 2 + (speed_ - 1) * length - delay) / p) * p;
            } else if (speed_ < 1 && delay + ahead > centre + p / 2) {
                by = std::ceil((delay - (centre + p / 2 - (1 - speed_) * length)) / p) * p;
            } else if (speed_ == 1 && std::abs(delay - centre) > p / 2) {
                by = std::round((delay - centre) / p) * p;
            } else if (speed_ == 1 && delay != centre && !switching) {
                const double off = delay - centre;
                if (std::abs(off) < 1e-6) {
                    tap_ = static_cast<double>(clock_) - centre;
                } else {
                    speed_ = 1 + std::clamp(off, -0.001, 0.001);
                }
            }
            if (by != 0 || switching) {
                old_ = tap_;
                oldBand_ = band_;
                band_ = want;
                switches_ += switching ? 1 : 0;
                tap_ += by;
                fadeLength_ = fade_ = static_cast<std::size_t>(length);
            }
        }
        const double now = static_cast<double>(clock_);
        shortest_ = std::min(shortest_, now - tap_);
        farthest_ = std::max(farthest_, now - tap_);
        if (fade_ > 0) {
            shortest_ = std::min(shortest_, now - old_);
            farthest_ = std::max(farthest_, now - old_);
        }
        const double mix = mix_.next();
        const double fadeIn = fade_ == 0 ? 1.0
                                         : 0.5 - 0.5 * std::cos(std::numbers::pi * static_cast<double>(fadeLength_ - fade_ + 1) /
                                                                 static_cast<double>(fadeLength_ + 1));
        for (std::size_t c = 0; c < 2; ++c) {
            double wet = read(band_ < 0 ? in_[c] : low_[static_cast<std::size_t>(band_)][c], tap_);
            if (fade_ > 0) {
                wet = fadeIn * wet + (1 - fadeIn) * read(oldBand_ < 0 ? in_[c] : low_[static_cast<std::size_t>(oldBand_)][c], old_);
            }
            const double dry = in_[c][(clock_ + size_ - latency_) & (size_ - 1)];
            float* out = c == 0 ? outL : outR;
            if (out) {
                out[i] = static_cast<float>(dry + (wet - dry) * mix);
            }
        }
        tap_ += speed_;
        if (fade_ > 0) {
            old_ += speed_;
            --fade_;
        }
        ++clock_;
    }
}

}  // namespace adi::dsp
