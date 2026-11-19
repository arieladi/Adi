// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/media/collect_export.hpp"
#include "adi/media/publish_file.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/media/blake3.hpp"
#include "adi/store.hpp"
#include "adi/digest.hpp"
#include "adi/check.hpp"
#include "adi/history.hpp"
#include "adi/ui/project_document.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <miniz.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <thread>
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
void concurrentDestination(const fs::path& copy) {
    write(copy.parent_path().parent_path() / "target", "another writer's output");
}
void concurrentDestinationAndFail(const fs::path& copy) {
    concurrentDestination(copy);
    failCopy(copy);
}
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
    result = media::publishFile(source, target, {refuseRename, concurrentDestination},
                                media::PublishPolicy::AtomicOnly);
    check(result.status == media::PublishStatus::failed && !result.usedFallback &&
          !fs::exists(target) && read(source) == bytes,
          "atomic-only refusal reports failure without starting the copy");
    result = media::publishFile(source, target, {refuseRename});
    check(result.status == media::PublishStatus::published && result.usedFallback && read(target) == bytes, "refused rename takes verified streaming fallback");
    check(result.identity.has_value(), "verified publication reports destination identity");
    check(!fs::exists(source), "fallback consumes staging only after verification");
    fs::remove(target); write(source, bytes);
    result = media::publishFile(source, target, {refuseRename, corruptCopy});
    check(result.status == media::PublishStatus::failed, "fallback rehash catches corrupted target");
    check(!fs::exists(target) && read(source) == bytes, "hash failure removes target and retains source");
    result = media::publishFile(source, target, {refuseRename, failCopy});
    check(result.status == media::PublishStatus::failed, "fallback reports injected copy failure");
    check(!fs::exists(target), "failed fallback removes partial target");
    result = media::publishFile(source, target, {refuseRename, concurrentDestination});
    check(result.status == media::PublishStatus::exists && read(target) == "another writer's output",
          "late fallback collision leaves the other writer's output intact");
    check(read(source) == bytes, "late collision retains the original stage");
    fs::remove(target);
    result = media::publishFile(source, target, {refuseRename, concurrentDestinationAndFail});
    check(result.status == media::PublishStatus::failed && read(target) == "another writer's output",
          "fallback failure cleanup never touches a concurrently published destination");
    check(read(source) == bytes, "failed copy retains the original stage");
    check(std::distance(fs::directory_iterator(temp.path()), fs::directory_iterator{}) == 2,
          "private copy staging is removed on success, collision and failure");
}
void collectionCancellation() {
    using media::ArchivePhase;
    Fixture f;
    f.add("one.wav", "first sample", 1);
    f.add("two.wav", "second sample", 2);
    const auto before = digestProject(*f.store).text;
    const auto output = f.temp.path() / "portable.zip";
    for (const auto phase : {ArchivePhase::Snapshot, ArchivePhase::CollectMedia,
                             ArchivePhase::WriteArchive, ArchivePhase::Publish}) {
        std::stop_source stop;
        bool sawBoundary = false;
        const auto result = media::collectExport(f.store->path(), output, {stop.get_token(),
            [&](media::ArchiveProgress progress) {
                if (progress.phase != phase ||
                    (phase == ArchivePhase::WriteArchive && progress.entriesWritten == 0)) return;
                sawBoundary = true;
                stop.request_stop();
            }});
        check(sawBoundary && result.cancelled && !result.ok && !fs::exists(output),
              "cancellation at a worker boundary publishes no archive");
        bool stageRemains = false;
        for (const auto& entry : fs::directory_iterator(f.temp.path()))
            stageRemains = stageRemains || entry.path().filename().string().starts_with(".adi-media-");
        check(!stageRemains && digestProject(*f.store).text == before,
              "cancel discards private files and leaves the open source intact");
    }

    bool beforePublish = false;
    const auto late = media::collectExport(f.store->path(), output, {{},
        [&](media::ArchiveProgress progress) {
            if (progress.phase != ArchivePhase::Publish) return;
            beforePublish = progress.entriesWritten == 3;
            write(output, "another writer's ZIP");
        }});
    check(beforePublish && !late.ok && !late.cancelled && read(output) == "another writer's ZIP",
          "a late arrival after ZIP finalization survives exclusive publication");
}

