// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/clip_property_ops.hpp"
#include "adi/edit_time.hpp"
#include "adi/engine/snapshot.hpp"
#include "adi/row_capture.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace adi::clip_properties {
namespace {
using detail::signedInteger;
constexpr std::int64_t tickLimit = std::int64_t{1} << 50;
std::int64_t id(const Payload& payload) { return signedInteger(payload.at("id")); }
Payload core(OpContext& context, std::int64_t clipId) {
    auto row = detail::captureRow(context.db, "clips", "id", clipId);
    if (!row) throw std::runtime_error("no such clip");
    return std::move(*row);
}
void validTick(std::int64_t value) {
    if (value < 0 || value > tickLimit) throw std::runtime_error("clip tick outside supported range");
}
std::int64_t defaultEnd(OpContext& context, const Payload& row) {
    auto length = row.at("length_ticks").is_null() ? std::int64_t{0} : signedInteger(row.at("length_ticks"));
    if (row.at("time_base") == 1) {
        engine::TempoMap tempo;
        SQLite::Statement query(context.db, "SELECT pos_ticks,bpm,curve,tension FROM tempo_map ORDER BY pos_ticks");
        double maxBpm = 0;
        while (query.executeStep()) {
            const auto bpm = query.getColumn(1).getDouble();
            if (!std::isfinite(bpm) || bpm <= 0)
                throw std::runtime_error("clip property conversion needs a valid tempo map");
            maxBpm = std::max(maxBpm, bpm);
            tempo.events.push_back({query.getColumn(0).getInt64(),bpm,query.getColumn(2).getInt(),
                                    query.getColumn(3).getDouble()});
        }
        if (tempo.events.empty()) { tempo.events.push_back({0,120,0}); maxBpm = 120; }
        const auto pos = signedInteger(row.at("pos_ns"));
        const auto nanos = signedInteger(row.at("length_ns"));
        if (pos < 0 || nanos <= 0 || pos > std::numeric_limits<std::int64_t>::max() - nanos)
            throw std::runtime_error("invalid linear clip time");
        const double begin = static_cast<double>(pos)*1e-9;
        const double finish = static_cast<double>(pos+nanos)*1e-9;
        const long double bound = static_cast<long double>(finish)*maxBpm*5765760.0L/60.0L;
        if (!std::isfinite(bound) || bound >= 0x1p62L) throw std::runtime_error("clip musical duration out of range");
        length = tempo.secondsToTicks(finish) - tempo.secondsToTicks(begin);
    }
    const auto offset = signedInteger(row.at("content_offset_ticks"));
    if (offset < -tickLimit || offset > tickLimit)
        throw std::runtime_error("clip source offset outside supported range");
    validTick(length);
    if (length <= 0 || offset < -length || length > tickLimit-offset)
        throw std::runtime_error("clip end marker out of range");
    return offset + length;
}
std::int64_t currentEnd(OpContext& context, const Payload& row) {
    const auto properties = detail::captureRow(context.db, "clip_properties", "clip_id", signedInteger(row.at("id")));
    if (properties && !properties->at("end_marker_ticks").is_null())
        return signedInteger(properties->at("end_marker_ticks"));
    return defaultEnd(context, row);
}
void setProperties(OpContext& context, std::int64_t clipId, const Payload& columns) {
    // All column names are fixed by the handlers below, not payload keys.
    std::string names, slots, assignments;
    for (auto it = columns.begin(); it != columns.end(); ++it) {
        names += ',' + it.key(); slots += ",?";
        if (!assignments.empty()) assignments += ',';
        assignments += it.key() + "=excluded." + it.key();
    }
    SQLite::Statement query(context.db, "INSERT INTO clip_properties(clip_id" + names + ") VALUES(?" + slots +
        ") ON CONFLICT(clip_id) DO UPDATE SET " + assignments);
    query.bind(1,clipId);
    int index = 1;
    for (auto it = columns.begin(); it != columns.end(); ++it) {
        ++index;
        if (it->is_null()) query.bind(index);
        else if (it->is_string()) query.bind(index,it->get<std::string>());
        else query.bind(index,signedInteger(*it));
    }
    query.exec();
}
bool present(SQLite::Database& db,const char* table) {
    SQLite::Statement query(db,"SELECT 1 FROM sqlite_master WHERE type='table' AND name=? UNION ALL "
        "SELECT 1 FROM sqlite_temp_master WHERE type='table' AND name=?");
    query.bind(1,table);query.bind(2,table);return query.executeStep();
}
void clearClocks(OpContext& context,std::int64_t clipId,const char* assignments) {
    if(!present(context.db,"clip_playback_details"))return;
    SQLite::Statement update(context.db,std::string("UPDATE clip_playback_details SET ")+assignments+" WHERE clip_id=?");
    update.bind(1,clipId);update.exec();
}
void setStartTicks(OpContext& context, std::int64_t clipId, std::int64_t ticks) {
    SQLite::Statement update(context.db, "UPDATE clips SET content_offset_ticks=? WHERE id=?");
    update.bind(1, ticks);
    update.bind(2, clipId);
    update.exec();
}
struct SourcePins {
    std::optional<std::int64_t> start;
    std::optional<std::int64_t> end;
};
std::optional<SourcePins> unwarpedPins(OpContext& context, std::int64_t clipId) {
    if (!present(context.db, "clip_playback_details")) return std::nullopt;
    SQLite::Statement query(context.db,
        "SELECT p.content_offset_ns,p.source_end_ns FROM audio_clips a"
        " LEFT JOIN clip_playback_details p ON p.clip_id=a.clip_id"
        " WHERE a.clip_id=? AND a.warp_enabled=0");
    query.bind(1, clipId);
    if (!query.executeStep()) return std::nullopt;
    SourcePins pins;
    if (!query.isColumnNull(0)) pins.start = query.getColumn(0).getInt64();
    if (!query.isColumnNull(1)) pins.end = query.getColumn(1).getInt64();
    return pins;
}
enum class Marker { Start, End };
std::int64_t displayTick(const edit::TimeMap& time, std::int64_t origin, std::int64_t ns, Marker marker) {
    auto tick = std::max<std::int64_t>(0, time.ticks(edit::addTime(time.nanos(origin), ns)) - origin);
    const auto projected = time.duration(origin, tick);
    // Round the preserved boundary outward when it lies between displayed ticks.
    if (marker == Marker::End && projected < ns) ++tick;
    if (marker == Marker::Start && projected > ns && tick > 0) --tick;
    validTick(tick);
    return tick;
}
void setUnwarpedMarkers(OpContext& context, std::int64_t clipId, const Payload& row,
                        const SourcePins& pins, Marker marker, std::int64_t ticks) {
    const edit::TimeMap time(context.db);
    const auto origin = row.at("time_base") == 0 ? signedInteger(row.at("pos_ticks"))
                                                : time.ticks(signedInteger(row.at("pos_ns")));
    std::int64_t startTicks = ticks, endTicks = ticks;
    std::int64_t startNs = 0, endNs = 0;
    if (marker == Marker::Start) {
        startNs = time.duration(origin, ticks);
        endTicks = pins.end ? displayTick(time, origin, *pins.end, Marker::End) : currentEnd(context, row);
        endNs = pins.end ? *pins.end : time.duration(origin, endTicks);
    } else {
        startTicks = pins.start ? displayTick(time, origin, *pins.start, Marker::Start)
                                : signedInteger(row.at("content_offset_ticks"));
        startNs = pins.start ? *pins.start : time.duration(origin, startTicks);
        endNs = time.duration(origin, ticks);
    }
    validTick(startTicks);
    validTick(endTicks);
    if (startTicks >= endTicks || startNs >= endNs) throw std::runtime_error("Start must be before End");
    setStartTicks(context, clipId, startTicks);
    setProperties(context, clipId, {{"end_marker_ticks",endTicks}});
    SQLite::Statement write(context.db,
        "INSERT INTO clip_playback_details(clip_id,content_offset_ns,source_end_ns) VALUES(?,?,?)"
        " ON CONFLICT(clip_id) DO UPDATE SET content_offset_ns=excluded.content_offset_ns,"
        " source_end_ns=excluded.source_end_ns");
    write.bind(1, clipId);
    write.bind(2, startNs);
    write.bind(3, endNs);
    write.exec();
    if (marker == Marker::Start)
        clearClocks(context, clipId, "in_begin_ns=NULL,in_end_ns=NULL,out_begin_ns=NULL,out_end_ns=NULL");
}
bool restore(OpContext& context, const Payload& payload) {
    if (!payload.contains("row")) return false;
    const auto clipId = id(payload);
    if (signedInteger(payload.at("row").at("id")) != clipId) throw std::runtime_error("captured clip id differs");
    (void)core(context,clipId);
    detail::restoreRow(context.db,"clips",payload.at("row"),"id");
    const auto& properties = payload.at("properties");
    if (properties.is_null()) {
        SQLite::Statement erase(context.db,"DELETE FROM clip_properties WHERE clip_id=?");
        erase.bind(1,clipId); erase.exec();
    } else {
        if (signedInteger(properties.at("clip_id")) != clipId) throw std::runtime_error("captured properties id differs");
        detail::restoreRow(context.db,"clip_properties",properties,"clip_id");
    }
    if(payload.contains("playback")) {
        if(!present(context.db,"clip_playback_details"))throw std::runtime_error("captured playback details table is missing");
        const auto& playback=payload.at("playback");
        if(playback.is_null()) {
            SQLite::Statement erase(context.db,"DELETE FROM clip_playback_details WHERE clip_id=?");erase.bind(1,clipId);erase.exec();
        } else {
            if(signedInteger(playback.at("clip_id"))!=clipId)throw std::runtime_error("captured playback owner differs");
            const auto exists=detail::captureRow(context.db,"clip_playback_details","clip_id",clipId).has_value();
            detail::restoreRow(context.db,"clip_playback_details",playback,exists?"clip_id":"");
        }
    }
    return true;
}
} // namespace

