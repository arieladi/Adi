// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/salvage.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/media/blake3.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3recover.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <memory>
#include <stdexcept>

namespace adi {
namespace {
namespace fs = std::filesystem;
constexpr const char* kSourceId = "2026-07-24 19:02:57 bf7c7f30031888f4e796e429ab3978879485813aaca6f641c7b33e4e09459bcc";

bool present(const fs::path& path) {
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    if (error && error != std::errc::no_such_file_or_directory)
        throw fs::filesystem_error("inspect salvage path", path, error);
    return fs::exists(status) || fs::is_symlink(status);
}
void noSidecars(const fs::path& path, const char* role) {
    for (const char* suffix : {"-wal", "-shm", "-journal"}) {
        auto sidecar = path; sidecar += suffix;
        if (present(sidecar)) throw std::runtime_error(std::string(role) + " must have no SQLite sidecars; found " + media::pathUtf8(sidecar));
    }
}
struct Header { std::int32_t applicationId = 0, userVersion = 0; };
Header header(const fs::path& path) {
    std::array<unsigned char, 100> bytes{};
    std::ifstream stream(path, std::ios::binary);
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!stream || std::memcmp(bytes.data(), "SQLite format 3\0", 16) != 0)
        throw std::runtime_error("not a complete SQLite database header");
    // A closed WAL-mode database can have no current sidecars. Opening it
    // read-only may still create WAL/SHM files; do not mutate that directory.
    if (bytes[18] != 1 || bytes[19] != 1)
        throw std::runtime_error("salvage requires rollback-format headers; WAL-mode input is unsupported");
    const auto integer = [&bytes](std::size_t offset) {
        const auto value = (std::uint32_t{bytes[offset]} << 24) |
            (std::uint32_t{bytes[offset + 1]} << 16) |
            (std::uint32_t{bytes[offset + 2]} << 8) | bytes[offset + 3];
        if (value > 0x7fffffffU) throw std::runtime_error("unsupported header identity/version");
        return static_cast<std::int32_t>(value);
    };
    return {integer(68), integer(60)};
}
class Stage {
public:
    explicit Stage(const fs::path& parent) {
        static std::atomic<unsigned long long> sequence{0};
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned attempt = 0; attempt < 128; ++attempt) {
            auto candidate = parent / (".adi-salvage-" + std::to_string(tick) + "-" +
                std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
            std::error_code error;
            if (fs::create_directory(candidate, error)) { path = std::move(candidate); return; }
            if (error && error != std::errc::file_exists)
                throw fs::filesystem_error("reserve salvage stage", candidate, error);
        }
        throw std::runtime_error("cannot reserve salvage stage");
    }
    ~Stage() { std::error_code ignored; fs::remove_all(path, ignored); }
    Stage(const Stage&) = delete;
    Stage& operator=(const Stage&) = delete;
    fs::path path;
};
struct RecoverDelete {
    void operator()(sqlite3_recover* handle) const noexcept { (void)sqlite3_recover_finish(handle); }
};
std::string lostTable(SQLite::Database& input) {
    // Require the input's schema catalogue to remain readable in this slice.
    // Choose an unused name rather than guessing which suffixed table upstream
    // created when an unknown extension already owns "lost_and_found".
    SQLite::Statement exists(input, "SELECT 1 FROM sqlite_schema WHERE name=? COLLATE NOCASE");
    for (int attempt = 0; attempt < 1000; ++attempt) {
        const auto name = attempt == 0 ? std::string("lost_and_found") : "lost_and_found_" + std::to_string(attempt);
        exists.bind(1, name);
        const bool used = exists.executeStep();
        exists.reset(); exists.clearBindings();
        if (!used) return name;
    }
    throw std::runtime_error("cannot reserve a distinct lost-and-found table name");
}
bool unreadable(const CheckReport& report) {
    for (const auto& finding : report.findings)
        if (finding.code == "sqlite.integrity" || finding.code.ends_with("checkFailed")) return true;
    return false;
}
} // namespace

