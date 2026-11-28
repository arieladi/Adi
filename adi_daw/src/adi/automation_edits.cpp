// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/automation_edits.hpp"
#include "adi/blob.hpp"
#include "adi/automation_range_rows.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <cmath>
#include <climits>
#include <cstring>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <stdexcept>

namespace adi::automation {
namespace {
using Bytes = std::vector<std::byte>;
struct Data {
    bool present = false;
    Bytes bytes;
    Payload ranges = Payload::array();
    int timeBase = 0;
    std::optional<double> minimum, maximum;
};

std::int64_t integer(const Payload& p) {
    if (!p.is_number_integer() || (p.is_number_unsigned() &&
        p.get<std::uint64_t>() > static_cast<std::uint64_t>(INT64_MAX)))
        throw std::runtime_error("automation: expected int64");
    return p.get<std::int64_t>();
}
std::uint64_t identity(const Payload& p) {
    if (!p.is_number_integer() || (!p.is_number_unsigned() && p.get<std::int64_t>() <= 0))
        throw std::runtime_error("automation: point ID must be a positive uint64");
    const auto id = p.get<std::uint64_t>();
    if (!id) throw std::runtime_error("automation: zero point ID");
    return id;
}
/// The optional `clip` field: absent reads as null, an arrangement stream, as
/// the op's schema says (`clip` is not required). The handlers used to read it
/// with at(), so a changeset that followed the schema and left it out failed.
const Payload& clipOf(const Payload& p) {
    static const Payload none;
    const auto it = p.find("clip");
    return it == p.end() ? none : *it;
}
Payload address(const Payload& p) {
    return {{"id", p.at("id")}, {"lane", p.at("lane")}, {"clip", clipOf(p)}};
}
Data read(OpContext& c, const Payload& p) {
    const auto id = integer(p.at("id")), lane = integer(p.at("lane"));
    if (id <= 0 || lane <= 0) throw std::runtime_error("automation: positive row IDs required");
    SQLite::Statement owner(c.db, "SELECT time_base,min_value,max_value FROM automation_lanes WHERE id=?");
    owner.bind(1, lane);
    if (!owner.executeStep()) throw std::runtime_error("automation: missing lane");
    Data result;
    result.timeBase = owner.getColumn(0).getInt();
    if (!owner.getColumn(1).isNull()) result.minimum = owner.getColumn(1).getDouble();
    if (!owner.getColumn(2).isNull()) result.maximum = owner.getColumn(2).getDouble();
    if (!clipOf(p).is_null()) {
        const auto clip = integer(clipOf(p));
        SQLite::Statement q(c.db, "SELECT 1 FROM clips WHERE id=?");
        q.bind(1, clip);
        if (clip <= 0 || !q.executeStep()) throw std::runtime_error("automation: missing clip");
    }
    SQLite::Statement q(c.db, "SELECT lane_id,clip_id,data FROM automation_data WHERE id=?");
    q.bind(1, id);
    if (!q.executeStep()) {
        SQLite::Statement slot(c.db, "SELECT 1 FROM automation_data WHERE lane_id=? AND clip_id IS ?");
        slot.bind(1, lane);
        if (clipOf(p).is_null()) slot.bind(2); else slot.bind(2, integer(clipOf(p)));
        if (slot.executeStep()) throw std::runtime_error("automation: slot has another data ID");
        return result;
    }
    if (q.getColumn(0).getInt64() != lane ||
        (q.getColumn(1).isNull() != clipOf(p).is_null()) ||
        (!clipOf(p).is_null() && q.getColumn(1).getInt64() != integer(clipOf(p))))
        throw std::runtime_error("automation: data address mismatch");
    result.present = true;
    const auto col = q.getColumn(2);
    const auto* b = static_cast<const std::byte*>(col.getBlob());
    if (col.getBytes() > 0) result.bytes.assign(b, b + col.getBytes());
    result.ranges=range_rows::capture(c.db,id);
    return result;
}
void store(OpContext& c, const Payload& p, const Bytes* bytes, bool exists) {
    const auto id = integer(p.at("id"));
    if (!bytes) {
        SQLite::Statement q(c.db, "DELETE FROM automation_data WHERE id=?");
        q.bind(1, id); q.exec(); return;
    }
    if (bytes->size() > static_cast<std::size_t>(INT_MAX))
        throw std::runtime_error("automation: stream exceeds SQLite binding limit");
    // SQLite distinguishes an empty BLOB from SQL NULL.
    const void* value = bytes->empty() ? static_cast<const void*>("") : bytes->data();
    const auto size = static_cast<int>(bytes->size());
    if (exists) {
        SQLite::Statement q(c.db, "UPDATE automation_data SET data=? WHERE id=?");
        q.bind(1, value, size);
        q.bind(2, id);
        q.exec();
        return;
    }
    SQLite::Statement q(c.db, "INSERT INTO automation_data(id,lane_id,clip_id,data) VALUES(?,?,?,?)");
    q.bind(1, id); q.bind(2, integer(p.at("lane")));
    if (clipOf(p).is_null()) q.bind(3); else q.bind(3, integer(clipOf(p)));
    q.bind(4, value, size);
    q.exec();
}
Payload bytesPayload(std::span<const std::byte> source) {
    std::vector<std::uint8_t> bytes(source.size());
    if (!bytes.empty()) std::memcpy(bytes.data(), source.data(), bytes.size());
    return Payload::binary(std::move(bytes));
}
Payload fullState(const Data& data) {
    return {{"present", data.present}, {"bytes", bytesPayload(data.bytes)}, {"ranges", data.ranges}};
}
bool capture(OpContext& c, const Payload& p, Payload& inv, std::string& err) {
    try {
        inv = address(p);
        inv["state"] = fullState(read(c, p));
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}
bool rangeSurvives(const AutomationPoint& oldLeft, const AutomationPoint* oldRight,
                   const AutomationPoint& newLeft, const AutomationPoint* newRight) {
    return oldLeft.value == newLeft.value && oldLeft.curve == newLeft.curve && oldLeft.tension == newLeft.tension &&
           (oldRight ? newRight && oldRight->point_id == newRight->point_id && oldRight->value == newRight->value
                     : newRight == nullptr);
}
std::size_t ordinal(const Payload& value, std::size_t count) {
    const auto index = integer(value);
    if (index < 0 || static_cast<std::uint64_t>(index) >= count)
        throw std::runtime_error("automation: record index outside stream");
    return static_cast<std::size_t>(index);
}
Payload droppedRanges(const Payload& ranges, const std::vector<AutomationPoint>& original,
                      const std::vector<AutomationPoint>& edited) {
    std::vector<std::size_t> order(edited.size()), newPosition(edited.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return edited[a].time < edited[b].time; });
    for (std::size_t i = 0; i < order.size(); ++i) newPosition[order[i]] = i;

    Payload dropped = Payload::array();
    for (const auto& range : ranges) {
        const auto old = ordinal(range.at("record_index"), original.size());
        const auto now = newPosition[old];
        const auto* oldRight = old + 1 < original.size() ? &original[old + 1] : nullptr;
        const auto* newRight = now + 1 < order.size() ? &edited[order[now + 1]] : nullptr;
        if (!rangeSurvives(original[old], oldRight, edited[old], newRight)) dropped.push_back(range);
    }
    return dropped;
}
bool captureMove(OpContext& c, const Payload& p, Payload& inv, std::string& err) {
    try {
        const auto data = read(c, p);
        inv = address(p);
        const StreamReader<AutomationPoint> reader(data.bytes, FourCC::Automation);
        if (!data.present || !reader.ok()) {
            inv["state"] = fullState(data);
            return true;
        }
        struct Position { std::int64_t time; double value; };
        std::map<std::uint64_t, Position> selected;
        const auto& points = p.at("points");
        if (!points.is_array() || points.empty()) throw std::runtime_error("automation: empty selection");
        for (const auto& point : points) {
            const Position position{integer(point.at("time")), point.at("value").get<double>()};
            if (!selected.emplace(identity(point.at("id")), position).second)
                throw std::runtime_error("automation: duplicate selected ID");
        }
        // For dense selections, per-record journal metadata can make a delta
        // larger than the old AAUT image.
        if (selected.size() > reader.count() / 2) {
            inv["state"] = fullState(data);
            return true;
        }
        Payload moved = Payload::array();
        std::vector<AutomationPoint> original, edited;
        const bool hasRanges = !data.ranges.empty();
        if (hasRanges) {
            original.reserve(reader.count());
            edited.reserve(reader.count());
        }
        const auto raw = std::span(data.bytes);
        std::int64_t previous = INT64_MIN;
        for (std::uint32_t i = 0; i < reader.count(); ++i) {
            auto point = *reader.at(i);
            // A forward edit will also sort unrelated records in a legacy
            // unsorted stream. Keep the old full inverse in that case.
            if (point.time < previous) {
                inv["state"] = fullState(data);
                return true;
            }
            previous = point.time;
            if (hasRanges) original.push_back(point);
            const auto move = selected.find(point.point_id);
            if (move != selected.end()) {
                const auto offset = sizeof(StreamHeader) + static_cast<std::size_t>(i) * reader.recSize();
                moved.push_back({{"ordinal", i}, {"record", bytesPayload(raw.subspan(offset, reader.recSize()))}});
                point.time = move->second.time;
                point.value = move->second.value;
            }
            if (hasRanges) edited.push_back(point);
        }
        if (moved.size() != selected.size()) throw std::runtime_error("automation: selected point missing or duplicated");
        inv["state"] = {{"count", reader.count()}, {"moved", std::move(moved)},
                        {"ranges", hasRanges ? droppedRanges(data.ranges, original, edited) : Payload::array()}};
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}
void restoreMove(OpContext& c, const Payload& p, const Payload& state, const Data& data) {
    const StreamReader<AutomationPoint> reader(data.bytes, FourCC::Automation);
    if (!data.present || !reader.ok() || integer(state.at("count")) != static_cast<std::int64_t>(reader.count()))
        throw std::runtime_error("automation: moved stream shape differs");
    const auto& moved = state.at("moved");
    const auto& dropped = state.at("ranges");
    if (!moved.is_array() || moved.empty() || moved.size() > reader.count() || !dropped.is_array())
        throw std::runtime_error("automation: invalid move inverse");
    const std::size_t count = reader.count(), stride = reader.recSize();
    std::vector<const Payload::binary_t*> originals(count, nullptr);
    struct MovedPoint { std::size_t original; bool found = false; };
    std::map<std::uint64_t, MovedPoint> movedPoints;
    for (const auto& item : moved) {
        const auto index = ordinal(item.at("ordinal"), count);
        const auto& raw = item.at("record").get_binary();
        if (raw.size() != stride || originals[index]) throw std::runtime_error("automation: invalid captured record");
        AutomationPoint point{};
        std::memcpy(&point, raw.data(), sizeof point);
        if (!point.point_id || !movedPoints.emplace(point.point_id, MovedPoint{index}).second)
            throw std::runtime_error("automation: invalid captured point ID");
        originals[index] = &raw;
    }
    std::vector<std::size_t> restoredPosition(count, count);
    std::size_t found = 0;
    for (std::uint32_t i = 0; i < reader.count(); ++i) {
        const auto point = *reader.at(i);
        const auto move = movedPoints.find(point.point_id);
        if (move == movedPoints.end()) continue;
        if (move->second.found) throw std::runtime_error("automation: duplicate moved point");
        move->second.found = true;
        ++found;
        restoredPosition[i] = move->second.original;
    }
    if (found != movedPoints.size()) throw std::runtime_error("automation: moved point missing");

    // The forward stable sort retains the relative order of unselected
    // records. Fill the old moved slots and merge those other records back
    // in order, copying whole strides and leaving header and trailer intact.
    Bytes out = data.bytes;
    std::size_t next = 0;
    for (std::size_t i = 0; i < count; ++i) {
        auto* destination = out.data() + sizeof(StreamHeader) + i * stride;
        if (originals[i]) {
            std::memcpy(destination, originals[i]->data(), stride);
            continue;
        }
        while (next < count && restoredPosition[next] != count) ++next;
        if (next == count) throw std::runtime_error("automation: incomplete captured move");
        std::memcpy(destination, data.bytes.data() + sizeof(StreamHeader) + next * stride, stride);
        restoredPosition[next++] = i;
    }
    auto ranges = data.ranges;
    for (auto& range : ranges)
        range["record_index"] = restoredPosition[ordinal(range.at("record_index"), count)];
    for (const auto& range : dropped) {
        (void)ordinal(range.at("record_index"), count);
        ranges.push_back(range);
    }
    std::sort(ranges.begin(), ranges.end(), [](const Payload& a, const Payload& b) {
        return integer(a.at("id")) < integer(b.at("id"));
    });
    store(c, p, &out, true);
    range_rows::replace(c.db, integer(p.at("id")), clipOf(p), ranges);
}
bool restore(OpContext& c, const Payload& p, std::string& err) {
    try {
        const auto data = read(c, p);
        const auto& state = p.at("state");
        if (state.contains("moved")) {
            restoreMove(c, p, state, data);
            return true;
        }
        const auto& bin = state.at("bytes").get_binary();
        Bytes bytes(bin.size());
        if (!bytes.empty()) std::memcpy(bytes.data(), bin.data(), bytes.size());
        const bool present=state.at("present").get<bool>();
        const auto ranges=state.value("ranges",Payload::array());
        if(!present && !ranges.empty())throw std::runtime_error("automation: absent data with curve ranges");
        store(c,p,present?&bytes:nullptr,data.present);
        range_rows::replace(c.db,integer(p.at("id")),clipOf(p),ranges);return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}
enum class Edit { Write, Delete, Move, Curve };
template<Edit edit> bool apply(OpContext& c, const Payload& p, std::string& err) {
    try {
        auto d = read(c, p);
        if (!d.present) {
            if constexpr (edit != Edit::Write) throw std::runtime_error("automation: missing data row");
            d.bytes = writeStream<AutomationPoint>(FourCC::Automation, {}, 1,
                StreamFlags::SortedByTime | (d.timeBase == 1 ? StreamFlags::TimeIsNanos : 0u));
        }
        const StreamReader<AutomationPoint> reader(d.bytes, FourCC::Automation);
        if (!reader.ok()) throw std::runtime_error("automation: unreadable AAUT stream");
        auto h = reader.header();
        struct Record { AutomationPoint point; Bytes raw; std::size_t original=SIZE_MAX; };
        std::vector<Record> records;
        std::vector<AutomationPoint> originalPoints;
        std::map<std::uint64_t, std::size_t> indices;
        for (std::uint32_t n = 0; n < h.count; ++n) {
            Record r{};
            const auto offset = sizeof(StreamHeader) + static_cast<std::size_t>(n) * h.rec_size;
            r.raw.assign(d.bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                         d.bytes.begin() + static_cast<std::ptrdiff_t>(offset + h.rec_size));
            std::memcpy(&r.point, r.raw.data(), sizeof r.point);
            if (!r.point.point_id || !indices.emplace(r.point.point_id, records.size()).second)
                throw std::runtime_error("automation: ambiguous point IDs");
            r.original=originalPoints.size();originalPoints.push_back(r.point);
            records.push_back(std::move(r));
        }
        std::map<std::size_t,Payload> originalRanges;
        for(const auto& range:d.ranges){
            const auto index=integer(range.at("record_index"));
            if(index<0 || static_cast<std::uint64_t>(index)>=originalPoints.size() ||
               range.at("clip_id")!=clipOf(p) ||
               !originalRanges.emplace(static_cast<std::size_t>(index),range).second)
                throw std::runtime_error("automation: invalid retained curve range");
        }
        const auto& items = p.at(edit == Edit::Delete || edit == Edit::Curve ? "ids" : "points");
        if (!items.is_array() || items.empty()) throw std::runtime_error("automation: empty selection");
        std::set<std::uint64_t> selected;
        for (const auto& item : items) {
            const auto id = identity(edit == Edit::Delete || edit == Edit::Curve ? item : item.at("id"));
            if (!selected.insert(id).second) throw std::runtime_error("automation: duplicate selected ID");
            auto found = indices.find(id);
            if (found == indices.end()) {
                if constexpr (edit != Edit::Write) throw std::runtime_error("automation: selected point missing");
                if (h.rec_size != sizeof(AutomationPoint) || h.version != 1 ||
                    (h.flags & ~(StreamFlags::SortedByTime | StreamFlags::TimeIsNanos)) != 0)
                    throw std::runtime_error("automation: cannot invent future record fields");
                indices[id] = records.size();
                records.push_back({{}, Bytes(sizeof(AutomationPoint))});
                found = indices.find(id);
            }
            auto& r = records[found->second]; r.point.point_id = id;
            if constexpr (edit == Edit::Write || edit == Edit::Move) {
                r.point.time = integer(item.at("time")); r.point.value = item.at("value").get<double>();
                if (r.point.time < 0 || !std::isfinite(r.point.value))
                    throw std::runtime_error("automation: invalid time/value");
                // Existing logs can contain values admitted before lane bounds were enforced.
                if (!c.replay && ((d.minimum && r.point.value < *d.minimum) ||
                                  (d.maximum && r.point.value > *d.maximum)))
                    throw std::runtime_error("automation: point value is outside the lane range");
            }
            if constexpr (edit == Edit::Write || edit == Edit::Curve) {
                const auto& shape = edit == Edit::Write ? item : p;
                const auto curve = integer(shape.at("curve"));
                const double tension = shape.at("tension").get<double>();
                if (curve < 0 || curve > 5 || !std::isfinite(tension) || tension < -1.0 || tension > 1.0)
                    throw std::runtime_error("automation: invalid curve/tension");
                r.point.curve = static_cast<std::uint8_t>(curve); r.point.tension = static_cast<float>(tension);
            }
            std::memcpy(r.raw.data(), &r.point, sizeof r.point);
        }
        if constexpr (edit == Edit::Delete)
            std::erase_if(records, [&](const Record& r) { return selected.contains(r.point.point_id); });
        // Stable order gives coincident steps deterministic last-point precedence,
        // while opaque record tails travel with their point's identity.
        std::stable_sort(records.begin(), records.end(), [](const Record& a, const Record& b) {
            return a.point.time < b.point.time;
        });
        if (records.size() > UINT32_MAX) throw std::runtime_error("automation: too many points");
        h.count = static_cast<std::uint32_t>(records.size());
        Bytes out(sizeof h); std::memcpy(out.data(), &h, sizeof h);
        for (const auto& r : records) out.insert(out.end(), r.raw.begin(), r.raw.end());
        const auto tail = sizeof(StreamHeader) + static_cast<std::size_t>(reader.header().count) * h.rec_size;
        out.insert(out.end(), d.bytes.begin() + static_cast<std::ptrdiff_t>(tail), d.bytes.end());
        Payload retained=Payload::array();
        for(std::size_t index=0;index<records.size();++index){
            const auto& record=records[index];const auto old=originalRanges.find(record.original);
            if(old==originalRanges.end())continue;
            const auto& left=originalPoints[record.original];
            const auto* oldRight = record.original + 1 < originalPoints.size() ? &originalPoints[record.original + 1] : nullptr;
            const auto* newRight = index + 1 < records.size() ? &records[index + 1].point : nullptr;
            if (!rangeSurvives(left, oldRight, record.point, newRight)) continue;
            auto range=old->second;range["record_index"]=static_cast<std::int64_t>(index);retained.push_back(std::move(range));
        }
        store(c,p,&out,d.present);
        range_rows::replace(c.db,integer(p.at("id")),clipOf(p),retained);return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}
constexpr Field pointsFields[] = {{"id", FieldType::Int}, {"lane", FieldType::Int}, {"clip", FieldType::Int, false}, {"points", FieldType::Array}};
constexpr Field idsFields[] = {{"id", FieldType::Int}, {"lane", FieldType::Int}, {"clip", FieldType::Int, false}, {"ids", FieldType::Array}};
constexpr Field curveFields[] = {{"id", FieldType::Int}, {"lane", FieldType::Int}, {"clip", FieldType::Int, false}, {"ids", FieldType::Array}, {"curve", FieldType::Int}, {"tension", FieldType::Real}};
constexpr Field restoreFields[] = {{"id", FieldType::Int}, {"lane", FieldType::Int}, {"clip", FieldType::Int, false}, {"state", FieldType::Object}};
const OpDescriptor ops[] = {
    {"automation.writePoints", "Upsert explicit automation points", Scope::Edit, EngineImpact::Snapshot,
        pointsFields, false, false, apply<Edit::Write>, capture, "automation.restoreData"},
    {"automation.deletePoints", "Delete selected automation points", Scope::Edit, EngineImpact::Snapshot,
        idsFields, false, false, apply<Edit::Delete>, capture, "automation.restoreData"},
    {"automation.movePoints", "Move selected automation points", Scope::Edit, EngineImpact::Snapshot,
        pointsFields, false, false, apply<Edit::Move>, captureMove, "automation.restoreData"},
    {"automation.setCurve", "Shape selected automation segments", Scope::Edit, EngineImpact::Snapshot,
        curveFields, false, false, apply<Edit::Curve>, capture, "automation.restoreData"},
    {"automation.restoreData", "Restore captured automation bytes or absence", Scope::Edit, EngineImpact::Snapshot,
        restoreFields, false, false, restore, capture, ""},
};
OpRequest request(const char* name, std::int64_t data, std::int64_t lane, Payload clip) {
    OpRequest r; r.opType = name; r.payload = {{"id", data}, {"lane", lane}, {"clip", std::move(clip)}}; return r;
}
} // namespace
OpRequest writePoints(std::int64_t data, std::int64_t lane, Payload clip, Payload points) {
    auto r = request("automation.writePoints", data, lane, std::move(clip)); r.payload["points"] = std::move(points); return r;
}
OpRequest deletePoints(std::int64_t data, std::int64_t lane, Payload clip, Payload ids) {
    auto r = request("automation.deletePoints", data, lane, std::move(clip)); r.payload["ids"] = std::move(ids); return r;
}
OpRequest movePoints(std::int64_t data, std::int64_t lane, Payload clip, Payload points) {
    auto r = request("automation.movePoints", data, lane, std::move(clip)); r.payload["points"] = std::move(points); return r;
}
OpRequest setCurve(std::int64_t data, std::int64_t lane, Payload clip, Payload ids, std::uint8_t curve, double tension) {
    auto r = request("automation.setCurve", data, lane, std::move(clip)); r.payload["ids"] = std::move(ids);
    r.payload["curve"] = curve; r.payload["tension"] = tension; return r;
}
std::span<const OpDescriptor> descriptors() { return ops; }
} // namespace adi::automation
