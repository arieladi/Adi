// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/automation_splice.hpp"
#include "adi/engine/automation.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <limits>
#include <utility>

namespace {
using namespace adi;
using namespace adi::automation;
int checks = 0, failures = 0;
void check(bool ok, const char *message) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", message);
    }
}
SpliceKnot point(std::int64_t time, double value, std::uint8_t curve = 1, float tension = 0,
                 std::uint64_t id = 0) {
    SpliceKnot result;
    result.point.time = time;
    result.sample = time;
    result.point.value = value;
    result.point.curve = curve;
    result.point.tension = tension;
    result.point.point_id = id;
    return result;
}
bool equal(const SpliceKnot &a, const SpliceKnot &b) {
    return std::memcmp(&a.point, &b.point, sizeof(AutomationPoint)) == 0 && a.sample == b.sample &&
           a.slice.has_value() == b.slice.has_value() &&
           (!a.slice || std::memcmp(&*a.slice, &*b.slice, sizeof(engine::CurveSlice)) == 0);
}
bool equal(std::span<const SpliceKnot> a, std::span<const SpliceKnot> b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(),
                      [](const auto &x, const auto &y) { return equal(x, y); });
}
engine::AutomationLaneProgram program(std::span<const SpliceKnot> knots, double fallback) {
    engine::AutomationLaneProgram result;
    result.defaultValue = fallback;
    for (const auto &k : knots)
        result.points.push_back({k.sample, k.point.value, k.point.tension, k.point.curve,
                                 k.slice ? &*k.slice : nullptr});
    return result;
}
engine::AutomationLaneProgram program(const SplicePlan &plan, double fallback) {
    engine::AutomationLaneProgram result;
    result.defaultValue = fallback;
    for (const auto &p : plan.knots) {
        const auto &k = p.knot;
        result.points.push_back({k.sample, k.point.value, k.point.tension, k.point.curve,
                                 k.slice ? &*k.slice : nullptr});
    }
    return result;
}
bool near(double a, double b) {
    if (!std::isfinite(a) || !std::isfinite(b))
        return false;
    const auto scale = std::max({1.0, std::abs(a), std::abs(b)});
    return std::abs(a / scale - b / scale) < 3e-12;
}
void rendered(const SplicePlan &plan, std::span<const SpliceKnot> old, double fallback,
              SpliceBounds bounds, std::span<const SpliceKnot> incoming, std::int64_t first,
              std::int64_t last) {
    bool outside = plan.ok, inside = plan.ok;
    if (plan.ok) {
        const auto before = program(old, fallback), replacement = program(incoming, 0),
                   after = program(plan, fallback);
        for (auto sample = first; sample < last; ++sample) {
            const auto actual = engine::valueAt(after, sample);
            if (sample >= bounds.begin.sample && sample < bounds.end.sample)
                inside = inside && near(actual, engine::valueAt(replacement, sample));
            else
                outside = outside && near(actual, engine::valueAt(before, sample));
        }
    }
    check(
        outside,
        "actual automation renderer retains every sampled value outside the half-open replacement");
    check(inside,
          "actual automation renderer uses the complete incoming curve inside the replacement");
}
bool ordered(const SplicePlan &plan) {
    if (!plan.ok)
        return false;
    for (std::size_t i = 1; i < plan.knots.size(); ++i)
        if (plan.knots[i - 1].knot.point.time > plan.knots[i].knot.point.time ||
            plan.knots[i - 1].knot.sample > plan.knots[i].knot.sample)
            return false;
    return true;
}
void curveFamilies() {
    for (std::uint8_t shape = 0; shape <= 5; ++shape)
        for (const float tension : {-1.0f, -0.6f, 0.0f, 0.6f, 1.0f})
            for (const bool sliced : {false, true})
                for (const auto &cuts :
                     {std::pair{25, 175}, std::pair{100, 225}, std::pair{1, 299}}) {
                    std::vector<SpliceKnot> old{point(0, -1, shape, tension, UINT64_MAX),
                                                point(300, 2, 1, 0, 17)};
                    if (sliced) {
                        old.at(0).slice = engine::CurveSlice{0.17, 0.83, -4, 7};
                        old.at(0).point.value = engine::curveValue(-4, 7, 0.17, shape, tension);
                        old.at(1).point.value = engine::curveValue(-4, 7, 0.83, shape, tension);
                    }
                    const auto untouched = old;
                    const SpliceBounds bounds{{cuts.first, cuts.first}, {cuts.second, cuts.second}};
                    const std::vector<SpliceKnot> incoming{
                        point(cuts.first, 0.2, 4, -0.5f, UINT64_MAX), point(cuts.second, 0.8)};
                    const auto plan = planEnvelopeSplice(old, 0.25, bounds, incoming);
                    check(plan.ok && ordered(plan) && equal(old, untouched),
                          "all native curve families splice without changing their inputs");
                    rendered(plan, old, 0.25, bounds, incoming, 0, 330);
                    bool origins = false, domains = false;
                    if (plan.ok && plan.knots.size() == 6) {
                        const auto &prefix = plan.knots.front();
                        const auto &suffix = plan.knots[plan.knots.size() - 2];
                        origins =
                            prefix.pointOrigin == SpliceOrigin{SplicePart::Original, 0} &&
                            prefix.knot.point.point_id == UINT64_MAX &&
                            suffix.pointOrigin == SpliceOrigin{SplicePart::Guard, 1} &&
                            suffix.knot.point.point_id == 0 &&
                            plan.knots.back().pointOrigin == SpliceOrigin{SplicePart::Original, 1};
                        const bool need = sliced || shape > 1;
                        domains = prefix.knot.slice.has_value() == need &&
                                  suffix.knot.slice.has_value() == need;
                        if (need)
                            domains = domains &&
                                      prefix.rangeOrigin == SpliceOrigin{SplicePart::Original, 0} &&
                                      suffix.rangeOrigin == SpliceOrigin{SplicePart::Original, 0} &&
                                      prefix.knot.slice->begin == (sliced ? 0.17 : 0) &&
                                      suffix.knot.slice->end == (sliced ? 0.83 : 1) &&
                                      prefix.knot.slice->end < suffix.knot.slice->begin;
                    }
                    check(origins, "point and range provenance distinguish full-width original IDs "
                                   "from newly authored guards");
                    check(domains,
                          "both outside pieces retain their original nonlinear progress domains");
                }
}
void stepsAndQuantization() {
    std::vector<SpliceKnot> old{point(0, 0.1), point(100, 0.2, 0, 0, 2),
                                point(100, 0.8, 2, 0.4f, 3), point(100, 0.3, 3, -0.5f, 4),
                                point(200, 0.9, 1, 0, 5)};
    const std::vector<SpliceKnot> incoming{point(50, 0.7), point(100, 0.6)};
    const SpliceBounds bounds{{50, 50}, {100, 100}};
    const auto plan = planEnvelopeSplice(old, 0.25, bounds, incoming);
    check(plan.ok && ordered(plan),
          "exact duplicate End points remain ordered after the incoming terminal");
    rendered(plan, old, 0.25, bounds, incoming, 0, 240);
    bool steps = false;
    if (plan.ok && plan.knots.size() == 8) {
        steps = plan.knots[4].pointOrigin == SpliceOrigin{SplicePart::Original, 1} &&
                plan.knots[5].pointOrigin == SpliceOrigin{SplicePart::Original, 2} &&
                plan.knots[6].pointOrigin == SpliceOrigin{SplicePart::Original, 3};
        const auto compiled = program(plan, 0.25);
        steps =
            steps && engine::valueAt(compiled, 50) == 0.7 && engine::valueAt(compiled, 100) == 0.3;
    }
    check(steps, "original coincident End identities and final outgoing value survive exactly");
    old = {point(0, 0, 4, 0.6f), point(100, 1, 2, 0.5f), point(101, 0.25, 5, -0.7f), point(200, 1)};
    old.at(1).sample = 10;
    old.at(2).sample = 10;
    old.at(3).sample = 20;
    for (const auto &cuts : {std::pair{SplicePosition{99, 10}, SplicePosition{150, 15}},
                             std::pair{SplicePosition{102, 10}, SplicePosition{150, 15}},
                             std::pair{SplicePosition{50, 5}, SplicePosition{98, 10}}}) {
        auto a = point(cuts.first.native, 0.65), b = point(cuts.second.native, 0.4);
        a.sample = cuts.first.sample;
        b.sample = cuts.second.sample;
        const std::vector<SpliceKnot> replacement{a, b};
        const SpliceBounds cut{cuts.first, cuts.second};
        const auto clipped = planEnvelopeSplice(old, 0.2, cut, replacement);
        check(clipped.ok && ordered(clipped),
              "native cuts sharing projected samples preserve stable point order");
        rendered(clipped, old, 0.2, cut, replacement, 0, 30);
    }
}
void defaultsAndOwnership() {
    const SpliceBounds bounds{{10, 10}, {20, 20}};
    std::vector<SpliceKnot> replacement{point(10, 0.75), point(20, 0.5)};
    const auto empty = planEnvelopeSplice({}, 0.25, bounds, replacement);
    check(empty.ok && empty.knots.size() == 4,
          "empty target receives explicit old-default guards at both boundaries");
    rendered(empty, {}, 0.25, bounds, replacement, 0, 40);
    for (const auto &cut :
         {SpliceBounds{{20, 20}, {40, 40}}, SpliceBounds{{120, 120}, {140, 140}}}) {
        const std::vector<SpliceKnot> single{point(100, 0.4, 5, 0.6f, UINT64_MAX)};
        replacement = {point(cut.begin.native, 0.7), point(cut.end.native, 0.2)};
        const auto plan = planEnvelopeSplice(single, 0.25, cut, replacement);
        check(plan.ok, "held values before the first and after the last point have guarded joins");
        rendered(plan, single, 0.25, cut, replacement, 0, 170);
    }
    std::vector<SpliceKnot> old{point(0, 0.1, 2, 0.8f, 0), point(100, 0.9, 1, 0, UINT64_MAX)};
    replacement = {point(25, 0.3), point(75, 0.4)};
    const auto original = old, copied = replacement;
    const auto owned = planEnvelopeSplice(old, 0.2, {{25, 25}, {75, 75}}, replacement);
    check(owned.ok && equal(old, original) && equal(replacement, copied),
          "preparation leaves all caller-owned values and literal legacy IDs unchanged");
    const auto before = program(owned, 0.2);
    const auto remembered = engine::valueAt(before, 10);
    old.clear();
    replacement.clear();
    check(
        owned.ok && engine::valueAt(program(owned, 0.2), 10) == remembered &&
            owned.knots.size() == 6 &&
            owned.knots.front().pointOrigin.part == SplicePart::Original &&
            owned.knots.front().knot.point.point_id == 0 &&
            owned.knots[1].pointOrigin.part == SplicePart::Guard,
        "returned geometry and domains own their storage and distinguish guards from old ID zero");
}
void limitsAndRefusals() {
    const auto reject = [](const std::vector<SpliceKnot> &old, double fallback, SpliceBounds bounds,
                           const std::vector<SpliceKnot> &incoming,
                           std::size_t budget = kSplicePointBudget) {
        const auto original = old, replacement = incoming;
        const auto result = planEnvelopeSplice(old, fallback, bounds, incoming, budget);
        check(!result.ok && !result.error.empty() && result.knots.empty() && equal(old, original) &&
                  equal(incoming, replacement),
              "invalid splice refuses explicitly with no output or input mutation");
    };
    const std::vector<SpliceKnot> baseline{point(0, 0, 2, 0.6f), point(100, 1)};
    const std::vector<SpliceKnot> replacement{point(25, 0.4), point(75, 0.8)};
    const SpliceBounds span{{25, 25}, {75, 75}};
    reject(baseline, INFINITY, span, replacement);
    reject(baseline, 0, {{25, 25}, {25, 75}}, replacement);
    reject(baseline, 0, {{25, 25}, {75, 25}}, replacement);
    reject(baseline, 0, {{-1, 25}, {75, 75}}, replacement);
    reject(baseline, 0, {{25, 125}, {75, 175}}, replacement);
    reject(baseline, 0, span, {});
    auto projectedBegin = point(10, 0.4), projectedEnd = point(20, 0.8);
    projectedBegin.sample = 1010;
    projectedEnd.sample = 1020;
    reject({point(1000, 0.25)}, 0, {{10, 1010}, {20, 1020}}, {projectedBegin, projectedEnd});
    reject(baseline, 0, span, {point(26, 0.4), point(75, 0.8)});
    reject(baseline, 0, span, replacement, 0);
    reject(baseline, 0, span, replacement, 3);
    reject(baseline, 0, span, replacement, (1u << 23) + 1);
    auto bad = baseline;
    bad.at(0).point.time = -1;
    reject(bad, 0, span, replacement);
    bad = baseline;
    bad.at(0).point.value = NAN;
    reject(bad, 0, span, replacement);
    bad = baseline;
    bad.at(0).point.tension = 1.1f;
    reject(bad, 0, span, replacement);
    bad = baseline;
    bad.at(0).point.curve = 6;
    reject(bad, 0, span, replacement);
    bad = baseline;
    bad.at(0).point.flags = 1;
    reject(bad, 0, span, replacement);
    bad = baseline;
    bad.at(0).point.reserved = 1;
    reject(bad, 0, span, replacement);
    bad = baseline;
    bad.at(1).sample = (std::int64_t{1} << 50) + 1;
    reject(bad, 0, span, replacement);
    bad = baseline;
    bad.at(1).sample = -1;
    reject(bad, 0, span, replacement);
    bad = baseline;
    std::reverse(bad.begin(), bad.end());
    reject(bad, 0, span, replacement);
    bad = baseline;
    bad.at(1).point.time = 0;
    reject(bad, 0, span, replacement);
    bad = baseline;
    bad.at(0).slice = engine::CurveSlice{0.7, 0.2, 0, 1};
    reject(bad, 0, span, replacement);
    bad = baseline;
    bad.at(1).slice = engine::CurveSlice{0, 1, 0, 1};
    reject(bad, 0, span, replacement);
    bad = replacement;
    bad.back().slice = engine::CurveSlice{0, 1, 0, 1};
    reject(baseline, 0, span, bad);
    bad = baseline;
    bad.at(0).slice = engine::CurveSlice{std::nextafter(1.0, 0.0), 1, 0, 1};
    reject(bad, 0, span, replacement);
    const auto native = INT64_MAX - 300, sample = (std::int64_t{1} << 50) - 300;
    std::vector<SpliceKnot> wide{point(native, -1, 4, 0.8f, UINT64_MAX), point(INT64_MAX, 1)};
    wide.at(0).sample = sample;
    wide.at(1).sample = sample + 300;
    auto a = point(native + 25, 0.2), b = point(native + 275, 0.8);
    a.sample = sample + 25;
    b.sample = sample + 275;
    const std::vector<SpliceKnot> inside{a, b};
    const SpliceBounds bounds{{a.point.time, a.sample}, {b.point.time, b.sample}};
    const auto result = planEnvelopeSplice(wide, 0, bounds, inside);
    check(result.ok, "full int64 native positions and the admitted sample bound remain exact "
                     "without native subtraction");
    rendered(result, wide, 0, bounds, inside, sample, sample + 301);
}
void seededSplices() {
    std::uint32_t seed = 0xa0050217;
    const auto next = [&] {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return seed;
    };
    std::vector<SpliceKnot> original;
    std::int64_t at = 0;
    for (std::size_t i = 0; i < 16; ++i) {
        if (i % 5 != 0)
            at += 5 + next() % 30;
        auto knot =
            point(at, static_cast<double>(next() % 1000) / 1000, static_cast<std::uint8_t>(i % 6),
                  static_cast<float>(static_cast<int>(next() % 201) - 100) / 100,
                  i % 3 == 0 ? 0 : UINT64_MAX - i);
        if (i + 1 < 16 && i % 4 == 0)
            knot.slice = engine::CurveSlice{0.2, 0.8, -1, 2};
        original.push_back(knot);
    }
    const auto untouched = original;
    for (int iteration = 0; iteration < 120; ++iteration) {
        const auto begin = static_cast<std::int64_t>(next()) % (at + 30),
                   end = begin + 1 + next() % 100;
        const std::vector<SpliceKnot> replacement{point(begin, 0.1, 2, -0.6f), point(end, 0.9)};
        const SpliceBounds bounds{{begin, begin}, {end, end}};
        const auto plan = planEnvelopeSplice(original, 0.25, bounds, replacement);
        check(plan.ok && ordered(plan) && equal(original, untouched),
              "seeded explicit replacements preserve immutable input and monotone output");
        rendered(plan, original, 0.25, bounds, replacement, 0, at + 140);
    }
}
} // namespace
int main() {
    try {
        curveFamilies();
        stepsAndQuantization();
        defaultsAndOwnership();
        limitsAndRefusals();
        seededSplices();
    } catch (const std::exception &error) {
        check(false, error.what());
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
