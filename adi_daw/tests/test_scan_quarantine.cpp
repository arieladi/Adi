// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/plugin_registry.hpp"
#include "adi/audio/io_audit.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <new>
#include <thread>
namespace {std::atomic<unsigned> allocations{0};}
void* operator new(std::size_t n){if(adi::audio::onAudioThread)++allocations;if(void* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return operator new(n);}
void operator delete(void* p) noexcept{std::free(p);}
void operator delete[](void* p) noexcept{operator delete(p);}
void operator delete(void* p,std::size_t) noexcept{operator delete(p);}
void operator delete[](void* p,std::size_t) noexcept{operator delete(p);}
namespace {
using namespace adi;namespace fs=std::filesystem;
int checks=0,failures=0;
void check(bool ok,const char* label){++checks;if(!ok){++failures;std::printf("FAIL: %s\n",label);}}
std::string utf8(const fs::path& path){const auto bytes=path.u8string();return {bytes.begin(),bytes.end()};}
struct Fixture {
    test::TempDirectory temp{"quarantine","registry"};
    fs::path file=temp.path()/"registry.sqlite";
    std::string error,path=utf8(temp.path()/fs::path(std::u8string(u8"plug-in caf\u00e9.clap")));
    std::unique_ptr<PluginRegistry> registry=PluginRegistry::open(file,error);
    Fixture(){if(!registry)throw std::runtime_error(error);}
};
void lifecycle() {
    Fixture f;auto& reg=*f.registry;
    auto ticket=reg.beginScan("clap",f.path,100,f.error);
    check(ticket && ticket->format()=="clap" && ticket->path()==f.path,"begin returns owned exact format/path only after its pending marker commits");if(!ticket)return;
    auto other=PluginRegistry::open(f.file,f.error);if(!other)throw std::runtime_error(f.error);
    auto rows=other->scanBlocks(f.error);
    check(rows && rows->size()==1 && rows->front().format=="clap" && rows->front().path==f.path && rows->front().state==ScanBlockState::Pending && rows->front().updatedUtc==100,"another suite connection immediately sees committed exact pending identity and state");
    check(!other->beginScan("clap",f.path,101,f.error),"a pending key cannot start twice across applications");
    check(!other->finishScan(*ticket,ScanDisposition::Succeeded,"",101,f.error),"a different registry connection cannot complete an owner's ticket");
    check(reg.finishScan(*ticket,ScanDisposition::Succeeded,"",102,f.error) && reg.scanBlocks(f.error)->empty(),"successful scan removes exactly its pending marker");
    check(!reg.finishScan(*ticket,ScanDisposition::Succeeded,"",103,f.error),"duplicate completion refuses");
    auto next=reg.beginScan("clap",f.path,104,f.error);
    check(next && !reg.finishScan(*ticket,ScanDisposition::Succeeded,"",105,f.error),"deleted identities are never reused for replacement attempts");if(!next)return;
    check(reg.finishScan(*next,ScanDisposition::Failed,"signal 11\nfactory crash",106,f.error),"failed scan persists a named diagnostic");
    rows=reg.scanBlocks(f.error);
    check(rows && rows->size()==1 && rows->front().state==ScanBlockState::Blocked && rows->front().reason=="signal 11\nfactory crash" && rows->front().updatedUtc==106,"blocklist exposes exact failure and explicit timestamp");
    check(!reg.beginScan("clap",f.path,107,f.error),"blocked scan never starts automatically");
    check(reg.unblockScan("clap",f.path,f.error) && reg.unblockScan("clap",f.path,f.error),"explicit re-enable is idempotent");
    auto abandoned=reg.beginScan("clap",f.path,108,f.error);
    check(abandoned && reg.finishScan(*abandoned,ScanDisposition::Abandoned,"",109,f.error) && reg.scanBlocks(f.error)->empty(),"cancellation/helper launch abandonment releases only its own pending marker");
    reg.blockScan("clap",f.path,"CLAP only",109,f.error);
    auto independent=reg.beginScan("vst3",f.path,109,f.error);
    check(independent && reg.finishScan(*independent,ScanDisposition::Succeeded,"",109,f.error) && reg.scanBlocks(f.error)->size()==1,"formats have independent quarantine even at the exact same path");
    reg.unblockScan("clap",f.path,f.error);
    auto active=reg.beginScan("clap",f.path,110,f.error);if(!active)return;
    check(other->blockScan("clap",f.path,"disabled by user",111,f.error) && !reg.finishScan(*active,ScanDisposition::Succeeded,"",112,f.error),"explicit user block invalidates in-flight completion");
    rows=reg.scanBlocks(f.error);
    check(rows && rows->size()==1 && rows->front().reason=="disabled by user","late completion cannot clear a newer user block");
    reg.unblockScan("clap",f.path,f.error);
    auto replacement=reg.beginScan("clap",f.path,113,f.error);
    check(replacement && !reg.finishScan(*active,ScanDisposition::Failed,"old crash",114,f.error),"old failures also cannot poison a re-enabled replacement attempt");
    check(replacement && reg.finishScan(*replacement,ScanDisposition::Succeeded,"",115,f.error),"replacement remains completable after stale refusal");
    auto pending=reg.beginScan("vst3",f.path,116,f.error);if(!pending)return;
    f.registry.reset();f.registry=PluginRegistry::open(f.file,f.error);if(!f.registry)throw std::runtime_error(f.error);
    check(!f.registry->beginScan("vst3",f.path,117,f.error) && !f.registry->finishScan(*pending,ScanDisposition::Succeeded,"",117,f.error),"interrupted pending survives reopen and expired tickets cannot complete there");
    check(f.registry->unblockScan("vst3",f.path,f.error) && f.registry->scanBlocks(f.error)->empty(),"only explicit re-enable removes interrupted pending after reopen");
}
void validationAndCallbacks() {
    Fixture f;auto& reg=*f.registry;
    const auto ticket=reg.beginScan("clap",f.path,1,f.error);if(!ticket)throw std::runtime_error(f.error);
    bool refused=true;
    for(const auto& format:std::array<std::string,4>{"","CLAP","au","internal"})
        refused=refused && !reg.beginScan(format,f.path,2,f.error) && !reg.blockScan(format,f.path,"bad",2,f.error) && !reg.unblockScan(format,f.path,f.error);
    for(const auto& path:std::array<std::string,5>{"","relative.clap",std::string("/a\0b",4),"/bad\xff", "/"+std::string(65536,'x')})
        refused=refused && !reg.beginScan("clap",path,2,f.error) && !reg.blockScan("clap",path,"bad",2,f.error) && !reg.unblockScan("clap",path,f.error);
    check(refused,"invalid formats/relative/NUL/malformed UTF-8/oversized paths refuse every write");
    bool reasons=true;
    for(const auto& reason:std::array<std::string,4>{"",std::string("a\0b",3),"bad\xff",std::string(4097,'x')})
        reasons=reasons && !reg.blockScan("clap",f.path,reason,2,f.error) && !reg.finishScan(*ticket,ScanDisposition::Failed,reason,2,f.error);
    check(reasons,"failure diagnostics must be nonempty bounded UTF-8 without NUL");
    check(!reg.beginScan("vst3",f.path,-1,f.error) && !reg.blockScan("clap",f.path,"negative",-1,f.error) && !reg.finishScan(*ticket,ScanDisposition::Succeeded,"",-1,f.error),"negative explicit timestamps refuse without changing pending");
    check(!reg.finishScan(*ticket,ScanDisposition::Succeeded,"ignored",2,f.error) && !reg.finishScan(*ticket,static_cast<ScanDisposition>(99),"",2,f.error),"unknown completion values and extraneous success diagnostics refuse");
    check(!reg.scanBlocks(f.error,0) && !reg.scanBlocks(f.error,PluginRegistry::kMaximumScanBlocks+1),"read bounds refuse zero/oversized requests");
    // Every argument is built before the callback scope: MSVC's Debug CRT allocates an
    // iterator proxy whenever a std::string is constructed, even an empty one.
    const std::string format="clap",reason="crash",uid="known",none;f.error="untouched";
    bool callback=false;
    {audio::CallbackScope scope;
        callback=!PluginRegistry::open(f.file,f.error) && !reg.route(format,uid) &&
            !reg.remember(format,uid,engine::RouteChoice::Plain,2,f.error) &&
            !reg.beginScan(format,f.path,2,f.error) && !reg.finishScan(*ticket,ScanDisposition::Succeeded,none,2,f.error) &&
            !reg.blockScan(format,f.path,reason,2,f.error) && !reg.unblockScan(format,f.path,f.error) && !reg.scanBlocks(f.error);
    }
    check(callback && f.error=="untouched" && allocations==0 && audio::callbackFileIo==0,"all registry callback entry points refuse before allocating or mutating diagnostics");
    auto rows=reg.scanBlocks(f.error);
    check(rows && rows->size()==1 && rows->front().state==ScanBlockState::Pending,"every invalid/callback request leaves original pending data unchanged");
    check(reg.finishScan(*ticket,ScanDisposition::Failed,std::string(4096,'x'),2,f.error),"diagnostic exactly at byte limit remains valid");
    const auto root=utf8(f.temp.path().root_path());
    const auto longPath=root+std::string(65536-root.size(),'p');
    check(reg.blockScan("clap",longPath,"bounded path",3,f.error),"absolute path key exactly at byte limit needs no filesystem lookup");
}
void migrationAndIsolation() {
    test::TempDirectory temp("quarantine","migration");const auto file=temp.path()/"legacy.sqlite";
    {
        SQLite::Database db(utf8(file),SQLite::OPEN_READWRITE|SQLite::OPEN_CREATE);
        db.exec("PRAGMA application_id="+std::to_string(PluginRegistry::kApplicationId));db.exec("PRAGMA user_version=1");
        db.exec("CREATE TABLE plugin_routes(format TEXT,uid TEXT,route TEXT,updated_utc INTEGER,PRIMARY KEY(format,uid)) STRICT,WITHOUT ROWID");
        db.exec("INSERT INTO plugin_routes VALUES('clap','legacy','mpe_midi',42)");
    }
    std::string error;auto reg=PluginRegistry::open(file,error);
    check(reg && reg->route("clap","legacy")==engine::RouteChoice::MpeMidi,"version-one registry migrates without losing route preferences");if(!reg)return;
    const auto path=utf8(temp.path()/"legacy.clap");
    check(reg->blockScan("clap",path,"migrated",50,error),"migrated registry admits quarantine independently of routes");
    {
        SQLite::Database db(utf8(file),SQLite::OPEN_READONLY);
        check(db.execAndGet("PRAGMA user_version").getInt()==2 && db.execAndGet("SELECT updated_utc FROM plugin_routes").getInt64()==42,"migration bumps only application schema version and preserves authored route time");
    }
    const auto poisoned=temp.path()/"bad-legacy.sqlite";
    {
        SQLite::Database db(utf8(poisoned),SQLite::OPEN_READWRITE|SQLite::OPEN_CREATE);
        db.exec("PRAGMA application_id="+std::to_string(PluginRegistry::kApplicationId));db.exec("PRAGMA user_version=1");
        db.exec("CREATE TABLE plugin_scan_blocks(wrong_column TEXT) STRICT");
    }
    check(!PluginRegistry::open(poisoned,error),"incompatible pre-existing scan table refuses migration");
    {
        SQLite::Database db(utf8(poisoned),SQLite::OPEN_READONLY);
        check(db.execAndGet("PRAGMA user_version").getInt()==1 && db.execAndGet("SELECT count(*) FROM sqlite_master WHERE name='plugin_routes'").getInt()==0,"failed migration rolls back added tables and version atomically");
    }
    auto foreign=PluginRegistry::open(temp.path()/"other.sqlite",error);
    const auto ticket=reg->beginScan("vst3",path,51,error);
    const auto otherTicket=foreign?foreign->beginScan("vst3",path,51,error):std::nullopt;
    check(ticket && otherTicket && !foreign->finishScan(*ticket,ScanDisposition::Succeeded,"",52,error) && foreign->finishScan(*otherTicket,ScanDisposition::Succeeded,"",52,error),"same numeric identity/path in another database cannot accept a foreign ticket");
}
void atomicityAndBounds() {
    Fixture f;auto& reg=*f.registry;SQLite::Database db(utf8(f.file),SQLite::OPEN_READWRITE);
    db.exec("CREATE TRIGGER refuse_scan BEFORE INSERT ON plugin_scan_blocks BEGIN SELECT RAISE(ABORT,'scan write refused');END");
    check(!reg.beginScan("clap",f.path,1,f.error) && reg.scanBlocks(f.error)->empty(),"failed marker write returns no ticket or partial quarantine");
    db.exec("DROP TRIGGER refuse_scan");auto ticket=reg.beginScan("clap",f.path,2,f.error);if(!ticket)return;
    db.exec("CREATE TRIGGER refuse_finish BEFORE UPDATE ON plugin_scan_blocks BEGIN SELECT RAISE(ABORT,'finish refused');END");
    check(!reg.finishScan(*ticket,ScanDisposition::Failed,"crash",3,f.error) && reg.scanBlocks(f.error)->front().state==ScanBlockState::Pending,"failed completion retains pending quarantine");
    db.exec("DROP TRIGGER refuse_finish");
    db.exec("CREATE TRIGGER refuse_delete BEFORE DELETE ON plugin_scan_blocks BEGIN SELECT RAISE(ABORT,'delete refused');END");
    check(!reg.finishScan(*ticket,ScanDisposition::Succeeded,"",3,f.error) && !reg.finishScan(*ticket,ScanDisposition::Abandoned,"",3,f.error) && reg.scanBlocks(f.error)->size()==1,"failed successful/abandoned cleanup retains quarantine and refuses completion");
    db.exec("DROP TRIGGER refuse_delete");reg.unblockScan("clap",f.path,f.error);
    {
        SQLite::Statement fill(db,"WITH RECURSIVE n(i) AS (VALUES(1) UNION ALL SELECT i+1 FROM n WHERE i<65536) INSERT INTO plugin_scan_blocks(format,path,state,reason,updated_utc) SELECT 'clap',?||'/'||i,'blocked','capacity',1 FROM n");fill.bind(1,f.path);fill.exec();
    }
    check(!reg.beginScan("vst3",f.path,4,f.error) && !reg.blockScan("vst3",f.path,"full",4,f.error),"new markers/manual blocks refuse at exact shared capacity");
    check(reg.blockScan("clap",f.path+"/1","still disabled",5,f.error),"existing blocks can be updated at capacity");
    check(!reg.scanBlocks(f.error),"default row bound refuses whole list instead of silently truncating");
    auto all=reg.scanBlocks(f.error,65536);
    check(all && all->size()==65536 && all->front().path==f.path+"/1" && all->front().reason=="still disabled","full bounded list is sorted and retains exact updates");all.reset();
    db.exec("PRAGMA ignore_check_constraints=ON");db.exec("UPDATE plugin_scan_blocks SET state='unknown' WHERE path=(SELECT max(path) FROM plugin_scan_blocks)");
    check(!reg.scanBlocks(f.error,65536),"invalid later stored row refuses a whole otherwise valid list");
    db.exec("DELETE FROM plugin_scan_blocks");
    {
        SQLite::Statement fill(db,"WITH RECURSIVE n(i) AS (VALUES(1) UNION ALL SELECT i+1 FROM n WHERE i<300) INSERT INTO plugin_scan_blocks(format,path,state,reason,updated_utc) SELECT 'clap',?||'/'||i,'blocked',?,1 FROM n");
        fill.bind(1,utf8(f.temp.path().root_path())+std::string(60000,'b'));fill.bind(2,std::string(4096,'r'));fill.exec();
    }
    check(!reg.scanBlocks(f.error,65536) && f.error.find("text budget")!=f.error.npos,"aggregate read budget refuses repeated large authored text before returning a prefix");
}
void concurrentOwners() {
    Fixture f;std::array<std::unique_ptr<PluginRegistry>,4> owners;
    for(auto& owner:owners){owner=PluginRegistry::open(f.file,f.error);if(!owner)throw std::runtime_error(f.error);}
    std::array<std::optional<ScanTicket>,4> tickets;std::array<std::thread,4> threads;
    for(std::size_t i=0;i<threads.size();++i)threads[i]=std::thread([&,i]{std::string error;tickets[i]=owners[i]->beginScan("clap",f.path,100,error);});
    for(auto& thread:threads)thread.join();
    unsigned winners=0;for(const auto& ticket:tickets)if(ticket)++winners;
    check(winners==1,"concurrent suite connections admit exactly one pending ticket for a path");
    auto rows=f.registry->scanBlocks(f.error);check(rows && rows->size()==1 && rows->front().state==ScanBlockState::Pending,"concurrent refused attempts cannot overwrite the winning marker");
}
} // namespace
int main(int argc,char** argv) {
    // External Linux recovery review uses _Exit to preserve a committed WAL
    // marker while bypassing every registry/database destructor.
    if(argc==3 && std::string(argv[1])=="--leave-pending") {
        std::string error;auto reg=adi::PluginRegistry::open(fs::path(std::u8string(argv[2],argv[2]+std::char_traits<char>::length(argv[2]))),error);
        if(!reg || !reg->beginScan("clap","/controlled/recovery.clap",123,error))return 2;
        std::_Exit(0);
    }
    if(argc==3 && std::string(argv[1])=="--recover-pending") {
        std::string error;auto reg=adi::PluginRegistry::open(fs::path(std::u8string(argv[2],argv[2]+std::char_traits<char>::length(argv[2]))),error);
        if(!reg)return 2;
        const auto rows=reg->scanBlocks(error);
        if(!rows || rows->size()!=1 || rows->front().state!=ScanBlockState::Pending || rows->front().updatedUtc!=123 || reg->beginScan("clap","/controlled/recovery.clap",124,error))return 4;
        if(!reg->unblockScan("clap","/controlled/recovery.clap",error))return 5;
        const auto ticket=reg->beginScan("clap","/controlled/recovery.clap",125,error);
        if(!ticket || !reg->finishScan(*ticket,ScanDisposition::Succeeded,"",126,error))return 6;
        const auto empty=reg->scanBlocks(error);
        if(!empty || !empty->empty())return 7;
        std::printf("abrupt-exit quarantine, blocked reopen, explicit re-enable and new completion PASS\n");return 0;
    }
    if(argc!=1)return 3;
    try{lifecycle();validationAndCallbacks();migrationAndIsolation();atomicityAndBounds();concurrentOwners();}
    catch(const std::exception& error){++failures;std::printf("EXCEPTION: %s\n",error.what());}
    std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;
}
