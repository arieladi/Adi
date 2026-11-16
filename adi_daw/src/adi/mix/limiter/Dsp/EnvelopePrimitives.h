// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "ControlPrimitives.h"
#include "ClearUnpairedEnvelope.h"
#include "MultirateKernels.h"
#include <array>
#include <limits>
#if defined(_M_X64) || defined(__SSE2__)
#include <immintrin.h>
#endif

#if defined(_MSC_VER)
#define ADI_MIX_LIMITER_ENVELOPE_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define ADI_MIX_LIMITER_ENVELOPE_NOINLINE __attribute__((noinline))
#else
#define ADI_MIX_LIMITER_ENVELOPE_NOINLINE
#endif

namespace adi::mix::limiter
{
inline constexpr float envelopeEpsilon = std::numeric_limits<float>::epsilon();

inline float levelInDecibels(float magnitude) noexcept
{
    return std::max(-180.0f, 20.0f * std::log10(std::max(std::abs(magnitude), 1e-10f)));
}

inline float approximateDecibels(float input) noexcept
{
    const auto bits = std::bit_cast<std::uint32_t>(std::max(1e-10f, std::abs(input)));
    const auto mantissa = std::bit_cast<float>((bits & 0x007fffffu) | 0x3f000000u);
    const auto low = mantissa < 0.70710677f;
    const auto reduced = (mantissa - 1.0f) + (low ? mantissa : 0.0f);
    const auto exponent = static_cast<float>(static_cast<int>(bits >> 23) - 126 - (low ? 1 : 0));
    auto polynomial = 0.070376836f;
    for (const auto coefficient : {-0.1151461f, 0.116769984f, -0.12420141f, 0.14249323f,
                                   -0.16668057f, 0.20000714f, -0.24999994f, 0.3333333f})
        polynomial = polynomial * reduced + coefficient;
    polynomial = (polynomial * reduced) * (reduced * reduced);
    polynomial += exponent * -0.00021219444f;
    polynomial -= (reduced * reduced) * 0.5f;
    polynomial += reduced;
    polynomial += exponent * 0.6933594f;
    return std::max(-180.0f, polynomial * 8.685889f);
}

inline float approximateExponential(float input) noexcept
{
    const auto logarithm = std::clamp(input, -88.37625885009766f, 88.37625885009766f);
    const auto exponent = std::floor(logarithm * 1.442695f + 0.5f);
    const auto reduced = (logarithm - exponent * 0.6933594f) - exponent * -0.00021219444f;
    auto polynomial = 0.00019875691f;
    for (const auto coefficient : {0.0013981999f, 0.008333452f, 0.041665796f, 0.16666666f, 0.5f})
        polynomial = polynomial * reduced + coefficient;
    polynomial = polynomial * (reduced * reduced);
    polynomial = (polynomial + reduced) + 1.0f;
    const auto power = static_cast<float>(std::bit_cast<double>(static_cast<std::uint64_t>(static_cast<int>(exponent) + 1023) << 52));
    return polynomial * power;
}

inline float approximateAmplitude(float decibels) noexcept { return approximateExponential(decibels * 0.115129255f); }

inline float detectReduction(float magnitude, float threshold, float knee, float ratio,
                             float limit, bool decibelDomain, bool upwardExpansion) noexcept
{
    if (decibelDomain)
    {
        auto excess = approximateDecibels(magnitude) - threshold;
        const auto half = knee * 0.5f;
        const auto shifted = half + excess;
        if (knee > 0.0f && shifted > envelopeEpsilon && shifted <= knee)
        {
            const auto relative = shifted / knee;
            excess = (relative * relative) * half;
        }
        const auto slope = upwardExpansion ? ratio - 1.0f
                         : ratio >= 99.9999f ? 1.0f : 1.0f - 1.0f / std::max(ratio, envelopeEpsilon);
        return std::clamp(excess * slope, 0.0f, std::max(0.0f, limit));
    }
    auto excess = static_cast<double>(levelInDecibels(magnitude)) - threshold;
    if (knee > envelopeEpsilon && std::abs(excess) <= knee * 0.5)
        excess = (excess + knee * 0.5) * (excess + knee * 0.5) / (2.0 * knee);
    const auto slope = upwardExpansion ? static_cast<double>(ratio) - 1.0
                     : ratio >= 99.9999f ? 1.0 : 1.0 - 1.0 / std::max(ratio, envelopeEpsilon);
    const auto amount = std::clamp(excess * slope, 0.0, static_cast<double>(std::max(0.0f, limit)));
    return static_cast<float>(upwardExpansion ? std::pow(10.0, amount / 20.0)
                                             : 1.0 - std::pow(10.0, -amount / 20.0));
}

class AdaptiveEnvelope
{
    friend struct EnvelopeInspection;
public:
    struct StereoBlockView
    {
        const float* values = nullptr;
        float scalar = 0.0f;
        std::size_t channels = 0;

        float at(std::size_t sample, std::size_t channel) const noexcept
        {
            return values == nullptr ? scalar : values[sample * std::max<std::size_t>(1, channels) + (channels == 0 ? 0 : channel)];
        }
    };

