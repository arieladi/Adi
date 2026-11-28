// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/salvage.hpp"
#include "adi/store.hpp"
#include "adi/ops.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/audio/io_audit.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>
#if !defined(_WIN32)
#include <sys/wait.h>
#endif

namespace {
using namespace adi;
namespace fs = std::filesystem;
int checks = 0, failures = 0;
void check(bool value, const char* name) { ++checks; if (!value) { ++failures; std::printf("FAIL %s\n", name); } }
void require(bool value, const char* name) { if (!value) throw std::runtime_error(name); }
std::string read(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    require(bool(stream), "read fixture");
    return {std::istreambuf_iterator<char>(stream), {}};
}
void write(const fs::path& path, const std::string& bytes) {
    std::ofstream stream(path, std::ios::binary); stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    stream.close(); require(bool(stream), "write fixture");
}
std::string quote(const fs::path& path) {
#if defined(_WIN32)
    return "\"" + path.string() + "\"";
#else
    std::string value = "'";
    for (char c : path.string()) value += c == '\'' ? "'\\''" : std::string(1, c);
    return value + "'";
#endif
}
int cli(const fs::path& input, const fs::path& output, const fs::path& log) {
    auto command = quote(fs::path(ADI_TOOL_PATH)) + " salvage " + quote(input) + " " + quote(output) + " > " + quote(log) + " 2>&1";
#if defined(_WIN32)
    command = "\"" + command + "\"";
#endif
    const int result = std::system(command.c_str());
#if defined(_WIN32)
    return result;
#else
    return result != -1 && WIFEXITED(result) ? WEXITSTATUS(result) : -1;
#endif
}
std::unique_ptr<Store> create(const fs::path& path) {
    StoreError error = StoreError::Ok;
    auto store = Store::createNewProject(path, error);
    require(bool(store), "create actual project");
    return store;
}
void close(std::unique_ptr<Store>& store) { require(store->close(), "close fixture"); store.reset(); }
std::unique_ptr<Store> open(const fs::path& path) {
    StoreError error = StoreError::Ok; auto store = Store::open(path, error, true);
    require(store && error == StoreError::Ok, "open recovered fixture"); return store;
}
bool noStages(const fs::path& directory) {
    for (const auto& entry : fs::directory_iterator(directory))
        if (entry.path().filename().string().starts_with(".adi-salvage-")) return false;
    return true;
}
using Rows = std::map<std::int64_t, std::string>;
Rows ops(SQLite::Database& database) {
    Rows rows;
    SQLite::Statement query(database, "SELECT * FROM ops ORDER BY seq");
    while (query.executeStep()) {
        std::string value;
        for (int column = 0; column < query.getColumnCount(); ++column) {
            const auto cell = query.getColumn(column);
            const int type = cell.getType();
            std::string field;
            if (type == SQLite::INTEGER) field = std::to_string(cell.getInt64());
            else if (type == SQLite::FLOAT) field = std::to_string(std::bit_cast<std::uint64_t>(cell.getDouble()));
            else if (type == SQLite::TEXT) field = cell.getString();
            else if (type == SQLite::BLOB && cell.getBytes() > 0)
                field.assign(static_cast<const char*>(cell.getBlob()), static_cast<std::size_t>(cell.getBytes()));
            value += std::to_string(type) + ":" + std::to_string(field.size()) + ":" + field;
        }
        rows.emplace(query.getColumn(0).getInt64(), std::move(value));
    }
    return rows;
}
std::uint32_t be(const std::string& bytes, std::size_t at, std::size_t count) {
    require(at <= bytes.size() && count <= bytes.size() - at, "fixture page bounds");
    std::uint32_t result = 0;
    for (std::size_t i = 0; i < count; ++i) result = (result << 8) | static_cast<unsigned char>(bytes[at + i]);
    return result;
}
std::uint64_t varint(const std::string& bytes, std::size_t& at) {
    std::uint64_t result = 0;
    for (unsigned i = 0; i < 9; ++i) {
        require(at < bytes.size(), "fixture varint bounds");
        const auto byte = static_cast<unsigned char>(bytes[at++]);
        if (i == 8) return (result << 8) | byte;
        result = (result << 7) | (byte & 127U);
        if ((byte & 128U) == 0) return result;
    }
    throw std::runtime_error("fixture varint");
}
struct Leaf { std::uint32_t page = 0; std::set<std::int64_t> ids; };
void leaves(const std::string& bytes, std::uint32_t page, std::size_t pageSize,
    std::vector<Leaf>& result, std::set<std::uint32_t>& visited) {
    require(page > 0 && visited.insert(page).second, "acyclic fixture btree");
    const auto base = (static_cast<std::size_t>(page) - 1) * pageSize;
    const auto head = base + (page == 1 ? 100U : 0U);
    const auto type = be(bytes, head, 1), count = be(bytes, head + 3, 2);
    require(type == 5 || type == 13, "table btree fixture");
    Leaf leaf; leaf.page = page;
    for (std::uint32_t i = 0; i < count; ++i) {
        auto cell = base + be(bytes, head + (type == 5 ? 12U : 8U) + std::size_t{i} * 2, 2);
        if (type == 5) leaves(bytes, be(bytes, cell, 4), pageSize, result, visited);
        else {
            (void)varint(bytes, cell); // payload length, independent from rowid
            leaf.ids.insert(static_cast<std::int64_t>(varint(bytes, cell)));
        }
    }
    if (type == 5) leaves(bytes, be(bytes, head + 8, 4), pageSize, result, visited);
    else result.push_back(std::move(leaf));
}
void healthy() {
    test::TempDirectory temp("salvage", "healthy");
    const auto input = temp.path() / "song.adi", output = temp.path() / "recovered.adi";
    auto store = create(input);
    OpRequest edit; edit.opType = "project.setName"; edit.payload = {{"name", "Preserved"}};
    require(OpJournal(*store).commit(edit).ok, "actual journal edit");
    store->db().exec("CREATE TABLE future_extension(id INTEGER PRIMARY KEY, value BLOB); INSERT INTO future_extension VALUES(9,x'000102ff');");
    store->db().exec("INSERT INTO media_files(id,hash_blake3,rel_path) VALUES(90,'abcdef','audio/original.wav')");
    const auto expected = ops(store->db()); const auto version = store->db().execAndGet("PRAGMA user_version").getInt();
    close(store); const auto original = read(input);
    const auto result = salvageProject(input, output);
    if (result.exitCode() != 0) {
        for (const auto& finding : result.report.findings)
            std::printf("healthy finding: %s %s %s\n", finding.code.c_str(), finding.where.c_str(), finding.detail.c_str());
    }
    check(result.published && result.exitCode() == 0, "healthy copy published clean");
    if (!result.published) std::printf("healthy error: %s\n", result.error.c_str());
    check(result.report.checksRun >= 15 && result.report.clean(), "actual ADI checker ran");
    check(result.applicationId == kApplicationId && result.userVersion == version, "header versions retained");
    check(result.lostAndFoundRows == 0, "healthy copy needs no lost-and-found rows");
    if (result.published) {
        auto recovered = open(output);
        if (ops(recovered->db()) != expected) std::printf("healthy rows: expected %zu actual %zu\n", expected.size(), ops(recovered->db()).size());
        check(ops(recovered->db()) == expected, "actual journal bytes and identities survive");
        check(recovered->db().execAndGet("SELECT hex(value) FROM future_extension WHERE id=9").getString() == "000102FF", "unknown schema/data survive");
        check(recovered->db().execAndGet("PRAGMA user_version").getInt() == version, "output header never migrated");
        check(recovered->db().execAndGet("SELECT rel_path FROM media_files WHERE id=90").getString() == "audio/original.wav", "external media path retained verbatim");
    }
    check(read(input) == original && noStages(temp.path()), "healthy input intact and stage cleaned");
    const auto cliOut = temp.path() / "cli.adi", log = temp.path() / "cli.log";
    check(cli(input, cliOut, log) == 0 && fs::exists(cliOut), "real CLI publishes healthy file");
    check(read(log).find("External media paths are unchanged") != std::string::npos, "CLI states media/completeness boundary");
}
void versions() {
    test::TempDirectory temp("salvage", "versions");
    const auto old = temp.path() / "old.adi";
    {
        SQLite::Database database(media::pathUtf8(old), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        database.exec(read(fs::path(__FILE__).parent_path().parent_path() / "docs/format/history/schema-1.0.sql"));
        database.exec("INSERT INTO project(id,name) VALUES(1,'Old Song')");
    }
    const auto original = read(old);
    const auto result = salvageProject(old, temp.path() / "old-copy.adi");
    if (!result.published) std::printf("old error: %s\n", result.error.c_str());
    check(result.published && result.userVersion == 1000, "actual frozen old schema can be salvaged without migration");
    if (result.published) {
        SQLite::Database database(media::pathUtf8(temp.path() / "old-copy.adi"), SQLite::OPEN_READONLY);
        check(database.execAndGet("PRAGMA user_version").getInt() == 1000, "old header retained on disk");
        check(database.execAndGet("SELECT count(*) FROM sqlite_schema WHERE name='history_roots'").getInt() == 0, "read-only compatibility tables never persisted");
    }
    check(read(old) == original, "old input never upgraded");
    const auto future = temp.path() / "future.adi"; auto store = create(future);
    store->db().exec("PRAGMA user_version=1999"); close(store);
    const auto newer = salvageProject(future, temp.path() / "future-copy.adi");
    check(newer.published && newer.userVersion == 1999, "unknown minor retained without downgrade");
}
void damagedLeaf() {
    test::TempDirectory temp("salvage", "ops_leaf");
    const auto input = temp.path() / "song.adi", output = temp.path() / "recovered.adi";
    auto store = create(input);
    std::vector<OpRequest> edits;
    for (int i = 0; i < 128; ++i) {
        OpRequest request; request.opType = "project.setName"; request.payload = {{"name", "Take " + std::to_string(i)}};
        request.label = std::string(180, 'h') + std::to_string(i); edits.push_back(std::move(request));
    }
    require(OpJournal(*store).commit(edits).ok, "actual multi-operation history fixture");
    const auto expected = ops(store->db());
    const auto root = static_cast<std::uint32_t>(store->db().execAndGet("SELECT rootpage FROM sqlite_schema WHERE name='ops'").getInt());
    close(store); auto bytes = read(input);
    auto pageSize = static_cast<std::size_t>(be(bytes, 16, 2)); if (pageSize == 1) pageSize = 65536;
    std::vector<Leaf> pages; std::set<std::uint32_t> visited; leaves(bytes, root, pageSize, pages, visited);
    require(pages.size() >= 3, "real history spans several leaves");
    const auto& damaged = pages[pages.size() / 2];
    require(damaged.page != root && !damaged.ids.empty(), "damage a non-root nonempty ops leaf");
    bytes[(static_cast<std::size_t>(damaged.page) - 1) * pageSize] = '\0'; write(input, bytes);
    const auto result = salvageProject(input, output);
    if (!result.published) std::printf("leaf error: %s\n", result.error.c_str());
    check(result.published && result.exitCode() == 1, "broken history publishes a diagnosed salvage copy");
    check(result.report.errors > 0, "missing history references reported rather than repaired");
    if (result.published) {
        auto recovered = open(output); const auto actual = ops(recovered->db());
        std::printf("ops leaf: page=%u original=%zu damaged=%zu recovered=%zu\n",
            damaged.page, expected.size(), damaged.ids.size(), actual.size());
        check(actual.size() + damaged.ids.size() == expected.size(), "loss bounded by independently identified leaf rows");
        for (const auto& [id, value] : expected)
            check(damaged.ids.contains(id) ? !actual.contains(id) : actual.contains(id) && actual.at(id) == value,
                "every intact op retains every column's storage type and exact value");
        check(recovered->db().execAndGet("SELECT count(*) FROM tracks").getInt() == 7, "rest of actual project schema/data recovered");
    }
    check(read(input) == bytes && noStages(temp.path()), "damaged source unchanged and private stage removed");
    check(cli(input, temp.path() / "cli-dirty.adi", temp.path() / "cli.log") == 1, "actual CLI distinguishes published findings");
}
std::error_code denyRename(const fs::path&, const fs::path&) { return std::make_error_code(std::errc::permission_denied); }
std::error_code lateTarget(const fs::path&, const fs::path& target) { write(target, "late owner"); return std::make_error_code(std::errc::file_exists); }
void refusals() {
    test::TempDirectory temp("salvage", "refuse");
    const auto input = temp.path() / "song.adi", target = temp.path() / "output.adi";
    auto store = create(input);
    check(!salvageProject(input, target).published && !fs::exists(target), "live Store cannot be salvaged");
    close(store); const auto original = read(input);
    write(target, "another owner's file");
    check(!salvageProject(input, target).published && read(target) == "another owner's file", "existing target never replaced");
    check(!salvageProject(input, input).published && read(input) == original, "same source/output refused");
    const auto alias = temp.path() / "alias.adi"; fs::create_hard_link(input, alias);
    check(!salvageProject(input, alias).published && read(alias) == original, "hard-link output alias refused");
    for (const char* suffix : {"-wal", "-shm", "-journal"}) {
        auto sidecar = input; sidecar += suffix; write(sidecar, "closed-source test sidecar");
        const auto r = salvageProject(input, temp.path() / "sidecar-output.adi");
        check(!r.published && r.failure == SalvageFailure::Refused && read(sidecar) == "closed-source test sidecar", "each sidecar explicitly refused unchanged");
        fs::remove(sidecar);
    }
    const auto staleTarget = temp.path() / "stale-target.adi";
    for (const char* suffix : {"-wal", "-shm", "-journal"}) {
        auto sidecar = staleTarget; sidecar += suffix; write(sidecar, "another output owner's journal");
        const auto stale = salvageProject(input, staleTarget);
        check(!stale.published && !fs::exists(staleTarget) && read(sidecar) == "another output owner's journal",
            "absent output with a stale sidecar is refused without touching either");
        check(stale.error.find("output must have no SQLite sidecars") != std::string::npos, "output sidecar diagnostic identifies the output role");
        fs::remove(sidecar);
    }
    const auto wal = temp.path() / "closed-wal.adi";
    fs::copy_file(input, wal);
    { SQLite::Database database(media::pathUtf8(wal), SQLite::OPEN_READWRITE); database.exec("PRAGMA journal_mode=WAL"); }
    const auto walBytes = read(wal);
    require(be(walBytes, 18, 1) == 2 && !fs::exists(fs::path(wal.string() + "-wal")), "closed WAL-mode fixture has no sidecar");
    const auto walResult = salvageProject(wal, temp.path() / "wal-out.adi");
    check(!walResult.published && walResult.failure == SalvageFailure::Refused && read(wal) == walBytes,
        "WAL-format header refused before SQLite can create sidecars");
    check(!fs::exists(fs::path(wal.string() + "-wal")) && !fs::exists(fs::path(wal.string() + "-shm")), "read-only recovery created no WAL/SHM input files");
    const auto denied = temp.path() / "denied.adi";
    const auto r = salvageProject(input, denied, {denyRename});
    check(!r.published && r.failure == SalvageFailure::Publication && !fs::exists(denied), "atomic publication failure leaves no partial target");
    const auto late = temp.path() / "late.adi";
    check(!salvageProject(input, late, {lateTarget}).published && read(late) == "late owner", "late destination collision preserved");
    auto corrupt = original; corrupt[68] = '\0'; write(temp.path() / "foreign.adi", corrupt);
    check(!salvageProject(temp.path() / "foreign.adi", temp.path() / "foreign-out.adi").published, "foreign header refused");
    write(temp.path() / "tiny.adi", "SQLite");
    check(!salvageProject(temp.path() / "tiny.adi", temp.path() / "tiny-out.adi").published, "truncated header refused");
    check(cli(input, target, temp.path() / "cli.log") == 2, "actual CLI failure exit differs from published findings");
    auto nul = input.native(); nul.push_back(fs::path::value_type{}); nul += input.native();
    check(!salvageProject(fs::path(nul), temp.path() / "nul.adi").published, "embedded NUL cannot select a truncated input path");
    const bool prior = audio::onAudioThread; audio::onAudioThread = true;
    const auto callback = salvageProject(input, temp.path() / "callback.adi"); audio::onAudioThread = prior;
    check(!callback.published && !fs::exists(temp.path() / "callback.adi"), "callback entry refuses without I/O");
    check(read(input) == original && noStages(temp.path()), "every refusal preserves input and cleans owned staging");
}
void lostAndUnreadable() {
    test::TempDirectory temp("salvage", "lost");
    const auto input = temp.path() / "song.adi";
    auto store = create(input);
    store->db().exec("CREATE TABLE lost_and_found(note TEXT); INSERT INTO lost_and_found VALUES('original extension');"
        "CREATE TABLE orphan_probe(id INTEGER PRIMARY KEY, value TEXT); INSERT INTO orphan_probe VALUES(10,'recover me');");
    close(store);
    {
        SQLite::Database raw(media::pathUtf8(input), SQLite::OPEN_READWRITE);
        int defensive = 0;
        check(sqlite3_db_config(raw.getHandle(), SQLITE_DBCONFIG_DEFENSIVE, -1, &defensive) == SQLITE_OK && defensive == 1, "ordinary connection defensive by default");
        check(sqlite3_exec(raw.getHandle(), "UPDATE sqlite_dbpage SET data=data WHERE pgno=1", nullptr, nullptr, nullptr) != SQLITE_OK, "ordinary connection refuses raw-page writes");
        // Owned corruption fixture only: detach a table's page without freeing
        // it so the official recovery code must place its row in lost-and-found.
        require(sqlite3_db_config(raw.getHandle(), SQLITE_DBCONFIG_DEFENSIVE, 0, nullptr) == SQLITE_OK, "fixture disable defensive");
        raw.exec("PRAGMA writable_schema=ON; DELETE FROM sqlite_schema WHERE name='orphan_probe'; PRAGMA writable_schema=OFF;");
    }
    const auto original = read(input); const auto out = temp.path() / "recovered.adi";
    const auto result = salvageProject(input, out);
    check(result.published && result.exitCode() == 1 && result.lostAndFoundRows == 1, "orphan page recovered and accurately counted");
    check(result.lostAndFoundTable == "lost_and_found_1", "existing lost-and-found name not misattributed");
    if (result.published) {
        auto recovered = open(out);
        check(recovered->db().execAndGet("SELECT note FROM lost_and_found").getString() == "original extension", "original colliding extension preserved");
        check(recovered->db().execAndGet("SELECT c1 FROM lost_and_found_1").getString() == "recover me", "actual orphan data survives");
    }
    check(read(input) == original && noStages(temp.path()), "orphan input unchanged");
    const auto missing = temp.path() / "missing-table.adi"; auto broken = create(missing);
    broken->db().exec("DROP TABLE controller_maps"); close(broken);
    const auto refused = salvageProject(missing, temp.path() / "bad-out.adi");
    check(!refused.published && refused.failure == SalvageFailure::Validation && refused.report.errors > 0,
        "missing required schema is distinguished from semantic dirty copy");
    check(!fs::exists(temp.path() / "bad-out.adi") && noStages(temp.path()), "unreadable recovered schema never exposed");
}
} // namespace
int main() {
    try { healthy(); versions(); damagedLeaf(); refusals(); lostAndUnreadable(); }
    catch (const std::exception& error) { ++failures; std::printf("EXCEPTION %s\n", error.what()); }
    std::printf("salvage: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
