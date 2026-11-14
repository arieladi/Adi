// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "SmartAmount.h"

#include "EngineBank.h"
#include "DetectorEndpointCache.h"
#include "DetectorZeroRegion.h"
#include "DetectorAdjacentReuse.h"
#include "VectorMath.h"
#include <cassert>
#include <span>
#if defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#endif

#if defined(_MSC_VER)
#define ADIL_EXPRESSION_INLINE __forceinline
#else
#define ADIL_EXPRESSION_INLINE inline
#endif

namespace adi::mix::limiter::blocked
{
struct Frame
{
    using ExpressionTag = void;
    const float* values = nullptr;
    const float* shadowValues = nullptr;
    std::size_t count = 0;
    float scalar = 0.0f;
    bool hasShadow = false;
    Frame() = default;
    Frame(double value) : scalar(static_cast<float>(value)) {}
    ADIL_EXPRESSION_INLINE bool constant() const noexcept { return values == nullptr; }
    ADIL_EXPRESSION_INLINE float at(std::size_t sample, std::size_t channel) const noexcept
    {
        return values == nullptr ? scalar : values[sample * std::max<std::size_t>(1, count) + (count == 0 ? 0 : channel)];
    }
    ADIL_EXPRESSION_INLINE float shadow(std::size_t sample, std::size_t channel) const noexcept
    {
        return shadowValues == nullptr ? at(sample, channel) : shadowValues[sample * count + channel];
    }
#if defined(__SSE2__) || defined(_M_X64)
    ADIL_EXPRESSION_INLINE bool canVector(std::size_t width) const noexcept { return (width == 1 || width == 2) && (count == 0 || count == width); }
    ADIL_EXPRESSION_INLINE __m128 vectorAt(std::size_t first, std::size_t width, bool shadow) const noexcept
    {
        const auto* source = shadow && shadowValues ? shadowValues : values;
        if (!source) return _mm_set1_ps(scalar);
        if (count == width || width == 1) return _mm_loadu_ps(source + first);
        const auto pair = _mm_loadl_pi(_mm_setzero_ps(), reinterpret_cast<const __m64*>(source + first / 2));
        return _mm_unpacklo_ps(pair, pair);
    }
#endif
};

struct AddOperation
{
    float operator()(float left, float right) const noexcept { return left + right; }
#if defined(__SSE2__) || defined(_M_X64)
    __m128 vector(__m128 left, __m128 right) const noexcept { return _mm_add_ps(left, right); }
#endif
};
struct MultiplyOperation
{
    float operator()(float left, float right) const noexcept { return left * right; }
#if defined(__SSE2__) || defined(_M_X64)
    __m128 vector(__m128 left, __m128 right) const noexcept { return _mm_mul_ps(left, right); }
#endif
};
struct MaximumOperation
{
    float operator()(float left, float right) const noexcept { return std::max(left, right); }
#if defined(__SSE2__) || defined(_M_X64)
    __m128 vector(__m128 left, __m128 right) const noexcept
    {
        const auto replace = _mm_cmplt_ps(left, right);
        return _mm_or_ps(_mm_and_ps(replace, right), _mm_andnot_ps(replace, left));
    }
#endif
};
struct MinimumOperation
{
    float operator()(float left, float right) const noexcept { return std::min(left, right); }
#if defined(__SSE2__) || defined(_M_X64)
    __m128 vector(__m128 left, __m128 right) const noexcept
    {
        const auto replace = _mm_cmplt_ps(right, left);
        return _mm_or_ps(_mm_and_ps(replace, right), _mm_andnot_ps(replace, left));
    }
#endif
};
struct ClipOperation
{
    double lower, upper;
    double operator()(double sample, double) const noexcept { return std::clamp(sample, lower, upper); }
#if defined(__SSE2__) || defined(_M_X64)
    ADIL_EXPRESSION_INLINE bool canVector() const noexcept { return lower == static_cast<double>(static_cast<float>(lower)) && upper == static_cast<double>(static_cast<float>(upper)); }
    __m128 vector(__m128 input, __m128) const noexcept
    {
        return MinimumOperation{}.vector(MaximumOperation{}.vector(input, _mm_set1_ps(static_cast<float>(lower))), _mm_set1_ps(static_cast<float>(upper)));
    }
#endif
};

template<typename Value>
concept Expression = requires { typename Value::ExpressionTag; };

template<Expression Value>
inline Value operand(const Value& value) { return value; }
template<typename Value> requires std::is_arithmetic_v<Value>
inline Frame operand(Value value) { return Frame(value); }
template<Expression Value>
inline float at(const Value& value, std::size_t sample, std::size_t channel) noexcept { return value.at(sample, channel); }
template<typename Value> requires std::is_arithmetic_v<Value>
inline float at(Value value, std::size_t, std::size_t) noexcept { return static_cast<float>(value); }
template<Expression Value>
inline std::size_t channels(const Value& value) noexcept { return value.count; }
template<typename Value> requires std::is_arithmetic_v<Value>
inline std::size_t channels(Value) noexcept { return 0; }

template<typename First, typename Second, typename Operation, bool Audio>
struct BinaryExpression
{
    using ExpressionTag = void;
    First first;
    Second second;
    Operation operation;
    std::size_t count;
    bool hasShadow;
    bool scalarOnly;
    float scalar;
    ADIL_EXPRESSION_INLINE bool constant() const noexcept { return scalarOnly; }
    ADIL_EXPRESSION_INLINE float at(std::size_t sample, std::size_t channel) const noexcept
    {
        if (scalarOnly) return scalar;
        return static_cast<float>(operation(first.at(sample, channel), second.at(sample, channel)));
    }
    ADIL_EXPRESSION_INLINE float shadow(std::size_t sample, std::size_t channel) const noexcept
    {
        if constexpr (Audio)
            return static_cast<float>(operation(first.shadow(sample, channel), second.shadow(sample, channel)));
        else
            return at(sample, channel);
    }
#if defined(__SSE2__) || defined(_M_X64)
    ADIL_EXPRESSION_INLINE bool canVector(std::size_t width) const noexcept
    {
        if constexpr (requires { operation.vector(_mm_setzero_ps(), _mm_setzero_ps()); })
        {
            if constexpr (requires { operation.canVector(); })
                if (!operation.canVector()) return false;
            return first.canVector(width) && second.canVector(width);
        }
        else
            return false;
    }
    ADIL_EXPRESSION_INLINE __m128 vectorAt(std::size_t offset, std::size_t width, bool shadowValue) const noexcept
    {
        if (scalarOnly) return _mm_set1_ps(scalar);
        if constexpr (requires { operation.vector(_mm_setzero_ps(), _mm_setzero_ps()); })
            return operation.vector(first.vectorAt(offset, width, Audio && shadowValue), second.vectorAt(offset, width, Audio && shadowValue));
        else
            return _mm_setr_ps(at(offset / width, offset % width), at((offset + 1) / width, (offset + 1) % width),
                               at((offset + 2) / width, (offset + 2) % width), at((offset + 3) / width, (offset + 3) % width));
    }
#endif
};

template<bool Audio, typename First, typename Second, typename Operation>
inline auto binary(const First& first, const Second& second, Operation operation)
{
    const auto left = operand(first);
    const auto right = operand(second);
    const auto constant = left.constant() && right.constant();
    return BinaryExpression<decltype(left), decltype(right), Operation, Audio>{left, right, operation,
        std::max(left.count, right.count), Audio && (left.hasShadow || right.hasShadow), constant,
        constant ? static_cast<float>(operation(left.at(0, 0), right.at(0, 0))) : 0.0f};
}
template<Expression Value>
inline Value f32(const Value& value) { return value; }
template<typename First, typename Second> requires (Expression<First> || Expression<Second>)
inline auto add32(const First& first, const Second& second)
{
    return binary<true>(first, second, AddOperation{});
}
template<typename First, typename Second> requires (Expression<First> || Expression<Second>)
inline auto mul32(const First& first, const Second& second)
{
    return binary<true>(first, second, MultiplyOperation{});
}
template<typename First, typename Second> requires (Expression<First> || Expression<Second>)
inline auto maximum(const First& first, const Second& second)
{
    return binary<false>(first, second, MaximumOperation{});
}
template<typename First, typename Second> requires (Expression<First> || Expression<Second>)
inline auto minimum(const First& first, const Second& second)
{
    return binary<false>(first, second, MinimumOperation{});
}
template<Expression Value>
inline auto clip(const Value& value, double lower, double upper)
{
    return binary<false>(value, 0.0, ClipOperation{lower, upper});
}
struct PowerCurveOperation
{
    bool fixedFiveAndHalf = false;
    double operator()(double input, double amount) const noexcept
    {
        if (!fixedFiveAndHalf)
            return std::pow(input, static_cast<float>(15.0 * amount + 1.0));
        const auto value = static_cast<float>(input);
        const auto square = value * value;
        const auto fourth = square * square;
        const auto fifth = fourth * value;
        return fifth * std::sqrt(value);
    }
#if defined(__SSE2__) || defined(_M_X64)
    bool canVector() const noexcept { return fixedFiveAndHalf; }
    __m128 vector(__m128 value, __m128) const noexcept
    {
        const auto square = _mm_mul_ps(value, value);
        const auto fourth = _mm_mul_ps(square, square);
        const auto fifth = _mm_mul_ps(fourth, value);
        return _mm_mul_ps(fifth, _mm_sqrt_ps(value));
    }
#endif
};

template<typename First, typename Second> requires (Expression<First> || Expression<Second>)
inline auto power_curve(const First& value, const Second& strength)
{
    const auto input = operand(value);
    const auto amount = operand(strength);
    // Specialize the fixed x^(11/2) transfer; retain the general calculation
    // for constant inputs, varying exponents, and every other exponent.
    const bool fixed = !input.constant() && amount.constant() && amount.at(0, 0) == 0.3f;
    return binary<false>(value, strength, PowerCurveOperation{fixed});
}
template<typename First, typename Second> requires (Expression<First> || Expression<Second>)
inline auto exponential_curve(const First& value, const Second& strength)
{
    return binary<false>(value, strength, [](double input, double amount) { const auto factor = 15.0 * amount + 0.01; return std::expm1(factor * input) / std::expm1(factor); });
}
template<typename First, typename Second, typename Mix>
struct CrossfadeExpression
{
    using ExpressionTag = void;
    First first;
    Second second;
    Mix mix;
    std::size_t count;
    bool hasShadow;
    ADIL_EXPRESSION_INLINE bool constant() const noexcept { return first.constant() && second.constant() && mix.constant(); }
    ADIL_EXPRESSION_INLINE float at(std::size_t sample, std::size_t channel) const noexcept
    {
        const auto weight = 0.5 * (static_cast<double>(mix.at(sample, channel)) + 1.0);
        return static_cast<float>(first.at(sample, channel) * (1.0 - weight) + second.at(sample, channel) * weight);
    }
    ADIL_EXPRESSION_INLINE float shadow(std::size_t sample, std::size_t channel) const noexcept
    {
        const auto weight = 0.5 * (static_cast<double>(mix.at(sample, channel)) + 1.0);
        return static_cast<float>(first.shadow(sample, channel) * (1.0 - weight) + second.shadow(sample, channel) * weight);
    }
#if defined(__SSE2__) || defined(_M_X64)
    ADIL_EXPRESSION_INLINE bool canVector(std::size_t width) const noexcept { return first.canVector(width) && second.canVector(width) && mix.canVector(width); }
    ADIL_EXPRESSION_INLINE __m128 vectorAt(std::size_t offset, std::size_t width, bool shadowValue) const noexcept
    {
        const auto left = first.vectorAt(offset, width, shadowValue);
        const auto right = second.vectorAt(offset, width, shadowValue);
        const auto amount = mix.vectorAt(offset, width, false);
        const auto blend = [](__m128 firstPair, __m128 secondPair, __m128 amounts)
        {
            const auto weight = _mm_mul_pd(_mm_set1_pd(0.5), _mm_add_pd(_mm_cvtps_pd(amounts), _mm_set1_pd(1.0)));
            return _mm_cvtpd_ps(_mm_add_pd(_mm_mul_pd(_mm_cvtps_pd(firstPair), _mm_sub_pd(_mm_set1_pd(1.0), weight)),
                                         _mm_mul_pd(_mm_cvtps_pd(secondPair), weight)));
        };
        return _mm_movelh_ps(blend(left, right, amount), blend(_mm_movehl_ps(left, left), _mm_movehl_ps(right, right), _mm_movehl_ps(amount, amount)));
    }
#endif
};
template<typename First, typename Second, typename Mix>
    requires (Expression<First> || Expression<Second> || Expression<Mix>)
inline auto linear_crossfade(const First& first, const Second& second, const Mix& mix)
{
    const auto left = operand(first);
    const auto right = operand(second);
    const auto weight = operand(mix);
    return CrossfadeExpression<decltype(left), decltype(right), decltype(weight)>{left, right, weight,
        std::max({left.count, right.count, weight.count}), left.hasShadow || right.hasShadow};
}

template<typename Value>
struct PeakBufferSafe : std::false_type {};

template<>
struct PeakBufferSafe<Frame> : std::true_type {};

template<typename First, typename Second, typename Operation, bool Audio>
struct PeakBufferSafe<BinaryExpression<First, Second, Operation, Audio>> : std::bool_constant<
    PeakBufferSafe<std::remove_cvref_t<First>>::value && PeakBufferSafe<std::remove_cvref_t<Second>>::value
    && (std::is_same_v<Operation, AddOperation> || std::is_same_v<Operation, MultiplyOperation>
        || std::is_same_v<Operation, MaximumOperation> || std::is_same_v<Operation, MinimumOperation>
        || std::is_same_v<Operation, ClipOperation>)> {};

template<typename First, typename Second, typename Mix>
struct PeakBufferSafe<CrossfadeExpression<First, Second, Mix>> : std::bool_constant<
    PeakBufferSafe<std::remove_cvref_t<First>>::value && PeakBufferSafe<std::remove_cvref_t<Second>>::value
    && PeakBufferSafe<std::remove_cvref_t<Mix>>::value> {};

struct Controls
{
    Frame threshold_db, attack_seconds, release_seconds, lookahead_fraction;
};
}

