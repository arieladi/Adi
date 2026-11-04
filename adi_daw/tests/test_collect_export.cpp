// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/media/collect_export.hpp"
#include "adi/media/publish_file.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/media/blake3.hpp"
#include "adi/store.hpp"
#include "adi/digest.hpp"
#include "adi/check.hpp"
#include "adi/history.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <miniz.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
namespace {
using namespace adi;
namespace fs = std::filesystem;
int checks = 0, failures = 0;
void check(bool ok, const char* name) { ++checks; if (!ok) { ++failures; std::printf("FAIL %s\n", name); } }
void write(const fs::path& path, const std::string& data) { std::ofstream out(path, std::ios::binary); out << data; if (!out) throw std::runtime_error("fixture write"); }
std::string read(const fs::path& path) { std::ifstream in(path, std::ios::binary); return {std::istreambuf_iterator<char>(in), {}}; }
struct Fixture {
    adi::test::TempDirectory temp{"collect_export", "project"};
    std::unique_ptr<Store> store;
    Fixture() {
        StoreError error; store = Store::createEmpty(temp.path() / "song.adi", error);
        if (!store) throw std::runtime_error("fixture create");
        store->db().exec("INSERT INTO project(id,name) VALUES(1,'Export')");
    }
    void add(const std::string& file, const std::string& data, std::int64_t id) {
        const auto path = temp.path() / media::pathFromUtf8(file); fs::create_directories(path.parent_path()); write(path, data);
        if (!media::importMedia(*store, path, id, 123).ok) throw std::runtime_error("fixture import");
    }
};
std::string quote(const fs::path& path) {
#if defined(_WIN32)
    return "\"" + path.string() + "\"";
#else
    std::string result = "'";
    for (const char c : path.string()) result += c == '\'' ? "'\\''" : std::string(1, c);
    return result + "'";
#endif
}
int cli(const std::string& args, const fs::path& log) {
    std::string command = quote(fs::path(ADI_TOOL_PATH)) + " " + args + " > " + quote(log) + " 2>&1";
#if defined(_WIN32)
    command = "\"" + command + "\"";
#endif
    return std::system(command.c_str());
}
struct ZipReader {
    std::ifstream in; mz_zip_archive zip{}; bool ready = false;
    static size_t input(void* opaque, mz_uint64 offset, void* data, size_t n) {
        auto& stream = static_cast<ZipReader*>(opaque)->in;
        stream.clear(); stream.seekg(static_cast<std::streamoff>(offset));
        stream.read(static_cast<char*>(data), static_cast<std::streamsize>(n)); return static_cast<size_t>(stream.gcount());
    }
    static size_t output(void* opaque, mz_uint64, const void* data, size_t n) {
        auto& out = *static_cast<std::ofstream*>(opaque);
        out.write(static_cast<const char*>(data), static_cast<std::streamsize>(n)); return out ? n : 0;
    }
    explicit ZipReader(const fs::path& path) : in(path, std::ios::binary | std::ios::ate) {
        if (!in) return;
        const auto size = in.tellg(); if (size < 0) return;
        zip.m_pIO_opaque = this; zip.m_pRead = input;
        ready = mz_zip_reader_init(&zip, static_cast<mz_uint64>(size), 0) != 0;
    }
    ~ZipReader() { if (ready) mz_zip_reader_end(&zip); }
    bool extract(const fs::path& dir) {
        if (!ready) return false;
        for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip); ++i) {
            mz_zip_archive_file_stat stat{};
            if (!mz_zip_reader_file_stat(&zip, i, &stat) || stat.m_method != 0) return false;
            const auto path = dir / media::pathFromUtf8(stat.m_filename);
            fs::create_directories(path.parent_path()); std::ofstream out(path, std::ios::binary);
            if (!mz_zip_reader_extract_to_callback(&zip, i, output, &out, 0)) return false;
        }
        return true;
    }
};
std::error_code refuseRename(const fs::path&, const fs::path&) {
    return std::make_error_code(std::errc::operation_not_supported);
}
void corruptCopy(const fs::path& path) { write(path, "corrupt"); }
void failCopy(const fs::path& path) { write(path, "partial"); throw std::runtime_error("injected copy failure"); }
void publication() {
    adi::test::TempDirectory temp{"publish", "fallback"};
    const auto source = temp.path() / "source", target = temp.path() / "target";
    const std::string bytes(200000, 'p'); write(source, bytes);
    auto result = media::publishFile(source, target);
    check(result.status == media::PublishStatus::published && read(target) == bytes, "native publisher preserves bytes");
    check(!fs::exists(source), "successful publication consumes staging name");
    write(source, "new data");
    result = media::publishFile(source, target);
    check(result.status == media::PublishStatus::exists && read(target) == bytes, "native publication never clobbers");
    result = media::publishFile(source, target, {refuseRename});
    check(result.status == media::PublishStatus::exists && read(target) == bytes, "fallback never overwrites existing file");
    fs::remove(target); write(source, bytes);
    result = media::publishFile(source, target, {refuseRename});
    check(result.status == media::PublishStatus::published && result.usedFallback && read(target) == bytes, "refused rename takes verified streaming fallback");
    check(!fs::exists(source), "fallback consumes staging only after verification");
    fs::remove(target); write(source, bytes);
    result = media::publishFile(source, target, {refuseRename, corruptCopy});
    check(result.status == media::PublishStatus::failed, "fallback rehash catches corrupted target");
    check(!fs::exists(target) && read(source) == bytes, "hash failure removes target and retains source");
    result = media::publishFile(source, target, {refuseRename, failCopy});
    check(result.status == media::PublishStatus::failed, "fallback reports injected copy failure");
    check(!fs::exists(target), "failed fallback removes partial target");
}
void collection() {
    Fixture f;
    f.add("one/clip.wav", std::string(140000,'A') + "first", 1);
    f.add("two/clip.wav", std::string(140000,'B') + "second", 2);
    const auto before = digestProject(*f.store).text;
    const auto original = media::mediaRow(f.store->db(), 1);
    const auto zip = f.temp.path() / "portable.zip";
    check(fs::exists(f.temp.path() / "song.adi-wal"), "source WAL exists with uncheckpointed data");
    const auto result = cli("collect-export " + quote(f.store->path()) + " " + quote(zip), f.temp.path() / "collect.log");
    if (result != 0) std::printf("CLI diagnostic: %s\n", read(f.temp.path() / "collect.log").c_str());
    check(result == 0, "collect-export CLI succeeds with live source WAL");
    check(digestProject(*f.store).text == before && media::mediaRow(f.store->db(),1) == original, "collection never rewrites rows in original database");
    ZipReader archive(zip);
    check(archive.ready && mz_zip_is_zip64(&archive.zip), "collection produces ZIP64");
    const auto extracted = f.temp.path() / "elsewhere";
    check(archive.extract(extracted), "extract all archive entries with STORE methods and CRC verification");
    StoreError error; auto copy = Store::open(extracted / "song.adi", error, true);
    check(copy != nullptr, "extracted adi opens without its original WAL");
    if (copy) {
        check(copy->db().execAndGet("SELECT COUNT(*) FROM media_files").getInt() == 2, "backup included both committed WAL rows");
        check(checkProject(*copy, true).clean(), "extracted project check is clean including media hashes");
        const auto a = media::mediaRow(copy->db(),1), b = media::mediaRow(copy->db(),2);
        check(a.at("rel_path") != b.at("rel_path"), "same original basename gets collision-safe paths");
        bool resolved = true;
        for (const auto& row : {a,b}) {
            const auto path = media::pathFromUtf8(row.at("rel_path").get<std::string>());
            const auto hash = media::blake3File(extracted / path);
            resolved &= path.begin()->string() == "audio" && !path.is_absolute() && row.at("abs_path_hint").is_null() && hash && hash.hex == row.at("hash_blake3").get<std::string>();
        }
        check(resolved, "every exported media path resolves relative to extracted adi folder");
        copy.reset();
        check(cli("check " + quote(extracted / "song.adi"), f.temp.path() / "check.log") == 0, "adi_tool check extracted project exits zero");
    }
    const auto zipHash = media::blake3File(zip).hex;
    check(!media::collectExport(f.store->path(), zip).ok && media::blake3File(zip).hex == zipHash, "existing output is never overwritten");
    write(f.temp.path() / "one/clip.wav", "tampered");
    const auto bad = f.temp.path() / "bad.zip";
    check(cli("collect-export " + quote(f.store->path()) + " " + quote(bad), f.temp.path() / "bad.log") != 0, "hash mismatch makes CLI fail");
    check(read(f.temp.path() / "bad.log").find("clip.wav") != std::string::npos, "mismatch diagnostic names offending file");
    check(!fs::exists(bad), "hash mismatch leaves no partial zip");
    check(digestProject(*f.store).text == before, "failed export also leaves original unchanged");
    check(!checkProject(*f.store, true).clean(), "external-media check detects replaced source");
    fs::remove(f.temp.path() / "one/clip.wav");
    const auto missing = media::collectExport(f.store->path(), bad);
    check(!missing.ok && !fs::exists(bad), "missing source leaves no archive");
    bool staged = false; for (const auto& e : fs::directory_iterator(f.temp.path())) staged |= e.path().filename().string().starts_with(".adi-media-");
    check(!staged, "success and failure clean private staging directories");
}
bool resolves(Store& store,std::int64_t id) {
    std::string error;
    return !media::resolveMedia(media::projectFolder(store),media::mediaRow(store.db(),id),error).empty();
}
std::string identities(Store& store) {
    std::string result;
    SQLite::Statement rows(store.db(),"SELECT seq,txn_id,actor,op_type FROM ops ORDER BY seq");
    while(rows.executeStep())for(int col=0;col<4;++col)result+=rows.getColumn(col).getString()+"|";
    return result;
}
void collectActiveAndHistory() {
    Fixture f;f.add("external/current.bin","current data",1);
    f.add("external/undo.bin","undo data",2);
    OpRequest unlink;unlink.opType="media.unlink";unlink.payload={{"id",2}};
    check(OpJournal(*f.store).commit(unlink).ok,"unlink fixture keeps its inverse's media row");
    History history(*f.store);
    f.add("external/branch.bin","inactive branch data",3);
    check(history.undo().ok,"leave one imported asset only on an inactive history branch");
    OpRequest rename;rename.opType="project.setName";rename.payload={{"name","Fork"}};
    check(OpJournal(*f.store).commit(rename).ok,"fork from the current pool without branch media");
    std::int64_t firstBranch=0;
    for(const auto& branch:history.branches())if(!branch.isCurrent)firstBranch=branch.id;
    const auto ids=identities(*f.store);
    const auto result=f.store->collectAllAndSave();
    if(!result.ok)std::printf("collection diagnostic: %s\n",result.error.c_str());
    check(result.ok,"Collect All and Save includes current, unlinked and inactive branch media");
    check(identities(*f.store)==ids,"collection preserves every op identity, transaction and branch without appending an op");
    check(std::distance(fs::directory_iterator(f.temp.path()/"audio"),fs::directory_iterator{})==3,"one verified audio file is collected per distinct history hash");
    const auto before=digestProject(*f.store).text;
    check(f.store->collectAllAndSave().ok && digestProject(*f.store).text==before,"collection is idempotent");
    fs::remove_all(f.temp.path()/"external");
    check(resolves(*f.store,1),"current audio resolves after original media is removed");
    check(history.switchToBranch(firstBranch).ok && resolves(*f.store,3),"inactive branch imports resolve after original media is removed");
    check(history.undo().ok && history.undo().ok && resolves(*f.store,2),"undo unlink restores a collected media path");
    check(history.redo().ok && history.undo().ok && resolves(*f.store,2),"redo and undo retain portable captured paths");
    const auto current=media::mediaRow(f.store->db(),1);
    check(current.at("abs_path_hint").is_null() && current.at("rel_path").get<std::string>().starts_with("audio/"),"collected paths are local with no external absolute hints");
    check(f.store->close(),"collected WAL folds on close");f.store.reset();
    const auto moved=f.temp.path()/"moved";fs::create_directory(moved);
    fs::rename(f.temp.path()/"song.adi",moved/"song.adi");fs::rename(f.temp.path()/"audio",moved/"audio");
    StoreError error{};f.store=Store::open(moved/"song.adi",error);
    check(f.store && resolves(*f.store,1) && resolves(*f.store,2),"moving just the project and audio directory preserves playback sources");
}
void collectionRefusals() {
    Fixture f;f.add("a.bin","first",1);f.add("b.bin","second",2);
    const auto before=digestProject(*f.store).text;
    const auto a=media::mediaRow(f.store->db(),1),b=media::mediaRow(f.store->db(),2);
    const bool missingB=a.at("hash_blake3").get<std::string>()<b.at("hash_blake3").get<std::string>();
    const auto missing=f.temp.path()/(missingB?"b.bin":"a.bin");
    fs::remove(missing);
    check(!f.store->collectAllAndSave().ok,"a missing asset aborts collection");
    check(digestProject(*f.store).text==before,"failed collection leaves all rows and captured paths unchanged");
    check(fs::is_empty(f.temp.path()/"audio"),"failed collection removes only its newly published media and stages");
    write(missing,missingB?"second":"first");
    f.store->db().exec("CREATE TRIGGER refuse_collection BEFORE UPDATE OF rel_path ON media_files BEGIN SELECT RAISE(ABORT,'injected update failure'); END");
    check(!f.store->collectAllAndSave().ok && fs::is_empty(f.temp.path()/"audio"),"SQL failure after copying removes all files published by collection");
    check(digestProject(*f.store).text==before,"SQL failure restores pool and captured path bytes");
    f.store->db().exec("DROP TRIGGER refuse_collection");
    const auto aTarget=f.temp.path()/"audio"/(a.at("hash_blake3").get<std::string>()+".bin");
    const auto bTarget=f.temp.path()/"audio"/(b.at("hash_blake3").get<std::string>()+".bin");
    write(aTarget,"first");write(bTarget,"keep wrong existing file");
    check(!f.store->collectAllAndSave().ok && read(aTarget)=="first" && read(bTarget)=="keep wrong existing file","hash-addressed targets are verified and never overwritten or removed on failure");
    check(digestProject(*f.store).text==before,"existing corrupt destination also rolls back path changes");
    fs::remove(bTarget);
    check(f.store->collectAllAndSave().ok && read(aTarget)=="first","existing matching target is safely reused");
    {
        SQLite::Transaction transaction(f.store->db());
        check(!f.store->collectAllAndSave().ok,"collection refuses an active transaction");
        check(!f.store->collectAndExport(f.temp.path()/"uncommitted.zip").ok,"ZIP Store call refuses uncommitted state");
    }
    {
        SQLite::Statement live(f.store->db(),"SELECT id FROM media_files");
        check(!f.store->collectAllAndSave().ok,"collection refuses live SQL statements");
    }
    StoreError error{};auto ro=Store::open(f.store->path(),error,true);
    check(ro && !ro->collectAllAndSave().ok,"read-only collection is refused");
    const auto zip=f.temp.path()/"readonly.zip";
    check(ro && ro->collectAndExport(zip).ok,"read-only Store can collect and export a private copy");
    check(!ro->collectAndExport(zip).ok,"Store ZIP export never overwrites output");
    ro.reset();check(f.store->close(),"collection fixture closes");
    check(!f.store->collectAllAndSave().ok && !f.store->collectAndExport(f.temp.path()/"closed.zip").ok,"closed Store collection calls fail cleanly");
}
void collectRelink() {
    Fixture f;f.add("original.bin","relinked bytes",8);
    write(f.temp.path()/"moved.bin","relinked bytes");
    check(media::relinkMedia(*f.store,8,"moved.bin").ok,"collection relink fixture");
    check(f.store->collectAllAndSave().ok,"collection rewrites both captured relink paths");
    fs::remove(f.temp.path()/"original.bin");fs::remove(f.temp.path()/"moved.bin");
    History history(*f.store);
    check(history.undo().ok && resolves(*f.store,8),"collected relink inverse resolves without its original source");
    check(history.redo().ok && resolves(*f.store,8),"collected relink forward path stays portable");
}
void exportHistory() {
    Fixture f;f.add("elsewhere/hidden.bin","only in history",7);
    History history(*f.store);check(history.undo().ok,"ZIP fixture has media only in history");
    const auto before=digestProject(*f.store).text,ids=identities(*f.store);
    const auto zip=f.temp.path()/"history.zip";
    check(f.store->collectAndExport(zip).ok,"Store ZIP export collects history-only media");
    check(digestProject(*f.store).text==before && identities(*f.store)==ids,"ZIP export leaves live source pool and history unchanged");
    ZipReader archive(zip);const auto unpacked=f.temp.path()/"unpacked";
    check(archive.extract(unpacked),"history ZIP extracts");
    fs::remove_all(f.temp.path()/"elsewhere");
    StoreError error{};auto copy=Store::open(unpacked/"song.adi",error);
    check(copy && History(*copy).redo().ok && resolves(*copy,7),"ZIP's redo restores media without access to original files");
}
void legacy(Fixture& f, bool upgraded, bool badSecond = false) {
    auto& db = f.store->db();
    db.exec("DROP TRIGGER media_never_embedded_insert; DROP TRIGGER media_never_embedded_update; DROP TRIGGER media_blobs_forbidden;");
    for (const std::int64_t id : {10,11}) {
        const auto name = std::to_string(id) + ".wav";
        const auto data = std::string(80000, id == 10 ? 'x' : 'y');
        f.add(name, data, id);
        SQLite::Statement mark(db,"UPDATE media_files SET embedded=?,rel_path=NULL,abs_path_hint=NULL,orig_name='same.wav' WHERE id=?");
        mark.bind(1,id == 10 ? 1 : 0); mark.bind(2,id); mark.exec();
        for (int chunk = 0; chunk < 2; ++chunk) {
            auto part = data.substr(static_cast<std::size_t>(chunk) * 40000,40000);
            if (badSecond && id == 11 && chunk == 1) part.back() = 'z';
            SQLite::Statement insert(db,"INSERT INTO media_blobs VALUES(?,?,?)"); insert.bind(1,id); insert.bind(2,chunk); insert.bind(3,part.data(),static_cast<int>(part.size())); insert.exec();
        }
        fs::remove(f.temp.path() / name);
    }
    // Match the declared old minor, not a modern schema with only its version
    // number lowered: drop every table a later minor added (1.1 added only the
    // triggers, dropped above), so a schema bump cannot leave one behind.
    // Newest first: a later table may reference an earlier one (op_clocks -> op_clients).
    const auto added = adi::tablesAddedAfter(1);
    for (auto t = added.rbegin(); t != added.rend(); ++t) db.exec("DROP TABLE " + *t);
    db.exec(upgraded ? "PRAGMA user_version=1001" : "PRAGMA user_version=1000");
    if (upgraded) db.exec(
        "CREATE TRIGGER media_never_embedded_insert BEFORE INSERT ON media_files WHEN NEW.embedded<>0 BEGIN SELECT RAISE(ABORT,'no embedded'); END;"
        "CREATE TRIGGER media_never_embedded_update BEFORE UPDATE OF embedded ON media_files WHEN NEW.embedded<>0 BEGIN SELECT RAISE(ABORT,'no embedded'); END;"
        "CREATE TRIGGER media_blobs_forbidden BEFORE INSERT ON media_blobs BEGIN SELECT RAISE(ABORT,'no blobs'); END;");
}
void extraction(bool upgraded) {
    Fixture f; legacy(f, upgraded);
    fs::create_directory(f.temp.path() / "audio"); write(f.temp.path() / "audio/same.wav", "keep existing");
    const auto original = digestProject(*f.store).text;
    const auto zip = f.temp.path() / "legacy.zip";
    check(media::collectExport(f.store->path(), zip).ok, "collect legacy embedded data without upgrading source");
    check(digestProject(*f.store).text == original, "legacy collection leaves original flags and chunks untouched");
    ZipReader reader(zip); check(reader.extract(f.temp.path() / "unpacked"), "legacy collected ZIP extracts");
    StoreError err; auto unpacked = Store::open(f.temp.path() / "unpacked/song.adi",err,true);
    check(unpacked && checkProject(*unpacked,true).clean(), "legacy collected copy has clean external media");
    const auto extracted = media::extractMedia(*f.store);
    check(extracted.ok, "extract embedded and unflagged chunks on 1.0 or locked 1.1");
    check(f.store->db().execAndGet("SELECT COUNT(*) FROM media_blobs").getInt() == 0, "all blob chunks deleted after extraction");
    check(f.store->db().execAndGet("SELECT SUM(embedded) FROM media_files").getInt() == 0, "embedded flags cleared");
    check(read(f.temp.path() / "audio/same.wav") == "keep existing", "extraction never clobbers existing basename");
    const auto a = media::mediaRow(f.store->db(),10), b = media::mediaRow(f.store->db(),11);
    check(a.at("rel_path") != b.at("rel_path"), "legacy duplicate names receive distinct locations");
    check(checkProject(*f.store,true).clean(), "extracted files hash correctly and project check is clean");
    check(media::extractMedia(*f.store).ok, "extraction is idempotent");
}
void resolution() {
    Fixture f; f.add("external.bin", "resolve", 30);
    const auto row = media::mediaRow(f.store->db(),30);
    fs::create_directory(f.temp.path() / "audio");
    fs::rename(f.temp.path() / "external.bin", f.temp.path() / "audio/external.bin");
    std::string error;
    check(media::resolveMedia(f.temp.path(),row,error) == f.temp.path() / "audio/external.bin", "resolver tries project audio folder after missing relative path");
    auto captured = row; captured["abs_path_hint"] = "invalid-relative-hint";
    if (media::resolveMedia(f.temp.path(),captured,error).empty()) { std::printf("ERROR WAS: %s\n", error.c_str()); check(false, "valid earlier candidate takes precedence over unused hint"); } else check(true, "valid earlier candidate takes precedence over unused hint");
    captured["rel_path"] = nullptr; captured["orig_name"] = "";
    captured["abs_path_hint"] = media::pathUtf8(f.temp.path() / "audio/external.bin");
    check(!media::resolveMedia(f.temp.path(),captured,error).empty(), "absolute hint resolves when relative and basename absent");
    write(f.temp.path() / "external.bin", "wrong contents");
    check(media::resolveMedia(f.temp.path(),row,error).empty() && error.find("hash mismatch") != std::string::npos,
          "existing wrong relative candidate stops before matching audio fallback");
}
void rollbackAndCli() {
    Fixture f; legacy(f, true, true);
    const auto before = digestProject(*f.store).text;
    check(!media::extractMedia(*f.store).ok, "bad second embedded hash aborts whole extraction");
    check(digestProject(*f.store).text == before, "failed extraction rolls back all row and blob changes");
    check(fs::is_empty(f.temp.path() / "audio"), "failed extraction removes first newly published file");
    check(f.store->db().execAndGet("SELECT COUNT(*) FROM media_blobs").getInt() == 4, "rollback retains original blobs");
    Fixture good; legacy(good, false);
    const auto path = good.store->path(); check(good.store->close(), "close legacy fixture for command"); good.store.reset();
    check(cli("extract-media " + quote(path), good.temp.path() / "extract.log") == 0, "extract-media CLI succeeds on legacy file");
    check(cli("check " + quote(path), good.temp.path() / "check.log") == 0, "extracted legacy file passes CLI check");
    Fixture gap; legacy(gap,false); gap.store->db().exec("DELETE FROM media_blobs WHERE media_id=10 AND chunk_index=0");
    check(!media::extractMedia(*gap.store).ok, "chunk index gap refused");
}
}
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    try { publication(); collection(); collectActiveAndHistory(); collectionRefusals(); collectRelink(); exportHistory(); extraction(false); extraction(true); resolution(); rollbackAndCli(); }
    catch (const std::exception& e) { check(false, e.what()); }
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