void cancellationInsideEntry() {
    Fixture fixture;
    std::string samples(4 * 1024 * 1024 + 123, '\0');
    for (std::size_t i = 0; i < samples.size(); ++i) samples[i] = static_cast<char>((i * 29 + i / 17) & 255);
    fixture.add("large.wav", samples, 1);
    const auto source = fixture.temp.path() / "large.wav";
    const auto hash = media::blake3File(source).hex;
    const auto before = digestProject(*fixture.store).text;
    const auto owner = std::this_thread::get_id();
    std::stop_source stop;
    bool observed = false, bounded = true, sameWorker = true, publishSeen = false;
    std::uint64_t last = 0; unsigned chunks = 0;
    const auto output = fixture.temp.path() / "cancelled-inside.zip";
    const auto result = media::collectExport(fixture.store->path(), output, {stop.get_token(),
        [&](media::ArchiveProgress progress) {
            if (progress.phase == media::ArchivePhase::Publish) publishSeen = true;
            if (progress.phase != media::ArchivePhase::WriteArchive || !progress.entry.starts_with("audio/")) return;
            observed = true; sameWorker = sameWorker && std::this_thread::get_id() == owner;
            bounded = bounded && progress.entriesWritten == 1 && progress.entryBytesTotal == samples.size() &&
                progress.entryBytesRead >= last && progress.entryBytesRead - last <= 65536 && progress.entryBytesRead <= progress.entryBytesTotal;
            if (progress.entryBytesRead > last) ++chunks;
            last = progress.entryBytesRead;
            if (last >= 256 * 1024) stop.request_stop();
        }});
    check(observed && bounded && sameWorker && chunks == 4 && last == 256 * 1024 && last < samples.size(),
          "real collect cancellation occurs during the fourth bounded media-entry read");
    check(result.cancelled && !result.ok && !publishSeen && !fs::exists(output), "mid-entry cancellation never reaches publication");
    bool staged = false;
    for (const auto& entry : fs::directory_iterator(fixture.temp.path())) staged |= entry.path().filename().string().starts_with(".adi-media-");
    check(!staged && media::blake3File(source).hex == hash && digestProject(*fixture.store).text == before,
          "mid-entry cancellation cleans private ZIP/copy and preserves source bytes/project");
    bool threwInEntry = false;
    const auto failed = media::collectExport(fixture.store->path(), output, {{},
        [&](media::ArchiveProgress progress) {
            if (progress.entry.starts_with("audio/") && progress.entryBytesRead > 0) { threwInEntry = true; throw 17; }
        }});
    check(threwInEntry && !failed.ok && !failed.cancelled && failed.error.find("progress") != std::string::npos && !fs::exists(output),
          "nonstandard observer exception becomes an archive failure, never a C unwind or cancellation");
    staged = false;
    for (const auto& entry : fs::directory_iterator(fixture.temp.path())) staged |= entry.path().filename().string().starts_with(".adi-media-");
    check(!staged && media::blake3File(source).hex == hash && digestProject(*fixture.store).text == before,
          "observer failure preserves source and removes private stage");
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
    const auto missingResult=f.store->collectAllAndSave();
    check(!missingResult.ok,"a missing asset aborts collection");
    check(digestProject(*f.store).text==before,"failed collection leaves all rows and captured paths unchanged");
#if defined(_WIN32)
    check(fs::is_empty(f.temp.path()/"audio"),"failed collection removes only its newly published media and stages");
#else
    check(!missingResult.warning.empty() && !fs::is_empty(f.temp.path()/"audio"),"unsupported rollback retains and reports public media");
    fs::remove_all(f.temp.path()/"audio");fs::create_directory(f.temp.path()/"audio");
#endif
    write(missing,missingB?"second":"first");
    f.store->db().exec("CREATE TRIGGER refuse_collection BEFORE UPDATE OF rel_path ON media_files BEGIN SELECT RAISE(ABORT,'injected update failure'); END");
    const auto sqlResult=f.store->collectAllAndSave();
#if defined(_WIN32)
    check(!sqlResult.ok && sqlResult.warning.empty() && fs::is_empty(f.temp.path()/"audio"),"SQL failure after copying removes all files published by collection");
#else
    check(!sqlResult.ok && !sqlResult.warning.empty() && !fs::is_empty(f.temp.path()/"audio"),"SQL failure retains and reports public media without pathname unlink");
    fs::remove_all(f.temp.path()/"audio");fs::create_directory(f.temp.path()/"audio");
#endif
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
// On unix SQLite names a project by its resolved path (macOS's temporary folder
// /var/... is a link to /private/var/...) and ".." climbs physical parents. A
// link shallower than its target reproduces that on any unix system: paths
// rebased against the link's spelling climb too few levels. (Windows keeps the
// link's spelling and resolves ".." lexically, so there the link is harmless.)
void linkedOutputFolder() {
    Fixture f;f.add("media/linked.bin","linked bytes",9);
    const auto deep=f.temp.path()/"deep"/"er",shortcut=f.temp.path()/"shortcut";fs::create_directories(deep);
    std::error_code ec;fs::create_directory_symlink(deep,shortcut,ec);
    if(ec){
#if defined(_WIN32)
        std::printf("SKIP folder links unavailable: %s\n",ec.message().c_str());
#else
        std::printf("folder link diagnostic: %s\n",ec.message().c_str());
        check(false,"the test folder takes a directory symlink");
#endif
        return;
    }
    const auto exported=media::collectExport(f.store->path(),shortcut/"linked.zip");
    if(!exported.ok)std::printf("linked export diagnostic: %s\n",exported.error.c_str());
    check(exported.ok && fs::exists(deep/"linked.zip"),"Collect and Export into a folder reached through a symlink");
    check(f.store->saveCopy(shortcut/"Linked.adi").ok,"Save a Copy into a folder reached through a symlink");
    StoreError error{};auto copy=Store::open(shortcut/"Linked.adi",error,true);
    check(copy && resolves(*copy,9),"a copy saved through a symlinked folder resolves its media");
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
    const auto extracted=media::extractMedia(*f.store);
    check(!extracted.ok, "bad second embedded hash aborts whole extraction");
    check(digestProject(*f.store).text == before, "failed extraction rolls back all row and blob changes");
#if defined(_WIN32)
    check(extracted.warning.empty() && fs::is_empty(f.temp.path() / "audio"), "failed extraction removes first newly published file");
#else
    check(!extracted.warning.empty() && !fs::is_empty(f.temp.path()/"audio"), "failed extraction retains and reports public files on unsupported hosts");
#endif
    check(f.store->db().execAndGet("SELECT COUNT(*) FROM media_blobs").getInt() == 4, "rollback retains original blobs");
    Fixture good; legacy(good, false);
    const auto path = good.store->path(); check(good.store->close(), "close legacy fixture for command"); good.store.reset();
    check(cli("extract-media " + quote(path), good.temp.path() / "extract.log") == 0, "extract-media CLI succeeds on legacy file");
    check(cli("check " + quote(path), good.temp.path() / "check.log") == 0, "extracted legacy file passes CLI check");
    Fixture gap; legacy(gap,false); gap.store->db().exec("DELETE FROM media_blobs WHERE media_id=10 AND chunk_index=0");
    check(!media::extractMedia(*gap.store).ok, "chunk index gap refused");
}
struct ReplacementAbort {
    fs::path folder,target,saved;
    bool invoked=false,replaced=false;
    static void run(sqlite3_context* sql,int argc,sqlite3_value** argv) noexcept {
        auto& self=*static_cast<ReplacementAbort*>(sqlite3_user_data(sql));
        try {
            self.invoked=true;
            if(argc!=1 || sqlite3_value_type(argv[0])!=SQLITE_TEXT)throw std::runtime_error("missing collected path");
            self.target=self.folder/media::pathFromUtf8(reinterpret_cast<const char*>(sqlite3_value_text(argv[0])));
            self.saved=self.folder/"owned-before-replacement.bin";
            fs::rename(self.target,self.saved);
            write(self.target,"later writer's replacement");self.replaced=true;
            sqlite3_result_error(sql,"intentional SQL abort after replacement",-1);
        } catch(const std::exception& error) {sqlite3_result_error(sql,error.what(),-1);}
    }
};
void replacementDuringRollback() {
    for(const bool extracting:{false,true}) {
        Fixture f;
        if(extracting)legacy(f,true);else f.add("source.bin","original owned bytes",10);
        const auto before=digestProject(*f.store).text;
        ReplacementAbort abort;abort.folder=f.temp.path();
        const auto installed=sqlite3_create_function_v2(f.store->db().getHandle(),"replace_and_abort",1,SQLITE_UTF8,
            &abort,&ReplacementAbort::run,nullptr,nullptr,nullptr);
        check(installed==SQLITE_OK,"install deterministic SQL replacement fixture");
        f.store->db().exec("CREATE TEMP TRIGGER replace_before_rollback BEFORE UPDATE OF rel_path ON media_files "
            "WHEN NEW.id=10 BEGIN SELECT replace_and_abort(NEW.rel_path); END");
        bool ok=false;std::string error,warning;
        if(extracting) {const auto result=media::extractMedia(*f.store);ok=result.ok;error=result.error;warning=result.warning;}
        else {const auto result=f.store->collectAllAndSave();ok=result.ok;error=result.error;warning=result.warning;}
        check(!ok && abort.invoked && abort.replaced && error.find("intentional SQL abort after replacement")!=std::string::npos,
            "actual publication is replaced before a forced SQL failure");
        check(digestProject(*f.store).text==before,"replacement-triggered failure rolls back every SQL change");
        check(read(abort.target)=="later writer's replacement","rollback preserves a later writer's replacement at the published pathname");
        check(!warning.empty(),"rollback explicitly reports retained or changed public media");
        check(fs::exists(abort.saved),"rollback does not chase the original file to a different pathname");
        f.store->db().exec("DROP TRIGGER replace_before_rollback");
        check(sqlite3_create_function_v2(f.store->db().getHandle(),"replace_and_abort",1,SQLITE_UTF8,nullptr,nullptr,nullptr,nullptr,nullptr)==SQLITE_OK,
            "remove SQL replacement fixture");
    }
}
void ownedPublicationRemoval() {
    Fixture f;const auto staged=f.temp.path()/"stage",target=f.temp.path()/"published.bin";
    write(staged,"owned bytes");
    const auto published=media::publishFile(staged,target,{},media::PublishPolicy::AtomicOnly);
    check(published.status==media::PublishStatus::published && published.identity.has_value(),"removal fixture has actual published identity");
    if(!published.identity)return;
#if defined(_WIN32)
    {std::ifstream reader(target,std::ios::binary);
        check(reader.good() && media::removePublishedFile(target,*published.identity) && read(target)=="owned bytes",
              "busy publication is retained instead of falling back to pathname deletion");}
    check(!media::removePublishedFile(target,*published.identity) && !fs::exists(target),"exclusive DELETE handle removes the expected publication");
    check(!media::removePublishedFile(target,*published.identity),"already absent publication is clean");
#else
    check(media::removePublishedFile(target,*published.identity)==std::errc::operation_not_supported && read(target)=="owned bytes",
          "unsupported host retains public media rather than unsafe check-then-unlink");
    fs::remove(target);
#endif
    write(target,"another object");
    check(media::removePublishedFile(target,*published.identity) && read(target)=="another object", "different object at the published path survives");
    fs::remove(target);fs::create_directory(target);write(target/"keep","directory content");
    check(media::removePublishedFile(target,*published.identity) && read(target/"keep")=="directory content","directory replacement and its contents survive");
    fs::remove(target/"keep");fs::remove(target);
    const auto linked=f.temp.path()/"linked.bin";write(linked,"symlink target bytes");std::error_code linkError;
    fs::create_symlink(linked,target,linkError);
    if(linkError) {
#if defined(_WIN32)
        std::printf("SKIP rollback symlink fixture unavailable: %s\n",linkError.message().c_str());
#else
        check(false,"symlink fixture is available");
#endif
    } else {
        check(media::removePublishedFile(target,*published.identity) && fs::is_symlink(fs::symlink_status(target)) && read(linked)=="symlink target bytes",
              "symbolic link replacement and its referent survive");
        fs::remove(target);
    }
    write(staged,"hard-link bytes");
    const auto hard=media::publishFile(staged,target,{},media::PublishPolicy::AtomicOnly);
    check(hard.status==media::PublishStatus::published && hard.identity.has_value(),"hard-link rollback fixture publishes");
    if(hard.identity) {
        const auto alias=f.temp.path()/"alias.bin";fs::create_hard_link(target,alias);
#if defined(_WIN32)
        check(!media::removePublishedFile(target,*hard.identity) && !fs::exists(target) && read(alias)=="hard-link bytes",
              "handle deletion removes only the opened link and preserves another link");
#else
        check(media::removePublishedFile(target,*hard.identity) && read(target)=="hard-link bytes" && read(alias)=="hard-link bytes",
              "unsupported rollback preserves both hard links");
#endif
    }
}
void documentRollbackWarning() {
    Fixture f;f.add("source.bin","document-owned bytes",10);
    f.store->db().exec("INSERT INTO tracks(id,kind,name) VALUES(9,'master','Main')");
    const auto before=digestProject(*f.store).text;
    check(f.store->close(),"close collection document fixture");f.store.reset();
    std::string error;auto document=ui::ProjectDocument::open(f.temp.path()/"song.adi",false,{},error);
    check(document!=nullptr,"open real collection document");if(!document)return;
    ReplacementAbort abort;abort.folder=f.temp.path();
    auto& db=document->store().db();
    check(sqlite3_create_function_v2(db.getHandle(),"replace_and_abort",1,SQLITE_UTF8,&abort,
        &ReplacementAbort::run,nullptr,nullptr,nullptr)==SQLITE_OK,"install document replacement fixture");
    db.exec("CREATE TEMP TRIGGER replace_before_rollback BEFORE UPDATE OF rel_path ON media_files "
        "WHEN NEW.id=10 BEGIN SELECT replace_and_abort(NEW.rel_path); END");
    check(!document->collectAllAndSave() && abort.replaced,"document reports real failed collection");
    check(!document->warning().empty() && document->error().find(document->warning())!=std::string::npos,
          "document failure text exposes the retained-media warning consumed by the window");
    check(read(abort.target)=="later writer's replacement" && digestProject(document->store()).text==before,
          "document failure preserves replacement and project rows");
    db.exec("DROP TRIGGER replace_before_rollback");
    check(sqlite3_create_function_v2(db.getHandle(),"replace_and_abort",1,SQLITE_UTF8,nullptr,nullptr,nullptr,nullptr,nullptr)==SQLITE_OK,
          "remove document SQL replacement fixture");
}
}
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    try { publication(); collection(); collectionCancellation(); cancellationInsideEntry(); collectActiveAndHistory(); collectionRefusals(); collectRelink(); exportHistory(); linkedOutputFolder(); extraction(false); extraction(true); resolution(); rollbackAndCli(); replacementDuringRollback(); ownedPublicationRemoval(); documentRollbackWarning(); }
    catch (const std::exception& e) { check(false, e.what()); }
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