    static void processStereo(AdaptiveEnvelope& left, AdaptiveEnvelope& right, std::size_t frames,
                              StereoBlockView input, StereoBlockView attack, StereoBlockView release,
                              StereoBlockView attackShape, StereoBlockView releaseShape,
                              StereoBlockView attackSmooth, StereoBlockView releaseSmooth, float* output) noexcept
    {
        if (frames == 0) return;
        const auto pairingAt = [&](std::size_t sample)
        {
            return attackShape.channels == 0 && releaseShape.channels == 0
                && (std::abs(attackShape.at(sample, 0) - 1.0f) > envelopeEpsilon
                    || std::abs(releaseShape.at(sample, 0) - 1.0f) > envelopeEpsilon
                    || attackSmooth.channels != 0 || releaseSmooth.channels != 0
                    || std::abs(attackSmooth.at(sample, 0)) > envelopeEpsilon
                    || std::abs(releaseSmooth.at(sample, 0)) > envelopeEpsilon);
        };
        const auto fixedPairing = attackShape.channels != 0 || releaseShape.channels != 0
            || (attackShape.values == nullptr && releaseShape.values == nullptr
                && (attackSmooth.channels != 0 || releaseSmooth.channels != 0
                    || (attackSmooth.values == nullptr && releaseSmooth.values == nullptr)));
        const auto initialPairing = pairingAt(0);
        const bool linearShapeControls = attackShape.channels == 0 && releaseShape.channels == 0
            && attackSmooth.channels == 0 && releaseSmooth.channels == 0;
        if (fixedPairing && !initialPairing && attack.values == nullptr && release.values == nullptr
            && attackShape.values == nullptr && releaseShape.values == nullptr
            && attackSmooth.values == nullptr && releaseSmooth.values == nullptr
            && attackShape.scalar == 1.0f && releaseShape.scalar == 1.0f
            && attackSmooth.scalar == 0.0f && releaseSmooth.scalar == 0.0f)
        {
            const std::array<AdaptiveEnvelope*, 2> states{&left, &right};
            for (std::size_t channel = 0; channel < states.size(); ++channel)
            {
                auto& envelope = *states[channel];
                const auto attackCoefficient = envelope.coefficientFor(
                    static_cast<float>(std::max(0.0, static_cast<double>(attack.scalar)) * envelope.rate), false, true, true);
                const auto releaseCoefficient = envelope.coefficientFor(
                    static_cast<float>(std::max(0.0, static_cast<double>(release.scalar)) * envelope.rate), false, false, true);
                auto state = envelope.value;
                for (std::size_t sample = 0; sample < frames; ++sample)
                {
                    const auto difference = static_cast<double>(input.at(sample, channel)) - state;
                    const auto coefficient = difference <= 0.0 ? releaseCoefficient : attackCoefficient;
                    state += difference * coefficient;
                    if (std::abs(state) < 1e-30) state = 0.0;
                    output[sample * 2 + channel] = static_cast<float>(state);
                }
                envelope.value = state;
            }
            return;
        }
        if (fixedPairing && !initialPairing)
        {
#if defined(_M_X64) || defined(__SSE2__)
            const std::array views{input, attack, release, attackShape, releaseShape, attackSmooth, releaseSmooth};
            if (unpairedStereoAllowed(left, right, frames, views, output))
            {
#if defined(ADI_UNPAIRED_OBSERVE)
                ++unpairedSimdCalls;
                unpairedSimdFrames += frames;
#endif
                const bool linear = attackShape.values == nullptr && releaseShape.values == nullptr
                    && attackSmooth.values == nullptr && releaseSmooth.values == nullptr
                    && attackShape.scalar == 1.0f && releaseShape.scalar == 1.0f
                    && attackSmooth.scalar == 0.0f && releaseSmooth.scalar == 0.0f;
                if (linear) processUnpairedStereo<true>(left, right, frames, views, output);
                else processUnpairedStereo<false>(left, right, frames, views, output);
                return;
            }
#if defined(ADI_UNPAIRED_OBSERVE)
            ++unpairedFallbackCalls;
#endif
#endif
            const std::array<AdaptiveEnvelope*, 2> states{&left, &right};
            for (std::size_t channel = 0; channel < states.size(); ++channel)
                for (std::size_t sample = 0; sample < frames; ++sample)
                    output[sample * 2 + channel] = states[channel]->process(input.at(sample, channel), attack.at(sample, channel),
                        release.at(sample, channel), attackShape.at(sample, channel), releaseShape.at(sample, channel),
                        attackSmooth.at(sample, channel), releaseSmooth.at(sample, channel), false, linearShapeControls);
            return;
        }
#if defined(_M_X64) || defined(__SSE2__)
        if (fixedPairing && initialPairing)
        {
#if defined(__clang__) && defined(_M_X64)
            if (left.pairedAvxEnabled && right.pairedAvxEnabled)
            {
                processCachedPairAvx(left, right, frames, input, attack, release, attackShape,
                                     releaseShape, attackSmooth, releaseSmooth, output);
                return;
            }
#endif
            processCachedPair(left, right, frames, input, attack, release, attackShape,
                              releaseShape, attackSmooth, releaseSmooth, output);
            return;
        }
        auto state = _mm_set_pd(right.value, left.value);
        const auto sampleRate = _mm_setr_ps(static_cast<float>(left.rate), static_cast<float>(right.rate), 0.0f, 0.0f);
        const auto absoluteMask = _mm_castsi128_ps(_mm_set1_epi32(0x7fffffff));
        const auto exponentialTable = tangentTable().data();
#endif
        for (std::size_t sample = 0; sample < frames; ++sample)
        {
            const auto paired = fixedPairing ? initialPairing : pairingAt(sample);
#if defined(_M_X64) || defined(__SSE2__)
            if (paired)
            {
                const auto difference = _mm_sub_ps(loadStereo(input, sample), _mm_cvtpd_ps(state));
                const auto rising = _mm_cmpgt_ps(difference, _mm_setzero_ps());
                const auto duration = selectStereo(rising, loadStereo(attack, sample), loadStereo(release, sample));
                const auto shape = selectStereo(rising, loadStereo(attackShape, sample), loadStereo(releaseShape, sample));
                const auto smoothing = selectStereo(rising, loadStereo(attackSmooth, sample), loadStereo(releaseSmooth, sample));
                const auto magnitude = _mm_and_ps(difference, absoluteMask);
                const auto proportional = _mm_mul_ps(_mm_mul_ps(magnitude, duration), _mm_set1_ps(0.1f));
                const auto shaped = _mm_add_ps(_mm_mul_ps(shape, _mm_sub_ps(duration, proportional)), proportional);
                auto smoothed = shaped;
                const auto zeroSmoothing = _mm_cmpeq_ps(smoothing, _mm_setzero_ps());
                if ((_mm_movemask_ps(zeroSmoothing) & 3) != 3)
                {
                    const auto exponential = _mm_cvtpd_ps(tangentExpStereo(_mm_sub_ps(_mm_setzero_ps(), magnitude), exponentialTable));
                    smoothed = selectStereo(zeroSmoothing, shaped, _mm_add_ps(_mm_mul_ps(exponential, smoothing), shaped));
                }
                const auto samples = _mm_max_ps(_mm_mul_ps(smoothed, sampleRate), _mm_set1_ps(envelopeEpsilon));
                const auto coefficient = coefficientStereo(left, right, samples, _mm_movemask_ps(rising), exponentialTable);
                state = _mm_add_pd(state, _mm_cvtps_pd(_mm_mul_ps(difference, coefficient)));
                _mm_storel_epi64(reinterpret_cast<__m128i*>(output + sample * 2), _mm_castps_si128(_mm_cvtpd_ps(state)));
                continue;
            }
            alignas(16) double current[2];
            _mm_store_pd(current, state);
            left.value = current[0];
            right.value = current[1];
#endif
            output[sample * 2] = left.process(input.at(sample, 0), attack.at(sample, 0), release.at(sample, 0),
                attackShape.at(sample, 0), releaseShape.at(sample, 0), attackSmooth.at(sample, 0), releaseSmooth.at(sample, 0), paired, linearShapeControls);
            output[sample * 2 + 1] = right.process(input.at(sample, 1), attack.at(sample, 1), release.at(sample, 1),
                attackShape.at(sample, 1), releaseShape.at(sample, 1), attackSmooth.at(sample, 1), releaseSmooth.at(sample, 1), paired, linearShapeControls);
#if defined(_M_X64) || defined(__SSE2__)
            state = _mm_set_pd(right.value, left.value);
#endif
        }
#if defined(_M_X64) || defined(__SSE2__)
        alignas(16) double current[2];
        _mm_store_pd(current, state);
        left.value = current[0];
        right.value = current[1];
#endif
    }

