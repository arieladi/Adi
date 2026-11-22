// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/automation_splice_store.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/clip_image.hpp"
#include "adi/edit_time.hpp"
#include "adi/op_transaction_resources_detail.hpp"
#include "adi/row_capture.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <sqlite3.h>
#include <stdexcept>

namespace adi::automation {
namespace {
std::int64_t integer(const Payload &value) { return detail::signedInteger(value); }
void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
std::int64_t sampleAt(const edit::TimeMap &time, int base, std::int64_t native, double rate) {
    require(native >= 0, "automation splice needs nonnegative native positions");
    const double sample =
        (base == 0 ? time.seconds(native) : static_cast<double>(native) * 1e-9) * rate;
    require(std::isfinite(sample) && sample >= 0 && sample <= 0x1p50,
            "automation splice exceeds sample precision");
    return static_cast<std::int64_t>(std::llround(sample));
}
struct Envelope {
    StreamHeader header{};
    std::vector<SpliceKnot> knots;
    std::vector<std::uint8_t> trailer;
    std::map<std::size_t, Payload> ranges;
};
Envelope decode(const Payload &row, int base, const edit::TimeMap &time, double rate,
                std::size_t budget) {
    const auto &bytes = row.at("data").get_binary();
    StreamReader<AutomationPoint> reader(
        {reinterpret_cast<const std::byte *>(bytes.data()), bytes.size()}, FourCC::Automation);
    require(
        reader.ok() && reader.header().version == 1 &&
            reader.header().rec_size == sizeof(AutomationPoint) &&
            !(reader.header().flags & ~(StreamFlags::SortedByTime | StreamFlags::TimeIsNanos)) &&
            ((reader.header().flags & StreamFlags::TimeIsNanos) != 0) == (base == 1),
        "automation splice needs current AAUT layout and its declared clock");
    require(reader.count() <= budget, "automation splice input exceeds point budget");
    Envelope result;
    result.header = reader.header();
    for (std::uint32_t i = 0; i < reader.count(); ++i) {
        const auto point = *reader.at(i);
        result.knots.push_back({point, sampleAt(time, base, point.time, rate), {}});
    }
    const auto end = sizeof(StreamHeader) + result.knots.size() * sizeof(AutomationPoint);
    result.trailer.assign(bytes.begin() + static_cast<std::ptrdiff_t>(end), bytes.end());
    return result;
}
Payload encode(const Envelope &envelope, const std::vector<SplicedKnot> &knots) {
    require(knots.size() <= std::numeric_limits<std::uint32_t>::max(),
            "automation record count exhausted");
    auto header = envelope.header;
    header.count = static_cast<std::uint32_t>(knots.size());
    std::vector<std::uint8_t> bytes(sizeof header + knots.size() * sizeof(AutomationPoint));
    std::memcpy(bytes.data(), &header, sizeof header);
    for (std::size_t i = 0; i < knots.size(); ++i)
        std::memcpy(bytes.data() + sizeof header + i * sizeof(AutomationPoint),
                    &knots[i].knot.point, sizeof(AutomationPoint));
    bytes.insert(bytes.end(), envelope.trailer.begin(), envelope.trailer.end());
    return Payload::binary(std::move(bytes));
}

double progress(std::int64_t sample, std::span<const SpliceKnot> points) {
    return static_cast<double>(sample - points.front().sample) /
           static_cast<double>(points.back().sample - points.front().sample);
}

bool fitsCurve(std::span<const SpliceKnot> points, std::uint8_t curve, float tension,
               double tolerance) {
    const double first = points.front().point.value, last = points.back().point.value;
    double scale = 0;
    for (const auto &point : points)
        scale = std::max(scale, std::abs(point.point.value));
    const double allowed = tolerance - 16 * (std::numeric_limits<double>::epsilon() * scale +
                                             std::numeric_limits<double>::denorm_min());
    if (allowed < 0)
        return false;
    const auto fits = [&](double x, double value) {
        return std::abs(engine::curveValue(first, last, x, curve, tension) - value) <= allowed;
    };
    for (const auto &point : points) {
        if (!fits(progress(point.sample, points), point.point.value))
            return false;
    }
    const auto k = static_cast<double>(tension) * engine::kExpRange;
    if (k == 0.0)
        return true;
    const auto initialSlope = (last - first) * (k / std::expm1(k));
    if (!std::isfinite(initialSlope) || initialSlope == 0.0)
        return false;
    // Between recorded samples an exponential and a line can differ most
    // where their slopes agree. Check that point as well as every knot.
    for (std::size_t i = 1; i < points.size(); ++i) {
        const auto &a = points[i - 1], &b = points[i];
        const auto left = progress(a.sample, points), right = progress(b.sample, points);
        const auto ratio = ((b.point.value - a.point.value) / (right - left)) / initialSlope;
        if (!(ratio > 0.0) || !std::isfinite(ratio))
            continue;
        const auto x = std::log(ratio) / k;
        if (x > left && x < right &&
            !fits(x, std::lerp(a.point.value, b.point.value, (x - left) / (right - left))))
            return false;
    }
    return true;
}

std::optional<SpliceKnot> fitSegment(std::span<const SpliceKnot> points, double tolerance) {
    constexpr auto linear = static_cast<std::uint8_t>(Curve::Linear);
    constexpr auto exponential = static_cast<std::uint8_t>(Curve::Exponential);
    auto fitted = points.front();
    fitted.point.curve = linear;
    fitted.point.tension = 0;
    if (!fitsCurve(points, linear, 0.0f, tolerance)) {
        const auto &middle = points[points.size() / 2];
        const auto delta = points.back().point.value - points.front().point.value;
        const auto desired = (middle.point.value - points.front().point.value) / delta;
        if (!std::isfinite(desired) || desired <= 0.0 || desired >= 1.0)
            return {};
        const auto x = progress(middle.sample, points);
        double lo = -1.0, hi = 1.0;
        for (int iteration = 0; iteration < 32; ++iteration) {
            const double tension = (lo + hi) * 0.5;
            if (engine::curveShape(exponential, x, tension) > desired)
                lo = tension;
            else
                hi = tension;
        }
        fitted.point.curve = exponential;
        fitted.point.tension = static_cast<float>((lo + hi) * 0.5);
        if (!fitsCurve(points, exponential, fitted.point.tension, tolerance))
            return {};
    }
    return fitted;
}

std::vector<SpliceKnot> simplify(std::span<const SpliceKnot> original, std::int64_t begin,
                                std::int64_t end, double tolerance) {
    constexpr auto hold = static_cast<std::uint8_t>(Curve::Hold);
    constexpr auto linear = static_cast<std::uint8_t>(Curve::Linear);
    const auto before = [](const SpliceKnot &a, const SpliceKnot &b) {
        return a.point.time < b.point.time;
    };
    require(std::is_sorted(original.begin(), original.end(), before),
            "Simplify Envelope needs ordered source points");
    const auto first = std::lower_bound(original.begin(), original.end(), begin,
                                        [](const auto &p, auto at) { return p.point.time < at; });
    const auto last = std::upper_bound(first, original.end(), end,
                                      [](auto at, const auto &p) { return at < p.point.time; });
    const auto points = std::span(first, last);
    bool sampledLine = true;
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        const auto &a = points[i], &b = points[i + 1];
        require(std::isfinite(a.point.value) && std::isfinite(b.point.value),
                "Simplify Envelope needs finite source points");
        require(a.point.curve != hold && (a.sample != b.sample || a.point.value == b.point.value),
                "Simplify Envelope does not change Hold or stepped automation");
        sampledLine = sampledLine && a.point.curve == linear && !a.slice && a.sample < b.sample;
    }
    if (points.size() < 3 || !sampledLine)
        return {};

