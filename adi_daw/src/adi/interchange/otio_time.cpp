// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/interchange/otio_time.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <numeric>
#include <string_view>

namespace adi::interchange::otio {
namespace {
using Json = nlohmann::json;
constexpr auto signedMax = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());

const Json *member(const Json &value, const char *name) {
    const auto found = value.find(name);
    return found == value.end() ? nullptr : &*found;
}

bool schemaIs(const Json &object, const char *name) {
    const auto *schema = member(object, "OTIO_SCHEMA");
    return schema && *schema == name;
}

struct Decimal {
    std::int64_t numerator;
    std::int64_t denominator;
};

std::uint64_t magnitude(std::int64_t value) {
    const auto raw = static_cast<std::uint64_t>(value);
    return value < 0 ? std::uint64_t{0} - raw : raw;
}

std::int64_t signedMagnitude(std::uint64_t value, bool negative) {
    if (negative && value == signedMax + 1)
        return std::numeric_limits<std::int64_t>::min();
    const auto signedValue = static_cast<std::int64_t>(value);
    return negative ? -signedValue : signedValue;
}

std::optional<Decimal> decimal(const Json &number) {
    if (number.is_number_unsigned()) {
        const auto value = number.get<std::uint64_t>();
        if (value > signedMax)
            return {};
        return Decimal{static_cast<std::int64_t>(value), 1};
    }
    if (number.is_number_integer())
        return Decimal{number.get<std::int64_t>(), 1};
    if (!number.is_number_float() || !std::isfinite(number.get<double>()))
        return {};

    // The DOM retains integers as integers. For real-valued JSON numbers, use
    // the shortest decimal emitted for its finite floating representation.
    const auto wire = number.dump();
    auto mantissa = std::string_view(wire);
    int exponent = 0;
    if (const auto e = mantissa.find_first_of("eE"); e != std::string_view::npos) {
        auto suffix = mantissa.substr(e + 1);
        if (suffix.starts_with('+'))
            suffix.remove_prefix(1);
        const auto end = suffix.data() + suffix.size();
        const auto parsed = std::from_chars(suffix.data(), end, exponent);
        if (parsed.ec != std::errc{} || parsed.ptr != end)
            return {};
        mantissa = mantissa.substr(0, e);
    }
    const bool negative = mantissa.starts_with('-');
    if (negative)
        mantissa.remove_prefix(1);
    std::uint64_t digits = 0;
    const auto limit = signedMax + static_cast<unsigned>(negative);
    int scale = -exponent;
    bool afterPoint = false;
    for (const char ch : mantissa) {
        if (ch == '.') {
            afterPoint = true;
            continue;
        }
        const auto digit = static_cast<unsigned>(ch - '0');
        if (digit > 9 || digits > (limit - digit) / 10)
            return {};
        digits = digits * 10 + digit;
        if (afterPoint)
            ++scale;
    }
    if (digits == 0)
        return Decimal{0, 1};
    while (scale > 0 && digits % 10 == 0) {
        digits /= 10;
        --scale;
    }
    while (scale < 0) {
        if (digits > limit / 10)
            return {};
        digits *= 10;
        ++scale;
    }
    std::uint64_t denominator = 1;
    while (scale > 0) {
        if (denominator > signedMax / 10)
            return {};
        denominator *= 10;
        --scale;
    }
    const auto common = std::gcd(digits, denominator);
    return Decimal{signedMagnitude(digits / common, negative),
                   static_cast<std::int64_t>(denominator / common)};
}

std::optional<time_base::Rate> readRate(const Json &number) {
    if (!number.is_number())
        return {};
    const auto approximate = number.get<double>();
    constexpr std::array nominal{24000U, 30000U, 48000U, 60000U, 120000U};
    constexpr std::array aliases{23.976, 29.97, 47.952, 59.94, 119.88};
    for (std::size_t i = 0; i < nominal.size(); ++i) {
        if (approximate == aliases[i] || approximate == nominal[i] / 1001.0)
            return time_base::Rate{nominal[i], 1001};
    }
    const auto value = decimal(number);
    constexpr auto limit = std::numeric_limits<std::uint32_t>::max();
    if (!value || value->numerator <= 0 || value->numerator > limit || value->denominator > limit)
        return {};
    return time_base::Rate{static_cast<std::uint32_t>(value->numerator),
                           static_cast<std::uint32_t>(value->denominator)};
}
} // namespace