    void prepare(double sampleRate) noexcept
    {
        rate = sampleRate;
#if defined(__clang__) && defined(_M_X64)
        // CPU and OS-state detection occurs in prepare, never per sample.
        pairedAvxEnabled = multirate_detail::avxAvailable();
#endif
        clearUnpaired = {};
        clearUnpairedSynchronized = false;
        reset();
    }
    void reset() noexcept { value = 0.0; coefficientValid.fill(false); }
    bool isNeutral() const noexcept { return value == 0.0; }
    bool hasNonNeutralState() const noexcept { return std::abs(static_cast<float>(value)) > 1e-9f; }
    bool canResumeClearUnpaired(float input) const noexcept
    {
        const auto difference = static_cast<double>(input) - value;
        return clearUnpairedSynchronized || (difference > -10.0
            && difference <= -static_cast<double>(std::numeric_limits<float>::epsilon()));
    }
    void invalidateClearUnpaired() noexcept { clearUnpairedSynchronized = false; }

#if !defined(ADI_DSP_FORCE_SCALAR) && (defined(_M_X64) || defined(__SSE2__))
    template<bool ShareCoefficient>
    static void processClearStereo(AdaptiveEnvelope& left, AdaptiveEnvelope& right, std::size_t frames,
        const float* input, float inputScalar, const float* attack, const float* release, const float* shape,
        const float* smoothing, float* output, bool nearestExp) noexcept
    {
        left.clearUnpairedSynchronized = right.clearUnpairedSynchronized = true;
        if (input == nullptr)
            ClearUnpairedEnvelope::processStereo<ShareCoefficient, true>(left.clearUnpaired, right.clearUnpaired,
                left.value, right.value, static_cast<float>(left.rate), frames, input, inputScalar, attack, release,
                shape, smoothing, output, nearestExp);
        else
            ClearUnpairedEnvelope::processStereo<ShareCoefficient, false>(left.clearUnpaired, right.clearUnpaired,
                left.value, right.value, static_cast<float>(left.rate), frames, input, inputScalar, attack, release,
                shape, smoothing, output, nearestExp);
    }
#endif

    float processClearUnpaired(float input, float attack, float release, float releaseShape,
                              float releaseSmoothing, bool smoothingActive, std::size_t localSample, bool nearestExp = false) noexcept
    {
        clearUnpairedSynchronized = true;
        return clearUnpaired.process(value, static_cast<float>(rate), input, attack, release,
                                     releaseShape, releaseSmoothing, smoothingActive, localSample, nearestExp);
    }

    float process(float input, float attack, float release, float attackShape,
                  float releaseShape, float attackSmooth, float releaseSmooth, bool paired = false,
                  bool linearShapeControls = true) noexcept
    {
        if (paired)
        {
            const auto difference = input - static_cast<float>(value);
            const auto rising = difference > 0.0f;
            const auto duration = rising ? attack : release;
            const auto shape = rising ? attackShape : releaseShape;
            const auto smoothing = rising ? attackSmooth : releaseSmooth;
            const auto proportional = (std::abs(difference) * duration) * 0.1f;
            const auto shaped = shape * (duration - proportional) + proportional;
            const auto smoothed = smoothing == 0.0f ? shaped : static_cast<float>(tangentExp(-std::abs(difference))) * smoothing + shaped;
            const auto samples = std::max(envelopeEpsilon, smoothed * static_cast<float>(rate));
            const auto coefficient = coefficientFor(samples, true, rising);
            const auto increment = difference * coefficient;
            value += increment;
            return static_cast<float>(value);
        }
        const auto difference = static_cast<double>(input) - value;
        const bool simple = linearShapeControls && attackShape == 1.0f && releaseShape == 1.0f && attackSmooth == 0.0f && releaseSmooth == 0.0f;
        const auto falling = difference <= (simple ? 0.0 : -envelopeEpsilon);
        const auto time = falling ? release : attack;
        const auto shape = falling ? releaseShape : attackShape;
        const auto smoothing = falling ? releaseSmooth : attackSmooth;
        const auto magnitude = std::abs(difference);
        auto effective = time * (shape + (1.0 - shape) * magnitude / 10.0);
        if (smoothing != 0.0f && magnitude < 10.0)
            effective += smoothing * std::exp(-magnitude);
        const auto samples = static_cast<float>(std::max(0.0, effective) * rate);
        const auto coefficient = coefficientFor(samples, false, !falling, simple);
        value += difference * coefficient;
        if (std::abs(value) < 1e-30) value = 0.0;
        return static_cast<float>(value);
    }

private:
#if defined(_M_X64) || defined(__SSE2__)
    struct AddressSpan
    {
        std::uintptr_t first{}, end{};
    };

