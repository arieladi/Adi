// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/control/headless_project.hpp"

#include "adi/media/blake3.hpp"
#include "adi/ops.hpp"
#include "adi/store.hpp"

#include <SQLiteCpp/Backup.h>
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <random>
#include <span>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <signal.h>
#include <unistd.h>
#endif

namespace adi::control {
namespace fs = std::filesystem;

namespace {

std::string utf8(const fs::path& p) {
    const auto bytes = p.generic_u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::string randomHex(std::size_t bytes) {
    std::random_device rd;
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes * 2);
    for (std::size_t i = 0; i < bytes; ++i) {
        const auto b = static_cast<unsigned>(rd() & 0xffu);
        out += kHex[b >> 4];
        out += kHex[b & 0xfu];
    }
    return out;
}

std::uint64_t thisProcess() {
#if defined(_WIN32)
    return GetCurrentProcessId();
#else
    return static_cast<std::uint64_t>(getpid());
#endif
}

/// Whether a process with this id is running. When it cannot tell, it says
/// yes: a scratch folder is only ever removed for a process known to be gone.
bool processAlive(std::uint64_t pid) {
#if defined(_WIN32)
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!h) return GetLastError() == ERROR_ACCESS_DENIED;
    DWORD code = 0;
    const bool running = GetExitCodeProcess(h, &code) == 0 || code == STILL_ACTIVE;
    CloseHandle(h);
    return running;
#else
    if (pid == 0 || pid > static_cast<std::uint64_t>(INT32_MAX)) return true;
    if (kill(static_cast<pid_t>(pid), 0) == 0) return true;
    return errno != ESRCH;
#endif
}

constexpr const char* kScratchPrefix = "adi-mcp-";

/// The private folder a snapshot lives in: under the user's own temp folder,
/// named for this process, owner-only where the platform lets us say so.
bool makeScratch(fs::path& out, std::string& error) {
    std::error_code ec;
    const fs::path base = fs::temp_directory_path(ec);
    if (ec) {
        error = "no temporary folder: " + ec.message();
        return false;
    }
    const std::string stem = std::string(kScratchPrefix) + std::to_string(thisProcess()) + "-";
    for (int attempt = 0; attempt < 8; ++attempt) {
        const fs::path candidate = base / (stem + randomHex(8));
        if (fs::create_directory(candidate, ec)) {
#if !defined(_WIN32)
            fs::permissions(candidate, fs::perms::owner_all, fs::perm_options::replace, ec);
            ec.clear();
#endif
            out = candidate;
            return true;
        }
        if (ec) {
            error = "cannot create a scratch folder: " + ec.message();
            return false;
        }
    }
    error = "cannot create a scratch folder";
    return false;
}

std::string metaValue(Store& store, const char* key) {
    try {
        SQLite::Statement st(store.db(), "SELECT value FROM adi_meta WHERE key = ?");
        st.bind(1, key);
        if (st.executeStep() && !st.getColumn(0).isNull()) return st.getColumn(0).getString();
    } catch (const std::exception&) {
    }
    return {};
}

/// The undo head on `store`, reporting failure instead of reading it as the
/// root (OpJournal::headSeq answers nullopt for both).
std::optional<std::int64_t> headOf(Store& store, std::string& error) {
    try {
        SQLite::Statement st(store.db(), "SELECT head_seq FROM op_branches WHERE is_current = 1");
        if (!st.executeStep() || st.getColumn(0).isNull()) return std::nullopt;
        return st.getColumn(0).getInt64();
    } catch (const std::exception& e) {
        error = std::string("cannot read the head: ") + e.what();
        return std::nullopt;
    }
}

}  // namespace

