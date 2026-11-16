// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "FiniteSample.h"

#include "Multirate.h"

#include <array>
#if !defined(ADI_DSP_FORCE_SCALAR) && (defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2))
#include <immintrin.h>
#include <cstdint>
#endif

namespace adi::mix::limiter
{
class TruePeakDetector
{
public:
    void prepare(double sampleRate, int channels, int maximumFrames)
    {
        const auto rate = static_cast<float>(sampleRate);
        factor = 8;
        while (factor > 2 && rate * static_cast<float>(factor) > 384001.0f)
            factor /= 2;
        interpolator.prepare(sampleRate, channels, maximumFrames, factor, 60.0f, rate * 0.45351472f);
        delay = interpolator.getLatencySamples() / 2;
        maximumBlock = maximumFrames;
        states.resize(static_cast<std::size_t>(channels));
#if defined(__clang__) && defined(_M_X64) && !defined(ADI_DSP_FORCE_SCALAR) && !defined(ADI_DISABLE_MULTIRATE_AVX) && !defined(ADIL_DISABLE_TP_STEREO)
        const auto stagingSize = channels == 2 && multirate_detail::avxAvailable()
            ? static_cast<std::size_t>(maximumFrames) + static_cast<std::size_t>(delay) + 5 : 0;
        for (auto& window : contiguous) window.assign(stagingSize, 0.0f);
#endif
        for (auto& state : states)
        {
            state.history.assign(std::bit_ceil(static_cast<std::size_t>(delay + 12)), 0.0f);
            state.mask = state.history.size() - 1;
            state.position = 0;
        }
    }

    void reset() noexcept
    {
        interpolator.reset();
        for (auto& state : states)
        {
            std::fill(state.history.begin(), state.history.end(), 0.0f);
            state.position = 0;
        }
    }

    bool process(const float* const* input, float* const* output, int frames, float margin = 0.0f) noexcept
    {
        if (!input || !output || frames < 0 || frames > maximumBlock || !std::isfinite(margin) || margin < 0.0f)
            return false;
        for (std::size_t channel = 0; channel < states.size(); ++channel)
            if (!input[channel] || !output[channel])
                return false;
        const auto oversampled = interpolator.upsample(input, frames);
        if (!oversampled)
            return false;
#if defined(__clang__) && defined(_M_X64) && !defined(ADI_DSP_FORCE_SCALAR) && !defined(ADI_DISABLE_MULTIRATE_AVX) && !defined(ADIL_DISABLE_TP_STEREO)
        if (states.size() == 2 && frames > 0 && stereoBuffersDisjoint(input, output, frames)
            && multirate_detail::avxAvailable())
        {
            if (factor == 2) processStereoAvx<2>(input, output, oversampled, frames, margin);
            else if (factor == 4) processStereoAvx<4>(input, output, oversampled, frames, margin);
            else processStereoAvx<8>(input, output, oversampled, frames, margin);
            interpolator.finishUpsampling();
            return true;
        }
#endif
        for (std::size_t channel = 0; channel < states.size(); ++channel)
        {
            auto& state = states[channel];
            for (int frame = 0; frame < frames; ++frame)
            {
                state.position = (state.position + 1) & state.mask;
                state.history[state.position] = finiteSample(input[channel][frame]);
                float peak = 0.0f;
                for (int phase = 0; phase < factor; ++phase)
                    peak = std::max(peak, std::abs(oversampled[channel][frame * factor + phase]));
                peak *= 1.0f + margin;
#if !defined(ADI_DSP_FORCE_SCALAR) && (defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2))
                auto samples = _mm_setzero_ps();
                for (std::size_t tap = 0; tap < coefficients.size(); ++tap)
                {
                    const auto inputSample = _mm_set1_ps(state.history[(state.position - static_cast<std::size_t>(delay) - 5 + tap) & state.mask]);
                    samples = _mm_add_ps(samples, _mm_mul_ps(_mm_loadu_ps(coefficients[tap].data()), inputSample));
                }
                std::array<float, 4> phases;
                _mm_storeu_ps(phases.data(), samples);
                for (const auto sample : phases) peak = std::max(peak, std::abs(sample));
#else
                for (std::size_t phase = 0; phase < 4; ++phase)
                {
                    float sample = 0.0f;
                    for (std::size_t tap = 0; tap < coefficients.size(); ++tap)
                        sample += coefficients[tap][phase] * state.history[(state.position - static_cast<std::size_t>(delay) - 5 + tap) & state.mask];
                    peak = std::max(peak, std::abs(sample));
                }
#endif
                output[channel][frame] = peak;
            }
        }
        interpolator.finishUpsampling();
        return true;
    }