    static bool makeSpan(const void* address, std::size_t count, std::size_t width, AddressSpan& span) noexcept
    {
        if (address == nullptr || count == 0 || width == 0 || count > std::numeric_limits<std::size_t>::max() / width)
            return false;
        const auto bytes = count * width;
        span.first = reinterpret_cast<std::uintptr_t>(address);
        if (bytes > std::numeric_limits<std::uintptr_t>::max() - span.first) return false;
        span.end = span.first + bytes;
        return true;
    }

    static bool overlaps(AddressSpan first, AddressSpan second) noexcept
    {
        return first.first < second.end && second.first < first.end;
    }

    static bool finiteFloat(float sample) noexcept
    {
        return (std::bit_cast<std::uint32_t>(sample) & 0x7f800000u) != 0x7f800000u;
    }

    static bool finiteDouble(double sample) noexcept
    {
        return (std::bit_cast<std::uint64_t>(sample) & 0x7ff0000000000000ull) != 0x7ff0000000000000ull;
    }

    static bool unpairedStereoAllowed(const AdaptiveEnvelope& left, const AdaptiveEnvelope& right,
                                      std::size_t frames, const std::array<StereoBlockView, 7>& views,
                                      const float* output) noexcept
    {
        AddressSpan destination, leftState, rightState;
        if (frames > std::numeric_limits<std::size_t>::max() / 2
            || !makeSpan(output, frames * 2, sizeof(float), destination)
            || !makeSpan(&left, 1, sizeof(left), leftState) || !makeSpan(&right, 1, sizeof(right), rightState)
            || overlaps(leftState, rightState) || overlaps(destination, leftState) || overlaps(destination, rightState))
            return false;
        for (const auto* envelope : {&left, &right})
        {
            if (!finiteDouble(envelope->rate) || envelope->rate <= 0.0 || envelope->rate > 16777216.0
                || !finiteDouble(envelope->value)
                || std::abs(envelope->value) > 2.0 * std::numeric_limits<float>::max()) return false;
            for (const auto coefficient : envelope->coefficients)
                if (!finiteFloat(coefficient) || coefficient < 0.0f || coefficient > 1.0f) return false;
        }
        for (const auto& view : views)
        {
            if (view.channels != 0 && view.channels != 2) return false;
            if (view.values == nullptr)
            {
                if (!finiteFloat(view.scalar)) return false;
                continue;
            }
            AddressSpan source;
            const auto stride = view.channels == 0 ? 1u : 2u;
            if (!makeSpan(view.values, frames * stride, sizeof(float), source)
                || overlaps(source, destination) || overlaps(source, leftState) || overlaps(source, rightState)) return false;
        }
        const auto exponentMask = _mm_set1_epi32(0x7f800000);
        for (const auto& view : views)
            if (view.values != nullptr)
            {
                const auto count = frames * (view.channels == 0 ? 1u : 2u);
                std::size_t sample = 0;
                for (; sample + 4 <= count; sample += 4)
                {
                    const auto values = _mm_castps_si128(_mm_loadu_ps(view.values + sample));
                    const auto invalid = _mm_cmpeq_epi32(_mm_and_si128(values, exponentMask), exponentMask);
                    if (_mm_movemask_ps(_mm_castsi128_ps(invalid)) != 0) return false;
                }
                for (; sample < count; ++sample)
                    if (!finiteFloat(view.values[sample])) return false;
            }
        return true;
    }

    static __m128d selectUnpaired(__m128d mask, __m128d selected, __m128d other) noexcept
    {
        return _mm_or_pd(_mm_and_pd(mask, selected), _mm_andnot_pd(mask, other));
    }

