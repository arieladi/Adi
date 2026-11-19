// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/adi_shaper_noise.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace adi::dsp {
namespace {
float unit(float x, float fallback) noexcept {
    return std::isfinite(x) ? std::clamp(x, 0.0f, 1.0f) : fallback;
}

float cubic(float a, float b, float c, float d, float t) noexcept {
    const float linear = c - a;
    const float quadratic = 2*a - 5*b + 4*c - d;
    const float third = 3*(b-c) + d - a;
    return b + .5f*t*(linear + t*(quadratic + t*third));
}

// Positive-frequency pole bank fitted to a pink spectrum with a 20 Hz low shelf.
// Output is calibrated independently to the white source's 48 kHz RMS.
constexpr std::array<float, 6> pinkAmount{.00256128f, .01062880f, .04456856f,
                                          .16670628f, .52330312f, .89767629f};
constexpr std::array<float, 6> pinkWeight{5.6832294f, 3.6607564f, 1.1484753f,
                                          .71050745f, .26862816f, .54947581f};
const std::array<float, 65> pitchTable = [] {
    std::array<float, 65> values{};
    for (std::size_t i = 0; i < values.size(); ++i)
        values[i] = std::exp2(static_cast<float>(i) / 16.0f - 2.0f);
    return values;
}();
} // namespace

void AdiShaperNoise::prepare(double sampleRate, std::uint32_t seed) noexcept {
    rate_ = std::isfinite(sampleRate) && sampleRate >= 1.0 ? sampleRate : 0.0;
    step_ = rate_ > 0 ? std::min(1.0, 44100.0 / rate_) : 0.0;
    random_ = seed != 0 ? seed : 0x41534931u;
    updateFilters();
    reset();
}

void AdiShaperNoise::reset() noexcept {
    phase_ = 0;
    previousPitch_ = -1;
    pinkState_ = {};
    highState_ = {};
    lowState_ = {};
    history_ = {};
    // Preserve the random stream across transport resets.
    if (rate_ > 0 && source_ != Source::Off)
        for (auto& sample : history_) sample = nextSource();
}

void AdiShaperNoise::setSource(Source source) noexcept {
    if (source != Source::White && source != Source::Pink) source = Source::Off;
    if (source_ != source) {
        source_ = source;
        reset();
    }
}

void AdiShaperNoise::setTrim(float normalized) noexcept {
    const float trim = unit(normalized, .8f);
    if (trim != trim_) {
        trim_ = trim;
        gain_ = std::pow(10.0f, (60.0f * trim - 48.0f) / 20.0f);
    }
}

void AdiShaperNoise::setFilters(float highPass, float lowPass) noexcept {
    const float high = unit(highPass, .254f), low = unit(lowPass, 1.0f);
    if (high != high_ || low != low_) {
        if (high == 0) highState_ = {};
        if (low == 1) lowState_ = {};
        high_ = high;
        low_ = low;
        updateFilters();
    }
}

void AdiShaperNoise::updateFilters() noexcept {
    if (rate_ <= 0) return;
    const auto coefficient = [this](float cutoff) {
        const double hz = 20.58 * std::exp2(10.0 * cutoff);
        const double g = std::tan(std::numbers::pi * std::min(.49, hz / rate_));
        return static_cast<float>(g / (1.0 + g));
    };
    highCoeff_ = coefficient(high_);
    lowCoeff_ = coefficient(low_);
}

float AdiShaperNoise::uniform() noexcept {
    random_ ^= random_ << 13;
    random_ ^= random_ >> 17;
    random_ ^= random_ << 5;
    return static_cast<float>(random_ >> 8) * (1.0f / 8388608.0f) - 1.0f;
}

AdiShaperNoise::Stereo AdiShaperNoise::nextSource() noexcept {
    // The polar transform emits a statistically independent stereo pair.
    float x, y, radius;
    do {
        x = uniform();
        y = uniform();
        radius = x*x + y*y;
    } while (radius >= 1.0f || radius <= 0.0f);
    const float gain = .02475f * std::sqrt(-2.0f * std::log(radius) / radius);
    Stereo sample{x*gain, y*gain};
    if (source_ == Source::Pink) {
        const Stereo input = sample;
        sample = {-.31305603f * input[0], -.31305603f * input[1]};
        for (std::size_t i = 0; i < pinkState_.size(); ++i) {
            for (std::size_t ch = 0; ch < 2; ++ch) {
                auto& memory = pinkState_[i][ch];
                memory += pinkAmount[i] * (input[ch] - memory);
                sample[ch] += pinkWeight[i] * memory;
            }
        }
    }
    return sample;
}

void AdiShaperNoise::filter(Stereo& sample) noexcept {
    // Two cascaded first-order sections give Q = 0.5 at the nominal corner.
    const auto cascade = [&sample](FilterState& stages, float coefficient, bool highPass) {
        for (auto& state : stages) {
            for (std::size_t ch = 0; ch < 2; ++ch) {
                const float delta = coefficient * (sample[ch] - state[ch]);
                const float low = state[ch] + delta;
                sample[ch] = highPass ? sample[ch] - low : low;
                state[ch] += 2 * delta;
            }
        }
    };
    if (high_ > 0) cascade(highState_, highCoeff_, true);
    if (low_ < 1) cascade(lowState_, lowCoeff_, false);
}

void AdiShaperNoise::processStereo(float& left, float& right, float levelHeight,
                                   float pitchHeight) noexcept {
    if (only_) left = right = 0;
    if (source_ == Source::Off || rate_ <= 0) return;

    Stereo sample = history_[1];
    if (phase_ != 0.0) {
        const float t = static_cast<float>(phase_);
        for (std::size_t ch = 0; ch < 2; ++ch)
            sample[ch] = cubic(history_[0][ch], history_[1][ch], history_[2][ch], history_[3][ch], t);
    }
    double increment = step_;
    if (source_ == Source::Pink) {
        const float pitch = unit(pitchHeight, .5f);
        if (pitch != previousPitch_) {
            const float position = pitch * 64.0f;
            const auto i = std::min(static_cast<std::size_t>(position), pitchTable.size()-2);
            pitchRate_ = std::lerp(pitchTable[i], pitchTable[i+1], position - static_cast<float>(i));
            previousPitch_ = pitch;
        }
        increment *= pitchRate_;
    }
    phase_ += increment;
    while (phase_ >= 1.0) {
        phase_ -= 1.0;
        history_[0] = history_[1];
        history_[1] = history_[2];
        history_[2] = history_[3];
        history_[3] = nextSource();
    }
    if (high_ > 0 || low_ < 1) filter(sample);
    const float level = unit(levelHeight, 0) * gain_;
    left += level * sample[0];
    right += level * sample[mono_ ? 0 : 1];
}

} // namespace adi::dsp
