// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/plugin_registry.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <stdexcept>

namespace adi::test {
// Historical shipped schema, rather than a v3 database with a lowered pragma.
// v1: 012fa6cc^; v2: 012fa6cc, src/adi/plugin_registry.cpp.
inline void createLegacyPluginRegistry(const std::filesystem::path& file,int version) {
    if(version!=1 && version!=2)throw std::runtime_error("legacy fixture version");
    const auto bytes=file.u8string();
    SQLite::Database db(std::string(bytes.begin(),bytes.end()),SQLite::OPEN_READWRITE|SQLite::OPEN_CREATE);
    db.exec("PRAGMA application_id="+std::to_string(PluginRegistry::kApplicationId));
    db.exec("PRAGMA user_version="+std::to_string(version));
    db.exec("CREATE TABLE plugin_routes (format TEXT NOT NULL,uid TEXT NOT NULL,"
        "route TEXT NOT NULL CHECK(route IN ('note_expression','mpe_midi','plain')),"
        "updated_utc INTEGER NOT NULL,PRIMARY KEY(format,uid)) STRICT,WITHOUT ROWID");
    db.exec("INSERT INTO plugin_routes VALUES('clap','legacy.preference','mpe_midi',42)");
    if(version==2)db.exec("CREATE TABLE plugin_scan_blocks (id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "format TEXT NOT NULL CHECK(format IN ('clap','vst3')),"
        "path TEXT NOT NULL CHECK(length(CAST(path AS BLOB)) BETWEEN 1 AND 65536),"
        "state TEXT NOT NULL CHECK(state IN ('pending','blocked')),"
        "reason TEXT NOT NULL CHECK(length(CAST(reason AS BLOB)) BETWEEN 1 AND 4096),"
        "updated_utc INTEGER NOT NULL CHECK(updated_utc>=0),UNIQUE(format,path)) STRICT");
}
} // namespace adi::test