    template<bool Linear>
    ADI_MIX_LIMITER_ENVELOPE_NOINLINE static void processUnpairedStereo(AdaptiveEnvelope& left, AdaptiveEnvelope& right,
        std::size_t frames, const std::array<StereoBlockView, 7>& views, float* output) noexcept
    {
        auto state = _mm_set_pd(right.value, left.value);
        const auto rate = _mm_set_pd(right.rate, left.rate);
        const auto zero = _mm_setzero_pd();
        const auto absolute = _mm_castsi128_pd(_mm_set1_epi64x(0x7fffffffffffffffll));
        for (std::size_t sample = 0; sample < frames; ++sample)
        {
            const auto difference = _mm_sub_pd(_mm_cvtps_pd(loadStereo(views[0], sample)), state);
            const auto falling = _mm_cmple_pd(difference, _mm_set1_pd(Linear ? 0.0 : -static_cast<double>(envelopeEpsilon)));
            const auto time = selectUnpaired(falling, _mm_cvtps_pd(loadStereo(views[2], sample)),
                                                       _mm_cvtps_pd(loadStereo(views[1], sample)));
            auto effective = time;
            if constexpr (!Linear)
            {
                const auto shape = selectUnpaired(falling, _mm_cvtps_pd(loadStereo(views[4], sample)),
                                                            _mm_cvtps_pd(loadStereo(views[3], sample)));
                const auto smoothing = selectUnpaired(falling, _mm_cvtps_pd(loadStereo(views[6], sample)),
                                                                _mm_cvtps_pd(loadStereo(views[5], sample)));
                const auto magnitude = _mm_and_pd(difference, absolute);
                const auto shaped = _mm_add_pd(shape, _mm_div_pd(_mm_mul_pd(_mm_sub_pd(_mm_set1_pd(1.0), shape), magnitude),
                                                                _mm_set1_pd(10.0)));
                effective = _mm_mul_pd(time, shaped);
                const auto smoothMask = _mm_movemask_pd(_mm_and_pd(_mm_cmpneq_pd(smoothing, zero),
                                                                   _mm_cmplt_pd(magnitude, _mm_set1_pd(10.0))));
                if (smoothMask != 0)
                {
                    alignas(16) double effectiveValues[2], smoothingValues[2], magnitudes[2];
                    _mm_store_pd(effectiveValues, effective);
                    _mm_store_pd(smoothingValues, smoothing);
                    _mm_store_pd(magnitudes, magnitude);
                    for (int channel = 0; channel < 2; ++channel)
                        if ((smoothMask & (1 << channel)) != 0)
                            effectiveValues[channel] += smoothingValues[channel] * std::exp(-magnitudes[channel]);
                    effective = _mm_load_pd(effectiveValues);
                }
            }
            alignas(16) float samples[4];
            _mm_store_ps(samples, _mm_cvtpd_ps(_mm_mul_pd(_mm_max_pd(effective, zero), rate)));
            const auto directions = _mm_movemask_pd(falling);
            const auto leftCoefficient = left.coefficientFor(samples[0], false, (directions & 1) == 0, Linear);
            const auto rightCoefficient = right.coefficientFor(samples[1], false, (directions & 2) == 0, Linear);
            const auto coefficient = _mm_set_pd(static_cast<double>(rightCoefficient), static_cast<double>(leftCoefficient));
            state = _mm_add_pd(state, _mm_mul_pd(difference, coefficient));
            state = selectUnpaired(_mm_cmplt_pd(_mm_and_pd(state, absolute), _mm_set1_pd(1e-30)), zero, state);
            _mm_storel_epi64(reinterpret_cast<__m128i*>(output + sample * 2), _mm_castps_si128(_mm_cvtpd_ps(state)));
        }
        alignas(16) double values[2];
        _mm_store_pd(values, state);
        left.value = values[0];
        right.value = values[1];
    }

#if defined(ADI_UNPAIRED_OBSERVE)
    inline static thread_local std::uint64_t unpairedSimdCalls{}, unpairedSimdFrames{}, unpairedFallbackCalls{};
#endif

    struct PairedCache
    {
        explicit PairedCache(const AdaptiveEnvelope& left, const AdaptiveEnvelope& right) noexcept
        {
            for (std::size_t direction = 0; direction < 2; ++direction)
            {
                const auto slot = direction + 2;
                keys[direction] = _mm_castsi128_ps(_mm_setr_epi32(
                    left.coefficientValid[slot] ? static_cast<int>(left.coefficientSamples[slot]) : 0,
                    right.coefficientValid[slot] ? static_cast<int>(right.coefficientSamples[slot]) : 0, 0, 0));
                values[direction] = _mm_setr_ps(left.coefficients[slot], right.coefficients[slot], 0.0f, 0.0f);
            }
        }

        __m128 process(__m128 samples, __m128 rising, const double* table) noexcept
        {
            const auto selectedKey = selectStereo(rising, keys[1], keys[0]);
            const auto equal = _mm_cmpeq_epi32(_mm_castps_si128(selectedKey), _mm_castps_si128(samples));
            const auto changed = _mm_castsi128_ps(_mm_xor_si128(equal, _mm_set1_epi32(-1)));
            if ((_mm_movemask_ps(changed) & 3) != 0)
            {
                const auto exponential = _mm_cvtpd_ps(tangentExpStereo(_mm_div_ps(_mm_set1_ps(-2.3025851f), samples), table));
                const auto coefficient = _mm_sub_ps(_mm_set1_ps(1.0f), exponential);
                const auto attackChange = _mm_and_ps(changed, rising);
                const auto releaseChange = _mm_andnot_ps(rising, changed);
                keys[1] = selectStereo(attackChange, samples, keys[1]);
                keys[0] = selectStereo(releaseChange, samples, keys[0]);
                values[1] = selectStereo(attackChange, coefficient, values[1]);
                values[0] = selectStereo(releaseChange, coefficient, values[0]);
            }
            return selectStereo(rising, values[1], values[0]);
        }

        void commit(AdaptiveEnvelope& left, AdaptiveEnvelope& right) const noexcept
        {
            const std::array states{&left, &right};
            for (std::size_t direction = 0; direction < 2; ++direction)
            {
                alignas(16) std::uint32_t bits[4];
                alignas(16) float cachedValues[4];
                _mm_store_si128(reinterpret_cast<__m128i*>(bits), _mm_castps_si128(keys[direction]));
                _mm_store_ps(cachedValues, values[direction]);
                for (std::size_t channel = 0; channel < states.size(); ++channel)
                    if (bits[channel] != 0)
                    {
                        const auto slot = direction + 2;
                        states[channel]->coefficientSamples[slot] = bits[channel];
                        states[channel]->coefficients[slot] = cachedValues[channel];
                        states[channel]->coefficientValid[slot] = true;
                    }
            }
        }

