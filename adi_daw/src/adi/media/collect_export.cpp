// SPDX-License-Identifier: GPL-3.0-or-later
#include "collect_export.hpp"
#include "media_ops.hpp"
#include "blake3.hpp"
#include "zip_writer.hpp"
#include "adi/store.hpp"
#include "adi/check.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <SQLiteCpp/Backup.h>
#include <sqlite3.h>
#include <atomic>
#include <chrono>
#include <fstream>
#include <set>
#include <stdexcept>
#include <algorithm>
namespace adi::media {
namespace fs = std::filesystem;
namespace {
struct Stage {
    fs::path path;
    explicit Stage(const fs::path& parent) {
        static std::atomic<unsigned long long> serial{0};
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned i = 0; i < 128; ++i) {
            const auto next = parent / (".adi-media-" + std::to_string(tick) + "-" + std::to_string(serial.fetch_add(1)));
            std::error_code ec;
            if (fs::create_directory(next, ec)) { path = next; return; }
            if (ec && ec != std::errc::file_exists) throw fs::filesystem_error("create staging directory", next, ec);
        }
        throw std::runtime_error("cannot reserve staging directory");
    }
    ~Stage() { std::error_code ec; fs::remove_all(path, ec); }
};
struct Published {
    std::vector<fs::path> paths;
    bool committed = false;
    ~Published() { if (!committed) for (const auto& p : paths) { std::error_code ec; fs::remove(p, ec); } }
};
std::string safeName(const std::string& original, std::int64_t id) {
    std::string name = original;
    for (char& c : name) if (static_cast<unsigned char>(c) < 32 || std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos) c = '_';
    while (!name.empty() && (name.back() == '.' || name.back() == ' ')) name.pop_back();
    if (name.empty() || name == "." || name == ".." || name.size() > 180) name = "media-" + std::to_string(id) + ".bin";
    auto stem = name.substr(0, name.find('.'));
    for (char& c : stem) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" ||
        (stem.size() == 4 && (stem.starts_with("COM") || stem.starts_with("LPT")) && stem[3] >= '1' && stem[3] <= '9')) name = "_" + name;
    return name;
}
void audioDirectory(const fs::path& folder) {
    const auto audio = folder / "audio";
    std::error_code ec;
    const auto status = fs::symlink_status(audio, ec);
    if (fs::is_symlink(status)) throw std::runtime_error("audio/ must not be a symlink");
    if (ec && ec != std::errc::no_such_file_or_directory) throw fs::filesystem_error("inspect audio", audio, ec);
    fs::create_directories(audio);
}
fs::path publish(const fs::path& staged, const fs::path& folder, const std::string& original,
                 std::int64_t id, Published& owned) {
    const auto name = safeName(original, id);
    for (unsigned i = 0; i < 10000; ++i) {
        const auto target = folder / "audio" / pathFromUtf8(i == 0 ? name : std::to_string(id) + "-" + std::to_string(i) + "-" + name);
        owned.paths.push_back(target);
        std::error_code ec; fs::create_hard_link(staged, target, ec);
        if (!ec) return target;
        owned.paths.pop_back();
        if (ec != std::errc::file_exists) throw fs::filesystem_error("publish media without overwriting", target, ec);
    }
    throw std::runtime_error("too many filename collisions for " + original);
}
void verify(const fs::path& path, const std::string& expected) {
    const auto hash = blake3File(path);
    if (!hash || hash.hex != expected) throw std::runtime_error("hash mismatch or unreadable media: " + pathUtf8(path));
}
void relocate(SQLite::Database& db, std::int64_t id, const fs::path& folder, const fs::path& path) {
    SQLite::Statement q(db, "UPDATE media_files SET embedded=0,rel_path=?,abs_path_hint=NULL,missing=0 WHERE id=?");
    q.bind(1, pathUtf8(path.lexically_relative(folder))); q.bind(2, id); q.exec();
}
std::vector<std::int64_t> embeddedIds(SQLite::Database& db) {
    SQLite::Statement q(db, "SELECT id FROM media_files m WHERE embedded<>0 OR EXISTS(SELECT 1 FROM media_blobs WHERE media_id=m.id) ORDER BY id");
    std::vector<std::int64_t> ids;
    while (q.executeStep()) ids.push_back(q.getColumn(0).getInt64());
    return ids;
}
void streamChunks(SQLite::Database& db, std::int64_t id, const fs::path& target) {
    std::ofstream out(target, std::ios::binary);
    if (!out) throw std::runtime_error("cannot write " + pathUtf8(target));
    SQLite::Statement chunks(db, "SELECT chunk_index,length(data) FROM media_blobs WHERE media_id=? ORDER BY chunk_index");
    chunks.bind(1, id); std::int64_t expectedIndex = 0;
    while (chunks.executeStep()) {
        const auto index = chunks.getColumn(0).getInt64();
        if (index != expectedIndex++) throw std::runtime_error("non-contiguous media chunks: " + pathUtf8(target));
        const auto length = chunks.getColumn(1).getInt64();
        for (std::int64_t pos = 0; pos < length;) {
            const auto count = std::min<std::int64_t>(65536, length - pos);
            SQLite::Statement piece(db, "SELECT substr(data,?,?) FROM media_blobs WHERE media_id=? AND chunk_index=?");
            piece.bind(1, pos + 1); piece.bind(2, count); piece.bind(3, id); piece.bind(4, index);
            if (!piece.executeStep() || piece.getColumn(0).getBytes() != count) throw std::runtime_error("truncated media chunk");
            out.write(static_cast<const char*>(piece.getColumn(0).getBlob()), static_cast<std::streamsize>(count));
            if (!out) throw std::runtime_error("write failed: " + pathUtf8(target));
            pos += count;
        }
    }
    out.close(); if (!out) throw std::runtime_error("close failed: " + pathUtf8(target));
}
}
FileResult extractMedia(Store& store) {
    try {
        if (store.readOnly()) return {false, "project is read-only"};
        SQLite::Transaction transaction(store.db());
        const auto ids = embeddedIds(store.db());
        if (ids.empty()) { transaction.commit(); return {true, {}}; }
        const auto folder = projectFolder(store); audioDirectory(folder);
        Stage stage(folder / "audio"); Published published;
        for (const auto id : ids) {
            const auto row = mediaRow(store.db(), id);
            const auto name = row.at("orig_name").get<std::string>();
            const auto staged = stage.path / pathFromUtf8(safeName(name, id));
            streamChunks(store.db(), id, staged);
            verify(staged, row.at("hash_blake3").get<std::string>());
            const auto target = publish(staged, folder, name, id, published);
            relocate(store.db(), id, folder, target);
            SQLite::Statement erase(store.db(), "DELETE FROM media_blobs WHERE media_id=?"); erase.bind(1, id); erase.exec();
            fs::remove(staged);
        }
        transaction.commit(); published.committed = true; return {true, {}};
    } catch (const std::exception& e) { return {false, e.what()}; }
}
FileResult collectExport(const fs::path& input, const fs::path& output) {
    try {
        const auto target = fs::absolute(output);
        if (fs::exists(target) || fs::is_symlink(fs::symlink_status(target))) return {false, "output already exists: " + pathUtf8(target)};
        StoreError error;
        auto source = Store::open(fs::absolute(input), error, true);
        if (!source || error != StoreError::Ok) return {false, "cannot open source: " + pathUtf8(input) + ": " + toString(error)};
        const auto originalFolder = projectFolder(*source);
        Stage stage(target.parent_path());
        const auto project = stage.path / "project"; fs::create_directory(project);
        const auto filename = input.extension() == ".adi" ? safeName(pathUtf8(input.filename()), 0) : std::string("project.adi");
        const auto copyPath = project / pathFromUtf8(filename);
        {
            SQLite::Database copy(pathUtf8(copyPath), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
            SQLite::Backup backup(copy, source->db());
            if (backup.executeStep() != SQLITE_DONE) throw std::runtime_error("database snapshot busy or incomplete");
        }
        source.reset();
        auto copy = Store::open(copyPath, error);
        if (!copy || copy->readOnly()) throw std::runtime_error("cannot open snapshot for relocation");
        const auto legacyList = embeddedIds(copy->db());
        const std::set<std::int64_t> legacy(legacyList.begin(), legacyList.end());
        const auto extraction = extractMedia(*copy);
        if (!extraction.ok) throw std::runtime_error(extraction.error);
        audioDirectory(project);
        Published published;
        {
            SQLite::Transaction transaction(copy->db());
            std::vector<std::int64_t> ids;
            { SQLite::Statement q(copy->db(), "SELECT id FROM media_files ORDER BY id"); while (q.executeStep()) ids.push_back(q.getColumn(0).getInt64()); }
            for (const auto id : ids) {
                if (legacy.contains(id)) continue;
                const auto row = mediaRow(copy->db(), id); std::string resolveError;
                const auto resolved = resolveMedia(originalFolder, row, resolveError);
                if (resolved.empty()) throw std::runtime_error(resolveError);
                const auto staged = stage.path / "media.tmp";
                fs::copy_file(resolved, staged, fs::copy_options::overwrite_existing);
                verify(staged, row.at("hash_blake3").get<std::string>());
                const auto dest = publish(staged, project, row.at("orig_name").get<std::string>(), id, published);
                relocate(copy->db(), id, project, dest);
                fs::remove(staged); // Do not overwrite an inode linked into audio/.
            }
            transaction.commit(); published.committed = true;
        }
        const auto report = checkProject(*copy, true);
        if (!report.clean()) throw std::runtime_error("collected project failed check: " + report.findings.front().detail);
        if (!copy->close()) throw std::runtime_error("could not fold copied database WAL");
        copy.reset();
        if (fs::exists(pathFromUtf8(pathUtf8(copyPath) + "-wal"))) throw std::runtime_error("copied WAL survived close");
        const auto zipPath = stage.path / "archive.zip";
        ZipWriter zip(zipPath);
        if (!zip.addFile(copyPath, filename)) throw std::runtime_error("cannot archive copied database");
        for (const auto& entry : fs::directory_iterator(project / "audio")) {
            if (!zip.addFile(entry.path(), "audio/" + pathUtf8(entry.path().filename()))) throw std::runtime_error("cannot archive " + pathUtf8(entry.path()));
        }
        if (!zip.finish()) throw std::runtime_error("ZIP finalization failed");
        fs::create_hard_link(zipPath, target); // Atomic no-clobber publication.
        return {true, {}};
    } catch (const std::exception& e) { return {false, e.what()}; }
}
} // namespace adi::media