#undef ADIL_EXPRESSION_INLINE

namespace adi::mix::limiter
{
template<std::size_t fixedChannels = 0>
class BlockEngineImplementation
{
public:
    static constexpr std::size_t maximumFrames = 1024;
    static constexpr std::size_t fixedChannelCount = fixedChannels;
    explicit BlockEngineImplementation(EngineBank& source) : engine(source) {}

    void prepare(std::size_t channels)
    {
        ClearUnpairedEnvelope::prepareTable();
        if constexpr (fixedChannels != 0)
            if (channels != fixedChannels) throw std::invalid_argument("Fixed block engine channel mismatch");
        runtimeChannelCount = channels;
        storage.resize(maximumFrames * channels * 192);
        reductions.resize(maximumFrames * channels);
    }

    void begin(std::size_t frames, const float* transient, const float* release) noexcept
    {
        assert(frames <= maximumFrames);
        frameCount = frames;
        clearShapeComputed = false;
        used = 0;
        transientLink = transient;
        releaseLink = release;
        std::fill_n(reductions.begin(), frameCount * channelsFixed(), 0.0f);
    }

    float reduction(std::size_t sample, std::size_t channel) const noexcept { return reductions[sample * channelsFixed() + channel]; }
    void setShadowAliased(bool value) noexcept { shadowAliased = value; }