std::optional<RationalTime> readTime(const Json &object) {
    if (!schemaIs(object, "RationalTime.1"))
        return {};
    const auto *valueMember = member(object, "value");
    const auto *rateMember = member(object, "rate");
    if (!valueMember || !rateMember)
        return {};
    const auto value = decimal(*valueMember);
    const auto rate = readRate(*rateMember);
    if (!value || !rate)
        return {};
    return RationalTime{value->numerator, value->denominator, *rate};
}

std::optional<TimeRange> readRange(const Json &object) {
    if (!schemaIs(object, "TimeRange.1"))
        return {};
    const auto *startMember = member(object, "start_time");
    const auto *durationMember = member(object, "duration");
    if (!startMember || !durationMember)
        return {};
    const auto start = readTime(*startMember);
    const auto duration = readTime(*durationMember);
    if (!start || !duration || duration->valueNumerator < 0)
        return {};
    return TimeRange{*start, *duration};
}

std::optional<Json> writeTime(const RationalTime &time) {
    if (time.valueDenominator <= 0 || time.rate.numerator == 0 || time.rate.denominator == 0)
        return {};
    const auto commonValue = static_cast<std::int64_t>(
        std::gcd(magnitude(time.valueNumerator), static_cast<std::uint64_t>(time.valueDenominator)));
    const auto commonRate = std::gcd(time.rate.numerator, time.rate.denominator);
    const auto valueNumerator = time.valueNumerator / commonValue;
    const auto valueDenominator = time.valueDenominator / commonValue;
    const auto rateNumerator = time.rate.numerator / commonRate;
    const auto rateDenominator = time.rate.denominator / commonRate;
    const Json value = valueDenominator == 1 ? Json(valueNumerator)
                       : Json(static_cast<double>(valueNumerator) / static_cast<double>(valueDenominator));
    const Json rate = rateDenominator == 1 ? Json(rateNumerator) : Json(rateNumerator / static_cast<double>(rateDenominator));
    Json result{{"OTIO_SCHEMA", "RationalTime.1"}, {"value", value}, {"rate", rate}};
    const auto decoded = readTime(result);
    if (!decoded || decoded->valueNumerator != valueNumerator || decoded->valueDenominator != valueDenominator ||
        decoded->rate.numerator != rateNumerator || decoded->rate.denominator != rateDenominator)
        return {};
    return result;
}

std::optional<Json> writeRange(const TimeRange &range) {
    const auto start = writeTime(range.start);
    const auto duration = writeTime(range.duration);
    if (!start || !duration || range.duration.valueNumerator < 0)
        return {};
    return Json{{"OTIO_SCHEMA", "TimeRange.1"}, {"start_time", *start}, {"duration", *duration}};
}

std::optional<std::int64_t> toNanoseconds(const RationalTime &time, time_base::Rounding rounding) noexcept {
    if (time.valueDenominator <= 0 || time.rate.numerator == 0 || time.rate.denominator == 0)
        return {};
    std::array<std::uint64_t, 3> factors{magnitude(time.valueNumerator), 1'000'000'000, time.rate.denominator};
    std::array<std::uint64_t, 2> divisors{static_cast<std::uint64_t>(time.valueDenominator), time.rate.numerator};
    for (auto &divisor : divisors) {
        for (auto &factor : factors) {
            const auto common = std::gcd(divisor, factor);
            divisor /= common;
            factor /= common;
        }
    }
    if (divisors[0] > signedMax / divisors[1])
        return {};
    // The positive scale cannot exceed 1e9 * UINT32_MAX.
    return time_base::rescale(signedMagnitude(factors[0], time.valueNumerator < 0),
                              static_cast<std::int64_t>(factors[1] * factors[2]),
                              static_cast<std::int64_t>(divisors[0] * divisors[1]), rounding);
}

} // namespace adi::interchange::otio
