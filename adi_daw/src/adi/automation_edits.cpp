// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/automation_edits.hpp"
#include "adi/blob.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <cmath>
#include <climits>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace adi::automation {
namespace {
using Bytes = std::vector<std::byte>;
struct Data { bool present = false; Bytes bytes; };

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
Payload address(const Payload& p) {
    return {{"id", p.at("id")}, {"lane", p.at("lane")}, {"clip", p.at("clip")}};
}
Data read(OpContext& c, const Payload& p) {
    const auto id = integer(p.at("id")), lane = integer(p.at("lane"));
    if (id <= 0 || lane <= 0) throw std::runtime_error("automation: positive row IDs required");
    SQLite::Statement owner(c.db, "SELECT 1 FROM automation_lanes WHERE id=?");
    owner.bind(1, lane);
    if (!owner.executeStep()) throw std::runtime_error("automation: missing lane");
    if (!p.at("clip").is_null()) {
        const auto clip = integer(p.at("clip"));
        SQLite::Statement q(c.db, "SELECT 1 FROM clips WHERE id=?");
        q.bind(1, clip);
        if (clip <= 0 || !q.executeStep()) throw std::runtime_error("automation: missing clip");
    }
    SQLite::Statement q(c.db, "SELECT lane_id,clip_id,data FROM automation_data WHERE id=?");
    q.bind(1, id);
    if (!q.executeStep()) {
        SQLite::Statement slot(c.db, "SELECT 1 FROM automation_data WHERE lane_id=? AND clip_id IS ?");
        slot.bind(1, lane);
        if (p.at("clip").is_null()) slot.bind(2); else slot.bind(2, integer(p.at("clip")));
        if (slot.executeStep()) throw std::runtime_error("automation: slot has another data ID");
        return {};
    }
    if (q.getColumn(0).getInt64() != lane ||
        (q.getColumn(1).isNull() != p.at("clip").is_null()) ||
        (!p.at("clip").is_null() && q.getColumn(1).getInt64() != integer(p.at("clip"))))
        throw std::runtime_error("automation: data address mismatch");
    Data result; result.present = true;
    const auto col = q.getColumn(2);
    const auto* b = static_cast<const std::byte*>(col.getBlob());
    if (col.getBytes() > 0) result.bytes.assign(b, b + col.getBytes());
    return result;
}
void store(OpContext& c, const Payload& p, const Bytes* bytes) {
    if (!bytes) {
        SQLite::Statement q(c.db, "DELETE FROM automation_data WHERE id=?");
        q.bind(1, integer(p.at("id"))); q.exec(); return;
    }
    if (bytes->size() > static_cast<std::size_t>(INT_MAX))
        throw std::runtime_error("automation: stream exceeds SQLite binding limit");
    SQLite::Statement q(c.db, "INSERT INTO automation_data(id,lane_id,clip_id,data) VALUES(?,?,?,?) "
        "ON CONFLICT(id) DO UPDATE SET data=excluded.data");
    q.bind(1, integer(p.at("id"))); q.bind(2, integer(p.at("lane")));
    if (p.at("clip").is_null()) q.bind(3); else q.bind(3, integer(p.at("clip")));
    // SQLite distinguishes an empty BLOB from SQL NULL.
    q.bind(4, bytes->empty() ? static_cast<const void*>("") : bytes->data(), static_cast<int>(bytes->size()));
    q.exec();
}
bool capture(OpContext& c, const Payload& p, Payload& inv, std::string& err) {
    try {
        const auto d = read(c, p); inv = address(p); inv["state"] = {{"present", d.present}};
        std::vector<std::uint8_t> bytes(d.bytes.size());
        if (!bytes.empty()) std::memcpy(bytes.data(), d.bytes.data(), bytes.size());
        inv["state"]["bytes"] = Payload::binary(std::move(bytes)); return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}
bool restore(OpContext& c, const Payload& p, std::string& err) {
    try {
        (void)read(c, p);
        const auto& bin = p.at("state").at("bytes").get_binary();
        Bytes bytes(bin.size());
        if (!bytes.empty()) std::memcpy(bytes.data(), bin.data(), bytes.size());
        store(c, p, p.at("state").at("present").get<bool>() ? &bytes : nullptr); return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}
enum class Edit { Write, Delete, Move, Curve };
template<Edit edit> bool apply(OpContext& c, const Payload& p, std::string& err) {
    try {
        auto d = read(c, p);
        if (!d.present) {
            if constexpr (edit != Edit::Write) throw std::runtime_error("automation: missing data row");
            SQLite::Statement lane(c.db, "SELECT time_base FROM automation_lanes WHERE id=?");
            lane.bind(1, integer(p.at("lane"))); (void)lane.executeStep();
            d.bytes = writeStream<AutomationPoint>(FourCC::Automation, {}, 1,
                StreamFlags::SortedByTime | (lane.getColumn(0).getInt() == 1 ? StreamFlags::TimeIsNanos : 0u));
        }
        const StreamReader<AutomationPoint> reader(d.bytes, FourCC::Automation);
        if (!reader.ok()) throw std::runtime_error("automation: unreadable AAUT stream");
        auto h = reader.header();
        struct Record { AutomationPoint point; Bytes raw; };
        std::vector<Record> records;
        std::map<std::uint64_t, std::size_t> indices;
        for (std::uint32_t n = 0; n < h.count; ++n) {
            Record r{};
            const auto offset = sizeof(StreamHeader) + static_cast<std::size_t>(n) * h.rec_size;
            r.raw.assign(d.bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                         d.bytes.begin() + static_cast<std::ptrdiff_t>(offset + h.rec_size));
            std::memcpy(&r.point, r.raw.data(), sizeof r.point);
            if (!r.point.point_id || !indices.emplace(r.point.point_id, records.size()).second)
                throw std::runtime_error("automation: ambiguous point IDs");
            records.push_back(std::move(r));
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
        store(c, p, &out); return true;
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
        pointsFields, false, false, apply<Edit::Move>, capture, "automation.restoreData"},
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
