// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/library/index.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/store.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <thread>
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
    check(f.index->entries().size()==2&&f.index->entries()[0].hash.empty()&&f.index->stats().hashed==0,"new files browsable before worker starts");
    f.finish();check(f.index->stats().hashed==2&&f.index->entries()[0].hash.size()==64,"background worker hashes new rows");
    check(f.index->stats().lowPriority,"worker lowers its own OS priority");
    SQLite::Database observer(media::pathUtf8(f.dir.path()/"index.sqlite"));
    const auto dataVersion=observer.execAndGet("PRAGMA data_version").getInt64();
    scan=f.scan();f.index->waitIdle();
    check(observer.execAndGet("PRAGMA data_version").getInt64()==dataVersion,"unchanged scan performs no database writes");
    check(scan.ok&&scan.unchanged==2&&f.index->stats().hashed==2,"unchanged scan hashes nothing");
    check(f.index->scan(f.volume,"samples/.").ok,"normalized folder spelling is accepted");
    f.index->waitIdle();
    const auto before=f.index->entries()[0].hash;
    const auto moved=f.dir.path()/"different-mount";fs::rename(f.root,moved);f.root=moved;
    check(provider.identify(moved,f.volume,error)&&f.volume.id==again.id,"moved mount preserves volume identity");
    scan=f.scan();f.index->waitIdle();check(scan.ok&&scan.unchanged==2&&f.index->stats().hashed==2,"moved mount rehashes nothing");
    check(f.index->entries()[0].hash==before,"moving volume preserves content identity");
    auto a=f.root/"samples/a";const auto original=fs::last_write_time(a);
    fs::last_write_time(a,original+std::chrono::seconds(2));scan=f.scan();f.index->waitIdle();
    check(scan.unchanged==2&&f.index->stats().hashed==2,"exactly two seconds is unchanged");
    fs::last_write_time(a,original+std::chrono::seconds(3));scan=f.scan();f.index->waitIdle();
    check(scan.queued==1&&f.index->stats().hashed==3,"tolerance stays anchored to last hash time");
    write(a,"longer content");fs::last_write_time(a,original+std::chrono::seconds(3));scan=f.scan();f.index->waitIdle();
    check(scan.queued==1&&f.index->stats().hashed==4,"size change rehashes even with same time");
    check(!f.index->scan(f.volume,"..").ok,"scan cannot escape volume");
    fs::remove(a);scan=f.index->scan(f.volume,"samples");f.index->waitIdle();
    check(scan.ok&&!f.index->entries()[0].present,"successful scoped scan marks absent file missing");
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
    check(f.index->entries()[0].relativePath==media::pathUtf8(nfc),"actual spelling retained for file IO");
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
    const auto hash=a.index->entries()[0].hash;std::string error;
    const Json data={{"version",1},{"media",{{hash,{{"tags",{"warm"}},{"bpm",120},{"rating",4}}}}}};
    check(a.index->importJson(data.dump(),error),"metadata accepts content hash key");
    check(b.index->importJson(a.index->exportJson(),error),"metadata imports across paths");
    check(Json::parse(b.index->exportJson()).at("media").contains(b.index->entries()[0].hash),"import matches by hash not path");
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
    check(f.index->entries()[0].hash.empty(),"browse remains available while hash runs");
    f.index->cancel();check(f.index->entries()[0].hash.empty()&&f.index->stats().hashed==0,"cancel leaves resumable pending row");
    std::string error;f.index.reset();f.index=library::Index::open(f.dir.path()/"index.sqlite",error);f.index->waitIdle();
    check(f.index->stats().hashed==1,"reopen resumes pending hashes");
    std::stop_source stop;stop.request_stop();check(media::blake3File(f.root/"big",stop.get_token()).error==media::HashError::cancelled,"chunk hasher honors stop token");
    Fixture changed(true,[](const fs::path& path,std::stop_token stop){auto hash=media::blake3File(path,stop);write(path,"changed size after read");return hash;});
    write(changed.root/"file","x");changed.scan();changed.index->waitIdle();
    check(changed.index->entries()[0].hash.empty()&&!changed.index->entries()[0].error.empty(),"concurrent modification never publishes stale hash");
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
    StoreError storeError;auto project=Store::create(temp.path()/"song.adi",storeError);project->close();project.reset();
    check(!library::Index::open(temp.path()/"song.adi",error),"adi project refused as library");
    check(!library::Index::open({},error),"empty caller path refused");
}
}
int main(){std::setvbuf(stdout,nullptr,_IONBF,0);try{identityAndScan();names();annotations();cancellation();foreignDatabases();}catch(const std::exception& e){check(false,e.what());}std::printf("%s -- %d checks, %d failure(s)\n",failures?"FAIL":"PASS",checks,failures);return failures?1:0;}
