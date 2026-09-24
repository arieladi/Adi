// SPDX-License-Identifier: GPL-3.0-or-later
#include "media_ops.hpp"
#include "blake3.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <array>
#include <algorithm>
#include <limits>
#include <stdexcept>
namespace adi::media {
namespace fs = std::filesystem;
namespace {
constexpr std::array<const char*, 14> columns{
    "hash_blake3", "orig_name", "rel_path", "abs_path_hint", "sample_rate",
    "channels", "frames", "duration_ns", "format", "bit_depth", "size_bytes",
    "missing", "imported_utc", "peaks"};
bool textColumn(std::size_t i) { return i < 4 || i == 8; }
bool validHash(const std::string& h) {
    return h.size() == 64 && std::all_of(h.begin(), h.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
std::string columnList() {
    std::string result;
    for (const auto* name : columns) { if (!result.empty()) result += ','; result += name; }
    return result;
}
void validateRow(const Payload& row) {
    if (!row.is_object() || row.size() != columns.size()) throw std::runtime_error("invalid media row shape");
    for (std::size_t i = 0; i < columns.size(); ++i) {
        const auto& v = row.at(columns[i]);
        if (v.is_null() && i != 0 && i != 1 && i != 8 && i != 11) continue;
        if (i == 13) {
            if (!v.is_array()) throw std::runtime_error("peaks must be a byte array or null");
            for (const auto& byte : v)
                if (!byte.is_number_integer() || byte.get<std::int64_t>() < 0 || byte.get<std::int64_t>() > 255)
                    throw std::runtime_error("invalid peaks byte");
        } else if (textColumn(i) ? !v.is_string() : !v.is_number_integer()) {
            throw std::runtime_error(std::string("invalid media column: ") + columns[i]);
        }
    }
    if (!validHash(row.at("hash_blake3").get<std::string>())) throw std::runtime_error("invalid BLAKE3 hash");
    const auto missing = row.at("missing").get<std::int64_t>();
    if (missing != 0 && missing != 1) throw std::runtime_error("invalid missing flag");
}
Payload capturedPaths(const Store& store, const fs::path& input) {
    const auto folder = projectFolder(store);
    const auto source = fs::weakly_canonical(input.is_absolute() ? input : folder / input);
    std::error_code ec;
    const auto relative = fs::relative(source, folder, ec);
    return {{"rel_path", !ec && !relative.empty() ? Payload(pathUtf8(relative)) : Payload(nullptr)},
            {"abs_path_hint", ec || relative.empty() ? Payload(pathUtf8(source)) : Payload(nullptr)},
            {"missing", 0}};
}
bool noEmbedded(SQLite::Database& db, std::int64_t id, std::string& error) {
    SQLite::Statement q(db, "SELECT embedded OR EXISTS(SELECT 1 FROM media_blobs WHERE media_id=m.id) FROM media_files m WHERE id=?");
    q.bind(1, id);
    if (!q.executeStep()) { error = "no such media"; return false; }
    if (q.getColumn(0).getInt() != 0) { error = "extract embedded media first"; return false; }
    return true;
}
}
std::string pathUtf8(const fs::path& path) {
    const auto u = path.generic_u8string();
    return {reinterpret_cast<const char*>(u.data()), u.size()};
}
fs::path pathFromUtf8(const std::string& text) { return fs::path(std::u8string(text.begin(), text.end())); }
fs::path projectFolder(const Store& store) {
    const auto* filename = sqlite3_db_filename(store.db().getHandle(), "main");
    if (!filename || !*filename) throw std::runtime_error("media needs a disk-backed project");
    return pathFromUtf8(filename).parent_path();
}
Payload mediaRow(SQLite::Database& db, std::int64_t id) {
    SQLite::Statement q(db, "SELECT " + columnList() + " FROM media_files WHERE id=?");
    q.bind(1, id);
    if (!q.executeStep()) throw std::runtime_error("no such media");
    Payload row = Payload::object();
    for (std::size_t i = 0; i < columns.size(); ++i) {
        const auto c = q.getColumn(static_cast<int>(i));
        if (c.isNull()) row[columns[i]] = nullptr;
        else if (i == 13) {
            const auto* bytes = static_cast<const unsigned char*>(c.getBlob());
            row[columns[i]] = Payload::array();
            for (int n = 0; n < c.getBytes(); ++n) row[columns[i]].push_back(bytes[n]);
        } else if (textColumn(i)) row[columns[i]] = c.getString();
        else row[columns[i]] = c.getInt64();
    }
    return row;
}
fs::path resolveMedia(const fs::path& folder, const Payload& row, std::string& error) {
    std::vector<fs::path> candidates;
    if (!row.at("rel_path").is_null()) {
        const auto relative = pathFromUtf8(row.at("rel_path").get<std::string>());
        if (relative.is_absolute()) { error = "absolute rel_path is invalid"; return {}; }
        candidates.push_back(folder / relative);
    }
    const auto basename = pathFromUtf8(row.at("orig_name").get<std::string>()).filename();
    if (!basename.empty()) candidates.push_back(folder / "audio" / basename);
    if (!row.at("abs_path_hint").is_null()) {
        const auto hint = pathFromUtf8(row.at("abs_path_hint").get<std::string>());
        candidates.push_back(hint.is_absolute() ? hint : fs::path{});
    }
    for (const auto& path : candidates) {
        if (path.empty()) { error = "abs_path_hint is not absolute"; return {}; }
        std::error_code ec;
        if (!fs::exists(path, ec)) { if (ec) { error = "cannot inspect " + pathUtf8(path); return {}; } continue; }
        const auto hash = blake3File(path);
        if (!hash) { error = "cannot hash " + pathUtf8(path); return {}; }
        if (hash.hex != row.at("hash_blake3").get<std::string>()) { error = "hash mismatch: " + pathUtf8(path); return {}; }
        return path;
    }
    error = "missing media: " + row.at("orig_name").get<std::string>();
    return {};
}
bool importApply(OpContext& c, const Payload& p, std::string& error) {
    try {
        const auto& row = p.at("row"); validateRow(row);
        SQLite::Statement q(c.db, "INSERT INTO media_files(id," + columnList() + ",embedded) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,0)");
        q.bind(1, p.at("id").get<std::int64_t>());
        for (std::size_t i = 0; i < columns.size(); ++i) {
            const auto& v = row.at(columns[i]); const auto index = static_cast<int>(i) + 2;
            if (v.is_null()) q.bind(index);
            else if (i == 13) {
                const auto bytes = v.get<std::vector<unsigned char>>();
                if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) throw std::runtime_error("peaks too large");
                const unsigned char empty = 0;
                q.bind(index, bytes.empty() ? &empty : bytes.data(), static_cast<int>(bytes.size()));
            } else if (textColumn(i)) q.bind(index, v.get<std::string>());
            else q.bind(index, v.get<std::int64_t>());
        }
        q.exec(); return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
bool importInverse(OpContext&, const Payload& p, Payload& inverse, std::string&) {
    inverse = {{"id", p.at("id")}}; return true;
}
bool unlinkApply(OpContext& c, const Payload& p, std::string& error) {
    try {
        const auto id = p.at("id").get<std::int64_t>();
        if (!noEmbedded(c.db, id, error)) return false;
        SQLite::Statement refs(c.db, "SELECT (SELECT COUNT(*) FROM audio_clips WHERE media_id=?) + (SELECT COUNT(*) FROM tracks WHERE freeze_media_id=?)");
        refs.bind(1, id); refs.bind(2, id); refs.executeStep();
        if (refs.getColumn(0).getInt64() != 0) { error = "media is still referenced"; return false; }
        SQLite::Statement q(c.db, "DELETE FROM media_files WHERE id=?"); q.bind(1, id); q.exec(); return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
bool unlinkInverse(OpContext& c, const Payload& p, Payload& inverse, std::string& error) {
    try {
        const auto id = p.at("id").get<std::int64_t>();
        if (!noEmbedded(c.db, id, error)) return false;
        const auto row = mediaRow(c.db, id); validateRow(row);
        inverse = {{"id", id}, {"row", row}}; return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
bool relinkApply(OpContext& c, const Payload& p, std::string& error) {
    try {
        const auto id = p.at("id").get<std::int64_t>();
        if (!noEmbedded(c.db, id, error)) return false;
        const auto old = mediaRow(c.db, id);
        if (p.at("hash") != old.at("hash_blake3")) { error = "relink hash differs from media identity"; return false; }
        const auto& paths = p.at("paths");
        if (!paths.is_object() || paths.size() != 3 || !paths.contains("missing")) throw std::runtime_error("invalid captured paths");
        const auto missing = paths.at("missing").get<std::int64_t>();
        if (missing != 0 && missing != 1) throw std::runtime_error("invalid missing flag");
        SQLite::Statement q(c.db, "UPDATE media_files SET rel_path=?,abs_path_hint=?,missing=? WHERE id=?");
        int index = 1;
        for (const auto* key : {"rel_path", "abs_path_hint"}) {
            if (paths.at(key).is_null()) q.bind(index); else q.bind(index, paths.at(key).get<std::string>());
            ++index;
        }
        q.bind(3, missing); q.bind(4, id); q.exec(); return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
bool relinkInverse(OpContext& c, const Payload& p, Payload& inverse, std::string& error) {
    try {
        const auto row = mediaRow(c.db, p.at("id").get<std::int64_t>());
        inverse = {{"id", p.at("id")}, {"hash", row.at("hash_blake3")},
            {"paths", {{"rel_path", row.at("rel_path")}, {"abs_path_hint", row.at("abs_path_hint")}, {"missing", row.at("missing")}}}};
        return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
MediaResult importMedia(Store& store, const fs::path& path, std::int64_t id, std::int64_t time, Actor actor) {
    try {
        if (store.readOnly()) return {false, "project is read-only"};
        const auto source = path.is_absolute() ? path : projectFolder(store) / path;
        const auto hash = blake3File(source);
        if (!hash) return {false, "cannot hash " + pathUtf8(source)};
        SQLite::Statement existing(store.db(), "SELECT id FROM media_files WHERE hash_blake3=?"); existing.bind(1, hash.hex);
        if (existing.executeStep()) {
            const auto found = existing.getColumn(0).getInt64(); std::string error;
            if (!noEmbedded(store.db(), found, error)) return {false, error};
            return {true, {}, found, true};
        }
        Payload row = Payload::object(); for (const auto* key : columns) row[key] = nullptr;
        row.update(capturedPaths(store, source)); row["hash_blake3"] = hash.hex;
        row["orig_name"] = pathUtf8(source.filename()); row["format"] = "";
        const auto size = fs::file_size(source);
        if (size > static_cast<std::uintmax_t>(std::numeric_limits<std::int64_t>::max())) return {false, "media size exceeds signed 64-bit range"};
        row["size_bytes"] = static_cast<std::int64_t>(size); row["imported_utc"] = time;
        OpRequest request; request.opType = "media.import"; request.payload = {{"id", id}, {"row", row}}; request.actor = actor;
        const auto result = OpJournal(store).commit(request);
        return {result.ok, result.error, id};
    } catch (const std::exception& e) { return {false, e.what()}; }
}
MediaResult relinkMedia(Store& store, std::int64_t id, const fs::path& path, Actor actor) {
    try {
        const auto source = path.is_absolute() ? path : projectFolder(store) / path;
        const auto hash = blake3File(source); const auto row = mediaRow(store.db(), id);
        if (!hash || hash.hex != row.at("hash_blake3").get<std::string>()) return {false, "hash mismatch or unreadable file: " + pathUtf8(source)};
        OpRequest request; request.opType = "media.relink"; request.actor = actor;
        request.payload = {{"id", id}, {"hash", hash.hex}, {"paths", capturedPaths(store, source)}};
        const auto result = OpJournal(store).commit(request); return {result.ok, result.error, id};
    } catch (const std::exception& e) { return {false, e.what()}; }
}
} // namespace adi::media
