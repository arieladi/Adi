// SPDX-License-Identifier: GPL-3.0-or-later
//
// See plugin_registry.hpp. ADR-0149.

#include "adi/plugin_registry.hpp"
#include "adi/audio/io_audit.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <utf8proc.h>

#include <stdexcept>
#include <system_error>
#include <utility>

namespace adi {

namespace {

// Version 2 adds durable scan quarantine. A later version appends the scan
// cache/AudioGridder catalogue (ADR-0145 d6); older builds refuse newer files.
constexpr const char* kSchema =
    "CREATE TABLE IF NOT EXISTS plugin_routes ("
    "  format      TEXT    NOT NULL,"
    "  uid         TEXT    NOT NULL,"
    "  route       TEXT    NOT NULL CHECK (route IN ('note_expression','mpe_midi','plain')),"
    "  updated_utc INTEGER NOT NULL,"
    "  PRIMARY KEY (format, uid)"
    ") STRICT, WITHOUT ROWID;"
    "CREATE TABLE IF NOT EXISTS plugin_scan_blocks ("
    "  id          INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  format      TEXT NOT NULL CHECK (format IN ('clap','vst3')),"
    "  path        TEXT NOT NULL CHECK (length(CAST(path AS BLOB)) BETWEEN 1 AND 65536),"
    "  state       TEXT NOT NULL CHECK (state IN ('pending','blocked')),"
    "  reason      TEXT NOT NULL CHECK (length(CAST(reason AS BLOB)) BETWEEN 1 AND 4096),"
    "  updated_utc INTEGER NOT NULL CHECK (updated_utc >= 0),"
    "  UNIQUE (format,path)"
    ") STRICT;";

bool text(const std::string& value, std::size_t maximum, bool empty = false) {
    if (value.size() > maximum || (!empty && value.empty()) || value.find('\0') != value.npos) return false;
    std::size_t offset = 0;
    while (offset < value.size()) {
        utf8proc_int32_t code = 0;
        const auto consumed = utf8proc_iterate(
            reinterpret_cast<const utf8proc_uint8_t*>(value.data() + offset),
            static_cast<utf8proc_ssize_t>(value.size() - offset), &code);
        if (consumed <= 0) return false;
        offset += static_cast<std::size_t>(consumed);
    }
    return true;
}
bool key(const std::string& format, const std::string& path) {
    return (format == "clap" || format == "vst3") && text(path,65536) &&
           std::filesystem::path(std::u8string(path.begin(),path.end())).is_absolute();
}
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void verifyScanColumns(SQLite::Database& db) {
    SQLite::Statement probe(db,"SELECT id,format,path,state,reason,updated_utc FROM plugin_scan_blocks LIMIT 0");
    probe.executeStep();
}

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
    if (audio::onAudioThread) return nullptr;
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
        db->setBusyTimeout(2000);   // ADI and ADiJ may both have it open
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
            verifyScanColumns(*db);
            db->exec("PRAGMA application_id = " + std::to_string(kApplicationId));
            db->exec("PRAGMA user_version = " + std::to_string(kVersion));
            txn.commit();
        }
        verifyScanColumns(*db);
        return std::unique_ptr<PluginRegistry>(new PluginRegistry(std::move(db)));
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
}

