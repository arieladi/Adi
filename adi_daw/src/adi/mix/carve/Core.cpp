// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Core.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace adi::mix::carve {
namespace {
constexpr double pi = 3.14159265358979323846;
constexpr float quietEnter = 1.e-8f, quietExit = 1.e-7f;
constexpr float settledReduction = 1.e-6f;
void clearSmall(float &value) noexcept {
    if (value >= -1.e-8f && value <= 1.e-8f)
        value = 0;
}
template <class T> bool within(T value, T low, T high) noexcept {
    return std::isfinite(value) && value >= low && value <= high;
}
} // namespace

bool valid(const Settings &s) noexcept {
    return within(s.amount, 0.f, 1.f) && within(s.pan, 0.f, 1.f) &&
           within(s.attackMs, .01f, 100.f) && within(s.releaseMs, 1.f, 700.f) &&
           within(s.lowHz, 20., 20000.) && within(s.highHz, 20., 20000.) &&
           (s.mode == Mode::Stereo || s.mode == Mode::MidSide);
}

float Core::Memory::tick(float input, const Coefficients &c) noexcept {
    const float output = input * c.b0 + first;
    first = (input * c.b1 + second) - output * c.a1;
    second = input * c.b2 - output * c.a2;
    return output;
}
void Core::Memory::clearTiny() noexcept {
    clearSmall(first);
    clearSmall(second);
}

// Bilinear transform of complementary second-order band-pass/notch sections.
Core::Coefficients Core::design(float frequency, float q, double rate, bool notch) noexcept {
    const double reciprocalTangent = 1. / std::tan(static_cast<double>(frequency) * pi / rate);
    const double square = reciprocalTangent * reciprocalTangent;
    const double damping = notch ? reciprocalTangent / static_cast<double>(q)
                                 : (1. / static_cast<double>(q)) * reciprocalTangent;
    const double scale = 1. / (damping + 1. + square);
    Coefficients result;
    result.a1 = static_cast<float>((scale + scale) * (1. - square));
    result.a2 = static_cast<float>(((1. - damping) + square) * scale);
    if (notch) {
        result.b0 = result.b2 = static_cast<float>((square + 1.) * scale);
        result.b1 = result.a1;
    } else {
        const double numerator = (scale * reciprocalTangent) / static_cast<double>(q);
        result.b0 = static_cast<float>(numerator);
        result.b2 = static_cast<float>(-numerator);
    }
    return result;
}

void Core::admit(const Settings &s) noexcept {
    if (!coefficientsValid_ || s.lowHz != settings_.lowHz || s.highHz != settings_.highHz) {
        const double upper = std::max(s.highHz, s.lowHz + 1.);
        const double spacing = std::pow(10., std::log10(upper / s.lowHz) / 31.);
        const float q = static_cast<float>((1. - (upper - s.lowHz) / 19980.) * .5 * 8. + 1.414);
        double frequency = s.lowHz;
        for (auto &band : bank_) {
            const float center = static_cast<float>(frequency);
            frequency *= spacing;
            band.detector = design(center, q, spec_.sampleRate, false);
            const float programmeCenter = std::clamp(center, 1.f, static_cast<float>(spec_.sampleRate * .5));
            band.bandpass = design(programmeCenter, q, spec_.sampleRate, false);
            band.notch = design(programmeCenter, q, spec_.sampleRate, true);
        }
    }
    if (!coefficientsValid_ || s.attackMs != settings_.attackMs || s.releaseMs != settings_.releaseMs) {
        const float timeScale = ((1.f / static_cast<float>(spec_.sampleRate)) * -6.2831855f) * 1000.f;
        attack_ = std::exp(timeScale / (s.attackMs * 10.f));
        release_ = std::exp(timeScale / (s.releaseMs * 10.f));
    }
    if (!coefficientsValid_ || s.amount != settings_.amount)
        amount_ = static_cast<float>(std::pow(s.amount, 2.f)) * 300.f;
    if (coefficientsValid_ && s.audition != settings_.audition) {
        // Monitoring owns only the detector memories; programme memories pause.
        quietFrames_ = 0;
        keyPresent_ = false;
        normalSleeping_ = false;
        auditionSleeping_ = false;
    }
    settings_ = s;
    coefficientsValid_ = true;
}

void Core::clearProgrammeState() noexcept {
    for (auto &band : bank_) {
        band.pass = {};
        band.reject = {};
    }
    programmeCleared_ = true;
}
void Core::clearSignalState(bool preserveHeld) noexcept {
    clearProgrammeState();
    for (auto &band : bank_) {
        band.detection = {};
        band.envelope = band.smoothing = 0;
        if (!preserveHeld)
            band.held = 0;
    }
}