    int getLatencySamples() const noexcept { return delay; }
    int getFactor() const noexcept { return factor; }

private:
#if defined(__clang__) && defined(_M_X64) && !defined(ADI_DSP_FORCE_SCALAR) && !defined(ADI_DISABLE_MULTIRATE_AVX) && !defined(ADIL_DISABLE_TP_STEREO)
    static bool stereoBuffersDisjoint(const float* const* input, float* const* output, int frames) noexcept
    {
        const auto bytes = static_cast<std::uintptr_t>(frames) * sizeof(float);
        const auto overlaps = [bytes](const void* a, const void* b)
        {
            const auto x = reinterpret_cast<std::uintptr_t>(a), y = reinterpret_cast<std::uintptr_t>(b);
            return x <= y ? y - x < bytes : x - y < bytes;
        };
        if (overlaps(output[0], output[1])) return false;
        for (int destination = 0; destination != 2; ++destination)
            for (int source = 0; source != 2; ++source)
                if (overlaps(output[destination], input[source])) return false;
        return true;
    }

    __attribute__((target("avx"), always_inline))
    static float maximumFour(__m128 value) noexcept
    {
        value = _mm_max_ps(value, _mm_movehl_ps(value, value));
        value = _mm_max_ps(value, _mm_shuffle_ps(value, value, _MM_SHUFFLE(1, 1, 1, 1)));
        return _mm_cvtss_f32(value);
    }
    __attribute__((target("avx"), always_inline))
    static float maximumEightAbsolute(const float* input) noexcept
    {
        const auto mask = _mm256_castsi256_ps(_mm256_set1_epi32(0x7fffffff));
        // Original reduction starts at+0 and ignores NaNs. All remaining
        // absolute values are nonnegative, so max regrouping is exact.
        const auto values = _mm256_max_ps(_mm256_and_ps(_mm256_loadu_ps(input), mask), _mm256_setzero_ps());
        return maximumFour(_mm_max_ps(_mm256_castps256_ps128(values), _mm256_extractf128_ps(values, 1)));
    }
    template<int Factor>
    __attribute__((target("avx"), always_inline))
    static float maximumAbsolute(const float* input) noexcept
    {
        if constexpr (Factor == 8) return maximumEightAbsolute(input);
        else
        {
            const auto values = Factor == 4 ? _mm_loadu_ps(input)
                : _mm_castsi128_ps(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(input)));
            const auto absolute = _mm_and_ps(values, _mm_castsi128_ps(_mm_set1_epi32(0x7fffffff)));
            return maximumFour(_mm_max_ps(absolute, _mm_setzero_ps()));
        }
    }