std::size_t HeadlessProject::sweepAbandonedScratch() {
    std::error_code ec;
    const fs::path base = fs::temp_directory_path(ec);
    if (ec) return 0;
    std::size_t removed = 0;
    fs::directory_iterator it(base, fs::directory_options::skip_permission_denied, ec);
    if (ec) return 0;
    const std::string prefix(kScratchPrefix);
    for (; it != fs::directory_iterator(); it.increment(ec)) {
        if (ec) break;
        const std::string name = utf8(it->path().filename());
        if (name.rfind(prefix, 0) != 0) continue;
        const auto dash = name.find('-', prefix.size());
        if (dash == std::string::npos || dash == prefix.size()) continue;
        std::uint64_t pid = 0;
        bool digits = true;
        for (std::size_t i = prefix.size(); i < dash; ++i) {
            const char c = name[i];
            if (c < '0' || c > '9' || pid > (UINT64_MAX - 9) / 10) {
                digits = false;
                break;
            }
            pid = pid * 10 + static_cast<std::uint64_t>(c - '0');
        }
        if (!digits || pid == thisProcess() || processAlive(pid)) continue;
        std::error_code rm;
        if (fs::is_directory(it->path(), rm) && fs::remove_all(it->path(), rm) > 0 && !rm) ++removed;
    }
    return removed;
}

HeadlessProject::HeadlessProject() = default;

std::unique_ptr<Store> HeadlessProject::openReader(std::string& error) const {
    StoreError se = StoreError::Ok;
    auto reader = Store::open(file_, se, /*readOnly=*/true);
    if (!reader) error = std::string("cannot open the project read-only: ") + toString(se);
    return reader;
}

std::shared_ptr<HeadlessProject> HeadlessProject::open(const fs::path& file, std::string& error) {
    std::error_code ec;
    const fs::path absolute = fs::absolute(file, ec);
    if (ec || !fs::is_regular_file(absolute, ec)) {
        error = "no project file at that path";
        return nullptr;
    }
    std::shared_ptr<HeadlessProject> p(new HeadlessProject());
    p->file_ = absolute;
    auto reader = p->openReader(error);
    if (!reader) return nullptr;

    // ADR-0232 d2's explicit project: the file's project_uuid when it has one
    // (AGT-02 writes it), else a stable opaque handle. A path never leaves.
    p->handle_ = metaValue(*reader, "project_uuid");
    if (p->handle_.empty()) {
        const std::string id = utf8(fs::weakly_canonical(absolute, ec));
        const auto hash = media::blake3Bytes(std::as_bytes(std::span(id.data(), id.size())));
        p->handle_ = "file-" + (hash ? hash.hex.substr(0, 16) : randomHex(8));
    }
    try {
        SQLite::Statement st(reader->db(), "SELECT name FROM project WHERE id = 1");
        if (st.executeStep() && !st.getColumn(0).isNull()) p->label_ = st.getColumn(0).getString();
    } catch (const std::exception&) {
    }
    if (p->label_.empty()) p->label_ = utf8(absolute.stem());
    reader->close();   // read-only: closing writes nothing (Store::closeFinalized)
    return p;
}

HeadlessProject::~HeadlessProject() {
    std::lock_guard lock(mutex_);
    dropSnapshotLocked();
    if (!scratch_.empty()) {
        std::error_code ec;
        fs::remove_all(scratch_, ec);
    }
}

std::string HeadlessProject::label() const { return label_; }

ReadSnapshot HeadlessProject::read(std::string& error) {
    std::lock_guard lock(mutex_);
    ReadSnapshot out;
    auto reader = openReader(error);
    if (!reader) return out;
    try {
        // One read transaction: the head first, which starts the snapshot,
        // then the rows, which rows::readModel reads inside it rather than in
        // a transaction of its own.
        SQLite::Transaction snapshot(reader->db(), SQLite::TransactionBehavior::DEFERRED);
        out.head = headOf(*reader, error);
        if (error.empty()) out.model = rows::readModel(*reader);
    } catch (const std::exception& e) {
        error = std::string("cannot read the project: ") + e.what();
    }
    reader->close();
    return out;
}

std::optional<std::int64_t> HeadlessProject::head(std::string& error) {
    std::lock_guard lock(mutex_);
    auto reader = openReader(error);
    if (!reader) return std::nullopt;
    const auto h = headOf(*reader, error);
    reader->close();
    return h;
}