bool inverse(OpContext& context, const Payload& payload, Payload& captured, std::string& error) {
    try {
        captured = {{"id",id(payload)},{"row",core(context,id(payload))},
            {"properties",detail::captureRow(context.db,"clip_properties","clip_id",id(payload)).value_or(Payload(nullptr))}};
        if(present(context.db,"clip_playback_details"))
            captured["playback"]=detail::captureRow(context.db,"clip_playback_details","clip_id",id(payload)).value_or(Payload(nullptr));
        return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
bool start(OpContext& context, const Payload& payload, std::string& error) {
    try {
        if (restore(context,payload)) return true;
        const auto clipId = id(payload);
        const auto row = core(context, clipId);
        const auto value = signedInteger(payload.at("ticks"));
        validTick(value);
        if (const auto pins = unwarpedPins(context, clipId)) {
            setUnwarpedMarkers(context, clipId, row, *pins, Marker::Start, value);
            return true;
        }
        const auto finish = currentEnd(context,row);
        validTick(finish);
        if (value >= finish) throw std::runtime_error("Start must be before End");
        setProperties(context,clipId,{{"end_marker_ticks",finish}}); // pin End while Start moves
        setStartTicks(context,clipId,value);
        clearClocks(context,clipId,"content_offset_ns=NULL,in_begin_ns=NULL,in_end_ns=NULL,out_begin_ns=NULL,out_end_ns=NULL");
        return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
bool end(OpContext& context, const Payload& payload, std::string& error) {
    try {
        if (restore(context,payload)) return true;
        const auto clipId = id(payload);
        const auto row = core(context,clipId);
        const auto value = signedInteger(payload.at("ticks"));
        validTick(value);
        if (const auto pins = unwarpedPins(context, clipId)) {
            setUnwarpedMarkers(context, clipId, row, *pins, Marker::End, value);
            return true;
        }
        if (value <= signedInteger(row.at("content_offset_ticks"))) throw std::runtime_error("End must follow Start");
        setProperties(context,clipId,{{"end_marker_ticks",value}});
        clearClocks(context,clipId,"source_end_ns=NULL");
        return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
bool loop(OpContext& context, const Payload& payload, std::string& error) {
    try {
        if (restore(context,payload)) return true;
        const auto row = core(context,id(payload)); const auto on = payload.at("on").get<bool>();
        const auto begin = payload.contains("start") ? signedInteger(payload.at("start")) : signedInteger(row.at("loop_start_ticks"));
        std::optional<std::int64_t> length;
        if (payload.contains("length")) length = signedInteger(payload.at("length"));
        else if (!row.at("loop_len_ticks").is_null()) length = signedInteger(row.at("loop_len_ticks"));
        if (on || payload.contains("start")) validTick(begin);
        if (on || payload.contains("length")) {
            if (!length || *length <= 0) throw std::runtime_error("Loop needs a positive length");
            validTick(*length);
            if (begin < 0 || begin > tickLimit-*length) throw std::runtime_error("Loop end out of range");
        }
        SQLite::Statement update(context.db,"UPDATE clips SET loop_enabled=?,loop_start_ticks=?,loop_len_ticks=? WHERE id=?");
        update.bind(1,on ? 1 : 0); update.bind(2,begin);
        if (length) update.bind(3,*length); else update.bind(3);
        update.bind(4,id(payload)); update.exec();
        if(payload.contains("start") || payload.contains("length"))
            clearClocks(context,id(payload),"loop_begin_ns=NULL,loop_end_ns=NULL");
        return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
bool signature(OpContext& context, const Payload& payload, std::string& error) {
    try {
        if (restore(context,payload)) return true;
        (void)core(context,id(payload));
        Payload columns = {{"signature_numerator",nullptr},{"signature_denominator",nullptr}};
        if (payload.contains("num") || payload.contains("den")) {
            if (!payload.contains("num") || !payload.contains("den")) throw std::runtime_error("signature needs both numerator and denominator");
            const auto num = signedInteger(payload.at("num")), den = signedInteger(payload.at("den"));
            if (num < 1 || num > 255 || den < 1 || den > 128 || (den & (den-1)) != 0) throw std::runtime_error("invalid clip signature");
            columns["signature_numerator"] = num; columns["signature_denominator"] = den;
        }
        setProperties(context,id(payload),columns);
        return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
bool scale(OpContext& context, const Payload& payload, std::string& error) {
    try {
        if (restore(context,payload)) return true;
        (void)core(context,id(payload));
        Payload columns = {{"scale_root",nullptr},{"scale_id",nullptr},{"scale_mask",nullptr}};
        if (payload.contains("root") || payload.contains("scale") || payload.contains("mask")) {
            if (!payload.contains("root") || !payload.contains("scale") || !payload.contains("mask")) throw std::runtime_error("scale needs root, scale id and mask");
            const auto root = signedInteger(payload.at("root")), mask = signedInteger(payload.at("mask"));
            const auto name = payload.at("scale").get<std::string>();
            if (root < 0 || root > 11 || mask < 1 || mask > 4095 || (mask & 1) == 0 || name.empty()) throw std::runtime_error("invalid clip scale");
            columns["scale_root"] = root; columns["scale_mask"] = mask; columns["scale_id"] = name;
        }
        setProperties(context,id(payload),columns);
        return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}

} // namespace adi::clip_properties