    template<int Factor>
    __attribute__((target("avx"), noinline))
    void processStereoAvx(const float* const* input, float* const* output,
                          const float* const* oversampled, int frames, float margin) noexcept
    {
        if (frames >= 8 && delay >= 6
            && static_cast<std::size_t>(delay) + 5 < states[0].history.size()
            && static_cast<std::size_t>(delay) + 5 < states[1].history.size()
            && static_cast<std::size_t>(frames + delay + 5) <= contiguous[0].size()
            && static_cast<std::size_t>(frames + delay + 5) <= contiguous[1].size())
        {
            const auto prefix = static_cast<std::size_t>(delay) + 5;
            for (int channel = 0; channel < 2; ++channel)
            {
                auto& state = states[static_cast<std::size_t>(channel)];
                auto* window = contiguous[static_cast<std::size_t>(channel)].data();
                const auto first = (state.position + 1 - prefix) & state.mask;
                const auto firstCount = std::min(prefix, state.history.size() - first);
                std::copy_n(state.history.data() + first, firstCount, window);
                std::copy_n(state.history.data(), prefix - firstCount, window + firstCount);
                for (int i = 0; i < frames; ++i) window[prefix + static_cast<std::size_t>(i)] = finiteSample(input[static_cast<std::size_t>(channel)][i]);
                int frame = 0;
                while (frame + 8 <= frames)
                {
                    alignas(32) float peaks[8];
                    for (int i = 0; i < 8; ++i)
                        peaks[i] = maximumAbsolute<Factor>(oversampled[channel] + (frame + i) * Factor) * (1.0f + margin);
                    auto a = _mm256_setzero_ps(), b = a, c = a, d = a;
                    for (std::size_t tap = 0; tap < coefficients.size(); ++tap)
                    {
                        const auto samples = _mm256_loadu_ps(window + frame + tap);
                        a = _mm256_add_ps(a, _mm256_mul_ps(_mm256_broadcast_ss(&coefficients[tap][0]), samples));
                        b = _mm256_add_ps(b, _mm256_mul_ps(_mm256_broadcast_ss(&coefficients[tap][1]), samples));
                        c = _mm256_add_ps(c, _mm256_mul_ps(_mm256_broadcast_ss(&coefficients[tap][2]), samples));
                        d = _mm256_add_ps(d, _mm256_mul_ps(_mm256_broadcast_ss(&coefficients[tap][3]), samples));
                    }
                    const auto absolute = _mm256_castsi256_ps(_mm256_set1_epi32(0x7fffffff));
                    auto peak = _mm256_load_ps(peaks);
                    peak = _mm256_max_ps(_mm256_and_ps(a, absolute), peak);
                    peak = _mm256_max_ps(_mm256_and_ps(b, absolute), peak);
                    peak = _mm256_max_ps(_mm256_and_ps(c, absolute), peak);
                    peak = _mm256_max_ps(_mm256_and_ps(d, absolute), peak);
                    _mm256_storeu_ps(output[channel] + frame, peak);
                    frame += 8;
                }
                for (; frame < frames; ++frame)
                {
                    auto samples = _mm_setzero_ps();
                    for (std::size_t tap = 0; tap < coefficients.size(); ++tap)
                        samples = _mm_add_ps(samples, _mm_mul_ps(_mm_loadu_ps(coefficients[tap].data()), _mm_set1_ps(window[static_cast<std::size_t>(frame) + tap])));
                    samples = _mm_and_ps(samples, _mm_castsi128_ps(_mm_set1_epi32(0x7fffffff)));
                    const auto peak = maximumAbsolute<Factor>(oversampled[channel] + frame * Factor) * (1.0f + margin);
                    output[channel][frame] = maximumFour(_mm_max_ps(samples, _mm_set1_ps(peak)));
                }
                // Earlier writes in an over-capacity block would be overwritten.
                // Retain exactly the final ring contents and cursor, in two spans.
                const auto retained = std::min(static_cast<std::size_t>(frames), state.history.size());
                const auto skipped = static_cast<std::size_t>(frames) - retained;
                const auto write = (state.position + skipped + 1) & state.mask;
                const auto count = std::min(retained, state.history.size() - write);
                std::copy_n(window + prefix + skipped, count, state.history.data() + write);
                std::copy_n(window + prefix + skipped + count, retained - count, state.history.data());
                state.position = (state.position + static_cast<std::size_t>(frames)) & state.mask;
            }
            return;
        }
        auto& left = states[0];
        auto& right = states[1];
        for (int frame = 0; frame < frames; ++frame)
        {
            left.position = (left.position + 1) & left.mask;
            right.position = (right.position + 1) & right.mask;
            left.history[left.position] = finiteSample(input[0][frame]);
            right.history[right.position] = finiteSample(input[1][frame]);
            float peakLeft = maximumAbsolute<Factor>(oversampled[0] + frame * Factor);
            float peakRight = maximumAbsolute<Factor>(oversampled[1] + frame * Factor);
            peakLeft *= 1.0f + margin;
            peakRight *= 1.0f + margin;
            auto samples = _mm256_setzero_ps();
            for (std::size_t tap = 0; tap < coefficients.size(); ++tap)
            {
                const auto l = _mm_set1_ps(left.history[(left.position - static_cast<std::size_t>(delay) - 5 + tap) & left.mask]);
                const auto r = _mm_set1_ps(right.history[(right.position - static_cast<std::size_t>(delay) - 5 + tap) & right.mask]);
                const auto pair = _mm256_insertf128_ps(_mm256_castps128_ps256(l), r, 1);
                const auto coefficient = _mm256_broadcast_ps(reinterpret_cast<const __m128*>(coefficients[tap].data()));
                samples = _mm256_add_ps(samples, _mm256_mul_ps(coefficient, pair));
            }
            const auto absolute = _mm_castsi128_ps(_mm_set1_epi32(0x7fffffff));
            const auto leftPhases = _mm_and_ps(_mm256_castps256_ps128(samples), absolute);
            const auto rightPhases = _mm_and_ps(_mm256_extractf128_ps(samples, 1), absolute);
            peakLeft = maximumFour(_mm_max_ps(leftPhases, _mm_set1_ps(peakLeft)));
            peakRight = maximumFour(_mm_max_ps(rightPhases, _mm_set1_ps(peakRight)));
            output[0][frame] = peakLeft;
            output[1][frame] = peakRight;
        }
    }
#endif