    blocked::Frame capture(const blocked::Frame& value) noexcept { return value; }

    template<typename Value>
    blocked::Frame capture(const Value& value)
    {
        const auto input = blocked::operand(value);
        if (input.constant())
        {
            blocked::Frame output(input.at(0, 0));
            output.count = input.count;
            return output;
        }
        return input.hasShadow && !shadowAliased ? captureBuffer<true>(input) : captureBuffer<false>(input);
    }

    template<typename Value>
    auto materializeStatefulInput(blocked::Frame destination, const Value& value) noexcept
    {
        const auto input = blocked::operand(value);
        if constexpr (std::is_same_v<std::remove_cvref_t<decltype(input)>, blocked::Frame>)
            return input;
        else
        {
            if (input.constant()) return blocked::Frame(input.at(0, 0));
            const auto width = channelsFixed();
            std::size_t position = 0;
#if defined(__SSE2__) || defined(_M_X64)
            if (input.canVector(width))
                for (; position + 4 <= frameCount * width; position += 4)
                    _mm_storeu_ps(data(destination) + position, input.vectorAt(position, width, false));
#endif
            for (std::size_t sample = position / width; sample < frameCount; ++sample)
                for (std::size_t channel = 0; channel < width; ++channel)
                    data(destination)[sample * width + channel] = input.at(sample, channel);
            return destination;
        }
    }

    template<bool CopyShadow, typename Value>
    blocked::Frame captureBuffer(const Value& input)
    {
        auto output = allocate(input.hasShadow, input.count);
        const auto width = std::max<std::size_t>(1, input.count);
        std::size_t position = 0;
#if defined(__SSE2__) || defined(_M_X64)
        if (input.canVector(width))
            for (; position + 4 <= frameCount * width; position += 4)
            {
                _mm_storeu_ps(data(output) + position, input.vectorAt(position, width, false));
                if constexpr (CopyShadow)
                    _mm_storeu_ps(shadowData(output) + position, input.vectorAt(position, width, true));
            }
#endif
        for (std::size_t sample = position / width; sample < frameCount; ++sample)
            for (std::size_t channel = 0; channel < width; ++channel)
            {
                data(output)[sample * width + channel] = input.at(sample, channel);
                if constexpr (CopyShadow) shadowData(output)[sample * width + channel] = input.shadow(sample, channel);
            }
        return output;
    }

