// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace adi::mix::limiter
{
enum class NoiseShape { none, basic, optimized, weighted };

class Dither
{
public:
    void prepare(std::size_t channels, int bitDepth, NoiseShape shape, float ceiling)
    {
        if (channels == 0 || channels > 64) throw std::invalid_argument("Invalid dither channel count");
        states.resize(channels);
        std::uint32_t seed = 1;
        for (auto& state : states)
        {
            seed = seed * 214013u + 2531011u;
            state.random = ((seed >> 16u) & 0x7fffu) + 1u;
        }
        configure(bitDepth, shape, ceiling);
        reset();
    }

    void configure(int bitDepth, NoiseShape shape, float ceiling)
    {
        if (bitDepth != 0 && bitDepth != 16 && bitDepth != 18 && bitDepth != 20 && bitDepth != 22 && bitDepth != 24)
            throw std::invalid_argument("Unsupported output word length");
        bits = bitDepth;
        coefficients = filter(shape);
        step = bits == 0 ? 0.0f : std::ldexp(1.0f, 1 - bits);
        const auto reserveUnit = 1.5f * step;
        reserve = 0.0f;
        for (const auto coefficient : coefficients) reserve += std::abs(coefficient * reserveUnit);
        reserve += reserveUnit;
        setCeiling(ceiling);
    }

    void setCeiling(float ceiling) noexcept
    {
        headroom = ceiling > 0.0f ? std::max(0.0f, ceiling - reserve) / ceiling : 0.0f;
    }

    void reset() noexcept
    {
        for (auto& state : states)
            state.errors.fill(0.0f);
    }

    float process(std::size_t channel, float input) noexcept
    {
        if (bits == 0) return input;
        auto& state = states[channel];
        state.random = state.random * 1103515245u + 12345u;
        const auto first = static_cast<float>(state.random & 0x7fffffffu) * 0x1p-31f;
        state.random = state.random * 1103515245u + 12345u;
        const auto second = static_cast<float>(state.random & 0x7fffffffu) * 0x1p-31f;
        const auto triangular = ((first - 0.5f) + second) - 0.5f;
        double feedback = 0.0;
        for (std::size_t tap = 0; tap < coefficients.size(); ++tap)
            feedback += coefficients[tap] * state.errors[tap];
        const auto adjusted = (static_cast<double>(input) - feedback) * headroom;
        const auto quantized = std::floor((adjusted + static_cast<double>(triangular * step)) / step + 0.5) * step;
        if (!coefficients.empty())
        {
            for (std::size_t tap = 1; tap < coefficients.size(); ++tap) state.errors[tap - 1] = state.errors[tap];
            state.errors[coefficients.size() - 1] = static_cast<float>(quantized - adjusted);
        }
        return static_cast<float>(quantized);
    }

private:
    static std::span<const float> filter(NoiseShape shape)
    {
        static constexpr std::array basic{-0.5f, 1.0f};
        static constexpr std::array optimized{0.003067f, -0.00616555f, 0.00123066f, 0.0285161f, -0.106726f, 0.267859f,
                                               -0.557931f, 0.926704f, -1.36613f, 1.64436f, -1.59032f, 1.47933f};
        static constexpr std::array weighted{0.0847f, -0.569f, 1.281f, -2.205f, 3.353f, -4.174f, 3.937f, -3.37f, 2.412f};
        switch (shape)
        {
            case NoiseShape::none: return {};
            case NoiseShape::basic: return basic;
            case NoiseShape::optimized: return optimized;
            case NoiseShape::weighted: return weighted;
        }
        throw std::invalid_argument("Invalid noise shape");
    }

    struct Channel { std::uint32_t random = 1; std::array<float, 12> errors{}; };
    std::vector<Channel> states;
    std::span<const float> coefficients;
    float step = 0.0f, headroom = 1.0f, reserve = 0.0f;
    int bits = 0;
};
}