    // If one curve cannot fit the whole selection, split at an original
    // knot and try each side. Each depth scans disjoint spans.
    std::vector<std::pair<std::size_t, std::size_t>> pending{{0, points.size() - 1}};
    std::vector<SpliceKnot> result;
    while (!pending.empty()) {
        const auto [left, right] = pending.back();
        pending.pop_back();
        if (right == left + 1) {
            result.push_back(points[left]);
        } else if (auto fitted = fitSegment(points.subspan(left, right - left + 1), tolerance)) {
            result.push_back(std::move(*fitted));
        } else {
            const auto mid = left + (right - left) / 2;
            pending.emplace_back(mid, right);
            pending.emplace_back(left, mid);
        }
    }
    if (result.size() + 1 == points.size())
        return {};
    auto endpoint = points.back();
    endpoint.slice.reset();
    result.push_back(std::move(endpoint));
    return result;
}
} // namespace

static PreparedEnvelopeSplice prepareSplice(Store &store, std::int64_t laneId, std::int64_t begin,
                                             std::int64_t end,
                                             std::span<const NativeSpliceKnot> replacement,
                                             double rate, std::size_t budget,
                                             EnvelopeSpliceRequest intent,
                                             std::optional<double> simplification) {
    PreparedEnvelopeSplice result;
    try {
        require(!audio::onAudioThread, "automation splice belongs off the audio callback");
        require(!store.readOnly(), "project is read-only");
        require(std::isfinite(rate) && rate > 0 && rate <= 768000,
                "automation splice needs a supported session sample rate");
        require(budget > 0 && budget <= (1u << 23) && replacement.size() <= budget,
                "automation splice point budget is invalid");
        require(!intent.dataId || *intent.dataId > 0, "automation data ID must be positive");
        require(!simplification || (std::isfinite(*simplification) && *simplification >= 0.0),
                "Simplify Envelope needs a finite nonnegative tolerance");
        auto &db = store.db();
        std::optional<SQLite::Transaction> read;
        if (sqlite3_get_autocommit(db.getHandle()) != 0)
            read.emplace(db, SQLite::TransactionBehavior::DEFERRED);
        const auto head = OpJournal(store).headSeq();
        const auto lane = detail::captureRow(db, "automation_lanes", "id", laneId);
        require(lane.has_value(), "automation lane is missing");
        const auto base64 = integer(lane->at("time_base"));
        require(base64 == 0 || base64 == 1, "automation lane clock is unknown");
        const auto base = static_cast<int>(base64);
        require(lane->at("owner_kind") != "clip", "splice needs a global automation lane");
        require(!simplification || lane->at("value_domain") != "enum",
                "Simplify Envelope does not change enum automation");
        OpTransactionInputs inputs;
        inputs.rowGuards.push_back({"automation_lanes", {{"id", laneId}}, lane});
        const auto data = detail::captureGuardRows(store, "automation_data", {{"lane_id", laneId}});
        inputs.rowSetGuards.push_back({"automation_data", {{"lane_id", laneId}}, data});
        inputs.rowSetGuards.push_back(
            {"tempo_map", Payload::object(),
             detail::captureGuardRows(store, "tempo_map", Payload::object())});
        const edit::TimeMap time(db);
        const auto beginSample = sampleAt(time, base, begin, rate);
        const auto endSample = sampleAt(time, base, end, rate);
        SpliceBounds bounds{{begin, beginSample}, {end, endSample}};
        require(begin < end, "automation splice needs ordered native bounds");
        if (beginSample == endSample) {
            result.subSample = true;
            throw std::runtime_error("automation splice boundaries project to one sample");
        }
        clip_edit::CopyIds ids(db);
        std::optional<Payload> row;
        for (const auto &candidate : data) {
            if (!candidate.at("clip_id").is_null())
                continue;
            require(!row, "automation lane has multiple global streams");
            row = candidate;
        }
        Envelope original;
        std::int64_t dataId = 0;
        if (row) {
            dataId = integer(row->at("id"));
            require(!intent.dataId || *intent.dataId == dataId,
                    "automation data address differs from the lane's stream");
            original = decode(*row, base, time, rate, budget);
            const auto ranges =
                detail::captureGuardRows(store, "automation_segment_ranges", {{"data_id", dataId}});
            inputs.rowSetGuards.push_back(
                {"automation_segment_ranges", {{"data_id", dataId}}, ranges});
            for (const auto &range : ranges) {
                const auto ordinal = integer(range.at("record_index"));
                require(ordinal >= 0 &&
                            static_cast<std::uint64_t>(ordinal) + 1 < original.knots.size() &&
                            range.at("clip_id").is_null(),
                        "automation curve range has no outgoing segment");
                const auto index = static_cast<std::size_t>(ordinal);
                require(original.ranges.emplace(index, range).second,
                        "duplicate automation curve range");
                original.knots.at(index).slice =
                    engine::CurveSlice{range.at("progress_begin"), range.at("progress_end"),
                                       range.at("source_first"), range.at("source_last")};
            }
        } else {
            if (simplification) {
                result.ok = true;
                return result;
            }
            if (intent.dataId) {
                dataId = *intent.dataId;
                require(!detail::captureRow(db, "automation_data", "id", dataId),
                        "automation data ID is already used");
            } else {
                dataId = ids.next("automation_data");
            }
            inputs.rowGuards.push_back({"automation_data", {{"id", dataId}}, std::nullopt});
            original.header = {
                static_cast<std::uint32_t>(FourCC::Automation), 1, sizeof(AutomationPoint), 0,
                StreamFlags::SortedByTime | (base == 1 ? StreamFlags::TimeIsNanos : 0)};
            row = edit::defaultRow(db, "automation_data",
                                   {{"id", dataId},
                                    {"lane_id", laneId},
                                    {"clip_id", nullptr},
                                    {"data", Payload::binary(std::vector<std::uint8_t>{})}});
        }
        std::vector<SpliceKnot> incoming;
        if (simplification) {
            incoming = simplify(original.knots, begin, end, *simplification);
            if (incoming.empty()) {
                result.ok = true;
                return result;
            }
            bounds = {{incoming.front().point.time, incoming.front().sample},
                      {incoming.back().point.time, incoming.back().sample}};
        }
        for (const auto &knot : replacement) {
            require((lane->at("min_value").is_null() ||
                     knot.point.value >= lane->at("min_value").get<double>()) &&
                        (lane->at("max_value").is_null() ||
                         knot.point.value <= lane->at("max_value").get<double>()),
                    "replacement value is outside lane bounds");
            incoming.push_back(
                {knot.point, sampleAt(time, base, knot.point.time, rate), knot.slice});
        }
        auto splice = planEnvelopeSplice(
            original.knots, lane->at("default_value"), bounds, incoming, budget);
        require(splice.ok, splice.error.c_str());
        if (simplification) {
            // The existing right endpoint already carries the original outgoing
            // curve. The left guard is redundant when it has the fitted value.
            std::erase_if(splice.knots, [&](const auto &knot) {
                return (knot.pointOrigin.part == SplicePart::Replacement &&
                        knot.pointOrigin.index + 1 == incoming.size()) ||
                       (knot.pointOrigin.part == SplicePart::Guard && knot.pointOrigin.index == 0 &&
                        knot.knot.point.value == incoming.front().point.value);
            });
        }
        for (const auto &knot : original.knots)
            require((lane->at("min_value").is_null() ||
                     knot.point.value >= lane->at("min_value").get<double>()) &&
                        (lane->at("max_value").is_null() ||
                         knot.point.value <= lane->at("max_value").get<double>()),
                    "original automation value is outside lane bounds");
        std::set<std::uint64_t> used;
        for (const auto &knot : original.knots)
            used.insert(knot.point.point_id);
        std::uint64_t nextPoint = 1;
        auto freshPoint = [&]() {
            while (used.contains(nextPoint)) {
                require(nextPoint != UINT64_MAX, "automation point identities exhausted");
                ++nextPoint;
            }
            const auto fresh = nextPoint;
            used.insert(fresh);
            return fresh;
        };
        edit::RowEdits edits(db);
        std::set<std::int64_t> retained;
        for (std::size_t i = 0; i < splice.knots.size(); ++i) {
            auto &output = splice.knots[i];
            const bool retainedKnot = simplification && output.pointOrigin.part == SplicePart::Replacement;
            if (output.pointOrigin.part != SplicePart::Original && !retainedKnot)
                output.knot.point.point_id = freshPoint();
            if (!output.knot.slice)
                continue;
            std::optional<Payload> source;
            if (output.rangeOrigin && output.rangeOrigin->part == SplicePart::Original) {
                const auto found = original.ranges.find(output.rangeOrigin->index);
                if (found != original.ranges.end())
                    source = found->second;
            }
            std::int64_t rangeId;
            if (source && !retained.contains(integer(source->at("id"))))
                rangeId = integer(source->at("id"));
            else {
                rangeId = ids.next("automation_segment_ranges");
                inputs.rowGuards.push_back(
                    {"automation_segment_ranges", {{"id", rangeId}}, std::nullopt});
            }
            const auto &slice = *output.knot.slice;
            Payload range = source
                                ? *source
                                : edit::defaultRow(db, "automation_segment_ranges",
                                                   {{"id", rangeId},
                                                    {"data_id", dataId},
                                                    {"record_index", static_cast<std::int64_t>(i)},
                                                    {"progress_begin", slice.begin},
                                                    {"progress_end", slice.end},
                                                    {"source_first", slice.sourceFirst},
                                                    {"source_last", slice.sourceLast}});
            range["id"] = rangeId;
            range["data_id"] = dataId;
            range["clip_id"] = nullptr;
            range["record_index"] = static_cast<std::int64_t>(i);
            range["progress_begin"] = slice.begin;
            range["progress_end"] = slice.end;
            range["source_first"] = slice.sourceFirst;
            range["source_last"] = slice.sourceLast;
            edits.set("automation_segment_ranges", rangeId, std::move(range));
            retained.insert(rangeId);
        }
        for (const auto &[index, range] : original.ranges) {
            (void)index;
            const auto id = integer(range.at("id"));
            if (!retained.contains(id))
                edits.set("automation_segment_ranges", id, std::nullopt);
        }
        (*row)["data"] = encode(original, splice.knots);
        edits.set("automation_data", dataId, row);
        const auto changes = edits.changes();
        OpContext context{store, db};
        std::string validation;
        require(edit::validateRows(context, {{"changes", changes}}, validation),
                validation.c_str());
        OpRequest request;
        request.opType = "arranger.splice";
        request.payload = {{"id", 0}, {"changes", changes}};
        request.label = intent.label;
        request.actorDetail = intent.actorDetail;
        request.targetKind = "automation";
        request.targetId = laneId;
        result.requests.push_back(std::move(request));
        const auto token = CapturedTransactionAccess::seal(appliedSource(store), head,
                                                           result.requests, std::move(inputs));
        result.requests.front().capturedTransaction = token;
        result.ok = true;
    } catch (const std::exception &error) {
        result.error = error.what();
        result.requests.clear();
    }
    return result;
}
PreparedEnvelopeSplice prepareEnvelopeSplice(Store &store, std::int64_t lane, std::int64_t begin,
                                             std::int64_t end,
                                             std::span<const NativeSpliceKnot> replacement,
                                             double rate, std::size_t budget,
                                             EnvelopeSpliceRequest intent) {
    return prepareSplice(store, lane, begin, end, replacement, rate, budget, intent, std::nullopt);
}

PreparedEnvelopeSplice prepareSimplifyEnvelope(Store &store, std::int64_t lane, std::int64_t begin,
                                               std::int64_t end, double rate, double maxValueError) {
    return prepareSplice(store, lane, begin, end, {}, rate, kSplicePointBudget,
                         {{}, "Simplify Envelope"}, maxValueError);
}
} // namespace adi::automation
