// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/controller_lanes.hpp"
#include "adi/blob.hpp"
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
namespace adi::controllers {
namespace {
std::int64_t integer(const Payload &value) {
    if (!value.is_number_integer() ||
        (value.is_number_unsigned() &&
         value.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())))
        throw std::runtime_error("controller integer is outside int64");
    return value.get<std::int64_t>();
}
std::uint64_t pointId(const Payload &value) {
    if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<std::int64_t>() < 1))
        throw std::runtime_error("controller point ID must be positive uint64");
    const auto id = value.get<std::uint64_t>();
    if (!id)
        throw std::runtime_error("controller point ID is unassigned");
    return id;
}
Address address(const Payload &payload) {
    Address result;
    result.id = integer(payload.at("id"));
    result.clipId = integer(payload.at("clip"));
    const auto channel = integer(payload.at("channel")), controller = integer(payload.at("controller"));
    if (channel < 0 || channel > 15 || controller < 0 || controller > 127)
        throw std::runtime_error("invalid MIDI controller address");
    result.channel = static_cast<int>(channel);
    result.controller = static_cast<int>(controller);
    return result;
}
Payload packet(Address location) {
    return {{"id", location.id},
            {"clip", location.clipId},
            {"channel", location.channel},
            {"controller", location.controller}};
}
std::vector<std::byte> blob(const SQLite::Column &value) {
    if (value.isNull() || value.getBytes() == 0)
        return {};
    const auto *first = static_cast<const std::byte *>(value.getBlob());
    return {first, first + value.getBytes()};
}
struct Record {
    AutomationPoint point{};
    std::vector<std::byte> bytes;
};
struct Stream {
    std::vector<std::byte> source;
    StreamHeader header{};
    std::vector<Record> records;
    explicit Stream(std::vector<std::byte> data) : source(std::move(data)) {
        StreamReader<AutomationPoint> reader(source, FourCC::Controller);
        if (!reader.ok() || reader.version() < 1 ||
            reader.count() > StreamReader<AutomationPoint>::kMaxRecordsAtOnce ||
            (reader.header().flags & StreamFlags::TimeIsNanos))
            throw std::runtime_error("invalid or unsupported musical controller stream");
        header = reader.header();
        records.reserve(reader.count());
        std::set<std::uint64_t> ids;
        std::int64_t previous = std::numeric_limits<std::int64_t>::min();
        for (std::uint32_t i = 0; i < reader.count(); ++i) {
            const auto point = *reader.at(i);
            if (point.point_id == 0 || !ids.insert(point.point_id).second || !std::isfinite(point.value) ||
                point.value < 0 || point.value > 1 || !std::isfinite(point.tension) || point.tension < -1 ||
                point.tension > 1 || point.curve > static_cast<std::uint8_t>(Curve::Bezier) ||
                point.reserved != 0)
                throw std::runtime_error("invalid controller point");
            if ((header.flags & StreamFlags::SortedByTime) && point.time < previous)
                throw std::runtime_error("controller stream has a false time-order flag");
            previous = point.time;
            const auto offset = sizeof(StreamHeader) + static_cast<std::size_t>(i) * reader.recSize();
            const auto *first = source.data() + offset;
            records.push_back({point, {first, first + reader.recSize()}});
        }
    }
    std::vector<std::byte> encode() {
        std::stable_sort(records.begin(), records.end(), [](const Record &left, const Record &right) {
            return left.point.time < right.point.time;
        });
        if (records.size() > StreamReader<AutomationPoint>::kMaxRecordsAtOnce)
            throw std::runtime_error("too many controller points");
        const auto oldEnd = sizeof(StreamHeader) + static_cast<std::size_t>(header.count) * header.rec_size;
        const std::vector<std::byte> trailer(source.begin() + static_cast<std::ptrdiff_t>(oldEnd),
                                             source.end());
        header.count = static_cast<std::uint32_t>(records.size());
        if (records.size() >
            (std::numeric_limits<std::size_t>::max() - sizeof(StreamHeader) - trailer.size()) /
                header.rec_size)
            throw std::runtime_error("controller stream size overflows");
        std::vector<std::byte> result(sizeof(StreamHeader) + records.size() * header.rec_size +
                                      trailer.size());
        std::memcpy(result.data(), &header, sizeof header);
        for (std::size_t i = 0; i < records.size(); ++i) {
            auto &record = records[i];
            std::memcpy(record.bytes.data(), &record.point, sizeof(AutomationPoint));
            std::memcpy(result.data() + sizeof(StreamHeader) + i * header.rec_size, record.bytes.data(),
                        record.bytes.size());
        }
        if (!trailer.empty())
            std::memcpy(result.data() + result.size() - trailer.size(), trailer.data(), trailer.size());
        return result;
    }
};
std::optional<std::vector<std::byte>> readSlot(SQLite::Database &db, Address location) {
    SQLite::Statement clip(db, "SELECT 1 FROM clips WHERE id=?");
    clip.bind(1, location.clipId);
    if (!clip.executeStep())
        throw std::runtime_error("no such controller clip");
    SQLite::Statement rows(db,
                           "SELECT id,clip_id,stream_kind,channel,controller,data FROM event_streams WHERE "
                           "id=? OR (clip_id=? AND stream_kind='cc' AND channel=? AND controller=?)");
    rows.bind(1, location.id);
    rows.bind(2, location.clipId);
    rows.bind(3, location.channel);
    rows.bind(4, location.controller);
    std::optional<std::vector<std::byte>> result;
    while (rows.executeStep()) {
        if (result || rows.getColumn(0).getInt64() != location.id ||
            rows.getColumn(1).getInt64() != location.clipId || rows.getColumn(2).getString() != "cc" ||
            rows.getColumn(3).isNull() || rows.getColumn(3).getInt64() != location.channel ||
            rows.getColumn(4).isNull() || rows.getColumn(4).getInt64() != location.controller)
            throw std::runtime_error("controller stream ID/address collision");
        result = blob(rows.getColumn(5));
    }
    return result;
}
bool save(OpContext &context, Address location, const std::optional<std::vector<std::byte>> &data,
          std::string &error) {
    try {
        (void)readSlot(context.db, location);
        if (!data) {
            SQLite::Statement remove(context.db, "DELETE FROM event_streams WHERE id=?");
            remove.bind(1, location.id);
            remove.exec();
            return true;
        }
        SQLite::Statement write(context.db,
                                "INSERT INTO event_streams(id,clip_id,stream_kind,channel,controller,data) "
                                "VALUES(?,?,'cc',?,?,?) ON CONFLICT(id) DO UPDATE SET data=excluded.data");
        write.bind(1, location.id);
        write.bind(2, location.clipId);
        write.bind(3, location.channel);
        write.bind(4, location.controller);
        if (data->size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
            throw std::runtime_error("controller stream exceeds SQLite blob limit");
        write.bind(5, data->data(), static_cast<int>(data->size()));
        write.exec();
        return true;
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
}
AutomationPoint point(const Payload &payload) {
    if (!payload.is_object())
        throw std::runtime_error("invalid controller point packet");
    const std::set<std::string> fields{"id", "time", "value", "tension", "curve", "flags"};
    for (const auto &item : payload.items())
        if (!fields.contains(item.key()))
            throw std::runtime_error("unknown controller point field");
    AutomationPoint result{};
    result.point_id = pointId(payload.at("id"));
    result.time = integer(payload.at("time"));
    if (!payload.at("value").is_number())
        throw std::runtime_error("invalid controller value");
    result.value = payload.at("value").get<double>();
    const auto tension = payload.contains("tension") ? payload.at("tension").get<double>() : 0.0;
    const auto curve = payload.contains("curve") ? integer(payload.at("curve")) : 0,
               flags = payload.contains("flags") ? integer(payload.at("flags")) : 0;
    if (!std::isfinite(result.value) || result.value < 0 || result.value > 1 || !std::isfinite(tension) ||
        tension < -1 || tension > 1 || curve < 0 || curve > static_cast<int>(Curve::Bezier) || flags < 0 ||
        flags > 255)
        throw std::runtime_error("controller point is outside its domain");
    result.tension = static_cast<float>(tension);
    result.curve = static_cast<std::uint8_t>(curve);
    result.flags = static_cast<std::uint8_t>(flags);
    return result;
}
bool writeApply(OpContext &context, const Payload &payload, std::string &error) {
    try {
        const auto location = address(payload);
        auto previous = readSlot(context.db, location);
        Stream stream(previous ? std::move(*previous) : writeStream<AutomationPoint>(FourCC::Controller, {}));
        const auto &incoming = payload.at("points");
        if (!incoming.is_array() || incoming.empty())
            throw std::runtime_error("controller write requires explicit points");
        std::map<std::uint64_t, std::size_t> index;
        for (std::size_t i = 0; i < stream.records.size(); ++i)
            index.emplace(stream.records[i].point.point_id, i);
        std::set<std::uint64_t> changed;
        for (const auto &value : incoming) {
            const auto parsed = point(value);
            if (!changed.insert(parsed.point_id).second)
                throw std::runtime_error("duplicate written controller point ID");
            const auto found = index.find(parsed.point_id);
            if (found != index.end())
                stream.records[found->second].point = parsed;
            else {
                if (stream.header.rec_size != sizeof(AutomationPoint))
                    throw std::runtime_error("new points need a known controller record layout");
                index.emplace(parsed.point_id, stream.records.size());
                stream.records.push_back({parsed, std::vector<std::byte>(sizeof(AutomationPoint))});
            }
        }
        return save(context, location, stream.encode(), error);
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
}
bool deleteApply(OpContext &context, const Payload &payload, std::string &error) {
    try {
        const auto location = address(payload);
        auto previous = readSlot(context.db, location);
        if (!previous)
            throw std::runtime_error("no controller lane to edit");
        Stream stream(std::move(*previous));
        std::set<std::uint64_t> selected;
        for (const auto &value : payload.at("point_ids"))
            if (!selected.insert(pointId(value)).second)
                throw std::runtime_error("duplicate deleted controller point ID");
        if (selected.empty())
            throw std::runtime_error("controller delete requires explicit point IDs");
        std::size_t removed = 0;
        stream.records.erase(std::remove_if(stream.records.begin(), stream.records.end(),
                                            [&](const Record &record) {
                                                if (!selected.contains(record.point.point_id))
                                                    return false;
                                                ++removed;
                                                return true;
                                            }),
                             stream.records.end());
        if (removed != selected.size())
            throw std::runtime_error("deleted controller point does not exist");
        return save(context, location, stream.encode(), error);
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
}
bool capture(OpContext &context, const Payload &payload, Payload &inverse, std::string &error) {
    try {
        const auto location = address(payload);
        const auto previous = readSlot(context.db, location);
        inverse = packet(location);
        inverse["data"] = nullptr;
        if (previous) {
            (void)Stream(*previous);
            std::vector<std::uint8_t> bytes;
            bytes.reserve(previous->size());
            for (auto value : *previous)
                bytes.push_back(std::to_integer<std::uint8_t>(value));
            inverse["data"] = {{"bytes", Payload::binary(bytes)}};
        }
        return true;
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
}
bool restoreApply(OpContext &context, const Payload &payload, std::string &error) {
    try {
        const auto location = address(payload);
        const auto &data = payload.at("data");
        if (data.is_null())
            return save(context, location, std::nullopt, error);
        if (!data.is_object() || data.size() != 1 || !data.contains("bytes"))
            throw std::runtime_error("invalid controller stream packet");
        const auto &input = data.at("bytes");
        std::vector<std::byte> bytes;
        if (input.is_binary())
            for (auto value : input.get_binary())
                bytes.push_back(static_cast<std::byte>(value));
        else if (input.is_array())
            for (const auto &value : input) {
                const auto byte = integer(value);
                if (byte < 0 || byte > 255)
                    throw std::runtime_error("invalid controller stream byte");
                bytes.push_back(static_cast<std::byte>(byte));
            }
        else
            throw std::runtime_error("invalid controller bytes");
        (void)Stream(bytes);
        return save(context, location, bytes, error);
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
}
constexpr Field writeFields[] = {{"id", FieldType::Int},
                                 {"clip", FieldType::Int},
                                 {"channel", FieldType::Int},
                                 {"controller", FieldType::Int},
                                 {"points", FieldType::Array}};
constexpr Field deleteFields[] = {{"id", FieldType::Int},
                                  {"clip", FieldType::Int},
                                  {"channel", FieldType::Int},
                                  {"controller", FieldType::Int},
                                  {"point_ids", FieldType::Array}};
// data is nullable, so a nested packet keeps the outer schema typed/closed.
constexpr Field restoreFields[] = {{"id", FieldType::Int},
                                   {"clip", FieldType::Int},
                                   {"channel", FieldType::Int},
                                   {"controller", FieldType::Int},
                                   {"state", FieldType::Object}};
bool restorePacket(OpContext &context, const Payload &payload, std::string &error) {
    if (payload.at("state").size() != 1 || !payload.at("state").contains("data")) {
        error = "invalid controller restore state";
        return false;
    }
    auto decoded = payload;
    decoded["data"] = payload.at("state").at("data");
    return restoreApply(context, decoded, error);
}
bool capturePacket(OpContext &context, const Payload &payload, Payload &inverse, std::string &error) {
    if (!capture(context, payload, inverse, error))
        return false;
    const auto data = inverse.at("data");
    inverse.erase("data");
    inverse["state"] = {{"data", data}};
    return true;
}
const OpDescriptor descriptors[] = {
    {"cc.write", "Write explicit MIDI controller points", Scope::Edit, EngineImpact::Snapshot, writeFields,
     false, false, writeApply, capturePacket, "cc.restoreStream"},
    {"cc.delete", "Delete explicit MIDI controller points", Scope::Edit, EngineImpact::Snapshot, deleteFields,
     false, false, deleteApply, capturePacket, "cc.restoreStream"},
    {"cc.restoreStream", "Restore a captured controller stream or its absence", Scope::Edit,
     EngineImpact::Snapshot, restoreFields, false, false, restorePacket, capturePacket, ""}};
} // namespace
ReadResult readLanes(const Store &store, std::optional<std::int64_t> clip) {
    ReadResult result;
    try {
        auto &db = const_cast<SQLite::Database &>(store.db());
        std::optional<SQLite::Transaction> transaction;
        if (sqlite3_get_autocommit(db.getHandle()) != 0)
            transaction.emplace(db, SQLite::TransactionBehavior::DEFERRED);
        SQLite::Statement query(
            db, "SELECT id,clip_id,channel,controller,data FROM event_streams WHERE stream_kind='cc' AND (? "
                "IS NULL OR clip_id=?) ORDER BY clip_id,channel,controller,id");
        if (clip) {
            query.bind(1, *clip);
            query.bind(2, *clip);
        } else {
            query.bind(1);
            query.bind(2);
        }
        while (query.executeStep()) {
            Lane lane;
            lane.id = query.getColumn(0).getInt64();
            lane.clipId = query.getColumn(1).getInt64();
            if (!query.getColumn(2).isNull())
                lane.channel = query.getColumn(2).getInt64();
            if (!query.getColumn(3).isNull())
                lane.controller = query.getColumn(3).getInt64();
            lane.data = blob(query.getColumn(4));
            try {
                if (!lane.channel || !lane.controller || *lane.channel < 0 || *lane.channel > 15 ||
                    *lane.controller < 0 || *lane.controller > 127)
                    throw std::runtime_error("invalid/unbound controller lane address");
                const Stream stream(lane.data);
                for (const auto &record : stream.records) {
                    const auto &value = record.point;
                    lane.points.push_back(
                        {value.point_id, value.time, value.value, value.tension, value.curve, value.flags});
                }
            } catch (const std::exception &e) {
                lane.error = e.what();
            }
            result.lanes.push_back(std::move(lane));
        }
        if (transaction)
            transaction->commit();
        result.ok = true;
    } catch (const std::exception &e) {
        result.error = e.what();
        result.lanes.clear();
    }
    return result;
}
OpRequest writeRequest(Address location, std::span<const Point> points) {
    OpRequest request;
    request.opType = "cc.write";
    request.label = "Edit MIDI Controller";
    request.payload = packet(location);
    request.payload["points"] = Payload::array();
    for (const auto &value : points)
        request.payload["points"].push_back({{"id", value.id},
                                             {"time", value.timeTicks},
                                             {"value", value.value},
                                             {"tension", value.tension},
                                             {"curve", value.curve},
                                             {"flags", value.flags}});
    return request;
}
OpRequest deleteRequest(Address location, std::span<const std::uint64_t> ids) {
    OpRequest request;
    request.opType = "cc.delete";
    request.label = "Delete MIDI Controller Points";
    request.payload = packet(location);
    request.payload["point_ids"] = std::vector<std::uint64_t>(ids.begin(), ids.end());
    return request;
}
PreparedWrite prepareWrite(Store &store, std::int64_t clip, int channel, int controller,
                           std::span<const Point> input) {
    PreparedWrite result;
    if (store.readOnly()) {
        result.error = "project is open read-only";
        return result;
    }
    try {
        if (input.empty())
            throw std::runtime_error("controller write requires explicit points");
        std::optional<SQLite::Transaction> transaction;
        if (sqlite3_get_autocommit(store.db().getHandle()) != 0)
            transaction.emplace(store.db(), SQLite::TransactionBehavior::DEFERRED);
        auto location = address(packet({0, clip, channel, controller}));
        {
            SQLite::Statement slot(store.db(), "SELECT id FROM event_streams WHERE clip_id=? AND "
                                               "stream_kind='cc' AND channel=? AND controller=?");
            slot.bind(1, clip);
            slot.bind(2, channel);
            slot.bind(3, controller);
            if (slot.executeStep())
                location.id = slot.getColumn(0).getInt64();
            else {
                const auto maximum =
                    store.db().execAndGet("SELECT COALESCE(MAX(id),0) FROM event_streams").getInt64();
                if (maximum < std::numeric_limits<std::int64_t>::max())
                    location.id = std::max<std::int64_t>(0, maximum) + 1;
                else {
                    location.id = 1;
                    SQLite::Statement ids(store.db(), "SELECT id FROM event_streams WHERE id>0 ORDER BY id");
                    while (ids.executeStep()) {
                        const auto id = ids.getColumn(0).getInt64();
                        if (id > location.id)
                            break;
                        if (id == location.id) {
                            if (location.id == std::numeric_limits<std::int64_t>::max())
                                throw std::runtime_error("no free controller stream ID");
                            ++location.id;
                        }
                    }
                }
            }
        }
        auto previous = readSlot(store.db(), location);
        Stream stream(previous ? std::move(*previous) : writeStream<AutomationPoint>(FourCC::Controller, {}));
        std::set<std::uint64_t> existing, used, explicitIds;
        for (const auto &record : stream.records) {
            existing.insert(record.point.point_id);
            used.insert(record.point.point_id);
        }
        result.points.assign(input.begin(), input.end());
        for (const auto &value : result.points)
            if (value.id) {
                if (!explicitIds.insert(value.id).second)
                    throw std::runtime_error("duplicate written controller point ID");
                used.insert(value.id);
            }
        std::uint64_t next =
            used.empty()
                ? 1
                : (*used.rbegin() == std::numeric_limits<std::uint64_t>::max() ? 1 : *used.rbegin() + 1);
        for (auto &value : result.points) {
            if (!value.id) {
                while (used.contains(next))
                    next = next == std::numeric_limits<std::uint64_t>::max() ? 1 : next + 1;
                value.id = next;
                used.insert(next);
                next = next == std::numeric_limits<std::uint64_t>::max() ? 1 : next + 1;
            }
            if (stream.header.rec_size != sizeof(AutomationPoint) && !existing.contains(value.id))
                throw std::runtime_error("new points need a known controller record layout");
        }
        result.address = location;
        result.request = writeRequest(location, result.points);
        for (const auto &value : result.request.payload.at("points"))
            (void)point(value);
        result.expectedHead = OpJournal(store).headSeq();
        result.sourcePath = store.path();
        result.sourceClient = store.clientId();
        if (transaction)
            transaction->commit();
        result.ok = true;
    } catch (const std::exception &e) {
        result.error = e.what();
    }
    return result;
}
CommitResult commitPrepared(Store &store, const PreparedWrite &prepared) {
    CommitResult result;
    if (!prepared.ok) {
        result.error = prepared.error.empty() ? "controller edit was not prepared" : prepared.error;
        return result;
    }
    if (store.path() != prepared.sourcePath || store.clientId() != prepared.sourceClient) {
        result.error = "controller edit belongs to another document";
        return result;
    }
    CommitOptions options;
    options.expectHead.emplace(prepared.expectedHead);
    return OpJournal(store).commit(std::span<const OpRequest>(&prepared.request, 1), options);
}
std::span<const OpDescriptor> laneOps() {
    return descriptors;
}
} // namespace adi::controllers
