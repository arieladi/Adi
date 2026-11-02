// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/clip_property_ops.hpp"
#include "adi/engine/snapshot.hpp"
#include "adi/row_capture.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
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
        SQLite::Statement query(context.db, "SELECT pos_ticks,bpm,curve FROM tempo_map ORDER BY pos_ticks");
        double maxBpm = 0;
        while (query.executeStep()) {
            const auto bpm = query.getColumn(1).getDouble();
            if (!std::isfinite(bpm) || bpm <= 0 || query.getColumn(2).getInt() != 0)
                throw std::runtime_error("clip property conversion needs a supported step tempo map");
            maxBpm = std::max(maxBpm, bpm);
            tempo.events.push_back({query.getColumn(0).getInt64(),bpm,0});
        }
        if (tempo.events.empty()) { tempo.events.push_back({0,120,0}); maxBpm = 120; }
        const auto pos = signedInteger(row.at("pos_ns"));
        const auto nanos = signedInteger(row.at("length_ns"));
        if (pos < 0 || nanos <= 0) throw std::runtime_error("invalid linear clip time");
        const double begin = static_cast<double>(pos)*1e-9;
        const double finish = begin + static_cast<double>(nanos)*1e-9;
        const long double bound = static_cast<long double>(finish)*maxBpm*5765760.0L/60.0L;
        if (!std::isfinite(bound) || bound >= 0x1p62L) throw std::runtime_error("clip musical duration out of range");
        length = tempo.secondsToTicks(finish) - tempo.secondsToTicks(begin);
    }
    const auto offset = signedInteger(row.at("content_offset_ticks"));
    validTick(offset); validTick(length);
    if (length <= 0 || length > tickLimit-offset) throw std::runtime_error("clip end marker out of range");
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
struct Child { const char* table; const char* key; };
constexpr std::array<Child,7> children = {{{"event_streams","id"},{"note_expression","id"},
    {"audio_clips","clip_id"},{"automation_data","id"},{"automation_segment_ranges","id"},{"clip_properties","clip_id"},{"clip_playback_details","clip_id"}}};
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
        const auto row = core(context,id(payload));
        const auto value = signedInteger(payload.at("ticks")), finish = currentEnd(context,row);
        validTick(value); validTick(finish);
        if (value >= finish) throw std::runtime_error("Start must be before End");
        setProperties(context,id(payload),{{"end_marker_ticks",finish}}); // pin End while Start moves
        SQLite::Statement update(context.db,"UPDATE clips SET content_offset_ticks=? WHERE id=?");
        update.bind(1,value); update.bind(2,id(payload)); update.exec();
        clearClocks(context,id(payload),"content_offset_ns=NULL,in_begin_ns=NULL,in_end_ns=NULL,out_begin_ns=NULL,out_end_ns=NULL");
        return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
bool end(OpContext& context, const Payload& payload, std::string& error) {
    try {
        if (restore(context,payload)) return true;
        const auto row = core(context,id(payload)); const auto value = signedInteger(payload.at("ticks"));
        validTick(value);
        if (value <= signedInteger(row.at("content_offset_ticks"))) throw std::runtime_error("End must follow Start");
        setProperties(context,id(payload),{{"end_marker_ticks",value}});
        clearClocks(context,id(payload),"source_end_ns=NULL");
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

Payload captureDeleted(OpContext& context, std::int64_t clipId) {
    Payload result = {{"id",clipId},{"row",core(context,clipId)},{"children",Payload::object()},{"alias_links",Payload::array()}};
    for (const auto& child : children) {
        auto& records = result["children"][child.table]; records = Payload::array();
        if(!present(context.db,child.table))continue;
        SQLite::Statement query(context.db,"SELECT " + std::string(child.key) + " FROM " + child.table + " WHERE clip_id=? ORDER BY " + child.key);
        query.bind(1,clipId);
        while (query.executeStep()) records.push_back(*detail::captureRow(context.db,child.table,child.key,query.getColumn(0).getInt64()));
    }
    SQLite::Statement links(context.db,"SELECT id FROM clips WHERE alias_of=? ORDER BY id"); links.bind(1,clipId);
    while (links.executeStep()) result["alias_links"].push_back(links.getColumn(0).getInt64());
    return result;
}
void restoreDeleted(OpContext& context, const Payload& payload) {
    const auto clipId = id(payload);
    if (signedInteger(payload.at("row").at("id")) != clipId) throw std::runtime_error("captured clip id differs");
    detail::restoreRow(context.db,"clips",payload.at("row")); // never overwrite another id
    for (const auto& child : children) {
        if(!payload.at("children").contains(child.table))continue; // old captured images
        for (const auto& record : payload.at("children").at(child.table)) {
            if (signedInteger(record.at("clip_id")) != clipId) throw std::runtime_error("captured child clip differs");
            detail::restoreRow(context.db,child.table,record);
        }
    }
    for (const auto& link : payload.at("alias_links")) {
        SQLite::Statement update(context.db,"UPDATE clips SET alias_of=? WHERE id=?");
        update.bind(1,clipId); update.bind(2,signedInteger(link));
        if (update.exec() != 1) throw std::runtime_error("captured alias is missing");
    }
}
} // namespace adi::clip_properties