SalvageResult salvageProject(const fs::path& input, const fs::path& output, media::PublishHooks hooks) {
    SalvageResult result;
    if (audio::onAudioThread) { result.error = "salvage is unavailable on the audio thread"; return result; }
    try {
        if (std::strcmp(sqlite3_sourceid(), kSourceId) != 0 || sqlite3_libversion_number() != 3053004)
            throw std::runtime_error("SQLite runtime does not match the pinned recovery sources");
        if (input.empty() || output.empty()) throw std::runtime_error("input and output paths are required");
        if (input.native().find(fs::path::value_type{}) != fs::path::string_type::npos ||
            output.native().find(fs::path::value_type{}) != fs::path::string_type::npos)
            throw std::runtime_error("paths cannot contain embedded NUL");
        const auto source = fs::canonical(input);
        const auto target = fs::absolute(output).lexically_normal();
        if (!fs::is_regular_file(source)) throw std::runtime_error("input must be a regular file");
        if (present(target)) throw std::runtime_error("output already exists");
        noSidecars(target, "output");
        noSidecars(source, "input");
        std::error_code leaseError;
        const auto lease = media::ProjectFileLease::openExclusive(source, leaseError);
        if (!lease) throw std::runtime_error("input is open or inaccessible: " + leaseError.message());
        noSidecars(source, "input");
        const auto original = header(source);
        if (original.applicationId != kApplicationId || original.userVersion / 1000 != kSchemaMajor)
            throw std::runtime_error("input is not an ADI project of the supported schema major");
        result.applicationId = original.applicationId; result.userVersion = original.userVersion;
        const auto before = media::blake3File(source);
        if (!before) throw std::runtime_error("cannot fingerprint input");
        Stage stage(target.parent_path());
        const auto recovered = stage.path / "recovered.adi";
        {
            SQLite::Database database(media::pathUtf8(source), SQLite::OPEN_READONLY);
            if (sqlite3_db_readonly(database.getHandle(), "main") != 1 ||
                sqlite3_db_config(database.getHandle(), SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, nullptr) != SQLITE_OK)
                throw std::runtime_error("cannot configure read-only recovery input");
            result.lostAndFoundTable = lostTable(database);
            result.failure = SalvageFailure::Recovery;
            const auto destination = media::pathUtf8(recovered);
            std::unique_ptr<sqlite3_recover, RecoverDelete> recovery(
                sqlite3_recover_init(database.getHandle(), "main", destination.c_str()));
            if (!recovery) throw std::runtime_error("cannot allocate SQLite recovery handle");
            int ignoreFreelist = 0, preserveRowids = 1;
            if (sqlite3_recover_config(recovery.get(), SQLITE_RECOVER_LOST_AND_FOUND, result.lostAndFoundTable.data()) != SQLITE_OK ||
                sqlite3_recover_config(recovery.get(), SQLITE_RECOVER_FREELIST_CORRUPT, &ignoreFreelist) != SQLITE_OK ||
                sqlite3_recover_config(recovery.get(), SQLITE_RECOVER_ROWIDS, &preserveRowids) != SQLITE_OK)
                throw std::runtime_error("cannot configure SQLite recovery");
            if (sqlite3_recover_run(recovery.get()) != SQLITE_OK) {
                const char* message = sqlite3_recover_errmsg(recovery.get());
                throw std::runtime_error(message ? message : "SQLite recovery failed");
            }
            if (sqlite3_recover_finish(recovery.release()) != SQLITE_OK)
                throw std::runtime_error("SQLite recovery finalization failed");
        }
        result.failure = SalvageFailure::Validation;
        const auto restored = header(recovered);
        if (restored.applicationId != original.applicationId || restored.userVersion != original.userVersion)
            throw std::runtime_error("recovery did not preserve the project header identity/version");
        StoreError error = StoreError::Ok;
        auto store = Store::open(recovered, error, true);
        if (!store || error != StoreError::Ok) throw std::runtime_error("recovered project cannot be checked: " + std::string(toString(error)));
        result.report = checkProject(*store, false);
        if (unreadable(result.report)) throw std::runtime_error("recovered project has an unreadable schema or failed SQLite integrity check");
        {
            SQLite::Statement found(store->db(), "SELECT 1 FROM sqlite_schema WHERE type='table' AND name=?");
            found.bind(1, result.lostAndFoundTable);
            if (found.executeStep()) {
                // Generated identifier consists only of fixed ASCII letters,
                // underscores and decimal digits; no user text enters this SQL.
                result.lostAndFoundRows = store->db().execAndGet("SELECT count(*) FROM " + result.lostAndFoundTable).getInt64();
            }
            // Finalize every statement before closing the validation connection.
        }
        if (!store->close()) throw std::runtime_error("cannot close recovered project validation");
        store.reset();
        noSidecars(source, "input"); noSidecars(recovered, "recovered stage");
        const auto after = media::blake3File(source);
        if (!after || after.hex != before.hex) throw std::runtime_error("input changed during recovery; nothing published");
        result.failure = SalvageFailure::Publication;
        noSidecars(target, "output");
        auto publication = media::publishFile(recovered, target, hooks, media::PublishPolicy::AtomicOnly);
        if (publication.status != media::PublishStatus::published) {
            result.error = std::move(publication.error);
            return result;
        }
        result.published = true;
        result.failure = SalvageFailure::None;
        result.warning = std::move(publication.warning);
        result.durabilityConfirmed = publication.durabilityConfirmed;
        return result;
    } catch (const std::exception& error) { result.error = error.what(); return result; }
}
} // namespace adi