    auto detector(int slot, const auto& input, const auto& threshold, const auto&,
                  const auto& knee, const auto& ratio, const auto& limit, int domain, bool expansion)
    {
        auto output = linked(slot, input);
        const auto thresholdValues = capture(threshold);
        const auto kneeValues = capture(knee);
        const auto ratioValues = capture(ratio);
        const auto limitValues = capture(limit);
        const auto constantParameters = thresholdValues.constant() && kneeValues.constant()
            && ratioValues.constant() && limitValues.constant();
        if (domain != 0 && constantParameters && kneeValues.scalar == 0.0f)
        {
            constantDecibelDetector(data(output), frameCount * channelsFixed(), thresholdValues.scalar,
                ratioValues.scalar, limitValues.scalar, expansion);
            return constantZero(output);
        }
        if (domain == 0 && !expansion && constantParameters && kneeValues.scalar == 0.0f
            && ratioValues.scalar >= 99.9999f && limitValues.scalar >= 180.0f)
        {
            const auto amplitude = static_cast<float>(std::exp(static_cast<double>(thresholdValues.scalar) * 0.1151292546497023));
            for (std::size_t sample = 0; sample < frameCount * channelsFixed(); ++sample)
                data(output)[sample] = 1.0f - amplitude / std::max(amplitude, std::abs(data(output)[sample]));
            return constantZero(output);
        }
        if (domain == 0 && !expansion && kneeValues.constant() && kneeValues.scalar == 0.0f
            && limitValues.constant() && limitValues.scalar == 180.0f
            && (!ratioValues.constant() || std::abs(ratioValues.scalar - 100.0f) > envelopeEpsilon))
        {
            approximateDecibelsBlock(output.values, data(output), frameCount * channelsFixed());
            for (std::size_t sample = 0; sample < frameCount; ++sample)
                for (std::size_t channel = 0; channel < channelsFixed(); ++channel)
                {
                    const auto index = sample * channelsFixed() + channel;
                    const auto ratioValue = ratioValues.at(sample, channel);
                    const auto slope = 1.0f - (ratioValue < 99.9999f ? 1.0f / ratioValue : 0.0f);
                    data(output)[index] = std::min(thresholdValues.at(sample, channel) - data(output)[index], 0.0f) * slope;
                }
            approximateAmplitudeBlock(output.values, data(output), frameCount * channelsFixed());
            for (std::size_t sample = 0; sample < frameCount * channelsFixed(); ++sample)
                data(output)[sample] = 1.0f - data(output)[sample];
            return constantZero(output);
        }
        if (domain != 0)
        {
            approximateDecibelsBlock(output.values, data(output), frameCount * channelsFixed());
            for (std::size_t sample = 0; sample < frameCount; ++sample)
                for (std::size_t channel = 0; channel < channelsFixed(); ++channel)
                {
                    const auto index = sample * channelsFixed() + channel;
                    auto excess = data(output)[index] - thresholdValues.at(sample, channel);
                    const auto kneeValue = kneeValues.at(sample, channel);
                    const auto half = kneeValue * 0.5f;
                    const auto shifted = half + excess;
                    if (kneeValue > 0.0f && shifted > envelopeEpsilon && shifted <= kneeValue)
                    {
                        const auto relative = shifted / kneeValue;
                        excess = (relative * relative) * half;
                    }
                    const auto ratioValue = ratioValues.at(sample, channel);
                    const auto slope = expansion ? ratioValue - 1.0f : ratioValue >= 99.9999f ? 1.0f
                        : 1.0f - 1.0f / std::max(ratioValue, envelopeEpsilon);
                    data(output)[index] = std::clamp(excess * slope, 0.0f, std::max(0.0f, limitValues.at(sample, channel)));
                }
            return constantZero(output);
        }
        auto processGeneric = [&](auto& endpoints)
        {
#if defined(ADIL_PRIVATE_ADJACENT_STATS)
        detector_adjacent::statistics[static_cast<std::size_t>(engine.activeStyle)].genericSamples += frameCount * channelsFixed();
#endif
        if (channelsFixed() == 2 && !expansion && kneeValues.constant() && kneeValues.scalar == 0.0f
            && limitValues.constant() && detector_adjacent::finite(limitValues.scalar) && limitValues.scalar >= 0.0f)
        {
            for (std::size_t sample = 0; sample < frameCount; ++sample)
            {
                const auto leftMagnitude = output.at(sample, 0);
                const auto rightMagnitude = output.at(sample, 1);
                const auto leftThreshold = thresholdValues.at(sample, 0);
                const auto rightThreshold = thresholdValues.at(sample, 1);
                const auto leftRatio = ratioValues.at(sample, 0);
                const auto rightRatio = ratioValues.at(sample, 1);
                const auto reusable = detector_adjacent::sameKey(leftMagnitude, rightMagnitude,
                    leftThreshold, rightThreshold, leftRatio, rightRatio);
#if defined(ADIL_PRIVATE_ADJACENT_STATS)
                detector_adjacent::recordPair(engine.activeStyle, reusable, leftMagnitude, rightMagnitude,
                    leftThreshold, rightThreshold, leftRatio, rightRatio);
#endif
                const auto result = detector_adjacent::processPair(endpoints, leftMagnitude, rightMagnitude,
                    leftThreshold, rightThreshold, leftRatio, rightRatio, kneeValues.scalar, limitValues.scalar, reusable);
                data(output)[sample * 2] = result[0];
                data(output)[sample * 2 + 1] = result[1];
            }
            return constantZero(output);
        }
        for (std::size_t sample = 0; sample < frameCount; ++sample)
            for (std::size_t channel = 0; channel < channelsFixed(); ++channel)
                data(output)[sample * channelsFixed() + channel] = endpoints.process(output.at(sample, channel),
                    thresholdValues.at(sample, channel), kneeValues.at(sample, channel),
                    ratioValues.at(sample, channel), limitValues.at(sample, channel));
        return constantZero(output);
        };
        if (!expansion && kneeValues.constant() && kneeValues.scalar == 0.0f
            && limitValues.constant() && std::isfinite(limitValues.scalar) && limitValues.scalar >= 0.0f)
        {
            DetectorZeroRegion endpoints(limitValues.scalar);
            return processGeneric(endpoints);
        }
        DetectorEndpointCache endpoints(limitValues.constant(), limitValues.scalar, expansion);
        return processGeneric(endpoints);
    }

    auto peak(int slot, const auto& input, const auto& milliseconds)
    {
        auto& bank = engine.peaks.at(static_cast<std::size_t>(slot));
        engine.initialize(bank, [&](auto& state) { state.prepare(engine.maximumControlDelay); });
        auto output = allocate();
        const auto periods = capture(milliseconds);
        const auto converted = [&](float value) { return static_cast<std::size_t>(std::max(0.0,
            static_cast<double>(static_cast<float>(engine.rate) * value) * 0.001)); };
        const auto fixedPeriods = converted(periods.scalar);
        const auto source = blocked::operand(input);
        if constexpr (blocked::PeakBufferSafe<std::remove_cvref_t<decltype(source)>>::value)
        {
            if (periods.constant())
            {
                auto mainOnly = source;
                mainOnly.hasShadow = false;
                const auto buffered = capture(mainOnly);
                for (std::size_t channel = 0; channel < channelsFixed(); ++channel)
                {
                    if (buffered.constant())
                        bank[channel].processConstantBuffer(buffered.at(0, channel), data(output) + channel,
                            channelsFixed(), frameCount, fixedPeriods, peakScratch);
                    else
                    {
                        const auto stride = std::max<std::size_t>(1, buffered.count);
                        const auto* samples = buffered.values + (buffered.count == 0 ? 0 : channel);
                        bank[channel].processBuffer(samples, stride, data(output) + channel,
                            channelsFixed(), frameCount, fixedPeriods, peakScratch);
                    }
                }
                return constantZero(output);
            }
        }
        for (std::size_t channel = 0; channel < channelsFixed(); ++channel)
        {
            if (periods.constant() && source.constant())
                bank[channel].processConstantBlock(source.at(0, channel), data(output) + channel,
                    channelsFixed(), frameCount, fixedPeriods);
            else if (periods.constant())
                bank[channel].processBlock([&](std::size_t sample) { return source.at(sample, channel); },
                    data(output) + channel, channelsFixed(), frameCount, fixedPeriods);
            else
                for (std::size_t sample = 0; sample < frameCount; ++sample)
                    data(output)[sample * channelsFixed() + channel] = bank[channel].process(source.at(sample, channel),
                        converted(periods.at(sample, channel)));
        }
        return constantZero(output);
    }

    bool neutralEnvelope(int slot, const auto& input)
    {
        auto& bank = engine.envelopes.at(static_cast<std::size_t>(slot));
        engine.initialize(bank, [&](auto& state) { state.prepare(engine.rate); });
        if (std::any_of(bank.begin(), bank.end(), [](const auto& state) { return state.hasNonNeutralState(); })) return false;
        for(std::size_t sample=0;sample<frameCount;++sample)
            for(std::size_t channel=0;channel<channelsFixed();++channel)
                if(std::abs(blocked::at(input,sample,channel)) > 1e-9f) return false;
        return true;
    }