        __m128 keys[2];
        __m128 values[2];
    };

#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__)
    __attribute__((noinline))
#endif
    static void processCachedPair(AdaptiveEnvelope& left, AdaptiveEnvelope& right, std::size_t frames,
                                  StereoBlockView input, StereoBlockView attack, StereoBlockView release,
                                  StereoBlockView attackShape, StereoBlockView releaseShape,
                                  StereoBlockView attackSmooth, StereoBlockView releaseSmooth, float* output) noexcept
    {
        auto state = _mm_set_pd(right.value, left.value);
        const auto sampleRate = _mm_setr_ps(static_cast<float>(left.rate), static_cast<float>(right.rate), 0.0f, 0.0f);
        const auto absoluteMask = _mm_castsi128_ps(_mm_set1_epi32(0x7fffffff));
        const auto exponentialTable = tangentTable().data();
        PairedCache cache(left, right);
        for (std::size_t sample = 0; sample < frames; ++sample)
        {
            const auto difference = _mm_sub_ps(loadStereo(input, sample), _mm_cvtpd_ps(state));
            const auto rising = _mm_cmpgt_ps(difference, _mm_setzero_ps());
            const auto duration = selectStereo(rising, loadStereo(attack, sample), loadStereo(release, sample));
            const auto shape = selectStereo(rising, loadStereo(attackShape, sample), loadStereo(releaseShape, sample));
            const auto smoothing = selectStereo(rising, loadStereo(attackSmooth, sample), loadStereo(releaseSmooth, sample));
            const auto magnitude = _mm_and_ps(difference, absoluteMask);
            const auto proportional = _mm_mul_ps(_mm_mul_ps(magnitude, duration), _mm_set1_ps(0.1f));
            const auto shaped = _mm_add_ps(_mm_mul_ps(shape, _mm_sub_ps(duration, proportional)), proportional);
            auto smoothed = shaped;
            const auto zeroSmoothing = _mm_cmpeq_ps(smoothing, _mm_setzero_ps());
            if ((_mm_movemask_ps(zeroSmoothing) & 3) != 3)
            {
                const auto exponential = _mm_cvtpd_ps(tangentExpStereo(_mm_sub_ps(_mm_setzero_ps(), magnitude), exponentialTable));
                smoothed = selectStereo(zeroSmoothing, shaped, _mm_add_ps(_mm_mul_ps(exponential, smoothing), shaped));
            }
            const auto samples = _mm_max_ps(_mm_mul_ps(smoothed, sampleRate), _mm_set1_ps(envelopeEpsilon));
            const auto coefficient = cache.process(samples, rising, exponentialTable);
            state = _mm_add_pd(state, _mm_cvtps_pd(_mm_mul_ps(difference, coefficient)));
            _mm_storel_epi64(reinterpret_cast<__m128i*>(output + sample * 2), _mm_castps_si128(_mm_cvtpd_ps(state)));
        }
        alignas(16) double current[2];
        _mm_store_pd(current, state);
        left.value = current[0];
        right.value = current[1];
        cache.commit(left, right);
    }

#if defined(__clang__) && defined(_M_X64)
    // All masks here originate from packed comparisons and are all-zero/all-one
    // per lane. AVX blend preserves the exact selected bits with fewer operations.
    // The baseline entry and all shared helpers remain baseline-ISA functions.
    __attribute__((target("avx"), always_inline))
    static __m128 selectPairAvx(__m128 mask, __m128 first, __m128 second) noexcept
    {
        return _mm_blendv_ps(second, first, mask);
    }

