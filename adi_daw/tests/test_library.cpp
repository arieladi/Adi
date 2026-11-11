// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/library/index.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/store.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <nlohmann/json.hpp>
#include <thread>
#include <tuple>
namespace {
using namespace adi;
namespace fs=std::filesystem;
using Json=nlohmann::json;
int checks=0,failures=0;
void check(bool ok,const char* text) { ++checks; if(!ok){++failures;std::printf("FAIL %s\n",text);} }
void write(const fs::path& path,const std::string& bytes) { std::ofstream f(path,std::ios::binary);f<<bytes;f.close();if(!f)throw std::runtime_error("fixture write"); }
struct Fixture {
    test::TempDirectory dir{"library","fixture"};
    fs::path root=dir.path()/"volume";
    std::unique_ptr<library::Index> index;
    library::Volume volume;
    Fixture(bool automatic=false,library::Index::Hasher hasher={}) {
        fs::create_directory(root);std::string error;
        library::MarkerVolumeProvider provider;
        if(!provider.identify(root,volume,error))throw std::runtime_error(error);
        index=library::Index::open(dir.path()/"index.sqlite",error,automatic,std::move(hasher));
        if(!index)throw std::runtime_error(error);
    }
    library::ScanResult scan(){return index->scan(volume);}
    void finish(){index->start();index->waitIdle();}
};
void identityAndScan() {
    Fixture f;std::string error;library::MarkerVolumeProvider provider;library::Volume again;
    check(provider.identify(f.root,again,error)&&again.id==f.volume.id,"volume marker persists across probes");
    check(f.volume.id.size()==32,"identity is random marker, not root path");
    fs::create_directory(f.root/"samples");write(f.root/"samples/a", "alpha");write(f.root/"samples/b","beta");
    auto scan=f.scan();check(scan.ok&&scan.files==2&&scan.queued==2,"new files enumerated without hashing marker");
    check(f.index->entries().size() == 2 && f.index->entries().front().hash.empty() &&
              f.index->stats().hashed == 0,
          "new files browsable before worker starts");
    const auto epochNow=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const auto initialEntries = f.index->entries();
    const auto stored =
        initialEntries.empty() ? std::int64_t{0} : initialEntries.front().modifiedNs;
    check(stored>=epochNow-10000000000LL&&stored<=epochNow+10000000000LL,"timestamps share the Unix epoch across platforms");
    f.finish();
    check(f.index->stats().hashed == 2 && !f.index->entries().empty() &&
              f.index->entries().front().hash.size() == 64,
          "background worker hashes new rows");
    check(f.index->stats().lowPriority,"worker lowers its own OS priority");
    SQLite::Database observer(media::pathUtf8(f.dir.path()/"index.sqlite"));
    const auto dataVersion=observer.execAndGet("PRAGMA data_version").getInt64();
    scan=f.scan();f.index->waitIdle();
    check(observer.execAndGet("PRAGMA data_version").getInt64()==dataVersion,"unchanged scan performs no database writes");
    check(scan.ok&&scan.unchanged==2&&f.index->stats().hashed==2,"unchanged scan hashes nothing");
    check(f.index->scan(f.volume,"samples/.").ok,"normalized folder spelling is accepted");
    f.index->waitIdle();
    const auto originalEntries = f.index->entries();
    const auto before = originalEntries.empty() ? std::string{} : originalEntries.front().hash;
    const auto moved=f.dir.path()/"different-mount";fs::rename(f.root,moved);f.root=moved;
    check(provider.identify(moved,f.volume,error)&&f.volume.id==again.id,"moved mount preserves volume identity");
    scan=f.scan();f.index->waitIdle();check(scan.ok&&scan.unchanged==2&&f.index->stats().hashed==2,"moved mount rehashes nothing");
    check(!f.index->entries().empty() && f.index->entries().front().hash == before,
          "moving volume preserves content identity");
    auto a=f.root/"samples/a";const auto original=fs::last_write_time(a);
    fs::last_write_time(a,original+std::chrono::seconds(2));scan=f.scan();f.index->waitIdle();
    check(scan.unchanged==2&&f.index->stats().hashed==2,"exactly two seconds is unchanged");
    // FAT rounds odd seconds; four seconds is representable and outside the window.
    fs::last_write_time(a,original+std::chrono::seconds(4));scan=f.scan();f.index->waitIdle();
    check(scan.queued==1&&f.index->stats().hashed==3,"tolerance stays anchored to last hash time");
    write(a,"longer content");fs::last_write_time(a,original+std::chrono::seconds(4));scan=f.scan();f.index->waitIdle();
    check(scan.queued==1&&f.index->stats().hashed==4,"size change rehashes even with same time");
    check(!f.index->scan(f.volume,"..").ok,"scan cannot escape volume");
    fs::remove(a);scan=f.index->scan(f.volume,"samples");f.index->waitIdle();
    check(scan.ok && !f.index->entries().empty() && !f.index->entries().front().present,
          "successful scoped scan marks absent file missing");
    f.index->cancel();f.index.reset();f.index=library::Index::open(f.dir.path()/"index.sqlite",error);
    check(f.index!=nullptr,"index reopens independently of project schema");
    scan=f.scan();f.index->waitIdle();check(scan.queued==0&&f.index->stats().hashed==0,"restart uses persisted hashes");
}
void names() {
    Fixture f;
    const auto nfd=media::pathFromUtf8("cafe\xCC\x81.wav"),nfc=media::pathFromUtf8("caf\xC3\xA9.wav");
    write(f.root/nfd,"same");check(f.scan().ok,"NFD initial scan");f.finish();
    fs::rename(f.root/nfd,f.root/"temporary");fs::rename(f.root/"temporary",f.root/nfc);
    auto scan=f.scan();f.index->waitIdle();
    check(scan.ok&&scan.unchanged==1&&f.index->entries().size()==1&&f.index->stats().hashed==1,"NFD NFC rename is one unchanged file");
    check(!f.index->entries().empty() &&
              f.index->entries().front().relativePath == media::pathUtf8(nfc),
          "actual spelling retained for file IO");
    // Inject a probed-insensitive provider result independently of CI host OS.
    Fixture folded;folded.volume.caseSensitive=false;write(folded.root/"Kick","beat");folded.scan();folded.finish();
    fs::rename(folded.root/"Kick",folded.root/"temp");fs::rename(folded.root/"temp",folded.root/"KICK");
    scan=folded.scan();folded.index->waitIdle();check(scan.unchanged==1&&folded.index->stats().hashed==1,"case folded only when provider says insensitive");
    Fixture sensitive;
    if(sensitive.volume.caseSensitive) {
        write(sensitive.root/"a","lower");write(sensitive.root/"A","upper");sensitive.scan();sensitive.finish();
        check(sensitive.index->entries().size()==2,"case sensitive volume keeps different names");
    } else check(true,"case sensitive distinction skipped on insensitive test volume");
    Fixture collision;
    if(collision.volume.caseSensitive) {
        write(collision.root/nfd,"one");write(collision.root/nfc,"two");
        if(!fs::equivalent(collision.root/nfd,collision.root/nfc)) check(!collision.scan().ok&&collision.index->entries().empty(),"simultaneous canonical collision fails without mutation");
        else check(true,"filesystem already canonicalizes names");
    } else check(true,"filesystem already canonicalizes names");
}
void annotations() {
    Fixture a,b;write(a.root/"left","same bytes");write(b.root/"right","same bytes");a.scan();b.scan();a.finish();b.finish();
    const auto annotationEntries = a.index->entries();
    const auto hash = annotationEntries.empty() ? std::string{} : annotationEntries.front().hash;
    std::string error;
    const Json data={{"version",1},{"media",{{hash,{{"tags",{"warm"}},{"bpm",120},{"rating",4}}}}}};
    check(a.index->importJson(data.dump(),error),"metadata accepts content hash key");
    check(b.index->importJson(a.index->exportJson(),error),"metadata imports across paths");
    check(!b.index->entries().empty() && Json::parse(b.index->exportJson())
                                             .at("media")
                                             .contains(b.index->entries().front().hash),
          "import matches by hash not path");
    Json more={{"version",1},{"media",{{hash,{{"tags",{"soft","warm"}},{"rating",5}}}}}};
    check(b.index->importJson(more.dump(),error),"metadata merges");
    auto merged=Json::parse(b.index->exportJson())["media"][hash];
    check(merged["tags"].size()==2&&merged["bpm"]==120&&merged["rating"]==5,"merge unions tags preserves missing fields overwrites supplied scalar");
    const auto before=b.index->exportJson();more["media"]["not-a-hash"]={{"rating",1}};
    check(!b.index->importJson(more.dump(),error)&&b.index->exportJson()==before,"bad later key rolls back whole import");
    Json byPath={{"version",1},{"media",{{"right",{{"rating",1}}}}}};
    check(!b.index->importJson(byPath.dump(),error),"path keyed import refused");
    more=data;more["media"][hash]["rating"]=6;check(!b.index->importJson(more.dump(),error),"invalid rating refused");
    more=data;more["media"][hash]["bpm"]=0;check(!b.index->importJson(more.dump(),error),"invalid BPM refused");
    Fixture later;check(later.index->importJson(data.dump(),error),"hash metadata may arrive before its file");
}
void cancellation() {
    {
        Fixture idle(true);
        for (unsigned n=0;n<20;++n) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            idle.index->cancel(); idle.index->start();
        }
        idle.index->cancel();
        check(idle.index->stats().hashed==0,"idle worker cancellation and restart complete");
    }
    std::atomic<bool> entered=false;
    Fixture f(true,[&](const fs::path& path,std::stop_token stop){entered=true;while(!stop.stop_requested())std::this_thread::sleep_for(std::chrono::milliseconds(1));return media::blake3File(path,stop);});
    write(f.root/"big",std::string(1024,'x'));check(f.scan().ok,"cancellation setup");
    while(!entered.load())std::this_thread::yield();
    check(!f.index->entries().empty() && f.index->entries().front().hash.empty(),
          "browse remains available while hash runs");
    f.index->cancel();
    check(!f.index->entries().empty() && f.index->entries().front().hash.empty() &&
              f.index->stats().hashed == 0,
          "cancel leaves resumable pending row");
    std::string error;f.index.reset();f.index=library::Index::open(f.dir.path()/"index.sqlite",error);f.index->waitIdle();
    check(f.index->stats().hashed==1,"reopen resumes pending hashes");
    std::stop_source stop;stop.request_stop();check(media::blake3File(f.root/"big",stop.get_token()).error==media::HashError::cancelled,"chunk hasher honors stop token");
    Fixture changed(true,[](const fs::path& path,std::stop_token token){auto hash=media::blake3File(path,token);write(path,"changed size after read");return hash;});
    write(changed.root/"file","x");changed.scan();changed.index->waitIdle();
    check(!changed.index->entries().empty() && changed.index->entries().front().hash.empty() &&
              !changed.index->entries().front().error.empty(),
          "concurrent modification never publishes stale hash");
}
void foreignDatabases() {
    test::TempDirectory temp{"library","headers"};std::string error;
    auto file=temp.path()/"foreign.sqlite";
    {SQLite::Database db(media::pathUtf8(file),SQLite::OPEN_READWRITE|SQLite::OPEN_CREATE);db.exec("CREATE TABLE precious(value TEXT); INSERT INTO precious VALUES('keep');");}
    check(!library::Index::open(file,error),"populated id-zero database refused");
    {SQLite::Database db(media::pathUtf8(file));check(db.execAndGet("SELECT value FROM precious").getString()=="keep","foreign database untouched");}
    auto newer=temp.path()/"newer.sqlite";
    {SQLite::Database db(media::pathUtf8(newer),SQLite::OPEN_READWRITE|SQLite::OPEN_CREATE);db.exec("PRAGMA application_id=1094994252;PRAGMA user_version=2;");}
    check(!library::Index::open(newer,error),"future version refused");
    StoreError storeError;auto project=Store::createEmpty(temp.path()/"song.adi",storeError);project->close();project.reset();
    check(!library::Index::open(temp.path()/"song.adi",error),"adi project refused as library");
    check(!library::Index::open({},error),"empty caller path refused");
}
bool sameEntries(const std::vector<library::Entry> &a, const std::vector<library::Entry> &b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                      [](const auto &left, const auto &right) {
                          return std::tie(left.volumeId, left.relativePath, left.hash, left.error,
                                          left.size, left.modifiedNs, left.present) ==
                                 std::tie(right.volumeId, right.relativePath, right.hash,
                                          right.error, right.size, right.modifiedNs, right.present);
                      });
}
void scanCancellationAndProgress() {
    Fixture f;
    std::stop_source already;
    already.request_stop();
    unsigned notifications = 0;
    library::Volume absent;
    absent.root = f.root / "not-there";
    const auto precancelled =
        f.index->scan(absent, {}, already.get_token(), [&](const auto &) { ++notifications; });
    check(!precancelled.ok && precancelled.cancelled && precancelled.files == 0 &&
              precancelled.queued == 0 && !precancelled.error.empty(),
          "pre-cancelled scan refuses before volume validation or filesystem access");
    check(notifications == 0 && f.index->entries().empty(),
          "pre-cancelled scan emits no progress and creates no index rows");
    fs::create_directory(f.root / "inside");
    fs::create_directory(f.root / "outside");
    write(f.root / "inside/a", "alpha");
    write(f.root / "inside/b", "beta");
    write(f.root / "outside/c", "untouched");
    check(f.scan().ok, "seed a complete library before cancellation");
    f.finish();
    f.index->cancel();
    const auto before = f.index->entries();
    const auto hashed = f.index->stats().hashed;
    check(before.size() == 3 && hashed == 3, "baseline scan has three hashed entries");
    fs::remove(f.root / "inside/a");
    write(f.root / "inside/b", "changed beta bytes");
    for (unsigned i = 0; i < 64; ++i)
        write(f.root / "inside" / ("new-" + std::to_string(i)), "new bytes " + std::to_string(i));
    std::stop_source during;
    std::vector<library::ScanProgress> progress;
    bool unchangedDuringCallbacks = true;
    const auto interrupted =
        f.index->scan(f.volume, "inside", during.get_token(), [&](const auto &state) {
            progress.push_back(state);
            unchangedDuringCallbacks =
                unchangedDuringCallbacks && sameEntries(before, f.index->entries());
            if (state.phase == library::ScanProgress::Phase::Discovering && state.files == 2)
                during.request_stop();
        });
    check(!interrupted.ok && interrupted.cancelled && interrupted.files == 0 &&
              interrupted.unchanged == 0 && interrupted.queued == 0,
          "cancelling inside one folder reports no committed rows or hash jobs");
    check(!progress.empty() && progress.back().phase == library::ScanProgress::Phase::Discovering &&
              progress.back().files == 2,
          "discovery observes cancellation before reading another entry");
    check(unchangedDuringCallbacks && sameEntries(before, f.index->entries()),
          "progress can inspect the index without deadlock and cancelled discovery preserves all "
          "prior rows");
    f.finish();
    check(f.index->stats().hashed == hashed,
          "cancelled metadata scan queues no replacement or new hashes");
    f.index->cancel();
    progress.clear();
    bool monotonic = true;
    std::uint64_t seen = 0, files = 0;
    const auto complete = f.index->scan(f.volume, "inside", {}, [&](const auto &state) {
        monotonic = monotonic && state.visited >= seen && state.files >= files;
        seen = state.visited;
        files = state.files;
        progress.push_back(state);
        unchangedDuringCallbacks =
            unchangedDuringCallbacks && sameEntries(before, f.index->entries());
    });
    check(complete.ok && !complete.cancelled && complete.files == 65 && complete.queued == 65,
          "a later scoped scan publishes every file from the same folder");
    check(
        monotonic && !progress.empty() &&
            progress.front().phase == library::ScanProgress::Phase::Discovering &&
            progress.back().phase == library::ScanProgress::Phase::Publishing &&
            progress.back().files == 65,
        "progress advances through discovery and announces publication outside the database lock");
    check(unchangedDuringCallbacks,
          "no progress callback runs after the publication transaction begins");
    const auto after = f.index->entries();
    const auto a = std::find_if(after.begin(), after.end(),
                                [](const auto &entry) { return entry.relativePath == "inside/a"; });
    const auto c = std::find_if(after.begin(), after.end(), [](const auto &entry) {
        return entry.relativePath == "outside/c";
    });
    check(after.size() == 67 && a != after.end() && !a->present && c != after.end() && c->present &&
              !c->hash.empty(),
          "completed scoped scan marks the missing file and preserves the other folder");
    f.finish();
    check(f.index->stats().hashed == hashed + 65, "only the completed scan adds new hash work");
    f.index->cancel();
    const auto saved = f.index->entries();
    fs::remove(f.root / "inside/b");
    write(f.root / "inside/late", "late file");
    std::stop_source publish;
    const auto stoppedBeforePublish =
        f.index->scan(f.volume, "inside", publish.get_token(), [&](const auto &state) {
            if (state.phase == library::ScanProgress::Phase::Publishing)
                publish.request_stop();
        });
    check(!stoppedBeforePublish.ok && stoppedBeforePublish.cancelled &&
              stoppedBeforePublish.queued == 0 && sameEntries(saved, f.index->entries()),
          "a stop from the publication notification cannot mark a missing file or publish staged "
          "rows");
    const auto observerError = f.index->scan(
        f.volume, "inside", {}, [](const auto &) { throw std::runtime_error("observer refused"); });
    check(!observerError.ok && !observerError.cancelled &&
              observerError.error.find("observer refused") != std::string::npos &&
              sameEntries(saved, f.index->entries()),
          "observer exception is a named failure before database publication");
    const auto unknown = f.index->scan(f.volume, "inside", {}, [](const auto &) { throw 7; });
    check(!unknown.ok && !unknown.cancelled &&
              unknown.error.find("progress observer") != std::string::npos &&
              sameEntries(saved, f.index->entries()),
          "nonstandard observer exception cannot escape or mutate the index");
    check(f.index->scan(f.volume, "inside").ok,
          "existing two-argument scan remains available after cancellations and observer errors");
}

} // namespace
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    try {
        identityAndScan();
        names();
        annotations();
        cancellation();
        foreignDatabases();
        scanCancellationAndProgress();
    } catch (const std::exception &e) {
        check(false, e.what());
    }
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