    auto envelope(int slot, const auto& input, const auto& attack, const auto& release,
                  const auto& attackShape, const auto& releaseShape, const auto& attackSmooth, const auto& releaseSmooth)
    {
        auto& bank = engine.envelopes.at(static_cast<std::size_t>(slot));
        engine.initialize(bank, [&](auto& state) { state.prepare(engine.rate); });
        const auto source = blocked::operand(input);
        const auto supportedStereo = channelsFixed() == 2
            && engine.channelGroups[0].stereoPair && engine.channelGroups[1].stereoPair;
        if (engine.activeStyle == 0 && slot == 2 && supportedStereo)
        {
            clearShapeComputed = frameCount != 0 && channelsFixed() != 0;
            for (std::size_t channel = 0; channel < channelsFixed() && clearShapeComputed; ++channel)
                clearShapeComputed = bank[channel].hasNonNeutralState();
        }
        if (source.constant() && std::bit_cast<std::uint32_t>(source.at(0, 0)) == 0
            && std::all_of(bank.begin(), bank.end(), [](const auto& state) { return state.isNeutral(); }))
        {
            if (engine.activeStyle == 0 && slot == 0 && frameCount != 0)
                for (auto& state : bank) state.invalidateClearUnpaired();
            blocked::Frame output(0.0f);
            output.count = channelsFixed();
            return output;
        }
        auto output = allocate();
        const auto attackValues = capture(attack);
        const auto releaseValues = capture(release);
        const auto attackShapeValues = capture(attackShape);
        const auto releaseShapeValues = capture(releaseShape);
        const auto attackSmoothValues = capture(attackSmooth);
        const auto releaseSmoothValues = capture(releaseSmooth);
        blocked::Frame inputValues;
        if (supportedStereo) inputValues = capture(input);
        auto clearCacheReady = clearShapeComputed && supportedStereo && frameCount != 0;
        if (engine.activeStyle == 0 && slot == 0)
            for (std::size_t channel = 0; channel < channelsFixed() && clearCacheReady; ++channel)
                clearCacheReady = bank[channel].hasNonNeutralState()
                    && bank[channel].canResumeClearUnpaired(inputValues.at(0, channel));
        if (engine.activeStyle == 0 && slot == 0 && clearShapeComputed
            && supportedStereo && clearCacheReady && !releaseSmoothValues.constant()
            && attackShapeValues.constant() && attackShapeValues.scalar == 1.0f
            && attackSmoothValues.constant() && attackSmoothValues.scalar == 0.0f)
        {
            bool nearestExp = false;
#if defined(__SSE2__) || defined(_M_X64)
            nearestExp = (_mm_getcsr() & _MM_ROUND_MASK) == _MM_ROUND_NEAREST;
#endif
#if !defined(ADI_DSP_FORCE_SCALAR) && (defined(_M_X64) || defined(__SSE2__))
            if ((inputValues.constant() || inputValues.count == 2)
                && attackValues.values != nullptr && attackValues.count == 2
                && releaseValues.values != nullptr && releaseValues.count == 2
                && releaseShapeValues.values != nullptr && releaseShapeValues.count == 2
                && releaseSmoothValues.values != nullptr && releaseSmoothValues.count == 2)
            {
                AdaptiveEnvelope::processClearStereo<false>(bank[0], bank[1], frameCount,
                    inputValues.values, inputValues.scalar, attackValues.values, releaseValues.values, releaseShapeValues.values,
                    releaseSmoothValues.values, data(output), nearestExp);
                return constantZero(output);
            }
#endif
            for (std::size_t channel = 0; channel < channelsFixed(); ++channel)
                for (std::size_t sample = 0; sample < frameCount; ++sample)
                    data(output)[sample * channelsFixed() + channel] = bank[channel].processClearUnpaired(
                        inputValues.at(sample, channel), attackValues.at(sample, channel), releaseValues.at(sample, channel),
                        releaseShapeValues.at(sample, channel), releaseSmoothValues.at(sample, channel), true, sample, nearestExp);
            return constantZero(output);
        }
        if (engine.activeStyle == 0 && slot == 0 && frameCount != 0)
            for (auto& state : bank) state.invalidateClearUnpaired();
        if (supportedStereo)
        {
            const auto view = [](const blocked::Frame& value)
            {
                return AdaptiveEnvelope::StereoBlockView{value.values, value.scalar, value.count};
            };
            AdaptiveEnvelope::processStereo(bank[0], bank[1], frameCount, view(inputValues),
                view(attackValues), view(releaseValues), view(attackShapeValues), view(releaseShapeValues),
                view(attackSmoothValues), view(releaseSmoothValues), data(output));
            return constantZero(output);
        }
        for (std::size_t channel = 0; channel < channelsFixed(); ++channel)
            for (std::size_t sample = 0; sample < frameCount; ++sample)
            {
                const auto paired = engine.channelGroups[channel].stereoPair
                    && attackShapeValues.count == 0 && releaseShapeValues.count == 0
                    && (std::abs(attackShapeValues.at(sample, 0) - 1.0f) > envelopeEpsilon
                        || std::abs(releaseShapeValues.at(sample, 0) - 1.0f) > envelopeEpsilon
                        || attackSmoothValues.count != 0 || releaseSmoothValues.count != 0
                        || std::abs(attackSmoothValues.at(sample, 0)) > envelopeEpsilon
                        || std::abs(releaseSmoothValues.at(sample, 0)) > envelopeEpsilon);
                data(output)[sample * channelsFixed() + channel] = bank[channel].process(blocked::at(input, sample, channel),
                    attackValues.at(sample, channel), releaseValues.at(sample, channel),
                    attackShapeValues.at(sample, channel), releaseShapeValues.at(sample, channel),
                    attackSmoothValues.at(sample, channel), releaseSmoothValues.at(sample, channel), paired,
                    attackShapeValues.count == 0 && releaseShapeValues.count == 0
                        && attackSmoothValues.count == 0 && releaseSmoothValues.count == 0);
            }
        return constantZero(output);
    }

    auto filter(int slot, const auto& input, std::optional<float> highpass, std::optional<float> lowpass)
    {
        auto& bank = engine.filters.at(static_cast<std::size_t>(slot));
        engine.initialize(bank, [&](auto& state)
        {
            if (highpass) state.highpass.prepare(engine.rate, std::exp2(*highpass), true);
            if (lowpass) state.lowpass.prepare(engine.rate, std::exp2(*lowpass), false);
        });
        auto output = allocate();
        for (std::size_t channel = 0; channel < channelsFixed(); ++channel)
            for (std::size_t sample = 0; sample < frameCount; ++sample)
            {
                auto value = blocked::at(input, sample, channel);
                if (highpass) value = bank[channel].highpass.process(value);
                if (lowpass) value = bank[channel].lowpass.process(value);
                data(output)[sample * channelsFixed() + channel] = value;
            }
        return constantZero(output);
    }