void Core::prepare(PrepareSpec spec, const Settings &s) {
    if (!within(spec.sampleRate, 44100., 384000.) || spec.maximumFrames <= 0 ||
        (spec.channels != 1 && spec.channels != 2) || !valid(s))
        throw std::invalid_argument("Invalid Carve preparation");
    spec_ = spec;
    prepared_ = true;
    coefficientsValid_ = false;
    const float tangent = static_cast<float>(std::tan(250. * pi / spec.sampleRate));
    smoothing_ = tangent / (tangent + 1.f);
    blendStep_ = 1.f / static_cast<float>(std::max(1., std::round(spec.sampleRate * .005)));
    quietLimit_ = static_cast<std::int32_t>(std::max(1., std::round(spec.sampleRate * .020)));
    reset(s);
}
void Core::reset(const Settings &s) noexcept {
    if (!prepared_ || !valid(s))
        return;
    admit(s);
    clearSignalState();
    quietFrames_ = 0;
    keyPresent_ = false;
    firstActivation_ = true;
    blend_ = 0;
    normalSleeping_ = auditionSleeping_ = true;
    activity_ = Activity::Sleeping;
}

void Core::observeKey(float magnitude) noexcept {
    if (magnitude > quietExit) {
        keyPresent_ = true;
        quietFrames_ = 0;
    } else if (magnitude < quietEnter) {
        if (quietFrames_ < quietLimit_)
            ++quietFrames_;
        if (quietFrames_ >= quietLimit_)
            keyPresent_ = false;
    } else {
        quietFrames_ = 0;
    }
}

float Core::detect(float key, std::array<float, bands> &envelopes) noexcept {
    float maximum = 0;
    for (std::size_t i = 0; i < bands; ++i) {
        auto &band = bank_[i];
        const float magnitude = std::abs(band.detection[0].tick(key, band.detector));
        const float coefficient = magnitude > band.envelope ? attack_ : release_;
        band.envelope = (band.envelope - magnitude) * coefficient + magnitude;
        const float delta = (band.envelope - band.smoothing) * smoothing_;
        envelopes[i] = delta + band.smoothing;
        band.smoothing = delta + envelopes[i];
        maximum = std::max(maximum, envelopes[i]);
    }
    return maximum;
}

float Core::programme(float input, std::size_t channel, float weight,
                      const std::array<float, bands> &envelopes, bool frozen) noexcept {
    for (std::size_t i = 0; i < bands; ++i) {
        auto &band = bank_[i];
        const float envelope = frozen ? band.held : envelopes[i];
        const float gain = 1.f - std::min(envelope * weight, 1.f);
        const float passed = band.pass[channel].tick(gain * input, band.bandpass);
        const float rejected = band.reject[channel].tick(input, band.notch);
        input = rejected + passed;
    }
    return input;
}

void Core::finish(bool monitoring, const std::array<float, bands> &envelopes) noexcept {
    for (std::size_t i = 0; i < bands; ++i) {
        auto &band = bank_[i];
        band.detection[0].clearTiny();
        if (monitoring) {
            band.detection[1].clearTiny();
            continue;
        }
        clearSmall(band.envelope);
        clearSmall(band.smoothing);
        for (std::int32_t c = 0; c < spec_.channels; ++c) {
            band.pass[static_cast<std::size_t>(c)].clearTiny();
            band.reject[static_cast<std::size_t>(c)].clearTiny();
        }
        if (!settings_.freeze)
            band.held = envelopes[i];
    }
}

