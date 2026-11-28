// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/control/agent_log.hpp"

#include "adi/appdata.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/control/consent_file.hpp"
#include "adi/media/blake3.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <span>
#include <thread>
#include <utility>

namespace adi::control {
namespace {

using namespace std::chrono_literals;
using detail::PrivateFileStatus;

constexpr int kApplicationId = 0x41494C31; // AIL1
constexpr int kVersion = 1;
constexpr std::size_t kMaxFieldBytes = 1024;
constexpr std::size_t kMaxTextBytes = 1024 * 1024;
constexpr std::size_t kPageBytes = 4 * 1024 * 1024;
constexpr std::size_t kPageEntries = 1024;
constexpr int kLastKind = static_cast<int>(AgentLogKind::StopPress);

std::filesystem::path defaultDirectory() {
    const auto data = appdata::pathsFor(appdata::App::Daw).data;
    return data.empty() ? std::filesystem::path{} : data / "agent-log";
}

std::string pathUtf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

std::int64_t utcMicros() {
    using namespace std::chrono;
    return duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
}

bool knownKind(int kind) { return kind >= 0 && kind <= kLastKind; }

bool valid(const AgentLogEvent& event) {
    return knownKind(static_cast<int>(event.kind)) && event.text.size() <= kMaxTextBytes &&
           event.id.size() <= kMaxFieldBytes && event.state.size() <= kMaxFieldBytes &&
           event.actor.size() <= kMaxFieldBytes && event.source.size() <= kMaxFieldBytes;
}

std::shared_ptr<detail::PrivateFileLock> lockFile(const std::filesystem::path& file, bool create) {
    // The first writer can be establishing the schema, and another ADI
    // process can be appending here. Retry only a known busy native lock.
    const auto until = std::chrono::steady_clock::now() + 1s;
    for (;;) {
        auto tried = detail::lockPrivateFile(file, create);
        if (tried.lock || !tried.busy) return std::move(tried.lock);
        if (std::chrono::steady_clock::now() >= until) return {};
        std::this_thread::sleep_for(5ms);
    }
}

bool supported(SQLite::Database& db) {
    (void)sqlite3_limit(db.getHandle(), SQLITE_LIMIT_LENGTH, 2 * 1024 * 1024);
    return db.execAndGet("PRAGMA application_id").getInt() == kApplicationId &&
           db.execAndGet("PRAGMA user_version").getInt() == kVersion;
}

bool prepare(SQLite::Database& db) {
    if (supported(db)) return true;
    // A first append reserves an empty owner-only file before opening SQLite.
    // It can be initialized after a crash in that interval as well.
    if (db.execAndGet("PRAGMA page_count").getInt() != 0) return false;
    db.exec(
        "CREATE TABLE events("
        "sequence INTEGER PRIMARY KEY AUTOINCREMENT,"
        "at_utc INTEGER NOT NULL,kind INTEGER NOT NULL CHECK(kind BETWEEN 0 AND 5),"
        "id TEXT NOT NULL,text TEXT NOT NULL,state TEXT NOT NULL,actor TEXT NOT NULL,source TEXT NOT NULL"
        ") STRICT;"
        "PRAGMA application_id=" + std::to_string(kApplicationId) + ";"
        "PRAGMA user_version=" + std::to_string(kVersion));
    return true;
}

}  // namespace

AgentLog::AgentLog() : AgentLog(defaultDirectory()) {}

AgentLog::AgentLog(std::filesystem::path directory) : directory_(std::move(directory)) {}

std::filesystem::path AgentLog::fileFor(std::string_view projectUuid) const {
    if (directory_.empty() || projectUuid.empty() || projectUuid.size() > 128) return {};
    const auto hash = media::blake3Bytes(std::as_bytes(std::span(projectUuid)));
    return hash ? directory_ / (hash.hex + ".sqlite") : std::filesystem::path{};
}

bool AgentLog::append(std::string_view projectUuid, const AgentLogEvent& event) const {
    if (audio::onAudioThread || !valid(event)) return false;
    try {
        const auto file = fileFor(projectUuid);
        if (file.empty()) return false;
        const auto locked = lockFile(file, true);
        if (!locked) return false;
        const auto status = detail::privateFileStatus(file);
        if (status == PrivateFileStatus::Unavailable) return false;
        if (status == PrivateFileStatus::Missing && !detail::writeConsentFile(file, {})) return false;

        SQLite::Database db(pathUtf8(file), SQLite::OPEN_READWRITE | SQLite::OPEN_NOFOLLOW);
        db.exec("PRAGMA synchronous=FULL");
        SQLite::Transaction transaction(db);
        if (!prepare(db)) return false;
        SQLite::Statement insert(db, "INSERT INTO events(at_utc,kind,id,text,state,actor,source) VALUES(?,?,?,?,?,?,?)");
        insert.bind(1, utcMicros());
        insert.bind(2, static_cast<int>(event.kind));
        insert.bind(3, event.id);
        insert.bind(4, event.text);
        insert.bind(5, event.state);
        insert.bind(6, event.actor);
        insert.bind(7, event.source);
        insert.exec();
        transaction.commit();
        return true;
    } catch (...) {
        return false;
    }
}

std::optional<std::vector<AgentLogEntry>> AgentLog::read(
    std::string_view projectUuid, std::int64_t afterSequence, std::size_t maximum) const {
    if (audio::onAudioThread || afterSequence < 0) return std::nullopt;
    try {
        std::vector<AgentLogEntry> entries;
        const auto file = fileFor(projectUuid);
        if (file.empty()) return std::nullopt;
        const auto status = detail::privateFileStatus(file);
        if (status == PrivateFileStatus::Missing) return entries;
        if (status != PrivateFileStatus::Private) return std::nullopt;
        const auto locked = lockFile(file, false);
        if (!locked || detail::privateFileStatus(file) != PrivateFileStatus::Private) return std::nullopt;

        SQLite::Database db(pathUtf8(file), SQLite::OPEN_READONLY | SQLite::OPEN_NOFOLLOW);
        if (!supported(db)) {
            if (db.execAndGet("PRAGMA page_count").getInt() == 0) return entries;
            return std::nullopt;
        }
        SQLite::Statement rows(db,
            "SELECT sequence,at_utc,kind,id,text,state,actor,source FROM events "
            "WHERE sequence>? ORDER BY sequence LIMIT ?");
        rows.bind(1, afterSequence);
        rows.bind(2, static_cast<int>(std::min(maximum, kPageEntries)));
        std::size_t bytes = 0;
        while (rows.executeStep()) {
            const int kind = rows.getColumn(2).getInt();
            if (!knownKind(kind)) return std::nullopt;
            AgentLogEvent event{static_cast<AgentLogKind>(kind), rows.getColumn(3).getString(),
                                rows.getColumn(4).getString(), rows.getColumn(5).getString(),
                                rows.getColumn(6).getString(), rows.getColumn(7).getString()};
            if (!valid(event)) return std::nullopt;
            const auto size = event.id.size() + event.text.size() + event.state.size() +
                              event.actor.size() + event.source.size();
            if (size > kPageBytes - bytes) break;
            bytes += size;
            entries.push_back({rows.getColumn(0).getInt64(), rows.getColumn(1).getInt64(), std::move(event)});
        }
        return entries;
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace adi::control