    auto delay(int slot, const auto& input, const auto& milliseconds)
    {
        auto& bank = engine.delays.at(static_cast<std::size_t>(slot));
        engine.initialize(bank, [&](auto& state) { state.prepare(engine.maximumControlDelay); });
        auto output = allocate();
        const auto source = materializeStatefulInput(output, input);
        const auto periods = capture(milliseconds);
        const auto fixedPeriods = engine.samples(periods.scalar);
        for (std::size_t channel = 0; channel < channelsFixed(); ++channel)
        {
            SampleHistory::Block history(bank[channel]);
            for (std::size_t sample = 0; sample < frameCount; ++sample)
                data(output)[sample * channelsFixed() + channel] = history.process(source.at(sample, channel),
                    periods.constant() ? fixedPeriods : engine.samples(periods.at(sample, channel)));
        }
        return constantZero(output);
    }

    auto average(int slot, const auto& input, const auto& milliseconds)
    {
        auto& bank = engine.averages.at(static_cast<std::size_t>(slot));
        engine.initialize(bank, [&](auto& state) { state.average.prepare(engine.rate, engine.maximumControlDelay + 1); });
        auto output = allocate();
        const auto source = materializeStatefulInput(output, input);
        const auto periods = blocked::operand(milliseconds);
        if (frameCount != 0 && periods.constant())
        {
            const auto count = static_cast<std::size_t>(std::max(0.0f, static_cast<float>(static_cast<double>(static_cast<float>(engine.rate)) * 0.001)
                * periods.at(0, 0))) + 1;
            auto* destination = data(output);
            if (channelsFixed() == 2 && source.values != nullptr && source.count == 2)
            {
                for (std::size_t channel = 0; channel < 2; ++channel)
                {
                    auto& state = bank[channel];
                    if (state.count != count) { state.average.setWindow(count); state.count = count; }
                }
                if (bank[0].average.processStereoBlock(bank[1].average, frameCount, source.values, destination))
                    return constantZero(output);
            }
            for (std::size_t channel = 0; channel < channelsFixed(); ++channel)
            {
                auto& state = bank[channel];
                if (state.count != count) { state.average.setWindow(count); state.count = count; }
                state.average.processBlock(frameCount,
                    [&](std::size_t sample) { return source.at(sample, channel); },
                    [&](std::size_t sample, float value) { destination[sample * channelsFixed() + channel] = value; });
            }
            return constantZero(output);
        }
        for (std::size_t channel = 0; channel < channelsFixed(); ++channel)
            for (std::size_t sample = 0; sample < frameCount; ++sample)
            {
                const auto count = static_cast<std::size_t>(std::max(0.0f, static_cast<float>(static_cast<double>(static_cast<float>(engine.rate)) * 0.001)
                    * blocked::at(milliseconds, sample, channel))) + 1;
                auto& state = bank[channel];
                if (state.count != count) { state.average.setWindow(count); state.count = count; }
                data(output)[sample * channelsFixed() + channel] = state.average.process(source.at(sample, channel));
            }
        return constantZero(output);
    }

    auto smart(int slot, const auto& input, const auto& threshold, const auto&, int domain)
    {
        auto& bank = engine.smartEnvelopes.at(static_cast<std::size_t>(slot));
        engine.initialize(bank, [&](auto& state) { state.prepare(engine.smartDelay); });
        auto output = linked(slot, input);
        if (domain != 0)
            approximateDecibelsBlock(output.values, data(output), frameCount * channelsFixed());
        const auto thresholdValues = capture(threshold);
        const auto fixedAmplitude = domain == 0 && thresholdValues.constant()
            ? std::pow(10.0, thresholdValues.scalar / 20.0) : 1.0;
#if !defined(ADI_DSP_FORCE_SCALAR) && (defined(_M_X64) || defined(__SSE2__))
        if (channelsFixed() == 2 && domain == 0 && thresholdValues.constant())
        {
            for (std::size_t sample = 0; sample < frameCount; ++sample)
            {
                const float left = output.at(sample, 0), right = output.at(sample, 1);
                alignas(16) float amounts[4];
                _mm_store_ps(amounts, smartAmountsStereo(_mm_setr_ps(left, right, 0.0f, 0.0f), fixedAmplitude));
                data(output)[sample * 2] = bank[0].process(left, amounts[0]);
                data(output)[sample * 2 + 1] = bank[1].process(right, amounts[1]);
            }
            return constantZero(output);
        }
#endif
        for (std::size_t channel = 0; channel < channelsFixed(); ++channel)
            for (std::size_t sample = 0; sample < frameCount; ++sample)
            {
                const auto detectorInput = output.at(sample, channel);
                const auto thresholdValue = thresholdValues.at(sample, channel);
                const auto linearThreshold = domain == 0 && !thresholdValues.constant() ? std::pow(10.0, thresholdValue / 20.0) : fixedAmplitude;
                const auto amount = domain == 0 ? std::max(0.0, 1.0 - linearThreshold / std::max(linearThreshold, static_cast<double>(detectorInput)))
                                               : static_cast<double>(std::max(0.0f, detectorInput - thresholdValue));
                data(output)[sample * channelsFixed() + channel] = bank[channel].process(detectorInput, static_cast<float>(amount));
            }
        return constantZero(output);
    }

