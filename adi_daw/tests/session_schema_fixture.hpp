// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace adi::test {

inline std::filesystem::path sessionSchemaFixturePath() {
    return std::filesystem::path(__FILE__).parent_path().parent_path() /
        "docs" / "format" / "schema-session-1.26.sql";
}

inline void applySessionSchema(SQLite::Database& db,
                               const std::filesystem::path& sqlFile = sessionSchemaFixturePath()) {
    if (db.execAndGet("PRAGMA user_version").getInt() >= kSessionSchemaUserVersion) {
        const auto count = db.execAndGet("SELECT count(*) FROM main.sqlite_master WHERE type='table' "
                                         "AND name IN ('scenes','clip_slots','clip_launch')").getInt();
        if (count != 3)
            throw std::runtime_error("Session migration is incomplete");
        return;
    }
    std::ifstream in(sqlFile, std::ios::binary);
    if (!in)
        throw std::runtime_error("could not open the reserved Session schema");
    const std::string ddl{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    SQLite::Transaction transaction(db);
    db.exec(ddl);
    transaction.commit();
}

inline void seedSessionMarker(SQLite::Database& db) {
    db.exec("INSERT INTO extensions(ns,key,criticality,min_reader) "
            "VALUES('org.adidaw.session','clip_slots','essential',1026)");
}

} // namespace adi::test
