// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/adi_shaper_sweep.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <new>
#include <numbers>
#include <stdexcept>

namespace adi::dsp {
namespace {
// The measured center spans ten octaves above E0.
constexpr double minimumHz = 20.601722307054366;
float clean(float sample) noexcept {
    return std::abs(sample) < 1.0e-26f ? 0.0f : sample;
}
// Measured flanger loop transfer, including its dry component. Outside the
// fitted region the nonlinear half reaches its limit continuously at ±3.
float softDrive(float sample) noexcept {
    if (std::abs(sample) > 3.0f)
        return 0.5f * sample + std::copysign(0.5f, sample);
    const float square = sample * sample;
    return sample * ((27.0f + 5.0f * square) / (27.0f + 9.0f * square));
}
float height(float input) noexcept {
    return std::isfinite(input) ? std::clamp(input, 0.0f, 1.0f) : 0.5f;
}
} // namespace

bool AdiShaperSweep::prepare(double sampleRate) noexcept {
    ready_ = false;
    reset();
    // Well above professional audio rates, while keeping a corrupt device
    // report from requesting a multi-gigabyte audio buffer.
    if (!std::isfinite(sampleRate) || sampleRate < 1 || sampleRate > 1536000) return false;
    const auto needed = static_cast<std::size_t>(std::ceil(sampleRate / minimumHz)) + 3;
    try {
        delay_.resize(std::bit_ceil(needed));
    } catch (const std::bad_alloc&) {
        return false;
    } catch (const std::length_error&) {
        return false;
    }
    mask_ = delay_.size() - 1;
    highpassAmount_ = static_cast<float>(-std::expm1(-2 * std::numbers::pi * 100 / sampleRate));
    for (std::size_t i = 0; i <= intervals; ++i) {
        const double hz = minimumHz * std::exp2(10.0 * static_cast<double>(i) / intervals);
        const double g = std::tan(std::numbers::pi * std::min(hz / sampleRate, .495));
        tuning_[i] = {static_cast<float>(std::max(1.0, sampleRate / hz)),
                      static_cast<float>((1.0 - g) / (1.0 + g))};
    }
    // With spread the phaser extends the center axis by five octaves at
    // either end. Keep the centered table's original range and arithmetic.
    for (std::size_t i = 0; i < spreadPoles_.size(); ++i) {
        const double position = static_cast<double>(i) / intervals - .5;
        const double hz = minimumHz * std::exp2(10.0 * position);
        const double g = std::tan(std::numbers::pi * std::min(hz / sampleRate, .4999));
        spreadPoles_[i] = static_cast<float>((1.0 - g) / (1.0 + g));
    }
    center_ = feedback_ = -1;
    ready_ = true;
    return true;
}

void AdiShaperSweep::reset() noexcept {
    write_ = 0;
    valid_ = {};
    delayFault_ = {};
    low_ = {};
    states_ = {};
}

void AdiShaperSweep::setMode(Mode mode) noexcept {
    if (mode == mode_) return;
    switch (mode) {
        case Mode::Off:
        case Mode::Flange: stages_ = 0; break;
        case Mode::Phaser4: stages_ = 4; break;
        case Mode::Phaser8: stages_ = 8; break;
        case Mode::Phaser12: stages_ = 12; break;
        case Mode::Phaser16: stages_ = 16; break;
        default: mode = Mode::Off; stages_ = 0; break;
    }
    mode_ = mode;
    reset();
    center_ = feedback_ = -1;
}

void AdiShaperSweep::setPolarity(bool negative) noexcept {
    if (negative_ != negative) {
        negative_ = negative;
        feedback_ = -1;
    }
}

void AdiShaperSweep::setStereoOffset(float bipolar) noexcept {
    if (!std::isfinite(bipolar)) return;
    bipolar = std::clamp(bipolar, -1.f, 1.f);
    const float offset = .5f * bipolar * std::abs(bipolar);
    if (offset != offset_) {
        offset_ = offset;
        center_ = std::numeric_limits<float>::quiet_NaN();
    }
}

void AdiShaperSweep::updateCenter(float value) noexcept {
    center_ = height(value);
    if (offset_ == 0.f) {
        updateChannel(channels_[0], center_);
    } else {
        updateChannel(channels_[0], center_ + offset_);
        updateChannel(channels_[1], center_ - offset_);
    }
}

void AdiShaperSweep::updateChannel(ChannelTuning& channel, float center) const noexcept {
    if (stages_ != 0 && offset_ != 0.f) {
        // Separate the signed integer part before indexing the extended
        // table to retain the centered table's fractional resolution.
        const float position = center * intervals;
        const int lower = std::min(static_cast<int>(std::floor(position)), static_cast<int>(intervals * 3 / 2) - 1);
        const auto index = static_cast<std::size_t>(lower + static_cast<int>(intervals / 2));
        const float fraction = position - static_cast<float>(lower);
        channel.pole = spreadPoles_[index] + (spreadPoles_[index + 1] - spreadPoles_[index]) * fraction;
    } else {
        const float position = std::clamp(center, 0.f, 1.f) * intervals;
        const auto lower = std::min(static_cast<std::size_t>(position), intervals - 1);
        const float fraction = position - static_cast<float>(lower);
        const auto& a = tuning_[lower];
        const auto& b = tuning_[lower + 1];
        if (stages_ == 0) {
            const float frames = a.delay + (b.delay - a.delay) * fraction;
            channel.delayFrames = static_cast<std::size_t>(frames);
            channel.delayFraction = frames - static_cast<float>(channel.delayFrames);
            return;
        }
        channel.pole = a.pole + (b.pole - a.pole) * fraction;
    }
    const float square = channel.pole * channel.pole;
    const float four = square * square, eight = four * four;
    channel.cascadeGain = stages_ == 4 ? four : stages_ == 8 ? eight :
                          stages_ == 12 ? four * eight : eight * eight;
    channel.inverse = 1.0f / (1.0f + loop_ * channel.cascadeGain);
}

void AdiShaperSweep::updateFeedback(float value) noexcept {
    feedback_ = height(value);
    if (stages_ == 0) {
        const float amount = 0.99f * std::sqrt(feedback_);
        dry_ = 0.75f / (1.0f + 0.75f * amount);
        const float sign = negative_ ? -1.0f : 1.0f;
        const float highpassGain = 1.0f - 0.5f * highpassAmount_;
        wet_ = sign * highpassGain * (1.0f + amount) * dry_;
        loop_ = sign * highpassGain * amount;
    } else {
        if (negative_) {
            dry_ = 0.5f;
            loop_ = 0.55f * (1.0f - feedback_);
            wet_ = 0.5f * (1.0f + loop_ * loop_);
        } else {
            dry_ = 0.0f;
            loop_ = 0.99f * feedback_;
            wet_ = 1.0f - 0.5f * loop_ * loop_;
        }
        channels_[0].inverse = 1.0f / (1.0f + loop_ * channels_[0].cascadeGain);
        if (offset_ != 0.f) channels_[1].inverse = 1.0f / (1.0f + loop_ * channels_[1].cascadeGain);
    }
}

void AdiShaperSweep::flange(float& left, float& right) noexcept {
    // Old ring samples are inaccessible after a reset, so switching modes
    // takes bounded time even on a very high sample-rate stream.
    const Stereo input{left, right};
    Stereo output;
    for (std::size_t ch = 0; ch < 2; ++ch) {
        const auto& tuning = channels_[offset_ != 0.f ? ch : 0];
        auto& valid = valid_[ch];
        const float first = valid >= tuning.delayFrames ? delay_[(write_ - tuning.delayFrames) & mask_][ch] : 0.0f;
        const float second = valid > tuning.delayFrames ? delay_[(write_ - tuning.delayFrames - 1) & mask_][ch] : 0.0f;
        const float delayed = first + (second - first) * tuning.delayFraction;
        const float filtered = delayed - low_[ch];
        low_[ch] = clean(low_[ch] + highpassAmount_ * filtered);
        const float next = softDrive(input[ch] + loop_ * filtered);
        if (std::isfinite(next)) {
            delay_[write_][ch] = clean(next);
        } else {
            delay_[write_][ch] = 0.0f;
            delayFault_[ch] = true;
        }
        output[ch] = dry_ * input[ch] + wet_ * filtered;
        if (valid < mask_) ++valid;
    }
    left = output[0];
    right = output[1];
    write_ = (write_ + 1) & mask_;
}

void AdiShaperSweep::phase(float& left, float& right) noexcept {
    const auto& leftTuning = channels_[0];
    const auto& rightTuning = channels_[offset_ != 0.f ? 1 : 0];
    const Stereo poles{leftTuning.pole, rightTuning.pole};
    Stereo response{};
    if (loop_ != 0.0f) {
        // Evaluate the cascade's stored response to zero input. Its immediate
        // input gain is pole^stages, so the feedback loop can be solved at this
        // sample without inserting an extra sample of delay.
        for (unsigned i = 0; i < stages_; ++i) {
            response[0] = poles[0] * response[0] + states_[i][0];
            response[1] = poles[1] * response[1] + states_[i][1];
        }
    }
    Stereo work{(left - loop_ * response[0]) * leftTuning.inverse,
                (right - loop_ * response[1]) * rightTuning.inverse};
    for (unsigned i = 0; i < stages_; ++i) {
        for (std::size_t ch = 0; ch < 2; ++ch) {
            const float next = poles[ch] * work[ch] + states_[i][ch];
            states_[i][ch] = clean(poles[ch] * next - work[ch]);
            work[ch] = next;
        }
    }
    left = dry_ * left + wet_ * work[0];
    right = dry_ * right + wet_ * work[1];
}

void AdiShaperSweep::processStereo(float& left, float& right, float centerHeight,
                                    float feedbackHeight) noexcept {
    if (!ready_ || mode_ == Mode::Off) return;
    if (centerHeight != center_) updateCenter(centerHeight);
    if (feedbackHeight != feedback_) updateFeedback(feedbackHeight);
    if (stages_ == 0) flange(left, right);
    else phase(left, right);
}

bool AdiShaperSweep::finishBlock() noexcept {
    bool repaired = false;
    for (std::size_t ch = 0; ch < low_.size(); ++ch) {
        bool bad = delayFault_[ch] || !std::isfinite(low_[ch]);
        for (unsigned i = 0; i < stages_; ++i)
            bad |= !std::isfinite(states_[i][ch]);
        if (bad) {
            // Invalidate only the affected delay lane without walking its ring.
            valid_[ch] = 0;
            delayFault_[ch] = false;
            low_[ch] = 0.0f;
            for (auto& state : states_) state[ch] = 0.0f;
            repaired = true;
        }
    }
    return repaired;
}

} // namespace adi::dsp