    auto vca(int slot, const auto& input, const auto& amount, const auto& lookahead,
             int convolverCount, int smartCount, int domain)
    {
        const auto source = blocked::operand(input);
        const auto audioDelay = engine.maximumLookahead + static_cast<std::size_t>(convolverCount) * 256
                              + static_cast<std::size_t>(smartCount) * engine.smartDelay;
        auto& bank = engine.gains.at(static_cast<std::size_t>(slot));
        engine.initialize(bank, [&](auto& state)
        {
            state.audio.prepare(audioDelay); state.shadow.prepare(audioDelay);
            state.reduction.prepare(engine.maximumLookahead); state.meter.prepare(audioDelay);
        });
        auto output = allocate(source.hasShadow);
        const auto amountValues = capture(amount);
        const auto lookaheadValues = capture(lookahead);
        auto gainValues = allocate();
        const auto convertedDelay = [&](float value)
        {
            const auto lookaheadSamples = static_cast<std::size_t>(std::max(0.0, static_cast<double>(value) * engine.rate * 0.001));
            return engine.maximumLookahead - std::min(engine.maximumLookahead, lookaheadSamples);
        };
        const auto fixedGainDelay = convertedDelay(lookaheadValues.scalar);
        for (std::size_t channel = 0; channel < channelsFixed(); ++channel)
        {
            SampleHistory::Block history(bank[channel].reduction);
            for (std::size_t sample = 0; sample < frameCount; ++sample)
            {
                const auto gainDelay = lookaheadValues.constant() ? fixedGainDelay : convertedDelay(lookaheadValues.at(sample, channel));
                data(gainValues)[sample * channelsFixed() + channel] = history.process(amountValues.at(sample, channel), gainDelay);
            }
        }
        if (domain != 0)
            approximateAmplitudeBlock(gainValues.values, data(gainValues), frameCount * channelsFixed());
        const auto applyAudio = [&]<bool CopyShadow, bool MeterShadow>()
        {
            for (std::size_t channel = 0; channel < channelsFixed(); ++channel)
            {
                SampleHistory::Block audioHistory(bank[channel].audio);
                SampleHistory::Block shadowHistory(bank[channel].shadow);
                SampleHistory::Block meterHistory(bank[channel].meter);
                for (std::size_t sample = 0; sample < frameCount; ++sample)
                {
                    const auto gain = domain == 0 ? 1.0 - gainValues.at(sample, channel) : static_cast<double>(gainValues.at(sample, channel));
                    const auto index = sample * channelsFixed() + channel;
                    data(output)[index] = static_cast<float>(audioHistory.process(source.at(sample, channel), audioDelay) * gain);
                    if constexpr (CopyShadow)
                    {
                        const auto shadow = static_cast<float>(shadowHistory.process(source.shadow(sample, channel), audioDelay) * gain);
                        if constexpr (MeterShadow) shadowData(output)[index] = shadow;
                    }
                    if constexpr (MeterShadow)
                    {
                        const auto priorReduction = meterHistory.process(reductions[index], audioDelay);
                        reductions[index] = static_cast<float>(1.0 - (1.0 - priorReduction) * gain);
                    }
                }
            }
        };
        if (shadowAliased)
        {
            if (source.hasShadow) applyAudio.template operator()<false, true>();
            else applyAudio.template operator()<false, false>();
        }
        else
        {
            if (source.hasShadow) applyAudio.template operator()<true, true>();
            else applyAudio.template operator()<true, false>();
        }
        if (source.constant() && amountValues.constant() && lookaheadValues.constant() && !source.hasShadow)
        {
            const auto value = output.values[0];
            if (std::all_of(output.values, output.values + frameCount * channelsFixed(),
                            [value](float current) { return current == value; }))
                return blocked::Frame(value);
        }
        return output;
    }

    auto convolve(int slot, const auto& input, const auto& milliseconds, int shape,
                  const auto& smoothing, const auto& offset, const auto& power)
    {
        auto& bank = engine.convolvers.at(static_cast<std::size_t>(slot));
        engine.initialize(bank, [&](auto&) {});
        auto output = allocate();
        const auto source = materializeStatefulInput(output, input);
        for (std::size_t channel = 0; channel < channelsFixed(); ++channel)
        {
            auto& state = bank[channel];
            const auto count = engine.samples(blocked::at(milliseconds, frameCount - 1, channel)) + 1;
            const std::array<float, 3> properties{blocked::at(smoothing, frameCount - 1, channel),
                blocked::at(offset, frameCount - 1, channel), blocked::at(power, frameCount - 1, channel)};
            if (!state.convolver)
            {
                state.convolver = std::make_unique<KernelStage>();
                const auto initialCount = engine.samples(blocked::at(milliseconds, 0, channel)) + 1;
                const std::array<float, 3> initialProperties{blocked::at(smoothing, 0, channel),
                    blocked::at(offset, 0, channel), blocked::at(power, 0, channel)};
                state.convolver->prepare(engine.rate, std::max(initialCount, engine.maximumLookahead + 1), initialCount, shape, initialProperties);
            }
            for (std::size_t sample = 0; sample < frameCount; ++sample)
                data(output)[sample * channelsFixed() + channel] = state.convolver->process(source.at(sample, channel));
            state.count = count;
            state.properties = properties;
            state.shape = shape;
        }
        return constantZero(output);
    }

private:
    bool clearShapeComputed = false;

    PeakWindow::Scratch peakScratch;
    blocked::Frame constantZero(blocked::Frame output) const noexcept
    {
        if (output.values != nullptr && !output.hasShadow && std::all_of(output.values, output.values + frameCount * std::max<std::size_t>(1, output.count),
            [](float value) { return std::bit_cast<std::uint32_t>(value) == 0; }))
        {
            output.values = nullptr;
            output.scalar = 0.0f;
        }
        return output;
    }

    static void constantDecibelDetector(float* values, std::size_t count, float threshold,
                                        float ratio, float limit, bool expansion) noexcept
    {
        [[maybe_unused]] const auto slope = expansion ? ratio - 1.0f : ratio >= 99.9999f ? 1.0f
            : 1.0f - 1.0f / std::max(ratio, envelopeEpsilon);
        std::size_t index = 0;
#if defined(__SSE2__) || defined(_M_X64)
        const auto one = _mm_set1_ps(1.0f);
        const auto minimum = _mm_set1_ps(1e-10f);
        const auto absoluteMask = _mm_castsi128_ps(_mm_set1_epi32(0x7fffffff));
        const auto mantissaMask = _mm_set1_epi32(0x007fffff);
        const auto halfExponent = _mm_set1_epi32(0x3f000000);
        for (; index + 4 <= count; index += 4)
        {
            const auto input = _mm_max_ps(minimum, _mm_and_ps(_mm_loadu_ps(values + index), absoluteMask));
            const auto bits = _mm_castps_si128(input);
            const auto mantissa = _mm_castsi128_ps(_mm_or_si128(_mm_and_si128(bits, mantissaMask), halfExponent));
            const auto low = _mm_cmplt_ps(mantissa, _mm_set1_ps(0.70710677f));
            const auto reduced = _mm_add_ps(_mm_sub_ps(mantissa, one), _mm_and_ps(low, mantissa));
            const auto exponent = _mm_cvtepi32_ps(_mm_sub_epi32(_mm_sub_epi32(_mm_srli_epi32(bits, 23),
                _mm_set1_epi32(126)), _mm_and_si128(_mm_castps_si128(low), _mm_set1_epi32(1))));
            auto polynomial = _mm_set1_ps(0.070376836f);
            for (const auto coefficient : {-0.1151461f, 0.116769984f, -0.12420141f, 0.14249323f,
                                           -0.16668057f, 0.20000714f, -0.24999994f, 0.3333333f})
                polynomial = _mm_add_ps(_mm_mul_ps(polynomial, reduced), _mm_set1_ps(coefficient));
            polynomial = _mm_mul_ps(_mm_mul_ps(polynomial, reduced), _mm_mul_ps(reduced, reduced));
            polynomial = _mm_add_ps(polynomial, _mm_mul_ps(exponent, _mm_set1_ps(-0.00021219444f)));
            polynomial = _mm_sub_ps(polynomial, _mm_mul_ps(_mm_mul_ps(reduced, reduced), _mm_set1_ps(0.5f)));
            polynomial = _mm_add_ps(polynomial, reduced);
            polynomial = _mm_add_ps(polynomial, _mm_mul_ps(exponent, _mm_set1_ps(0.6933594f)));
            const auto decibels = _mm_max_ps(_mm_set1_ps(-180.0f), _mm_mul_ps(polynomial, _mm_set1_ps(8.685889f)));
            const auto excess = _mm_sub_ps(decibels, _mm_set1_ps(threshold));
            const auto reduction = _mm_mul_ps(excess, _mm_set1_ps(slope));
            _mm_storeu_ps(values + index, _mm_min_ps(_mm_set1_ps(std::max(0.0f, limit)), _mm_max_ps(_mm_setzero_ps(), reduction)));
        }
#endif
        for (; index < count; ++index)
            values[index] = detectReduction(values[index], threshold, 0.0f, ratio, limit, true, expansion);
    }

