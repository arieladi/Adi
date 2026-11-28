// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/time_base.hpp"
#include <bit>
#include <limits>
#include <numeric>

namespace adi::time_base {
namespace {
struct Division {
    std::uint64_t whole, remainder;
};

std::optional<Division> divideProduct(std::uint64_t value, std::uint64_t factor,
                                      std::uint64_t divisor, std::uint64_t limit) noexcept {
    if (value <= std::numeric_limits<std::uint64_t>::max() / factor) {
        const auto product = value * factor;
        if (product / divisor > limit)
            return std::nullopt;
        return Division{product / divisor, product % divisor};
    }

    const auto wholeFactor = factor / divisor;
    const auto remainderFactor = factor % divisor;
    Division result{0, 0};
    // Walk the bits of value, carrying the quotient and a remainder after
    // each doubling and addition. Both remainders are below INT64_MAX, so
    // their sum always fits in uint64_t, even for a full-width input.
    for (auto bits = std::bit_width(value); bits > 0; --bits) {
        if (result.whole > limit / 2)
            return std::nullopt;
        result.whole *= 2;
        auto remainder = result.remainder * 2;
        auto increment = remainder / divisor;
        remainder %= divisor;
        if (((value >> (bits - 1)) & 1U) != 0) {
            remainder += remainderFactor;
            increment += wholeFactor + remainder / divisor;
            remainder %= divisor;
        }
        if (increment > limit - result.whole)
            return std::nullopt;
        result.whole += increment;
        result.remainder = remainder;
    }
    return result;
}

constexpr std::int64_t nanosecondsPerSecond = 1'000'000'000;
struct TimecodeRate {
    int nominal, dropped;
};

std::optional<TimecodeRate> timecodeRate(Rate rate, bool dropFrame) noexcept {
    if (rate.numerator == 0 || rate.denominator == 0)
        return std::nullopt;
    const auto common = std::gcd(rate.numerator, rate.denominator);
    rate.numerator /= common;
    rate.denominator /= common;
    int nominal = 0;
    if (rate.denominator == 1) {
        switch (rate.numerator) {
        case 24: case 25: case 30: case 48: case 50: case 60: case 100: case 120:
            nominal = static_cast<int>(rate.numerator);
            break;
        default:
            return std::nullopt;
        }
    } else if (rate.denominator == 1001) {
        switch (rate.numerator) {
        case 24000: case 30000: case 48000: case 60000: case 120000:
            nominal = static_cast<int>(rate.numerator / 1000);
            break;
        default:
            return std::nullopt;
        }
    } else {
        return std::nullopt;
    }
    if (!dropFrame)
        return TimecodeRate{nominal, 0};
    if (rate.denominator != 1001 || (nominal != 30 && nominal != 60))
        return std::nullopt;
    return TimecodeRate{nominal, nominal / 15};
}
} // namespace

std::optional<std::int64_t> rescale(std::int64_t value, std::int64_t numerator,
                                    std::int64_t denominator, Rounding rounding) noexcept {
    if (numerator <= 0 || denominator <= 0)
        return std::nullopt;
    const auto factor = static_cast<std::uint64_t>(numerator);
    const auto divisor = static_cast<std::uint64_t>(denominator);
    constexpr auto signedMax = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    const bool negative = value < 0;
    const auto magnitude = negative ? std::uint64_t{0} - static_cast<std::uint64_t>(value)
                                    : static_cast<std::uint64_t>(value);
    const auto limit = signedMax + (negative ? 1U : 0U);
    const auto divided = divideProduct(magnitude, factor, divisor, limit);
    if (!divided)
        return std::nullopt;
    auto whole = divided->whole;
    if (divided->remainder != 0) {
        if (rounding == Rounding::Exact)
            return std::nullopt;
        const bool roundUp = rounding == Rounding::Floor ? negative
                            : divided->remainder >= divisor - divided->remainder;
        if (roundUp) {
            if (whole == limit)
                return std::nullopt;
            ++whole;
        }
    }
    if (negative && whole == signedMax + 1U)
        return std::numeric_limits<std::int64_t>::min();
    const auto result = static_cast<std::int64_t>(whole);
    return negative ? -result : result;
}

std::optional<std::int64_t> framesToNanoseconds(std::int64_t frames, Rate rate,
                                                Rounding rounding) noexcept {
    return rescale(frames, nanosecondsPerSecond * rate.denominator, rate.numerator, rounding);
}

std::optional<std::int64_t> nanosecondsToFrames(std::int64_t ns, Rate rate,
                                                Rounding rounding) noexcept {
    return rescale(ns, rate.numerator, nanosecondsPerSecond * rate.denominator, rounding);
}

std::optional<std::int64_t> timecodeToFrame(Timecode code, Rate rate, bool dropFrame) noexcept {
    const auto format = timecodeRate(rate, dropFrame);
    if (!format || code.hours < 0 || code.hours >= 24 || code.minutes < 0 || code.minutes >= 60 ||
        code.seconds < 0 || code.seconds >= 60 || code.frames < 0 || code.frames >= format->nominal)
        return std::nullopt;
    const int minute = code.hours * 60 + code.minutes;
    if (minute % 10 != 0 && code.seconds == 0 && code.frames < format->dropped)
        return std::nullopt;
    const auto label = (minute * 60 + code.seconds) * format->nominal + code.frames;
    return label - format->dropped * (minute - minute / 10);
}

std::optional<Timecode> frameToTimecode(std::int64_t frame, Rate rate, bool dropFrame) noexcept {
    const auto format = timecodeRate(rate, dropFrame);
    if (!format || frame < 0)
        return std::nullopt;
    const int framesPerMinute = format->nominal * 60 - format->dropped;
    const int framesPerTenMinutes = framesPerMinute * 10 + format->dropped;
    if (frame >= framesPerTenMinutes * 144) // six groups per hour
        return std::nullopt;
    const auto within = frame % framesPerTenMinutes;
    const auto skippedMinutes = within < format->dropped ? 0
                                : (within - format->dropped) / framesPerMinute;
    const auto label = frame + format->dropped * (9 * (frame / framesPerTenMinutes) + skippedMinutes);
    const auto seconds = static_cast<int>(label / format->nominal);
    return Timecode{seconds / 3600, seconds / 60 % 60, seconds % 60,
                    static_cast<int>(label % format->nominal)};
}
} // namespace adi::time_base
