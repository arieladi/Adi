// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/control/headless_project.hpp"

#include "adi/media/blake3.hpp"
#include "adi/ops.hpp"
#include "adi/store.hpp"

#include <SQLiteCpp/Backup.h>
#include <SQLiteCpp/SQLiteCpp.h>

#include <cstddef>
#include <exception>
#include <random>
#include <span>
#include <system_error>

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

/// The private folder a snapshot lives in: under the user's own temp folder,
/// owner-only where the platform lets us say so.
bool makeScratch(fs::path& out, std::string& error) {
    std::error_code ec;
    const fs::path base = fs::temp_directory_path(ec);
    if (ec) {
        error = "no temporary folder: " + ec.message();
        return false;
    }
    for (int attempt = 0; attempt < 8; ++attempt) {
        const fs::path candidate = base / ("adi-mcp-" + randomHex(8));
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

}  // namespace

HeadlessProject::HeadlessProject() = default;

std::shared_ptr<HeadlessProject> HeadlessProject::open(const fs::path& file, std::string& error) {
    std::error_code ec;
    const fs::path absolute = fs::absolute(file, ec);
    if (ec || !fs::is_regular_file(absolute, ec)) {
        error = "no project file at that path";
        return nullptr;
    }
    StoreError se = StoreError::Ok;
    auto reader = Store::open(absolute, se, /*readOnly=*/true);
    if (!reader) {
        error = std::string("cannot open the project read-only: ") + toString(se);
        return nullptr;
    }

    std::shared_ptr<HeadlessProject> p(new HeadlessProject());
    p->file_ = absolute;
    p->reader_ = std::move(reader);

    // ADR-0232 d2's explicit project: the file's project_uuid when it has one
    // (AGT-02 writes it), else a stable opaque handle. A path never leaves.
    p->handle_ = metaValue(*p->reader_, "project_uuid");
    if (p->handle_.empty()) {
        const std::string id = utf8(fs::weakly_canonical(absolute, ec));
        const auto hash = media::blake3Bytes(std::as_bytes(std::span(id.data(), id.size())));
        p->handle_ = "file-" + (hash ? hash.hex.substr(0, 16) : randomHex(8));
    }
    try {
        SQLite::Statement st(p->reader_->db(), "SELECT name FROM project WHERE id = 1");
        if (st.executeStep() && !st.getColumn(0).isNull()) p->label_ = st.getColumn(0).getString();
    } catch (const std::exception&) {
    }
    if (p->label_.empty()) p->label_ = utf8(absolute.stem());
    return p;
}

HeadlessProject::~HeadlessProject() {
    std::lock_guard lock(mutex_);
    dropSnapshotLocked();
    if (!scratch_.empty()) {
        std::error_code ec;
        fs::remove_all(scratch_, ec);
    }
    // The reader is read-only: closing it writes nothing (Store::closeFinalized).
    if (reader_) reader_->close();
}

std::string HeadlessProject::label() const { return label_; }

rows::Model HeadlessProject::readModel(std::string& error) {
    std::lock_guard lock(mutex_);
    rows::Model m = rows::readModel(*reader_);
    if (!m.problems.empty() && m.tracks.empty() && m.clips.empty()) error = m.problems.front();
    return m;
}

std::optional<std::int64_t> HeadlessProject::head(std::string& error) {
    std::lock_guard lock(mutex_);
    try {
        return OpJournal(*reader_).headSeq();
    } catch (const std::exception& e) {
        error = e.what();
        return std::nullopt;
    }
}

std::string HeadlessProject::fileSchema() {
    std::lock_guard lock(mutex_);
    try {
        const auto v = reader_->db().execAndGet("PRAGMA user_version").getInt64();
        return std::to_string(v / 1000) + "." + std::to_string(v % 1000);
    } catch (const std::exception&) {
        return {};
    }
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
    std::optional<std::int64_t> current;
    try {
        current = OpJournal(*reader_).headSeq();
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    if (snapshot_ && snapshotHead_ == current) return true;

    dropSnapshotLocked();
    if (scratch_.empty() && !makeScratch(scratch_, error)) return false;
    const fs::path copy = scratch_ / "snapshot.adi";
    try {
        // The backup API copies the committed database page by page under one
        // read transaction: a consistent snapshot, whoever else has the file
        // open, taken through the READ-ONLY connection.
        SQLite::Database destination(utf8(copy), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        SQLite::Backup backup(destination, reader_->db());
        backup.executeStep();
    } catch (const std::exception& e) {
        error = std::string("cannot snapshot the project: ") + e.what();
        return false;
    }
    StoreError se = StoreError::Ok;
    snapshot_ = Store::open(copy, se, /*readOnly=*/false);
    if (!snapshot_) {
        error = std::string("cannot open the snapshot: ") + toString(se);
        return false;
    }
    try {
        snapshotHead_ = OpJournal(*snapshot_).headSeq();
    } catch (const std::exception& e) {
        error = e.what();
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
    try {
        if (reader_->db().execAndGet("SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND "
                                     "name = 'agent_requests'").getInt() == 0)
            return false;
        SQLite::Statement st(reader_->db(), "SELECT COUNT(*) FROM agent_requests WHERE actor_detail = ?");
        st.bind(1, actorDetail);
        return st.executeStep() && st.getColumn(0).getInt64() > 0;
    } catch (const std::exception&) {
        return false;
    }
}

std::string HeadlessProject::applyHint() const {
    return "Nothing is applied until you apply it. Save `changeset` as a file and run: adi_tool "
           "propose \"" + utf8(file_.filename()) + "\" <saved changeset.json> --apply -- or open "
           "the project in ADI and apply it there.";
}

}  // namespace adi::control