    blocked::Frame allocate(bool shadow = false, std::size_t width = std::numeric_limits<std::size_t>::max())
    {
        if (width == std::numeric_limits<std::size_t>::max()) width = channelsFixed();
        const auto required = frameCount * std::max<std::size_t>(1, width);
        assert(used + required * (shadow && !shadowAliased ? 2 : 1) <= storage.size());
        blocked::Frame result;
        result.values = storage.data() + used;
        result.count = width;
        result.hasShadow = shadow;
        used += required;
        if (shadow)
        {
            result.shadowValues = shadowAliased ? result.values : storage.data() + used;
            if (!shadowAliased) used += required;
        }
        return result;
    }
    static float* data(const blocked::Frame& value) noexcept { return const_cast<float*>(value.values); }
    static float* shadowData(const blocked::Frame& value) noexcept { return const_cast<float*>(value.shadowValues); }

    auto linked(int slot, const auto& input)
    {
        auto output = allocate();
        if (channelsFixed() == 2 && engine.channelGroups[0].pair == engine.channelGroups[1].pair
            && engine.channelGroups[0].shared == engine.channelGroups[1].shared)
        {
            const auto constantLink = slot == 3 && engine.activeStyle != 5;
            const auto* linkValues = slot == 0 || (slot == 2 && engine.activeStyle == 2) ? transientLink : releaseLink;
            std::size_t beginning = 0;
#if defined(__SSE2__) || defined(_M_X64)
            const auto operand = blocked::operand(input);
            if (operand.canVector(2))
                for (; beginning + 2 <= frameCount; beginning += 2)
                {
                    const auto control = constantLink ? _mm_set1_ps(0.5f)
                        : _mm_setr_ps(linkValues[beginning], linkValues[beginning], linkValues[beginning + 1], linkValues[beginning + 1]);
                    const auto doubled = _mm_mul_ps(_mm_set1_ps(2.0f), control);
                    const auto pairAmount = _mm_min_ps(doubled, _mm_set1_ps(1.0f));
                    const auto sharedAmount = _mm_min_ps(_mm_set1_ps(1.0f), _mm_max_ps(_mm_setzero_ps(), _mm_sub_ps(doubled, _mm_set1_ps(1.0f))));
                    const auto source = _mm_and_ps(operand.vectorAt(beginning * 2, 2, false), _mm_castsi128_ps(_mm_set1_epi32(0x7fffffff)));
                    const auto swapped = _mm_shuffle_ps(source, source, _MM_SHUFFLE(2, 3, 0, 1));
                    const auto larger = _mm_cmpgt_ps(swapped, source);
                    const auto pairMaximum = _mm_or_ps(_mm_and_ps(larger, swapped), _mm_andnot_ps(larger, source));
                    const auto maximum = _mm_shuffle_ps(pairMaximum, pairMaximum, _MM_SHUFFLE(2, 2, 0, 0));
                    auto paired = _mm_add_ps(source, _mm_mul_ps(pairAmount, _mm_sub_ps(maximum, source)));
                    if (engine.channelGroups[0].shared)
                        paired = _mm_add_ps(paired, _mm_mul_ps(sharedAmount, _mm_sub_ps(maximum, paired)));
                    _mm_storeu_ps(data(output) + beginning * 2, paired);
                }
#endif
            for (std::size_t sample = beginning; sample < frameCount; ++sample)
            {
                const auto control = constantLink ? 0.5f : linkValues[sample];
                const auto pairAmount = std::min(2.0f * control, 1.0f);
                const auto sharedAmount = std::clamp(2.0f * control - 1.0f, 0.0f, 1.0f);
                const auto left = std::abs(blocked::at(input, sample, 0));
                const auto right = std::abs(blocked::at(input, sample, 1));
                const auto maximum = std::max(left, right);
                const auto leftPaired = left + pairAmount * (maximum - left);
                const auto rightPaired = right + pairAmount * (maximum - right);
                data(output)[sample * 2] = engine.channelGroups[0].shared
                    ? leftPaired + sharedAmount * (maximum - leftPaired) : leftPaired;
                data(output)[sample * 2 + 1] = engine.channelGroups[1].shared
                    ? rightPaired + sharedAmount * (maximum - rightPaired) : rightPaired;
            }
            return output;
        }
        for (std::size_t sample = 0; sample < frameCount; ++sample)
        {
            const auto control = slot == 0 || (slot == 2 && engine.activeStyle == 2) ? transientLink[sample]
                               : slot == 3 && engine.activeStyle != 5 ? 0.5f : releaseLink[sample];
            const auto pairAmount = std::min(2.0f * control, 1.0f);
            const auto sharedAmount = std::clamp(2.0f * control - 1.0f, 0.0f, 1.0f);
            float sharedMaximum = 0.0f;
            for (std::size_t channel = 0; channel < channelsFixed(); ++channel)
                if (engine.channelGroups[channel].shared)
                    sharedMaximum = std::max(sharedMaximum, std::abs(blocked::at(input, sample, channel)));
            for (std::size_t channel = 0; channel < channelsFixed(); ++channel)
            {
                const auto local = std::abs(blocked::at(input, sample, channel));
                auto pairMaximum = local;
                for (std::size_t other = 0; other < channelsFixed(); ++other)
                    if (engine.channelGroups[other].pair == engine.channelGroups[channel].pair)
                        pairMaximum = std::max(pairMaximum, std::abs(blocked::at(input, sample, other)));
                const auto paired = local + pairAmount * (pairMaximum - local);
                data(output)[sample * channelsFixed() + channel] = engine.channelGroups[channel].shared
                    ? paired + sharedAmount * (sharedMaximum - paired) : paired;
            }
        }
        return output;
    }

    constexpr std::size_t channelsFixed() const noexcept
    {
        if constexpr (fixedChannels != 0) return fixedChannels;
        else return runtimeChannelCount;
    }
    EngineBank& engine;
    bool shadowAliased = false;
    std::vector<float> storage, reductions;
    std::size_t runtimeChannelCount = 0, frameCount = 0, used = 0;
    const float* transientLink = nullptr;
    const float* releaseLink = nullptr;
};
using BlockEngine = BlockEngineImplementation<0>;
using StereoBlockEngine = BlockEngineImplementation<2>;

}
