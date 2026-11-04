// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/marker_ops.hpp"
#include "adi/row_capture.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <stdexcept>

namespace adi::marker {
namespace {
using detail::signedInteger;
std::int64_t id(const Payload& payload) { return signedInteger(payload.at("id")); }
Payload row(OpContext& context, const Payload& payload) {
    auto value = detail::captureRow(context.db, "markers", "id", id(payload));
    if (!value) throw std::runtime_error("no such marker");
    return std::move(*value);
}
bool restoreCore(OpContext& context, const Payload& payload) {
    if (!payload.contains("row")) return false;
    const auto& captured = payload.at("row");
    if (signedInteger(captured.at("id")) != id(payload))
        throw std::runtime_error("captured marker id differs");
    (void)row(context, payload);
    // Do not delete/reinsert: a scalar edit must not cascade child records.
    detail::restoreRow(context.db, "markers", captured, "id");
    return true;
}
void details(OpContext& context, const Payload& payload, const char* field) {
    (void)row(context, payload);
    if (payload.contains("details")) {
        const auto& captured = payload.at("details");
        if (captured.is_null()) {
            SQLite::Statement clear(context.db, "DELETE FROM marker_details WHERE marker_id=?");
            clear.bind(1, id(payload)); clear.exec();
        } else {
            if (signedInteger(captured.at("marker_id")) != id(payload))
                throw std::runtime_error("captured details id differs");
            detail::restoreRow(context.db, "marker_details", captured, "marker_id");
        }
        return;
    }
    SQLite::Statement update(context.db,
        "INSERT INTO marker_details(marker_id," + std::string(field) + ") VALUES(?,?) "
        "ON CONFLICT(marker_id) DO UPDATE SET " + field + "=excluded." + field);
    update.bind(1, id(payload)); update.bind(2, payload.at("text").get<std::string>()); update.exec();
}
} // namespace

bool create(OpContext& context, const Payload& payload, std::string& error) {
    try {
        // Restore payloads only come from captured history, never submission.
        // A create restore must not replace an existing marker.
        if (payload.contains("row")) {
            if (signedInteger(payload.at("row").at("id")) != id(payload))
                throw std::runtime_error("captured marker id differs");
            if (detail::captureRow(context.db, "markers", "id", id(payload)))
                throw std::runtime_error("marker already exists");
            detail::restoreRow(context.db, "markers", payload.at("row"));
            if (payload.contains("details") && !payload.at("details").is_null()) {
                const auto& captured = payload.at("details");
                if (signedInteger(captured.at("marker_id")) != id(payload))
                    throw std::runtime_error("captured marker details differ");
                detail::restoreRow(context.db, "marker_details", captured);
            }
            return true;
        }
        const auto base = payload.contains("time_base") ? signedInteger(payload.at("time_base")) : 0;
        const auto position = signedInteger(payload.at("pos"));
        const auto kind = payload.value("kind", payload.contains("length") ? std::string("cycle")
                                                                         : std::string("position"));
        if (base != 0 && base != 1) throw std::runtime_error("invalid marker time base");
        if (kind != "position" && kind != "cycle" && kind != "chapter" && kind != "sync")
            throw std::runtime_error("invalid marker kind");
        if (kind == "cycle" && (!payload.contains("length") || signedInteger(payload.at("length")) <= 0))
            throw std::runtime_error("cycle marker needs positive length");
        if (kind != "cycle" && payload.contains("length"))
            throw std::runtime_error("only cycle markers have length");
        Payload captured = {{"id", id(payload)}, {"time_base", base}, {"pos_ticks", nullptr}, {"pos_ns", nullptr},
            {"length_ticks", nullptr}, {"length_ns", nullptr}, {"name", payload.value("name", std::string{})},
            {"kind", kind}, {"color", nullptr}};
        captured[base == 0 ? "pos_ticks" : "pos_ns"] = position;
        if (payload.contains("length"))
            captured[base == 0 ? "length_ticks" : "length_ns"] = signedInteger(payload.at("length"));
        if (payload.contains("color")) {
            const auto index = signedInteger(payload.at("color"));
            if (index < 0 || index >= 70) throw std::runtime_error("marker color is not a Live palette index");
            captured["color"] = index;
        }
        detail::restoreRow(context.db, "markers", captured);
        const auto noteText = payload.value("note", std::string{});
        const auto promptText = payload.value("prompt", std::string{});
        if (!noteText.empty() || !promptText.empty())
            detail::restoreRow(context.db, "marker_details",
                               {{"marker_id", id(payload)}, {"note", noteText}, {"prompt", promptText}});
        return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
bool createInverse(OpContext&, const Payload& payload, Payload& inverse, std::string& error) {
    try { inverse = {{"id", id(payload)}}; return true; }
    catch (const std::exception& e) { error = e.what(); return false; }
}
bool erase(OpContext& context, const Payload& payload, std::string& error) {
    try {
        (void)row(context, payload);
        SQLite::Statement eraseRow(context.db, "DELETE FROM markers WHERE id=?");
        eraseRow.bind(1, id(payload)); eraseRow.exec();
        return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
bool eraseInverse(OpContext& context, const Payload& payload, Payload& inverse, std::string& error) {
    try {
        inverse = {{"id", id(payload)}, {"row", row(context, payload)}};
        inverse["details"] = detail::captureRow(context.db, "marker_details", "marker_id", id(payload))
                                 .value_or(Payload(nullptr));
        return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
bool move(OpContext& context, const Payload& payload, std::string& error) {
    try {
        const auto old = row(context, payload);
        SQLite::Statement update(context.db, old.at("time_base") == 0
            ? "UPDATE markers SET pos_ticks=? WHERE id=?" : "UPDATE markers SET pos_ns=? WHERE id=?");
        update.bind(1, signedInteger(payload.at("pos"))); update.bind(2, id(payload)); update.exec();
        return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
bool moveInverse(OpContext& context, const Payload& payload, Payload& inverse, std::string& error) {
    try {
        const auto old = row(context, payload);
        inverse = {{"id", id(payload)}, {"pos", old.at(old.at("time_base") == 0 ? "pos_ticks" : "pos_ns")}};
        return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
bool rename(OpContext& context, const Payload& payload, std::string& error) {
    try {
        (void)row(context, payload);
        SQLite::Statement update(context.db, "UPDATE markers SET name=? WHERE id=?");
        update.bind(1, payload.at("name").get<std::string>()); update.bind(2, id(payload)); update.exec();
        return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
bool renameInverse(OpContext& context, const Payload& payload, Payload& inverse, std::string& error) {
    try { inverse = {{"id", id(payload)}, {"name", row(context, payload).at("name")}}; return true; }
    catch (const std::exception& e) { error = e.what(); return false; }
}
bool color(OpContext& context, const Payload& payload, std::string& error) {
    try {
        if (restoreCore(context, payload)) return true;
        (void)row(context, payload);
        SQLite::Statement update(context.db, "UPDATE markers SET color=? WHERE id=?");
        if (payload.contains("color")) {
            const auto index = signedInteger(payload.at("color"));
            if (index < 0 || index >= 70) throw std::runtime_error("invalid marker palette index");
            update.bind(1, index);
        } else update.bind(1);
        update.bind(2, id(payload)); update.exec();
        return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
bool rowInverse(OpContext& context, const Payload& payload, Payload& inverse, std::string& error) {
    try { inverse = {{"id", id(payload)}, {"row", row(context, payload)}}; return true; }
    catch (const std::exception& e) { error = e.what(); return false; }
}
bool resize(OpContext& context, const Payload& payload, std::string& error) {
    try {
        if (restoreCore(context, payload)) return true;
        const auto old = row(context, payload);
        if (old.at("kind") != "cycle" || signedInteger(payload.at("length")) <= 0)
            throw std::runtime_error("resize requires a cycle marker and positive length");
        SQLite::Statement update(context.db, old.at("time_base") == 0
            ? "UPDATE markers SET length_ticks=? WHERE id=?" : "UPDATE markers SET length_ns=? WHERE id=?");
        update.bind(1, signedInteger(payload.at("length"))); update.bind(2, id(payload)); update.exec();
        return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
bool note(OpContext& context, const Payload& payload, std::string& error) {
    try { details(context, payload, "note"); return true; }
    catch (const std::exception& e) { error = e.what(); return false; }
}
bool prompt(OpContext& context, const Payload& payload, std::string& error) {
    try { details(context, payload, "prompt"); return true; }
    catch (const std::exception& e) { error = e.what(); return false; }
}
bool detailsInverse(OpContext& context, const Payload& payload, Payload& inverse, std::string& error) {
    try {
        (void)row(context, payload);
        inverse = {{"id", id(payload)},
            {"details", detail::captureRow(context.db, "marker_details", "marker_id", id(payload)).value_or(Payload(nullptr))}};
        return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
} // namespace adi::marker