template <class Sample>
bool Core::processImpl(Sample *const *main, const Sample *const *key, std::int32_t keyChannels,
                       std::int32_t frames, const Settings &s) noexcept {
    if (!prepared_ || !valid(s) || frames < 0 || frames > spec_.maximumFrames ||
        keyChannels < 0 || keyChannels > 2)
        return false;
    if (frames == 0)
        return true;
    if (!main)
        return false;
    for (std::int32_t c = 0; c < spec_.channels; ++c)
        if (!main[c])
            return false;
    if (!s.enabled)
        return true;
    if (keyChannels > 0) {
        if (!key)
            return false;
        for (std::int32_t c = 0; c < keyChannels; ++c)
            if (!key[c])
                return false;
    }
    admit(s);
    // A zero crossing at the beginning of an otherwise active first block is
    // not an idle period. Admit its programme history from the block boundary.
    if (firstActivation_ && !s.audition) {
        for (std::int32_t i = 0; i < frames && keyChannels > 0; ++i) {
            const float left = static_cast<float>(key[0][i]);
            const float folded = keyChannels == 2
                                     ? (left + static_cast<float>(key[1][i])) * .125f
                                     : left * .25f;
            if (std::abs(folded) > quietExit) {
                firstActivation_ = false;
                keyPresent_ = true;
                normalSleeping_ = false;
                blend_ = amount_ > 0 ? 1.f : 0.f;
                break;
            }
        }
    }
    std::array<float, bands> envelopes{};
    float heldMaximum = 0;
    for (const auto &band : bank_)
        heldMaximum = std::max(heldMaximum, band.held);
    const float leftWeight = (amount_ + amount_) * (1.f - s.pan);
    const float rightWeight = (amount_ + amount_) * s.pan;
    const float largestWeight = std::max(leftWeight, rightWeight);
    const bool heldCurve = s.freeze && heldMaximum > 0 && amount_ > 0;
    for (std::int32_t frame = 0; frame < frames; ++frame) {
        const float keyLeft = keyChannels ? static_cast<float>(key[0][frame]) : 0.f;
        const float keyRight = keyChannels == 2 ? static_cast<float>(key[1][frame]) : keyLeft;
        const float folded = keyChannels == 2 ? (keyLeft + keyRight) * .125f : keyLeft * .25f;
        observeKey(s.audition ? std::max(std::abs(keyLeft), std::abs(keyRight)) * .25f : std::abs(folded));
        if (s.audition) {
            if (auditionSleeping_ && !keyPresent_) {
                for (std::int32_t c = 0; c < spec_.channels; ++c)
                    main[c][frame] = Sample(0);
                activity_ = Activity::Sleeping;
                continue;
            }
            auditionSleeping_ = false;
            float residual = 0;
            for (std::int32_t c = 0; c < spec_.channels; ++c) {
                const auto channel = static_cast<std::size_t>(c);
                const float source = (c == 0 ? keyLeft : keyRight) * .25f;
                float sum = 0;
                for (auto &band : bank_) {
                    sum += band.detection[channel].tick(source, band.detector);
                    residual = std::max({residual, std::abs(band.detection[channel].first),
                                         std::abs(band.detection[channel].second)});
                }
                main[c][frame] = static_cast<Sample>(sum);
            }
            if (!keyPresent_ && residual <= quietEnter) {
                for (auto &band : bank_)
                    band.detection = {};
                auditionSleeping_ = true;
            }
            activity_ = auditionSleeping_ ? Activity::Sleeping : keyPresent_ ? Activity::Active : Activity::Releasing;
            continue;
        }
        if (normalSleeping_ && !keyPresent_ && !heldCurve)
            continue;
        normalSleeping_ = false;
        const float maximum = detect(folded, envelopes);
        const bool demand = heldCurve || (amount_ > 0 && (keyPresent_ || maximum * largestWeight > settledReduction));
        if (demand) {
            if (firstActivation_) {
                blend_ = 1;
                firstActivation_ = false;
            } else {
                blend_ = std::min(1.f, blend_ + blendStep_);
            }
        } else {
            blend_ = std::max(0.f, blend_ - blendStep_);
        }
        if (blend_ > 0) {
            const Sample originalLeft = main[0][frame];
            const Sample originalRight = spec_.channels == 2 ? main[1][frame] : originalLeft;
            const float left = static_cast<float>(originalLeft), right = static_cast<float>(originalRight);
            const bool midSide = s.mode == Mode::MidSide && spec_.channels == 2;
            float a = programme(midSide ? (left + right) * .5f : left, 0, leftWeight, envelopes, s.freeze);
            float b = spec_.channels == 2
                          ? programme(midSide ? (left - right) * .5f : right, 1, rightWeight, envelopes, s.freeze)
                          : 0;
            if (midSide) {
                const float mid = a;
                a = mid + b;
                b = mid - b;
            }
            if (blend_ == 1.f) {
                main[0][frame] = static_cast<Sample>(a);
                if (spec_.channels == 2)
                    main[1][frame] = static_cast<Sample>(b);
            } else {
                main[0][frame] = originalLeft + static_cast<Sample>(blend_) * (static_cast<Sample>(a) - originalLeft);
                if (spec_.channels == 2)
                    main[1][frame] = originalRight + static_cast<Sample>(blend_) * (static_cast<Sample>(b) - originalRight);
            }
            programmeCleared_ = false;
        } else if (!programmeCleared_) {
            clearProgrammeState();
        }
        if (!keyPresent_ && !heldCurve && blend_ == 0 && maximum <= quietEnter) {
            bool detectorSettled = true;
            for (const auto &band : bank_)
                detectorSettled = detectorSettled && std::abs(band.envelope) <= quietEnter &&
                                  std::abs(band.smoothing) <= quietEnter &&
                                  std::abs(band.detection[0].first) <= quietEnter &&
                                  std::abs(band.detection[0].second) <= quietEnter;
            if (detectorSettled) {
                clearSignalState(s.freeze);
                envelopes = {};
                normalSleeping_ = true;
            }
        }
        activity_ = blend_ == 0 || normalSleeping_ ? Activity::Sleeping
                    : demand && blend_ == 1 ? Activity::Active : Activity::Releasing;
    }
    finish(s.audition, envelopes);
    return true;
}

bool Core::process(float *const *main, const float *const *key, std::int32_t keyChannels,
                   std::int32_t frames, const Settings &s) noexcept {
    return processImpl(main, key, keyChannels, frames, s);
}
bool Core::process(double *const *main, const double *const *key, std::int32_t keyChannels,
                   std::int32_t frames, const Settings &s) noexcept {
    return processImpl(main, key, keyChannels, frames, s);
}
} // namespace adi::mix::carve