    __attribute__((target("avx"), always_inline))
    static __m128d tangentExpPairAvx(__m128 input, const double* table) noexcept
    {
        const auto bounded = _mm_min_ps(_mm_max_ps(input, _mm_set1_ps(-88.0f)), _mm_set1_ps(88.0f));
        constexpr auto step = std::numbers::ln2_v<double> / 2048.0;
        const auto indices = _mm_cvttps_epi32(_mm_mul_ps(bounded, _mm_set1_ps(static_cast<float>(1.0 / step))));
        if ((_mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(indices, _mm_setzero_si128()))) & 3) == 3)
            return _mm_add_pd(_mm_set1_pd(1.0), _mm_cvtps_pd(bounded));
        const auto remainders = _mm_and_si128(indices, _mm_set1_epi32(2047));
        const auto leftRemainder = _mm_cvtsi128_si32(remainders);
        const auto rightRemainder = _mm_cvtsi128_si32(_mm_srli_si128(remainders, 4));
        const auto exponents = _mm_add_epi32(_mm_srai_epi32(indices, 11), _mm_set1_epi32(1023));
        const auto powers = _mm_castsi128_pd(_mm_slli_epi64(_mm_unpacklo_epi32(exponents, _mm_setzero_si128()), 52));
        const auto mantissas = _mm_set_pd(table[rightRemainder], table[leftRemainder]);
        const auto residual = _mm_sub_pd(_mm_add_pd(_mm_set1_pd(1.0), _mm_cvtps_pd(bounded)),
                                         _mm_mul_pd(_mm_cvtepi32_pd(indices), _mm_set1_pd(step)));
        return _mm_mul_pd(residual, _mm_mul_pd(mantissas, powers));
    }

    struct PairedCacheAvx : PairedCache
    {
        using PairedCache::PairedCache;
        __attribute__((target("avx"), always_inline))
        __m128 process(__m128 samples, __m128 rising, const double* table) noexcept
        {
            const auto selectedKey = selectPairAvx(rising, keys[1], keys[0]);
            const auto equal = _mm_cmpeq_epi32(_mm_castps_si128(selectedKey), _mm_castps_si128(samples));
            const auto changed = _mm_castsi128_ps(_mm_xor_si128(equal, _mm_set1_epi32(-1)));
            if ((_mm_movemask_ps(changed) & 3) != 0)
            {
                const auto exponential = _mm_cvtpd_ps(tangentExpPairAvx(_mm_div_ps(_mm_set1_ps(-2.3025851f), samples), table));
                const auto coefficient = _mm_sub_ps(_mm_set1_ps(1.0f), exponential);
                const auto attackChange = _mm_and_ps(changed, rising);
                const auto releaseChange = _mm_andnot_ps(rising, changed);
                keys[1] = selectPairAvx(attackChange, samples, keys[1]);
                keys[0] = selectPairAvx(releaseChange, samples, keys[0]);
                values[1] = selectPairAvx(attackChange, coefficient, values[1]);
                values[0] = selectPairAvx(releaseChange, coefficient, values[0]);
            }
            return selectPairAvx(rising, values[1], values[0]);
        }

    };

    __attribute__((target("avx"), noinline))
    static void processCachedPairAvx(AdaptiveEnvelope& left, AdaptiveEnvelope& right, std::size_t frames,
                                  StereoBlockView input, StereoBlockView attack, StereoBlockView release,
                                  StereoBlockView attackShape, StereoBlockView releaseShape,
                                  StereoBlockView attackSmooth, StereoBlockView releaseSmooth, float* output) noexcept
    {
        auto state = _mm_set_pd(right.value, left.value);
        const auto sampleRate = _mm_setr_ps(static_cast<float>(left.rate), static_cast<float>(right.rate), 0.0f, 0.0f);
        const auto absoluteMask = _mm_castsi128_ps(_mm_set1_epi32(0x7fffffff));
        const auto exponentialTable = tangentTable().data();
        PairedCacheAvx cache(left, right);
        for (std::size_t sample = 0; sample < frames; ++sample)
        {
            const auto difference = _mm_sub_ps(loadStereo(input, sample), _mm_cvtpd_ps(state));
            const auto rising = _mm_cmpgt_ps(difference, _mm_setzero_ps());
            const auto duration = selectPairAvx(rising, loadStereo(attack, sample), loadStereo(release, sample));
            const auto shape = selectPairAvx(rising, loadStereo(attackShape, sample), loadStereo(releaseShape, sample));
            const auto smoothing = selectPairAvx(rising, loadStereo(attackSmooth, sample), loadStereo(releaseSmooth, sample));
            const auto magnitude = _mm_and_ps(difference, absoluteMask);
            const auto proportional = _mm_mul_ps(_mm_mul_ps(magnitude, duration), _mm_set1_ps(0.1f));
            const auto shaped = _mm_add_ps(_mm_mul_ps(shape, _mm_sub_ps(duration, proportional)), proportional);
            auto smoothed = shaped;
            const auto zeroSmoothing = _mm_cmpeq_ps(smoothing, _mm_setzero_ps());
            if ((_mm_movemask_ps(zeroSmoothing) & 3) != 3)
            {
                const auto exponential = _mm_cvtpd_ps(tangentExpPairAvx(_mm_sub_ps(_mm_setzero_ps(), magnitude), exponentialTable));
                smoothed = selectPairAvx(zeroSmoothing, shaped, _mm_add_ps(_mm_mul_ps(exponential, smoothing), shaped));
            }
            const auto samples = _mm_max_ps(_mm_mul_ps(smoothed, sampleRate), _mm_set1_ps(envelopeEpsilon));
            const auto coefficient = cache.process(samples, rising, exponentialTable);
            state = _mm_add_pd(state, _mm_cvtps_pd(_mm_mul_ps(difference, coefficient)));
            _mm_storel_epi64(reinterpret_cast<__m128i*>(output + sample * 2), _mm_castps_si128(_mm_cvtpd_ps(state)));
        }
        alignas(16) double current[2];
        _mm_store_pd(current, state);
        left.value = current[0];
        right.value = current[1];
        cache.commit(left, right);
    }

#endif

    static __m128 loadStereo(const StereoBlockView& view, std::size_t sample) noexcept
    {
        if (view.values == nullptr) return _mm_set1_ps(view.scalar);
        if (view.channels == 0) return _mm_set1_ps(view.values[sample]);
        return _mm_castsi128_ps(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(view.values + sample * view.channels)));
    }

    static __m128 selectStereo(__m128 mask, __m128 first, __m128 second) noexcept
    {
        return _mm_or_ps(_mm_and_ps(mask, first), _mm_andnot_ps(mask, second));
    }

    static __m128d tangentExpStereo(__m128 input, const double* table) noexcept
    {
        const auto bounded = _mm_min_ps(_mm_max_ps(input, _mm_set1_ps(-88.0f)), _mm_set1_ps(88.0f));
        constexpr auto step = std::numbers::ln2_v<double> / 2048.0;
        const auto indices = _mm_cvttps_epi32(_mm_mul_ps(bounded, _mm_set1_ps(static_cast<float>(1.0 / step))));
        if ((_mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(indices, _mm_setzero_si128()))) & 3) == 3)
            return _mm_add_pd(_mm_set1_pd(1.0), _mm_cvtps_pd(bounded));
        const auto remainders = _mm_and_si128(indices, _mm_set1_epi32(2047));
        const auto leftRemainder = _mm_cvtsi128_si32(remainders);
        const auto rightRemainder = _mm_cvtsi128_si32(_mm_srli_si128(remainders, 4));
        const auto exponents = _mm_add_epi32(_mm_srai_epi32(indices, 11), _mm_set1_epi32(1023));
        const auto powers = _mm_castsi128_pd(_mm_slli_epi64(_mm_unpacklo_epi32(exponents, _mm_setzero_si128()), 52));
        const auto mantissas = _mm_set_pd(table[rightRemainder], table[leftRemainder]);
        const auto residual = _mm_sub_pd(_mm_add_pd(_mm_set1_pd(1.0), _mm_cvtps_pd(bounded)),
                                         _mm_mul_pd(_mm_cvtepi32_pd(indices), _mm_set1_pd(step)));
        return _mm_mul_pd(residual, _mm_mul_pd(mantissas, powers));
    }

    static __m128 coefficientStereo(AdaptiveEnvelope& left, AdaptiveEnvelope& right, __m128 samples,
                                    int rising, const double* table) noexcept
    {
        alignas(16) std::uint32_t bits[4];
        _mm_store_si128(reinterpret_cast<__m128i*>(bits), _mm_castps_si128(samples));
        const auto leftSlot = static_cast<std::size_t>(2 + (rising & 1));
        const auto rightSlot = static_cast<std::size_t>(2 + ((rising >> 1) & 1));
        const auto updateLeft = !left.coefficientValid[leftSlot] || left.coefficientSamples[leftSlot] != bits[0];
        const auto updateRight = !right.coefficientValid[rightSlot] || right.coefficientSamples[rightSlot] != bits[1];
        if (updateLeft || updateRight)
        {
            const auto exponential = _mm_cvtpd_ps(tangentExpStereo(_mm_div_ps(_mm_set1_ps(-2.3025851f), samples), table));
            alignas(16) float coefficients[4];
            _mm_store_ps(coefficients, _mm_sub_ps(_mm_set1_ps(1.0f), exponential));
            if (updateLeft)
            {
                left.coefficients[leftSlot] = coefficients[0];
                left.coefficientSamples[leftSlot] = bits[0];
                left.coefficientValid[leftSlot] = true;
            }
            if (updateRight)
            {
                right.coefficients[rightSlot] = coefficients[1];
                right.coefficientSamples[rightSlot] = bits[1];
                right.coefficientValid[rightSlot] = true;
            }
        }
        return _mm_setr_ps(left.coefficients[leftSlot], right.coefficients[rightSlot], 0.0f, 0.0f);
    }
