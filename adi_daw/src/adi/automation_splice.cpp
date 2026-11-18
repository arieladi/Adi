// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/automation_splice.hpp"
#include <algorithm>
#include <cmath>
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
double value(std::span<const SpliceKnot> knots, double fallback, std::int64_t sample,
             bool incoming) {
    if (knots.empty())
        return fallback;
    const auto i =
        incoming ? static_cast<std::size_t>(std::lower_bound(knots.begin(), knots.end(), sample,
                                                             [](const auto &knot, std::int64_t at) {
                                                                 return knot.sample < at;
                                                             }) -
                                            knots.begin())
                 : afterSample(knots, sample);
    if (!i)
        return knots.front().point.value;
    if (i == knots.size())
        return knots.back().point.value;
    const auto &a = knots[i - 1];
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
    if (!first.slice && first.point.curve <= 1) {
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
        result.knots.push_back(
            guard(bounds.begin, value(original, defaultValue, bounds.begin.sample, true), 0));
        for (std::size_t i = 0; i < replacement.size(); ++i)
            result.knots.push_back(retained(replacement[i], SplicePart::Replacement, i));
        if (!exactEnd) {
            auto outgoing =
                guard(bounds.end, value(original, defaultValue, bounds.end.sample, false), 1);
            const auto after = afterSample(original, bounds.end.sample);
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
} // namespace adi::automation