    struct State
    {
        std::vector<float> history;
        std::size_t position = 0, mask = 0;
    };

    static constexpr std::array<std::array<float, 4>, 12> coefficients{{
        {0.001708984375f, -0.0291748046875f, -0.0189208984375f, -0.00830078125f},
        {0.010986328125f, 0.029296875f, 0.0330810546875f, 0.014892578125f},
        {-0.0196533203125f, -0.0517578125f, -0.0582275390625f, -0.026611328125f},
        {0.033203125f, 0.089111328125f, 0.1015625f, 0.047607421875f},
        {-0.0594482421875f, -0.16650390625f, -0.2003173828125f, -0.102294921875f},
        {0.1373291015625f, 0.465087890625f, 0.77978515625f, 0.97216796875f},
        {0.97216796875f, 0.77978515625f, 0.465087890625f, 0.1373291015625f},
        {-0.102294921875f, -0.2003173828125f, -0.16650390625f, -0.0594482421875f},
        {0.047607421875f, 0.1015625f, 0.089111328125f, 0.033203125f},
        {-0.026611328125f, -0.0582275390625f, -0.0517578125f, -0.0196533203125f},
        {0.014892578125f, 0.0330810546875f, 0.029296875f, 0.010986328125f},
        {-0.00830078125f, -0.0189208984375f, -0.0291748046875f, 0.001708984375f}
    }};

#if defined(__clang__) && defined(_M_X64) && !defined(ADI_DSP_FORCE_SCALAR) && !defined(ADI_DISABLE_MULTIRATE_AVX) && !defined(ADIL_DISABLE_TP_STEREO)
    std::array<std::vector<float>, 2> contiguous;
#endif
    Multirate interpolator;
    std::vector<State> states;
    int factor = 8, delay = 0, maximumBlock = 0;
};
}
