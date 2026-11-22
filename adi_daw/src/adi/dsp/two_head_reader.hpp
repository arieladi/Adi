// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>

namespace adi::dsp::pitch_heads {

// A head pair specifies sample ages. The reader supplies the interpolation
// and the history addressing. Neither policy owns the audio buffer.
template<class Law, class Read>
auto read(const Law& law, Read readAt) noexcept
{
    auto result = readAt(law.firstAge());
    if (law.needsSecond())
        result = law.mix(result, readAt(law.secondAge()));
    return result;
}

// Use normalized double phase for the sinusoidal heads, including wrapping
// phase + half before multiplying by the potentially non-integer window.
struct SineSquared {
    static double wrap(double phase) noexcept { return phase - std::floor(phase); }

    static double advance(double phase, double ratio, double window) noexcept
    {
        return wrap(phase + (1 - ratio) / window);
    }

    SineSquared(double phase, double span) noexcept
        : primary(wrap(phase)), secondary(wrap(phase + 0.5)), window(span) {}

    double firstAge() const noexcept { return primary * window; }
    double secondAge() const noexcept { return secondary * window; }
    static bool needsSecond() noexcept { return true; }

    double mix(double first, double second) const noexcept
    {
        const double a = std::sin(std::numbers::pi * primary);
        const double b = std::sin(std::numbers::pi * secondary);
        double value = 0;
        value += a * a * first;
        value += b * b * second;
        return value;
    }

    double primary, secondary, window;
};

// The sample-age policy intentionally accumulates float offsets. Changing a
// window preserves its normalized position and each wrap rounds back to float.
struct Smoothstep {
    float offset;
    double window, inverseWindow;

    static void advance(float& age, float step, double window, double inverseWindow) noexcept
    {
        age += step;
        if (age < 0 || age >= window)
            age = static_cast<float>(age - window * std::floor(age * inverseWindow));
    }

    double firstAge() const noexcept
    {
        const double half = 0.5 * window;
        return offset < half ? offset + half : offset - half;
    }
    double secondAge() const noexcept { return offset; }
    bool needsSecond() const noexcept { return offset != 0; }

    std::array<float, 2> mix(std::array<float, 2> first, const std::array<float, 2>& second) const noexcept
    {
        const double distance = std::min(static_cast<double>(offset), window - offset);
        const float t = static_cast<float>(2 * distance * inverseWindow);
        const float weight = t * t * (3 - 2 * t);
        first[0] += weight * (second[0] - first[0]);
        first[1] += weight * (second[1] - first[1]);
        return first;
    }
};

// The write index is the current sample and age is in [0, size). The caller
// supplies a prepared, nonempty ring long enough for its maximum window.
inline double linearRead(const float* buffer, std::size_t size, std::size_t write, double age) noexcept
{
    double position = static_cast<double>(write) - age;
    if (position < 0) position += static_cast<double>(size);
    const double whole = std::floor(position);
    const double fraction = position - whole;
    const auto first = static_cast<std::size_t>(whole) % size;
    const auto second = (first + 1) % size;
    return buffer[first] * (1 - fraction) + buffer[second] * fraction;
}

} // namespace adi::dsp::pitch_heads