std::string HeadlessProject::fileSchema() {
    std::lock_guard lock(mutex_);
    std::string error;
    auto reader = openReader(error);
    if (!reader) return {};
    std::string out;
    try {
        const auto v = reader->db().execAndGet("PRAGMA user_version").getInt64();
        out = std::to_string(v / 1000) + "." + std::to_string(v % 1000);
    } catch (const std::exception&) {
    }
    reader->close();
    return out;
}

void HeadlessProject::dropSnapshotLocked() {
    if (snapshot_) {
        snapshot_->close();
        snapshot_.reset();
    }
    snapshotHead_.reset();
    if (!scratch_.empty()) {
        std::error_code ec;
        for (const char* suffix : {"", "-wal", "-shm", "-journal"})
            fs::remove(scratch_ / (std::string("snapshot.adi") + suffix), ec);
    }
}

bool HeadlessProject::refreshSnapshotLocked(std::string& error) {
    auto reader = openReader(error);
    if (!reader) return false;
    const auto current = headOf(*reader, error);
    if (!error.empty()) return false;
    if (snapshot_ && snapshotHead_ == current) return true;

    dropSnapshotLocked();
    if (scratch_.empty() && !makeScratch(scratch_, error)) return false;
    const fs::path copy = scratch_ / "snapshot.adi";
    try {
        // The backup API copies the committed database page by page under one
        // read transaction: a consistent snapshot, whoever else has the file
        // open, taken through the READ-ONLY connection.
        SQLite::Database destination(utf8(copy), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        SQLite::Backup backup(destination, reader->db());
        const int rc = backup.executeStep();
        if (rc != SQLITE_DONE) {
            error = rc == SQLITE_BUSY || rc == SQLITE_LOCKED
                        ? "the project is busy: another program is writing it; try again"
                        : "cannot snapshot the project (SQLite " + std::to_string(rc) + ")";
            return false;
        }
    } catch (const std::exception& e) {
        error = std::string("cannot snapshot the project: ") + e.what();
        return false;
    }
    reader->close();
    StoreError se = StoreError::Ok;
    snapshot_ = Store::open(copy, se, /*readOnly=*/false);
    if (!snapshot_) {
        error = std::string("cannot open the snapshot: ") + toString(se);
        return false;
    }
    std::string snapError;
    snapshotHead_ = headOf(*snapshot_, snapError);
    if (!snapError.empty()) {
        error = snapError;
        dropSnapshotLocked();
        return false;
    }
    return true;
}

agent::Preview HeadlessProject::preview(const agent::Changeset& cs, const agent::Limits& limits) {
    std::lock_guard lock(mutex_);
    std::string error;
    if (!refreshSnapshotLocked(error)) {
        agent::Preview p;
        p.refusal = agent::Refusal::ReadOnly;
        p.error = error;
        return p;
    }
    return agent::preview(*snapshot_, cs, limits);
}

bool HeadlessProject::wasApplied(const std::string& actorDetail) {
    std::lock_guard lock(mutex_);
    std::string error;
    auto reader = openReader(error);
    if (!reader) return false;
    bool applied = false;
    try {
        // `main.` because an older-minor file opened read-only carries TEMP
        // stand-ins for the tables it lacks, which shadow main's.
        if (reader->db().execAndGet("SELECT COUNT(*) FROM main.sqlite_master WHERE type = 'table' AND "
                                    "name = 'agent_requests'").getInt() != 0) {
            SQLite::Statement st(reader->db(),
                                 "SELECT COUNT(*) FROM main.agent_requests WHERE actor_detail = ?");
            st.bind(1, actorDetail);
            applied = st.executeStep() && st.getColumn(0).getInt64() > 0;
        }
    } catch (const std::exception&) {
        applied = false;
    }
    reader->close();
    return applied;
}

std::string HeadlessProject::applyHint() const {
    return "Nothing is applied until you apply it. Call changeset_status with this id, save its "
           "`changeset` as a file, and run: adi_tool propose \"" + utf8(file_.filename()) +
           "\" <saved changeset.json> --apply";
}

}  // namespace adi::control
