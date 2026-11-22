// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/interchange/otio_time.hpp"

#include <cstdio>
#include <limits>

namespace {
using namespace adi::interchange::otio;
using adi::time_base::Rounding;
using Json = nlohmann::json;
int checks = 0, failures = 0;

void check(bool pass, const char *message) {
    ++checks;
    if (!pass) {
        ++failures;
        std::printf("FAIL %s\n", message);
    }
}

Json wireTime(const Json &value, const Json &rate) {
    return Json{{"OTIO_SCHEMA", "RationalTime.1"}, {"value", value}, {"rate", rate}};
}

void parseTime() {
    const auto wire = Json::parse(R"({"OTIO_SCHEMA":"RationalTime.1","value":9007199254740993,"rate":24})");
    const auto huge = readTime(wire);
    check(huge && huge->valueNumerator == 9'007'199'254'740'993 && huge->valueDenominator == 1 &&
              writeTime(*huge) == wire, "JSON integer ticks survive a read/write beyond binary64 precision");
    const auto fractional = readTime(wireTime(-12.75, 12.5));
    check(fractional && fractional->valueNumerator == -51 && fractional->valueDenominator == 4 &&
              fractional->rate.numerator == 25 && fractional->rate.denominator == 2 &&
              toNanoseconds(*fractional, Rounding::Exact) == -1'020'000'000,
          "fractional frame and time-base decimals convert as reduced ratios");
    check(fractional && writeTime(*fractional) == wireTime(-12.75, 12.5),
          "representable fractional times keep their exact rational value");
    const auto exponent = readTime(wireTime(2.5e-10, 0.5));
    check(exponent && exponent->valueNumerator == 1 && exponent->valueDenominator == 4'000'000'000 &&
              toNanoseconds(*exponent, Rounding::Nearest) == 1 &&
              !toNanoseconds(*exponent, Rounding::Exact), "decimal exponents and half-nanosecond rounding");
    check(!readTime(Json::array()) && !readTime(Json{{"OTIO_SCHEMA", "RationalTime.2"}, {"rate", 24}, {"value", 0}}) &&
              !readTime(wireTime("9", 24)) && !readTime(wireTime(9, false)) &&
              !readTime(wireTime(9, 0)) && !readTime(wireTime(9, -24)) &&
              !readTime(wireTime(9, std::numeric_limits<double>::infinity())),
          "only supported schemas and finite positive numeric time bases are accepted");
    check(!readTime(wireTime(std::uint64_t{1} << 63, 24)) &&
              !readTime(wireTime(1e20, 24)) && !writeTime({1, 3, {24, 1}}),
          "out-of-domain input and lossy fractional serialization are refused");
}

void ntscAndLimits() {
    const auto alias = readTime(wireTime(30'000, 29.97));
    check(alias && alias->rate.numerator == 30'000 && alias->rate.denominator == 1001 &&
              toNanoseconds(*alias, Rounding::Exact) == 1'001'000'000'000,
          "common NTSC shorthand uses exact 30000/1001 over long durations");
    const auto positive = toNanoseconds({1, 1, {30'000, 1001}}, Rounding::Nearest);
    const auto negative = toNanoseconds({-1, 1, {30'000, 1001}}, Rounding::Floor);
    check(positive == 33'366'667 && negative == -33'366'667 &&
              !toNanoseconds({1, 1, {30'000, 1001}}, Rounding::Exact),
          "frame conversion honors explicit rounding for fractional nanoseconds");
    const auto written = writeTime({2, 2, {60'000, 2002}});
    const auto back = written ? readTime(*written) : std::nullopt;
    check(back && back->rate.numerator == 30'000 && back->rate.denominator == 1001 &&
              back->valueNumerator == 1 && back->valueDenominator == 1,
          "writer canonicalizes common factors without losing NTSC rate");
    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    check(toNanoseconds({minimum, 1, {1'000'000'000, 1}}, Rounding::Exact) == minimum &&
              toNanoseconds({maximum, 1, {1'000'000'000, 1}}, Rounding::Exact) == maximum &&
              !toNanoseconds({maximum, 1, {1, 1}}, Rounding::Exact),
          "cross-cancellation preserves both signed endpoints and rejects output overflow");
    check(toNanoseconds({maximum, maximum, {4'000'000'000, 1}}, Rounding::Floor) == 0 &&
              !toNanoseconds({1, maximum, {4'000'000'000, 1}}, Rounding::Exact) &&
              !toNanoseconds({1, 1, {1, 0}}, Rounding::Exact),
          "reduce factors before multiplying and reject unsupported representational bounds");
}

void ranges() {
    const auto document = Json::parse(R"({
       "OTIO_SCHEMA":"TimeRange.1",
       "start_time":{"OTIO_SCHEMA":"RationalTime.1","value":-48,"rate":24},
       "duration":{"OTIO_SCHEMA":"RationalTime.1","value":1001,"rate":1000}
    })");
    const auto range = readRange(document);
    check(range && writeRange(*range) == document &&
              toNanoseconds(range->start, Rounding::Exact) == -2'000'000'000 &&
              toNanoseconds(range->duration, Rounding::Exact) == 1'001'000'000,
          "mixed-rate ranges retain their individual time bases and signed source start");
    auto malformed = document;
    malformed["duration"]["value"] = -1;
    check(!readRange(malformed) && !writeRange({{0, 1, {24, 1}}, {-1, 1, {24, 1}}}),
          "negative duration is rejected at both range boundaries");
    malformed = document;
    malformed.erase("start_time");
    check(!readRange(malformed) && writeRange({}).has_value(),
          "partial ranges are refused and zero length is representable");
}
} // namespace

int main() {
    parseTime();
    ntscAndLimits();
    ranges();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