std::optional<engine::RouteChoice> PluginRegistry::route(const std::string& format,
                                                         const std::string& uid) const {
    if (audio::onAudioThread) return std::nullopt;
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
    if (audio::onAudioThread) return false;
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

std::optional<ScanTicket> PluginRegistry::beginScan(const std::string& format,
                                                   const std::string& path,
                                                   std::int64_t nowUtc, std::string& error) {
    if (audio::onAudioThread) return std::nullopt;
    error.clear();
    try {
        require(key(format,path) && nowUtc >= 0,"invalid scan identity or time");
        // Copy before the committed marker: allocation failure cannot leave a
        // pending attempt for a child the owner could never obtain a ticket for.
        ScanTicket ticket;ticket.format_=format;ticket.path_=path;ticket.owner_=scanOwner_;
        SQLite::Statement insert(*db_,
            "INSERT INTO plugin_scan_blocks(format,path,state,reason,updated_utc) "
            "SELECT ?,?,'pending','scan pending or interrupted',? "
            "WHERE (SELECT count(*) FROM plugin_scan_blocks) < 65536 "
            "ON CONFLICT(format,path) DO NOTHING");
        insert.bind(1,format);insert.bind(2,path);insert.bind(3,nowUtc);
        require(insert.exec()==1,"scan is blocked/pending or quarantine capacity is exhausted");
        ticket.id_=db_->getLastInsertRowid();
        return std::optional<ScanTicket>(std::move(ticket));
    } catch (const std::exception& failure) { error=failure.what();return std::nullopt; }
}

bool PluginRegistry::finishScan(const ScanTicket& ticket, ScanDisposition disposition,
                                 const std::string& reason, std::int64_t nowUtc, std::string& error) {
    if (audio::onAudioThread) return false;
    error.clear();
    try {
        require(ticket.owner_.lock()==scanOwner_ && ticket.id_>0,"foreign or expired scan ticket");
        require(nowUtc>=0,"invalid scan completion time");
        require(disposition==ScanDisposition::Succeeded || disposition==ScanDisposition::Abandoned ||
                disposition==ScanDisposition::Failed,"invalid scan disposition");
        const bool failed=disposition==ScanDisposition::Failed;
        require(failed ? text(reason,4096) : reason.empty(),"invalid scan diagnostic");
        SQLite::Statement finish(*db_,failed ?
            "UPDATE plugin_scan_blocks SET state='blocked',reason=?,updated_utc=? "
            "WHERE id=? AND format=? AND path=? AND state='pending'" :
            "DELETE FROM plugin_scan_blocks WHERE id=? AND format=? AND path=? AND state='pending'");
        int parameter=1;
        if(failed) {finish.bind(parameter++,reason);finish.bind(parameter++,nowUtc);}
        finish.bind(parameter++,ticket.id_);finish.bind(parameter++,ticket.format_);finish.bind(parameter,ticket.path_);
        require(finish.exec()==1,"scan ticket is no longer pending");
        return true;
    } catch(const std::exception& failure) {error=failure.what();return false;}
}

bool PluginRegistry::blockScan(const std::string& format, const std::string& path,
                                const std::string& reason, std::int64_t nowUtc, std::string& error) {
    if (audio::onAudioThread) return false;
    error.clear();
    try {
        require(key(format,path) && text(reason,4096) && nowUtc>=0,"invalid scan block");
        SQLite::Statement block(*db_,
            "INSERT INTO plugin_scan_blocks(format,path,state,reason,updated_utc) "
            "SELECT ?,?,'blocked',?,? WHERE "
            "(SELECT count(*) FROM plugin_scan_blocks) < 65536 OR "
            "EXISTS(SELECT 1 FROM plugin_scan_blocks WHERE format=? AND path=?) "
            "ON CONFLICT(format,path) DO UPDATE SET state='blocked',reason=excluded.reason,updated_utc=excluded.updated_utc");
        block.bind(1,format);block.bind(2,path);block.bind(3,reason);block.bind(4,nowUtc);
        block.bind(5,format);block.bind(6,path);
        require(block.exec()==1,"quarantine capacity is exhausted");
        return true;
    } catch(const std::exception& failure) {error=failure.what();return false;}
}

bool PluginRegistry::unblockScan(const std::string& format, const std::string& path, std::string& error) {
    if (audio::onAudioThread) return false;
    error.clear();
    try {
        require(key(format,path),"invalid scan identity");
        SQLite::Statement remove(*db_,"DELETE FROM plugin_scan_blocks WHERE format=? AND path=?");
        remove.bind(1,format);remove.bind(2,path);remove.exec();return true;
    } catch(const std::exception& failure) {error=failure.what();return false;}
}

std::optional<std::vector<ScanBlock>> PluginRegistry::scanBlocks(std::string& error,
                                                                std::size_t maximumRows) const {
    if (audio::onAudioThread) return std::nullopt;
    error.clear();
    try {
        require(maximumRows>0 && maximumRows<=kMaximumScanBlocks,"invalid quarantine read bound");
        SQLite::Statement query(*db_,
            "SELECT id,format,path,state,reason,updated_utc,"
            "length(CAST(format AS BLOB)),length(CAST(path AS BLOB)),"
            "length(CAST(state AS BLOB)),length(CAST(reason AS BLOB)) "
            "FROM plugin_scan_blocks ORDER BY format,path LIMIT ?");
        query.bind(1,static_cast<std::int64_t>(maximumRows)+1);
        std::vector<ScanBlock> result;
        std::size_t bytes=0;
        while(query.executeStep()) {
            require(result.size()<maximumRows,"quarantine list exceeds requested row bound");
            require(query.getColumn(0).isInteger() && query.getColumn(5).isInteger() &&
                    query.getColumn(1).isText() && query.getColumn(2).isText() &&
                    query.getColumn(3).isText() && query.getColumn(4).isText(),"invalid stored quarantine types");
            require(query.getColumn(6).getInt64()<=4 && query.getColumn(7).getInt64()<=65536 &&
                    query.getColumn(8).getInt64()<=7 && query.getColumn(9).getInt64()<=4096,
                    "stored quarantine text exceeds row bounds");
            ScanBlock row;row.format=query.getColumn(1).getString();row.path=query.getColumn(2).getString();
            const auto state=query.getColumn(3).getString();row.reason=query.getColumn(4).getString();
            row.updatedUtc=query.getColumn(5).getInt64();
            require(query.getColumn(0).getInt64()>0 && key(row.format,row.path) && text(row.reason,4096) &&
                    row.updatedUtc>=0 && (state=="pending" || state=="blocked"),"invalid stored quarantine row");
            row.state=state=="pending"?ScanBlockState::Pending:ScanBlockState::Blocked;
            const auto size=row.format.size()+row.path.size()+row.reason.size();
            require(size<=16U*1024U*1024U-bytes,"quarantine list exceeds text budget");bytes+=size;
            result.push_back(std::move(row));
        }
        return result;
    } catch(const std::exception& failure) {error=failure.what();return std::nullopt;}
}

}  // namespace adi
