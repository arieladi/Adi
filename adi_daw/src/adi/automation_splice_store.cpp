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
void guardRow(OpTransactionInputs *inputs, const std::string &table, std::int64_t id,
              const std::optional<Payload> &expected) {
    if (!inputs)
        return;
    const Payload key = {{"id", id}};
    const auto guarded = std::find_if(inputs->rowGuards.begin(), inputs->rowGuards.end(),
                                      [&](const auto &guard) {
        return guard.table == table && guard.primaryKey == key;
    });
    if (guarded == inputs->rowGuards.end())
        inputs->rowGuards.push_back({table, key, expected});
}
std::vector<Payload> guardedRows(const Store &store, const std::string &table,
                                const Payload &selector, OpTransactionInputs *inputs) {
    if (inputs) {
        const auto guarded = std::find_if(inputs->rowSetGuards.begin(), inputs->rowSetGuards.end(),
                                          [&](const auto &guard) {
            return guard.table == table && guard.equalColumns == selector;
        });
        if (guarded != inputs->rowSetGuards.end())
            return guarded->expectedRows;
    }
    auto stored = detail::captureGuardRows(store, table, selector);
    if (inputs)
        inputs->rowSetGuards.push_back({table, selector, stored});
    return stored;
}
std::map<std::int64_t, Payload> currentRows(std::vector<Payload> stored, const char *table,
                                           const char *parentColumn, std::int64_t parentId,
                                           const Payload &changes) {
    std::map<std::int64_t, Payload> current;
    for (auto &row : stored) {
        const auto id = integer(row.at("id"));
        current.emplace(id, std::move(row));
    }
    for (const auto &change : changes) {
        if (change.at("table") != table)
            continue;
        const auto &before = change.at("before");
        const auto &after = change.at("after");
        const auto id = integer(change.at("id"));
        if (!after.is_null() && integer(after.at(parentColumn)) == parentId)
            current.insert_or_assign(id, after);
        else if (!before.is_null() && integer(before.at(parentColumn)) == parentId)
            current.erase(id);
    }
    return current;
}
} // namespace

