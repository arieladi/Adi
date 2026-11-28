// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/automation_splice.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace adi::automation {
namespace {
constexpr std::int64_t sampleLimit = std::int64_t{1} << 50;
constexpr std::size_t hardPointLimit = 1u << 23;
void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
void validate(std::span<const SpliceKnot> knots) {
    for (std::size_t i = 0; i < knots.size(); ++i) {
        const auto &knot = knots[i];
        const auto &p = knot.point;
        require(p.time >= 0 && knot.sample >= 0 && knot.sample <= sampleLimit,
                "automation splice position exceeds supported clock precision");
        require(std::isfinite(p.value) && std::isfinite(p.tension) && p.tension >= -1 &&
                    p.tension <= 1 && p.curve <= engine::kMaxCurve && !p.flags && !p.reserved,
                "automation splice needs supported point values, curves and flags");
        if (i)
            require(knots[i - 1].point.time <= p.time && knots[i - 1].sample <= knot.sample &&
                        (knots[i - 1].point.time != p.time || knots[i - 1].sample == knot.sample),
                    "automation splice points or projected clocks are inconsistent");
        if (knot.slice) {
            const auto &d = *knot.slice;
            require(i + 1 < knots.size() && std::isfinite(d.begin) && std::isfinite(d.end) &&
                        std::isfinite(d.sourceFirst) && std::isfinite(d.sourceLast) &&
                        d.begin >= 0 && d.end <= 1 && d.begin < d.end,
                    "automation splice has an invalid outgoing curve domain");
        }
    }
}
std::size_t atNative(std::span<const SpliceKnot> knots, std::int64_t position) {
    return static_cast<std::size_t>(
        std::lower_bound(knots.begin(), knots.end(), position,
                         [](const auto &knot, std::int64_t at) { return knot.point.time < at; }) -
        knots.begin());
}
void validateCut(std::span<const SpliceKnot> knots, SplicePosition at) {
    const auto i = atNative(knots, at.native);
    require(
        (!i || knots[i - 1].sample <= at.sample) &&
            (i == knots.size() || knots[i].sample >= at.sample) &&
            (i == knots.size() || knots[i].point.time != at.native || knots[i].sample == at.sample),
        "automation splice boundary contradicts the captured clock projection");
}
std::size_t afterSample(std::span<const SpliceKnot> knots, std::int64_t sample) {
    return static_cast<std::size_t>(
        std::upper_bound(knots.begin(), knots.end(), sample,
                         [](std::int64_t at, const auto &knot) { return at < knot.sample; }) -
        knots.begin());
}
double boundaryValue(std::span<const SpliceKnot> knots, double fallback, std::int64_t sample,
                     std::size_t i) {
    if (knots.empty())
        return fallback;
    if (!i)
        return knots.front().point.value;
    const auto &a = knots[i - 1];
    if (i == knots.size() || a.sample == sample)
        return a.point.value;
    const auto &b = knots[i];
    require(b.sample > a.sample, "automation splice cannot evaluate a zero-length segment");
    const auto fraction =
        static_cast<double>(sample - a.sample) / static_cast<double>(b.sample - a.sample);
    const auto result =
        engine::curveSliceValue(a.point.value, b.point.value, fraction, a.point.curve,
                                a.point.tension, a.slice ? &*a.slice : nullptr);
    require(std::isfinite(result), "automation splice boundary value is not finite");
    return result;
}
SplicedKnot retained(const SpliceKnot &knot, SplicePart part, std::size_t index) {
    const SpliceOrigin origin{part, index};
    return {knot, origin, knot.slice ? std::optional(origin) : std::nullopt};
}
SplicedKnot guard(SplicePosition at, double v, std::size_t index) {
    SplicedKnot result;
    result.knot.point.time = at.native;
    result.knot.sample = at.sample;
    result.knot.point.value = v;
    result.pointOrigin = {SplicePart::Guard, index};
    return result;
}
void retainDomain(SplicedKnot &output, std::span<const SpliceKnot> source, std::size_t index,
                  std::int64_t beginSample, std::int64_t endSample) {
    require(index + 1 < source.size(), "automation splice curve has no following point");
    const auto &first = source[index];
    const auto &last = source[index + 1];
    // A native interval can have no rendered sample. Its zero-length outgoing
    // segment carries no range; surviving same-sample points still win in order.
    if (beginSample == endSample) {
        output.knot.slice.reset();
        output.rangeOrigin.reset();
        return;
    }
    require(first.sample <= beginSample && beginSample < endSample && endSample <= last.sample &&
                first.sample < last.sample,
            "automation splice curve bounds contradict its sample clock");
    const bool wholeSegment = beginSample == first.sample && endSample == last.sample;
    if (!first.slice && (wholeSegment || first.point.curve <= 1)) {
        output.knot.slice.reset();
        output.rangeOrigin.reset();
        return;
    }
    const auto whole =
        first.slice.value_or(engine::CurveSlice{0, 1, first.point.value, last.point.value});
    const auto fraction = [&](std::int64_t at) {
        return static_cast<double>(at - first.sample) /
               static_cast<double>(last.sample - first.sample);
    };
    const auto lo = beginSample == first.sample
                        ? whole.begin
                        : whole.begin + (whole.end - whole.begin) * fraction(beginSample);
    const auto hi = endSample == last.sample
                        ? whole.end
                        : whole.begin + (whole.end - whole.begin) * fraction(endSample);
    require(std::isfinite(lo) && std::isfinite(hi) && lo < hi,
            "automation splice cannot preserve the requested curve progress precision");
    output.knot.slice = engine::CurveSlice{lo, hi, whole.sourceFirst, whole.sourceLast};
    output.rangeOrigin = SpliceOrigin{SplicePart::Original, index};
}
constexpr auto hold = static_cast<std::uint8_t>(engine::CurveShape::Hold);
constexpr auto linear = static_cast<std::uint8_t>(engine::CurveShape::Linear);
constexpr auto exponential = static_cast<std::uint8_t>(engine::CurveShape::Exp);
constexpr auto logarithmic = static_cast<std::uint8_t>(engine::CurveShape::Log);

double phaseAt(std::int64_t sample, std::span<const SpliceKnot> points) {
    return static_cast<double>(sample - points.front().sample) /
           static_cast<double>(points.back().sample - points.front().sample);
}

// Linear segments need only knot checks. For an exponential or logarithm,
// the error between neighboring source knots can also peak where slopes match.
bool fitsCurve(std::span<const SpliceKnot> points, std::uint8_t curve, float tension,
               double tolerance) {
    const double first = points.front().point.value, last = points.back().point.value;
    double scale = 0;
    for (const auto &point : points)
        scale = std::max(scale, std::abs(point.point.value));
    const auto allowed = tolerance - 16 * (std::numeric_limits<double>::epsilon() * scale +
                                           std::numeric_limits<double>::denorm_min());
    const auto fits = [&](double phase, double value) {
        return std::abs(engine::curveValue(first, last, phase, curve, tension) - value) <= allowed;
    };
    for (const auto &point : points)
        if (!fits(phaseAt(point.sample, points), point.point.value))
            return false;

    if (curve == linear)
        return true;
    // Log with t is the reflected exponential with -t.
    const double k = static_cast<double>(tension) * engine::kExpRange *
                     (curve == logarithmic ? -1.0 : 1.0);
    if (k == 0)
        return true;
    const double startSlope = (last - first) * (k / std::expm1(k));
    if (startSlope == 0 || !std::isfinite(startSlope))
        return false;
    const auto logStartSlope = std::log(std::abs(startSlope));
    for (std::size_t i = 1; i < points.size(); ++i) {
        const auto &a = points[i - 1], &b = points[i];
        const auto left = phaseAt(a.sample, points), right = phaseAt(b.sample, points);
        const auto sourceSlope = (b.point.value - a.point.value) / (right - left);
        if (!std::isfinite(sourceSlope))
            return false;
        if (sourceSlope == 0 || std::signbit(sourceSlope) != std::signbit(startSlope))
            continue;
        const auto extremum = (std::log(std::abs(sourceSlope)) - logStartSlope) / k;
        if (extremum > left && extremum < right &&
            !fits(extremum, std::lerp(a.point.value, b.point.value,
                                     (extremum - left) / (right - left))))
            return false;
    }
    return true;
}

std::optional<SpliceKnot> fitSegment(std::span<const SpliceKnot> points, double tolerance) {
    auto fitted = points.front();
    fitted.point.curve = linear;
    fitted.point.tension = 0;
    if (fitsCurve(points, linear, 0, tolerance))
        return fitted;
    const auto &middle = points[points.size() / 2];
    const auto delta = points.back().point.value - points.front().point.value;
    if (delta == 0 || !std::isfinite(delta))
        return {};
    const auto desired = (middle.point.value - points.front().point.value) / delta;
    if (!(desired > 0 && desired < 1))
        return {};
    const auto phase = phaseAt(middle.sample, points);
    const auto curve = desired < phase ? exponential : logarithmic;
    double lower = 0, upper = 1;
    for (int iteration = 0; iteration < 32; ++iteration) {
        const double tension = (lower + upper) * 0.5;
        const auto actual = engine::curveShape(curve, phase, tension);
        if (curve == exponential ? actual > desired : actual < desired)
            lower = tension;
        else
            upper = tension;
    }
    fitted.point.curve = curve;
    fitted.point.tension = static_cast<float>((lower + upper) * 0.5);
    return fitsCurve(points, curve, fitted.point.tension, tolerance) ? std::optional(fitted)
                                                                    : std::nullopt;
}

void appendReducedRun(std::span<const SpliceKnot> original, std::size_t first, std::size_t last,
                      double tolerance, std::vector<SplicedKnot> &out) {
    if (last == first + 1) {
        out.push_back(retained(original[first], SplicePart::Original, first));
        return;
    }
    std::vector<std::pair<std::size_t, std::size_t>> pending{{first, last}};
    while (!pending.empty()) {
        const auto [left, right] = pending.back();
        pending.pop_back();
        if (right == left + 1) {
            out.push_back(retained(original[left], SplicePart::Original, left));
        } else if (const auto fitted = fitSegment(original.subspan(left, right - left + 1), tolerance)) {
            out.push_back({*fitted, {SplicePart::Original, left}, {}});
        } else {
            // Balanced splits bound fit work to O(n log n), including noisy
            // runs where nearly every recorded point needs to survive.
            const auto middle = left + (right - left) / 2;
            pending.emplace_back(middle, right);
            pending.emplace_back(left, middle);
        }
    }
}

} // namespace
SplicePlan planEnvelopeSplice(std::span<const SpliceKnot> original, double defaultValue,
                              SpliceBounds bounds, std::span<const SpliceKnot> replacement,
                              std::size_t pointBudget) {
    SplicePlan result;
    try {
        require(pointBudget > 0 && pointBudget <= hardPointLimit &&
                    original.size() <= pointBudget && replacement.size() <= pointBudget,
                "automation splice point budget exceeded");
        require(std::isfinite(defaultValue), "automation splice default is not finite");
        require(bounds.begin.native >= 0 && bounds.begin.native < bounds.end.native &&
                    bounds.begin.sample >= 0 && bounds.begin.sample < bounds.end.sample &&
                    bounds.end.sample <= sampleLimit,
                "automation splice needs ordered native and sample bounds");
        validate(original);
        validate(replacement);
        validateCut(original, bounds.begin);
        validateCut(original, bounds.end);
        require(replacement.size() >= 2 && replacement.front().point.time == bounds.begin.native &&
                    replacement.front().sample == bounds.begin.sample &&
                    replacement.back().point.time == bounds.end.native &&
                    replacement.back().sample == bounds.end.sample && !replacement.back().slice,
                "automation splice replacement must cover both boundaries and end without an "
                "outgoing range");
        const auto prefix = atNative(original, bounds.begin.native),
                   suffix = atNative(original, bounds.end.native);
        const bool exactEnd =
            suffix < original.size() && original[suffix].point.time == bounds.end.native;
        const auto outside = prefix + original.size() - suffix;
        const std::size_t guards = exactEnd ? 1u : 2u;
        require(outside <= pointBudget && replacement.size() <= pointBudget - outside &&
                    guards <= pointBudget - outside - replacement.size(),
                "automation splice output point budget exceeded");
        result.knots.reserve(outside + replacement.size() + guards);
        for (std::size_t i = 0; i < prefix; ++i)
            result.knots.push_back(retained(original[i], SplicePart::Original, i));
        if (prefix && prefix < original.size())
            retainDomain(result.knots.back(), original, prefix - 1, original[prefix - 1].sample,
                         bounds.begin.sample);
        // The native cut determines which coincident points survived on the
        // left. If the clock later separates them, the guard holds the last
        // of those points up to the start of the replacement.
        result.knots.push_back(guard(
            bounds.begin, boundaryValue(original, defaultValue, bounds.begin.sample, prefix), 0));
        for (std::size_t i = 0; i < replacement.size(); ++i)
            result.knots.push_back(retained(replacement[i], SplicePart::Replacement, i));
        if (!exactEnd) {
            const auto after = afterSample(original, bounds.end.sample);
            auto outgoing = guard(
                bounds.end, boundaryValue(original, defaultValue, bounds.end.sample, after), 1);
            if (after && after < original.size() &&
                (suffix == original.size() || original[suffix].sample > bounds.end.sample)) {
                const auto index = after - 1;
                outgoing.knot.point.curve = original[index].point.curve;
                outgoing.knot.point.tension = original[index].point.tension;
                retainDomain(outgoing, original, index, bounds.end.sample, original[after].sample);
            }
            result.knots.push_back(std::move(outgoing));
        }
        for (std::size_t i = suffix; i < original.size(); ++i)
            result.knots.push_back(retained(original[i], SplicePart::Original, i));
        result.ok = true;
    } catch (const std::exception &error) {
        result.error = error.what();
        result.knots.clear();
    }
    return result;
}
SplicePlan planEnvelopeSimplification(std::span<const SpliceKnot> original, SpliceBounds bounds,
                                      double tolerance, std::size_t pointBudget) {
    SplicePlan result;
    try {
        require(pointBudget > 0 && pointBudget <= hardPointLimit && original.size() <= pointBudget,
                "automation splice point budget exceeded");
        require(std::isfinite(tolerance) && tolerance >= 0,
                "Simplify Envelope needs a finite nonnegative tolerance");
        require(bounds.begin.native >= 0 && bounds.begin.native < bounds.end.native &&
                    bounds.begin.sample >= 0 && bounds.begin.sample < bounds.end.sample &&
                    bounds.end.sample <= sampleLimit,
                "Simplify Envelope needs ordered native and sample bounds");
        validate(original);
        validateCut(original, bounds.begin);
        validateCut(original, bounds.end);
        if (original.size() > 1)
            require(std::any_of(original.begin(), original.end() - 1,
                        [](const auto &knot) { return knot.point.curve != hold; }),
                    "Simplify Envelope does not change a Hold envelope");

        const auto eligible = [&](std::size_t left) {
            if (left + 1 >= original.size())
                return false;
            const auto &a = original[left], &b = original[left + 1];
            return a.point.time >= bounds.begin.native && b.point.time <= bounds.end.native &&
                   a.sample < b.sample && !a.slice && (a.point.curve == linear || a.point.curve == hold);
        };
        result.knots.reserve(original.size());
        for (std::size_t i = 0; i < original.size();) {
            if (!eligible(i)) {
                result.knots.push_back(retained(original[i], SplicePart::Original, i));
                ++i;
                continue;
            }
            const auto first = i;
            const auto curve = original[first].point.curve;
            while (eligible(i) && original[i].point.curve == curve)
                ++i;
            if (curve == linear) {
                appendReducedRun(original, first, i, tolerance, result.knots);
            } else {
                // On a continuous lane, retain held steps larger than the bound
                // and thin only redundant or near-identical recorded levels.
                result.knots.push_back(retained(original[first], SplicePart::Original, first));
                auto anchor = first;
                for (auto j = first + 1; j < i; ++j) {
                    const auto level = original[j].point.value;
                    if (std::abs(level - original[anchor].point.value) > tolerance ||
                        std::abs(level - original[j - 1].point.value) > tolerance ||
                        std::abs(level - original[j + 1].point.value) > tolerance) {
                        result.knots.push_back(retained(original[j], SplicePart::Original, j));
                        anchor = j;
                    }
                }
            }
        }
        if (result.knots.size() == original.size())
            result.knots.clear();
        result.ok = true;
    } catch (const std::exception &error) {
        result.error = error.what();
        result.knots.clear();
    }
    return result;
}
} // namespace adi::automation
