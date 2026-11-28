// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/adi_shaper_follower.hpp"

#include <algorithm>
#include <cmath>
#include <new>
#include <numbers>

namespace adi::dsp {

namespace {
// The measured rise and fall share a range-reduced exponential. Its two
// polynomial values round separately before the single-precision division.
float followerPole(double rate, double milliseconds) noexcept {
    constexpr double timeScale = 1.00032789;
    constexpr double ln2 = std::numbers::ln2;
    const double z = (1000.0 / rate) / (timeScale * milliseconds);
    if (z >= 150.0 * ln2) return 0.f; // Smaller than half the least binary32 subnormal.
    if (z <= 0x1p-26) return 1.f;    // Both polynomial terms round to unity.
    const int shift = static_cast<int>(std::round(z / ln2));
    const double r = z - static_cast<double>(shift) * ln2;
    const auto polynomial = [](double x) {
        constexpr double quadratic = .09997512;
        constexpr double cubic = .5 * quadratic - 1.0 / 24.0;
        return static_cast<float>(1.0 + x * (.5 + x * (quadratic + x * cubic)));
    };
    return std::ldexp(polynomial(-r) / polynomial(r), -shift);
}
} // namespace

void AdiShaperFollower::prepare(double sampleRate) noexcept {
    if (!std::isfinite(sampleRate) || sampleRate < 1.0) return;
    rate = sampleRate;
    // Keep the complete half-second window through 768 kHz. At higher
    // rates limit memory to the same number of past stereo peaks.
    constexpr double maximumPastFrames = 384000;
    const auto capacity = static_cast<std::size_t>(std::min(.5 * rate, maximumPastFrames)) + 1;
    try {
        history.resize(capacity);
    } catch (const std::bad_alloc&) {
        // A failed growth leaves the existing window available.
    }
    updateTimes();
    reset();
}

void AdiShaperFollower::reset() noexcept {
    envelope = 0;
    front = count = selected = 0;
    frame = 0;
    windowChanged = false;
}

void AdiShaperFollower::updateTimes() noexcept {
    const double a = attack;
    const double attackMs = .005 + 99.995 * a * a * a;
    rise = 1.f - followerPole(rate, attackMs);
    const double limit = history.empty() ? 0.0 : static_cast<double>(history.size() - 1);
    const float frames = static_cast<float>(std::min(.5 * rate * (hold * hold), limit));
    const auto nextWindow = static_cast<std::uint32_t>(frames);
    if (holdFrames != nextWindow) {
        holdFrames = nextWindow;
        windowChanged = true;
    }

    // A 50 ms offset gives a logarithmic interpolation from 5 ms to 5 s.
    const double releaseMs = 5.0 + 55.0 * std::expm1(std::log(5050.0 / 55.0) * release);
    fall = followerPole(rate, releaseMs);
}

void AdiShaperFollower::set(Control control, float value) noexcept {
    if (!std::isfinite(value)) return;
    const float unit = std::clamp(value, 0.f, 100.f) / 100.f;
    switch (control) {
    case Control::Mode: multiply = value >= .5f; break;
    case Control::Amount: amount = unit; break;
    case Control::Depth: depth = static_cast<int>(std::round(std::clamp(value, 0.f, 2.f))); break;
    case Control::Offset: offset = 2.f * (unit - .5f); return;
    case Control::Threshold: threshold = unit; return;
    case Control::Attack: case Control::Hold: case Control::Release: {
        auto& setting = control == Control::Attack ? attack : control == Control::Hold ? hold : release;
        if (setting != unit) {
            setting = unit;
            updateTimes();
        }
        return;
    }
    case Control::On: {
        const bool next = value >= .5f;
        if (enabled_ != next) {
            enabled_ = next;
            reset();
        }
        return;
    }
    }
    scale = static_cast<float>(2 << depth) * (multiply ? amount : 2.f * amount - 1.f);
}

float AdiShaperFollower::heldPeak(float input) noexcept {
    // From oldest to newest, levels strictly decrease. Keep the entire
    // supported window when the live hold shrinks.
    const auto capacity = history.size();
    const auto at = [&](std::size_t offset) -> Peak& {
        const auto position = front + offset;
        return history[position < capacity ? position : position - capacity];
    };
    const auto now = frame++;
    if (count && now - at(0).frame >= capacity) {
        if (++front == capacity) front = 0;
        --count;
        if (selected) --selected;
    }
    while (count && at(count - 1).level <= input) --count;
    at(count) = {input, now};
    ++count;

    if (windowChanged) {
        std::size_t first = 0, last = count - 1;
        while (first < last) {
            const auto mid = first + (last - first) / 2;
            if (now - at(mid).frame > holdFrames) first = mid + 1;
            else last = mid;
        }
        selected = first;
        windowChanged = false;
    } else {
        selected = std::min(selected, count - 1);
        while (now - at(selected).frame > holdFrames) ++selected;
    }
    return at(selected).level;
}

float AdiShaperFollower::apply(float left, float right, float baseHeight) noexcept {
    const float peak = std::max(std::abs(left), std::abs(right));
    const float input = std::isfinite(peak) && peak >= threshold ? peak : 0.f;
    // A zero-frame hold pauses history. At equality the detector releases
    // for a frame before its attack moves back toward a sustained peak.
    const float target = holdFrames ? heldPeak(input) : input;
    if (target > envelope) envelope += rise * (target - envelope);
    else envelope *= fall;
    if (envelope < 1.e-30f) envelope = 0;
    const auto modulation = envelope * scale;
    const float combined = multiply ? baseHeight * modulation : baseHeight + modulation;
    // Multiply can invert the stereo side. A unipolar destination bounds its own input.
    return std::clamp(combined + offset, multiply ? -1.f : 0.f, 1.f);
}

} // namespace adi::dsp