#endif

    float coefficientFor(float samples, bool paired, bool rising, bool simple = false) noexcept
    {
        const auto slot = (simple ? 4u : static_cast<std::size_t>(paired) * 2) + static_cast<std::size_t>(rising);
        const auto bits = std::bit_cast<std::uint32_t>(samples);
        if (!coefficientValid[slot] || coefficientSamples[slot] != bits)
        {
            coefficients[slot] = paired ? 1.0f - static_cast<float>(tangentExp(-2.3025851f / samples))
                : samples <= envelopeEpsilon ? 1.0f : 1.0f - (simple ? approximateExponential(-2.3025851f / samples) : smallNegativeExpChecked(-2.3025851f / samples));
            coefficientSamples[slot] = bits;
            coefficientValid[slot] = true;
        }
        return coefficients[slot];
    }

    static const std::array<double, 2048>& tangentTable() noexcept
    {
        static const auto table = []
        {
            std::array<double, 2048> result;
            constexpr auto step = std::numbers::ln2_v<double> / 2048.0;
            for (std::size_t index = 0; index < result.size(); ++index)
                result[index] = std::exp(static_cast<double>(index) * step);
            return result;
        }();
        return table;
    }

    static double tangentExp(float input) noexcept
    {
        const auto& table = tangentTable();
        const auto value = std::clamp(input, -88.0f, 88.0f);
        constexpr auto step = std::numbers::ln2_v<double> / 2048.0;
        const auto index = static_cast<int>(value * static_cast<float>(1.0 / step));
        const auto remainder = ((index % 2048) + 2048) % 2048;
        const auto whole = (index - remainder) / 2048;
        const auto power = std::bit_cast<double>(static_cast<std::uint64_t>(whole + 1023) << 52);
        return (1.0 + value - index * step) * (table[static_cast<std::size_t>(remainder)] * power);
    }

    double rate = 48000.0, value = 0.0;
    ClearUnpairedEnvelope clearUnpaired;
    bool clearUnpairedSynchronized = false;
    bool pairedAvxEnabled = false;
    std::array<std::uint32_t, 6> coefficientSamples{};
    std::array<float, 6> coefficients{};
    std::array<bool, 6> coefficientValid{};
};

class MovingAverage
{
public:
    void prepare(std::size_t count)
    {
        length = std::max<std::size_t>(1, count);
        history.prepare(length);
        reset();
    }

    void reset() noexcept { history.reset(); sum = 0.0; }

    float process(float input) noexcept
    {
        history.push(input);
        sum += static_cast<double>(input) - history.read(length);
        return static_cast<float>(sum / static_cast<double>(length));
    }

private:
    SampleHistory history;
    std::size_t length = 1;
    double sum = 0.0;
};

class TurningPointEnvelope
{
public:
    void prepare(std::size_t latency)
    {
        delay = latency;
        reductions.prepare(delay);
        points.resize(std::bit_ceil(delay + 4));
        mask = points.size() - 1;
        reset();
    }

    void reset() noexcept
    {
        reductions.reset();
        time = 0;
        first = size = 0;
        previousInput = -180.0f;
        previousReduction = 0.0f;
        previousDirection = 1;
        left = {};
    }

    float process(float detectorInput, float reduction) noexcept
    {
        const auto difference = detectorInput - previousInput;
        const auto direction = difference < 0.0f ? 2 : difference <= envelopeEpsilon ? 1 : 0;
        const auto peak = direction != previousDirection && direction != 0 && previousDirection != 2;
        auto synthetic = false;
        const auto readTime = time - static_cast<std::int64_t>(delay);
        if (size != 0 && points[first].time < readTime)
        {
            synthetic = size == 1 && !peak && points[first].value != 0.0f;
            left = points[first];
            first = (first + 1) & mask;
            --size;
        }
        if (peak || synthetic)
        {
            if (size == 0 && peak) left = {readTime - 1, 0.0f};
            points[(first + size) & mask] = {time - 1, previousReduction};
            ++size;
        }
        double envelope = 0.0;
        if (size != 0)
        {
            const auto& right = points[first];
            const auto span = right.time - left.time;
            envelope = span > 0 ? left.value + static_cast<double>(readTime - left.time)
                * (static_cast<double>(right.value) - left.value) / static_cast<double>(span) : right.value;
        }
        previousInput = detectorInput;
        previousReduction = reduction;
        previousDirection = direction;
        ++time;
        return static_cast<float>(std::max(static_cast<double>(reductions.process(reduction, delay)), envelope));
    }

private:
    struct Point { std::int64_t time = 0; float value = 0.0f; };
    SampleHistory reductions;
    std::vector<Point> points;
    Point left;
    std::int64_t time = 0;
    std::size_t delay = 0, first = 0, size = 0, mask = 0;
    float previousInput = -180.0f, previousReduction = 0.0f;
    int previousDirection = 1;
};
}

#undef ADI_MIX_LIMITER_ENVELOPE_NOINLINE
