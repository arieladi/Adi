// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/media/media_ops.hpp"
#include "adi/media/blake3.hpp"
#include "adi/store.hpp"
#include "adi/history.hpp"
#include "adi/digest.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <cstdio>
#include <fstream>
namespace {
using namespace adi;
namespace fs = std::filesystem;
int checks = 0, failures = 0;
void check(bool ok, const char* name) { ++checks; if (!ok) { ++failures; std::printf("FAIL %s\n", name); } }
void write(const fs::path& p, const std::string& bytes) { std::ofstream out(p, std::ios::binary); out << bytes; if (!out) throw std::runtime_error("fixture write"); }
struct Fixture {
    adi::test::TempDirectory temp{"media_ops", "project"};
    std::unique_ptr<Store> store;
    Fixture() {
        fs::create_directory(temp.path() / "project");
        StoreError error; store = Store::create(temp.path() / "project" / "song.adi", error);
        if (!store) throw std::runtime_error("fixture create");
        store->db().exec("INSERT INTO project(id,name) VALUES(1,'Media')");
    }
    fs::path folder() const { return temp.path() / "project"; }
};
CommitResult op(Store& store, const char* type, Payload payload) {
    OpRequest r; r.opType = type; r.payload = std::move(payload); return OpJournal(store).commit(r);
}
void registry() {
    for (const char* name : {"media.import", "media.unlink", "media.relink"}) {
        const auto* d = OpRegistry::instance().find(name);
        check(d && d->apply && d->buildInverse && d->scope == Scope::Edit && !d->ephemeral, "media op registered with inverse");
    }
    check(!OpRegistry::instance().find("media.embed") && !OpRegistry::instance().find("media.extract"), "retired embedding ops never implemented");
}
void operations() {
    Fixture f; auto& s = *f.store;
    write(f.folder() / "source.bin", "first media content");
    const auto cwd = fs::current_path();
    struct Restore { fs::path p; ~Restore() { fs::current_path(p); } } restore{cwd};
    fs::current_path(f.temp.path());
    check(media::importMedia(s, "source.bin", 10, 123456).ok, "import hashes a project-relative input path");
    const auto original = media::mediaRow(s.db(), 10);
    check(original.at("rel_path") == "source.bin" && original.at("abs_path_hint").is_null(), "relative path is computed against adi folder, not cwd");
    check(original.at("hash_blake3") == media::blake3File(f.folder() / "source.bin").hex, "import stores actual BLAKE3");
    check(original.at("imported_utc") == 123456 && original.at("size_bytes") == 19, "import captures supplied time and measured size");
    check(s.db().execAndGet("SELECT embedded FROM media_files WHERE id=10").getInt() == 0, "import always referenced");
    const auto duplicate = media::importMedia(s, f.folder() / "source.bin", 11, 123457);
    check(duplicate.ok && duplicate.existing && duplicate.mediaId == 10, "same hash deduplicates to existing id");
    check(s.db().execAndGet("SELECT COUNT(*) FROM ops").getInt() == 1, "dedup creates no destructive undo entry");
    check(!media::importMedia(s, "absent", 12, 0).ok, "missing import reported");
    check(s.db().execAndGet("SELECT COUNT(*) FROM media_files").getInt() == 1, "failed import leaves no row");
    fs::rename(f.folder() / "source.bin", f.folder() / "moved.bin");
    check(media::relinkMedia(s, 10, "moved.bin").ok, "relink accepts moved identical content");
    check(media::mediaRow(s.db(), 10).at("rel_path") == "moved.bin", "relink updates location with same id");
    History history(s);
    check(history.undo().ok && media::mediaRow(s.db(), 10) == original, "relink inverse restores previous paths despite absent old file");
    check(history.redo().ok && media::mediaRow(s.db(), 10).at("rel_path") == "moved.bin", "relink redo applies captured paths");
    write(f.folder() / "wrong.bin", "different content");
    check(!media::relinkMedia(s, 10, "wrong.bin").ok, "relink refuses hash mismatch");
    check(media::mediaRow(s.db(), 10).at("rel_path") == "moved.bin", "failed relink leaves paths intact");
    s.db().exec("UPDATE media_files SET sample_rate=48000,channels=2,frames=99,duration_ns=42,format='test',bit_depth=24,peaks=X'0001FF',missing=1 WHERE id=10");
    const auto complete = media::mediaRow(s.db(), 10);
    check(op(s, "media.unlink", {{"id", 10}}).ok, "unlink removes pool row");
    check(fs::exists(f.folder() / "moved.bin"), "unlink never deletes media file");
    fs::remove(f.folder() / "moved.bin");
    check(history.undo().ok && media::mediaRow(s.db(), 10) == complete, "unlink inverse restores every metadata column and peaks without disk IO");
    check(history.redo().ok && s.db().execAndGet("SELECT COUNT(*) FROM media_files").getInt() == 0, "unlink redo");
    check(history.undo().ok, "restore for refusals");
    s.db().exec("INSERT INTO tracks(id,kind,name,freeze_media_id) VALUES(1,'audio','Frozen',10)");
    check(!op(s, "media.unlink", {{"id", 10}}).ok, "unlink refuses freeze references instead of losing inverse state");
    check(s.db().execAndGet("SELECT freeze_media_id FROM tracks WHERE id=1").getInt() == 10, "refused unlink preserves reference");
    check(!op(s, "media.relink", {{"id", 10}, {"hash", std::string(64,'0')}, {"paths", {{"rel_path", "wrong"}, {"abs_path_hint", nullptr}, {"missing", 0}}}}).ok, "raw relink cannot change content identity");
    check(!op(s, "media.import", {{"id", 99}, {"row", {{"hash_blake3", "bad"}}}}).ok, "malformed captured row rejected");
    check(!op(s, "media.unlink", {{"id", 999}}).ok, "unlink absent row fails transactionally");
}
void replay() {
    Fixture a, b;
    write(a.folder() / "a", "replay contents");
    check(media::importMedia(*a.store, "a", 20, 77).ok, "replay setup import");
    write(a.folder() / "b", "replay contents");
    check(media::relinkMedia(*a.store, 20, "b").ok, "replay setup relink");
    check(op(*a.store, "media.unlink", {{"id",20}}).ok, "replay setup unlink");
    fs::remove(a.folder() / "a"); fs::remove(a.folder() / "b");
    SQLite::Statement rows(a.store->db(), "SELECT op_type,payload FROM ops ORDER BY seq");
    bool applied = true;
    while (rows.executeStep()) {
        const auto data = rows.getColumn(1); std::string error;
        auto payload = decodeFromLog({static_cast<const std::byte*>(data.getBlob()), static_cast<std::size_t>(data.getBytes())}, error);
        if (!payload) { applied = false; break; }
        OpRequest request; request.opType = rows.getColumn(0).getString(); request.payload = *payload; request.actor = Actor::Script;
        applied &= OpJournal(*b.store).commit(request).ok;
    }
    check(applied, "persisted CBOR replays all three ops with source files removed");
    check(digestProject(*a.store).text == digestProject(*b.store).text, "replayed project digest matches");
    History ha(*a.store), hb(*b.store);
    check(ha.undo().ok && hb.undo().ok, "both replay histories undo unlink");
    check(media::mediaRow(a.store->db(),20) == media::mediaRow(b.store->db(),20), "replay captures identical full inverse row");
    check(ha.undo().ok && hb.undo().ok && ha.undo().ok && hb.undo().ok, "both histories undo relink and import without files");
}
}
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    try { registry(); operations(); replay(); } catch (const std::exception& e) { check(false, e.what()); }
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
