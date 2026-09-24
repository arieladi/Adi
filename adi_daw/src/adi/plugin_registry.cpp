// SPDX-License-Identifier: GPL-3.0-or-later
//
// See plugin_registry.hpp. ADR-0149.

#include "adi/plugin_registry.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <system_error>
#include <utility>

namespace adi {

namespace {

// Version 1. A later version appends tables (the scan cache, the AudioGridder
// catalogue of ADR-0145 d6); an older build refuses a newer file rather than
// guessing at it.
constexpr const char* kSchema =
    "CREATE TABLE IF NOT EXISTS plugin_routes ("
    "  format      TEXT    NOT NULL,"
    "  uid         TEXT    NOT NULL,"
    "  route       TEXT    NOT NULL CHECK (route IN ('note_expression','mpe_midi','plain')),"
    "  updated_utc INTEGER NOT NULL,"
    "  PRIMARY KEY (format, uid)"
    ") STRICT, WITHOUT ROWID;";

// SQLite takes UTF-8 file names; path::string() is the ANSI code page on
// Windows, and a profile folder with a Hebrew name would not open.
std::string utf8(const std::filesystem::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

}  // namespace

PluginRegistry::PluginRegistry(std::unique_ptr<SQLite::Database> db) : db_(std::move(db)) {}
PluginRegistry::~PluginRegistry() = default;

std::unique_ptr<PluginRegistry> PluginRegistry::open(const std::filesystem::path& file,
                                                     std::string& error) {
    if (file.empty()) {
        error = "no application data folder on this machine";
        return nullptr;
    }
    std::error_code ec;
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
    if (ec) {
        error = "cannot create " + utf8(file.parent_path()) + ": " + ec.message();
        return nullptr;
    }
    try {
        auto db = std::make_unique<SQLite::Database>(
            utf8(file), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        db->setBusyTimeout(2000);   // ADI DAW and aDiJ may both have it open
        const int appId = db->execAndGet("PRAGMA application_id").getInt();
        const int version = db->execAndGet("PRAGMA user_version").getInt();
        const bool fresh = appId == 0 && version == 0;
        if (!fresh && appId != kApplicationId) {
            error = utf8(file) + " is not a plugin registry";
            return nullptr;
        }
        if (version > kVersion) {
            error = utf8(file) + " is registry version " + std::to_string(version) +
                    "; this build reads up to " + std::to_string(kVersion);
            return nullptr;
        }
        db->exec("PRAGMA journal_mode = WAL");
        if (version < kVersion) {
            SQLite::Transaction txn(*db);
            db->exec(kSchema);
            db->exec("PRAGMA application_id = " + std::to_string(kApplicationId));
            db->exec("PRAGMA user_version = " + std::to_string(kVersion));
            txn.commit();
        }
        return std::unique_ptr<PluginRegistry>(new PluginRegistry(std::move(db)));
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
}

std::optional<engine::RouteChoice> PluginRegistry::route(const std::string& format,
                                                         const std::string& uid) const {
    try {
        SQLite::Statement st(*db_, "SELECT route FROM plugin_routes WHERE format = ? AND uid = ?");
        st.bind(1, format);
        st.bind(2, uid);
        if (!st.executeStep()) return std::nullopt;
        bool known = false;
        const engine::RouteChoice c = engine::routeChoiceFromName(st.getColumn(0).getString(), &known);
        if (!known || c == engine::RouteChoice::Auto) return std::nullopt;
        return c;
    } catch (const std::exception&) {
        return std::nullopt;   // a registry that cannot answer proposes nothing: Auto
    }
}

bool PluginRegistry::remember(const std::string& format, const std::string& uid,
                              engine::RouteChoice route, std::int64_t nowUtc,
                              std::string& error) {
    try {
        if (route == engine::RouteChoice::Auto) {
            SQLite::Statement del(*db_, "DELETE FROM plugin_routes WHERE format = ? AND uid = ?");
            del.bind(1, format);
            del.bind(2, uid);
            del.exec();
            return true;
        }
        SQLite::Statement st(*db_,
            "INSERT INTO plugin_routes(format, uid, route, updated_utc) VALUES (?,?,?,?) "
            "ON CONFLICT(format, uid) DO UPDATE SET route = excluded.route, "
            "updated_utc = excluded.updated_utc");
        st.bind(1, format);
        st.bind(2, uid);
        st.bind(3, engine::routeChoiceName(route));
        st.bind(4, nowUtc);
        st.exec();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

}  // namespace adi