static void stageSplice(Store &store, edit::RowEdits &edits, clip_edit::CopyIds &ids,
                        const edit::TimeMap &time, std::int64_t laneId, std::int64_t begin,
                        std::int64_t end, std::span<const NativeSpliceKnot> replacement,
                        double rate, OpTransactionInputs *guards, std::size_t budget,
                        std::optional<double> simplification) {
    require(!audio::onAudioThread, "automation splice belongs off the audio callback");
    require(!store.readOnly(), "project is read-only");
    require(std::isfinite(rate) && rate > 0 && rate <= 768000,
            "automation splice needs a supported session sample rate");
    require(budget > 0 && budget <= (1u << 23) && replacement.size() <= budget,
            "automation splice point budget is invalid");
    auto &db = store.db();
    const auto changes = edits.changes();
    const auto storedLane = detail::captureRow(db, "automation_lanes", "id", laneId);
    auto lane = storedLane;
    for (const auto &change : changes) {
        if (change.at("table") == "automation_lanes" && integer(change.at("id")) == laneId) {
            const auto &after = change.at("after");
            lane = after.is_null() ? std::nullopt : std::optional<Payload>{after};
            break;
        }
    }
    require(lane.has_value(), "automation lane is missing");
    const auto base64 = integer(lane->at("time_base"));
    require(base64 == 0 || base64 == 1, "automation lane clock is unknown");
    const auto base = static_cast<int>(base64);
    require(lane->at("owner_kind") != "clip", "splice needs a global automation lane");
    require(!simplification || lane->at("value_domain") != "enum",
            "Simplify Envelope does not change enum automation");
    require(!simplification || (std::isfinite(*simplification) && *simplification >= 0),
            "Simplify Envelope needs a finite nonnegative tolerance");
    guardRow(guards, "automation_lanes", laneId, storedLane);
    if (guards)
        (void)guardedRows(store, "tempo_map", Payload::object(), guards);
    const auto data = currentRows(guardedRows(store, "automation_data", {{"lane_id", laneId}}, guards),
                                  "automation_data", "lane_id", laneId, changes);
    std::optional<Payload> row;
    for (const auto &[id, candidate] : data) {
        (void)id;
        if (!candidate.at("clip_id").is_null())
            continue;
        require(!row, "automation lane has multiple global streams");
        row = candidate;
    }
    Envelope original;
    std::int64_t dataId = 0;
    if (row) {
        dataId = integer(row->at("id"));
        original = decode(*row, base, time, rate, budget);
        const auto ranges = currentRows(
            guardedRows(store, "automation_segment_ranges", {{"data_id", dataId}}, guards),
            "automation_segment_ranges", "data_id", dataId, changes);
        for (const auto &[id, range] : ranges) {
            (void)id;
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
        if (simplification)
            return;
        dataId = ids.next("automation_data");
        guardRow(guards, "automation_data", dataId, std::nullopt);
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
    for (const auto &knot : replacement) {
        require((lane->at("min_value").is_null() ||
                 knot.point.value >= lane->at("min_value").get<double>()) &&
                    (lane->at("max_value").is_null() ||
                     knot.point.value <= lane->at("max_value").get<double>()),
                "replacement value is outside lane bounds");
        incoming.push_back(
            {knot.point, sampleAt(time, base, knot.point.time, rate), knot.slice});
    }
    const SpliceBounds bounds{{begin, sampleAt(time, base, begin, rate)},
                              {end, sampleAt(time, base, end, rate)}};
    auto splice = simplification
        ? planEnvelopeSimplification(original.knots, bounds, *simplification, budget)
        : planEnvelopeSplice(original.knots, lane->at("default_value"), bounds, incoming, budget);
    require(splice.ok, splice.error.c_str());
    if (splice.knots.empty())
        return;
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
    std::set<std::int64_t> retained;
    for (std::size_t i = 0; i < splice.knots.size(); ++i) {
        auto &output = splice.knots[i];
        if (output.pointOrigin.part != SplicePart::Original)
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
            guardRow(guards, "automation_segment_ranges", rangeId, std::nullopt);
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
}

void stageEnvelopeSplice(Store &store, edit::RowEdits &edits, clip_edit::CopyIds &ids,
                         const edit::TimeMap &time, std::int64_t laneId, std::int64_t begin,
                         std::int64_t end, std::span<const NativeSpliceKnot> replacement,
                         double rate, OpTransactionInputs *guards, std::size_t budget) {
    stageSplice(store, edits, ids, time, laneId, begin, end, replacement, rate, guards, budget, {});
}

static PreparedEnvelopeSplice prepareSplice(Store &store, std::int64_t laneId, std::int64_t begin,
                                             std::int64_t end,
                                             std::span<const NativeSpliceKnot> replacement,
                                             double rate, std::size_t budget, std::string_view label,
                                             std::optional<Payload> selection,
                                             std::optional<double> simplification) {
    PreparedEnvelopeSplice result;
    try {
        require(!audio::onAudioThread, "automation splice belongs off the audio callback");
        require(!store.readOnly(), "project is read-only");
        auto &db = store.db();
        std::optional<SQLite::Transaction> read;
        if (sqlite3_get_autocommit(db.getHandle()) != 0)
            read.emplace(db, SQLite::TransactionBehavior::DEFERRED);
        const auto head = OpJournal(store).headSeq();
        OpTransactionInputs inputs;
        const edit::TimeMap time(db);
        clip_edit::CopyIds ids(db);
        edit::RowEdits edits(db);
        stageSplice(store, edits, ids, time, laneId, begin, end, replacement, rate,
                    &inputs, budget, simplification);
        const auto changes = edits.changes();
        if (simplification && changes.empty()) {
            result.ok = true;
            return result;
        }
        OpContext context{store, db};
        std::string validation;
        require(edit::validateRows(context, {{"changes", changes}}, validation),
                validation.c_str());
        OpRequest request;
        request.opType = "arranger.splice";
        request.payload = {{"id", 0}, {"changes", changes}};
        request.label = label;
        request.selBefore = std::move(selection);
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
PreparedEnvelopeSplice prepareEnvelopeSplice(Store &store, std::int64_t laneId, std::int64_t begin,
                                             std::int64_t end,
                                             std::span<const NativeSpliceKnot> replacement,
                                             double rate, std::size_t budget, std::string_view label,
                                             std::optional<Payload> selection) {
    return prepareSplice(store, laneId, begin, end, replacement, rate, budget, label,
                         std::move(selection), {});
}

PreparedEnvelopeSplice prepareSimplifyEnvelope(Store &store, std::int64_t laneId, std::int64_t begin,
                                                std::int64_t end, double rate, double maxValueError,
                                                std::optional<Payload> selection) {
    return prepareSplice(store, laneId, begin, end, {}, rate, kSplicePointBudget, "Simplify Envelope",
                         std::move(selection), maxValueError);
}
} // namespace adi::automation
