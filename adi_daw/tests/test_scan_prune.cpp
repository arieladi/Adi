// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/scan_prune.hpp"
#include "adi/plugin_registry.hpp"
#include "adi/audio/io_audit.hpp"
#include "temp_directory.hpp"
#include "legacy_plugin_registry.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#endif

namespace {
using namespace adi;using namespace adi::scan;namespace fs=std::filesystem;
int checks=0,failures=0;
void check(bool ok,const char* name){++checks;if(!ok){++failures;std::printf("FAIL: %s\n",name);}}
std::string utf8(const fs::path& p){const auto u=p.u8string();return {u.begin(),u.end()};}
void write(const fs::path& p){std::ofstream out(p);out<<"owned module fixture";if(!out)throw std::runtime_error("fixture write");}
struct Fixture {
    test::TempDirectory temp{"scan","prune"};
    fs::path root=fs::canonical(temp.path()),clap=root/"CLAP",vst3=root/"VST3",file=root/"registry.sqlite";
    std::string error;std::unique_ptr<PluginRegistry> registry;
    Fixture(){fs::create_directory(clap);fs::create_directory(vst3);registry=PluginRegistry::open(file,error);if(!registry)throw std::runtime_error(error);}
    ScanPruneRequest request()const {
        ScanPruneRequest request;request.roots={{"clap",clap},{"vst3",vst3}};request.discoveryComplete=true;
        const auto bound=bindScanPruneRoots(request);if(!bound)throw std::runtime_error(bound.error);return request;
    }
    void success(const std::string& format,const fs::path& path) {
        const auto ticket=registry->beginScan(format,utf8(path),1,error);
        if(!ticket || !registry->finishCachedScan(*ticket,std::string(64,'a'),"{}",2,error))throw std::runtime_error(error);
    }
    void rejected(const fs::path& path,bool twice=false) {
        for(unsigned n=0;n<(twice?2U:1U);++n) {
            const auto admitted=registry->beginStampedScan("clap",utf8(path),std::string(64,'a'),3+n*2,error);
            if(!admitted || !admitted->ticket || !registry->finishFailedScan(*admitted->ticket,std::string(64,'a'),
                twice?ScanFailureKind::Crashed:ScanFailureKind::Rejected,"owned failure",4+n*2,error))throw std::runtime_error(error);
        }
    }
    std::int64_t count(const char* table)const {
        SQLite::Database read(utf8(file),SQLite::OPEN_READONLY);
        return read.execAndGet(std::string("SELECT count(*) FROM ")+table).getInt64();
    }
    bool cached(const std::string& format,const fs::path& path)const {
        SQLite::Database read(utf8(file),SQLite::OPEN_READONLY);
        SQLite::Statement q(read,"SELECT 1 FROM plugin_scan_cache WHERE format=? AND path=?");
        q.bind(1,format);q.bind(2,utf8(path));return q.executeStep();
    }
};
void staleAndProtected() {
    Fixture f;const auto live=f.clap/"live.clap",dead=f.clap/"dead.clap",vst=f.vst3/"dead.vst3";
    write(live);write(dead);write(vst);f.success("clap",live);f.success("clap",dead);f.success("vst3",vst);
    fs::remove(dead);fs::remove(vst);
    const auto reject=f.clap/"rejected.clap",automatic=f.clap/"automatic.clap";
    f.rejected(reject);f.rejected(automatic,true);
    const auto manual=f.clap/"manual.clap",pending=f.clap/"pending.clap";
    f.success("clap",manual);f.success("clap",pending);
    if(!f.registry->blockScan("clap",utf8(manual),"manual",9,f.error) ||
       !f.registry->beginScan("clap",utf8(pending),10,f.error))throw std::runtime_error(f.error);
    const auto outside=f.root/"outside.clap",wrongFormat=f.clap/"wrong.vst3";
    f.success("clap",outside);f.success("vst3",wrongFormat);
    const auto optional=f.root/"optional";fs::create_directory(optional);const auto optionalModule=optional/"absent.clap";
    f.success("clap",optionalModule);fs::remove(optional);
    auto request=f.request();request.roots.push_back({"clap",optional});
    if(!bindScanPruneRoots(request))throw std::runtime_error("bind optional root");
    const auto result=pruneMissingScans(f.file,request);
    check(result && result.cacheRowsRemoved==2 && result.failureRowsRemoved==2 && result.automaticBlocksRemoved==1,
          "one completed two-format pass removes only absent cache/failure metadata and its owned automatic block");
    check(result.rootsWalked==2 && result.missingRootsSkipped==1,"optional missing root does not block healthy root cleanup");
    check(f.cached("clap",live) && f.cached("clap",manual) && f.cached("clap",pending) &&
          f.cached("clap",outside) && f.cached("vst3",wrongFormat) && f.cached("clap",optionalModule),
          "existing sibling, manual/pending metadata, out-of-scope format/path and missing-root rows survive");
    const auto blocks=f.registry->scanBlocks(f.error);
    check(blocks && blocks->size()==2 && f.count("plugin_scan_failures")==0,"manual and pending blocks stay intact after automatic ownership cleanup");
    check(fs::exists(live),"metadata pruning never deletes a module file");
}
void noWriteAndPartial() {
    Fixture f;const auto a=f.clap/"a.clap",b=f.vst3/"b.vst3";write(a);write(b);f.success("clap",a);f.success("vst3",b);
    SQLite::Database other(utf8(f.file),SQLite::OPEN_READWRITE);other.exec("BEGIN IMMEDIATE");
    const auto warm=pruneMissingScans(f.file,f.request());other.exec("ROLLBACK");
    check(warm && warm.cacheRowsRemoved==0,"unchanged warm inventory never requests a writer while another connection owns one");
    ScanPruneRequest none;none.discoveryComplete=true;
    check(pruneMissingScans({},none).status==ScanPruneStatus::Incomplete,"unbound pruning refuses without registry I/O");
    if(!bindScanPruneRoots(none))throw std::runtime_error("bind empty roots");
    check(static_cast<bool>(pruneMissingScans({},none)),"empty roots return before even validating/opening the registry");
    none.roots={{"clap",f.root/"not-installed"}};
    if(!bindScanPruneRoots(none))throw std::runtime_error("bind absent roots");
    const auto missing=pruneMissingScans({},none);
    check(missing && missing.missingRootsSkipped==1,"only absent optional roots also return before registry I/O");
    fs::remove(a);fs::remove(b);
    auto request=f.request();request.discoveryComplete=false;
    check(pruneMissingScans(f.file,request).status==ScanPruneStatus::Incomplete && f.count("plugin_scan_cache")==2,
          "partial startup preserves both format inventories");
    std::atomic<bool> cancel{true};request.discoveryComplete=true;request.cancel=&cancel;
    check(pruneMissingScans(f.file,request).status==ScanPruneStatus::Cancelled && f.count("plugin_scan_cache")==2,
          "cancelled startup preserves both format inventories");
    request.cancel=nullptr;const auto notDirectory=f.root/"not-a-directory";write(notDirectory);request.roots.push_back({"clap",notDirectory});
    check(bindScanPruneRoots(request).status==ScanPruneStatus::Incomplete,"inaccessible traversal binding refuses before discovery");
    const auto partial=pruneMissingScans(f.file,request);
    check(partial.status==ScanPruneStatus::Incomplete && !partial.error.empty() && f.count("plugin_scan_cache")==2,
          "an invalid traversal root fails the entire preflight before deleting from either format");
    ScanPruneResult callback;
    {audio::CallbackScope scope;callback=pruneMissingScans(f.file,request);}
    check(callback.status==ScanPruneStatus::CallbackRefused && f.count("plugin_scan_cache")==2,"callback pruning refuses without mutation");
}
struct Action {
    unsigned calls=0;std::atomic<bool> cancel{false};bool recreate=false,throwLater=false;
    static void run(ScanPrunePhase phase,const fs::path& path,void* context) {
        auto& self=*static_cast<Action*>(context);
        if(phase!=ScanPrunePhase::BeforeDelete)return;
        ++self.calls;
        if(self.recreate){write(path);return;}
        if(self.calls==2) {
            if(self.throwLater)throw std::runtime_error("owned checkpoint failure");
            self.cancel=true;
        }
    }
};
void transactionalRecheck() {
    for(const bool throws:{false,true}) {
        Fixture f;f.success("clap",f.clap/"a.clap");f.success("vst3",f.vst3/"b.vst3");
        Action action;action.throwLater=throws;auto request=f.request();request.cancel=&action.cancel;
        request.checkpoint=Action::run;request.context=&action;
        const auto result=pruneMissingScans(f.file,request);
        check(action.calls==2 && result.status==(throws?ScanPruneStatus::Failed:ScanPruneStatus::Cancelled) &&
              result.cacheRowsRemoved==0 && f.count("plugin_scan_cache")==2,
              "late cancellation or owner failure rolls back earlier deletion across both formats");
    }
    Fixture f;const auto path=f.clap/"returns.clap";f.success("clap",path);
    Action action;action.recreate=true;auto request=f.request();request.checkpoint=Action::run;request.context=&action;
    const auto appeared=pruneMissingScans(f.file,request);
    check(appeared && action.calls==1 && appeared.cacheRowsRemoved==0 && f.cached("clap",path) && fs::exists(path),
          "a module reappearing after preflight is statted again and retains its success row");
}
std::string bytes(const fs::path& path){std::ifstream in(path,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
void legacyNoMigration() {
    Fixture f;
    for(const int version:{1,2}) {
        const auto file=f.root/("legacy"+std::to_string(version)+".sqlite");test::createLegacyPluginRegistry(file,version);
        const auto before=bytes(file);const auto result=pruneMissingScans(file,f.request());
        check(result && result.legacySkipped && result.cacheRowsRemoved==0 && bytes(file)==before,
              "genuine legacy registry with empty existing roots is preserved byte-for-byte without migration");
        SQLite::Database db(utf8(file),SQLite::OPEN_READWRITE);
        db.exec("CREATE TRIGGER unexpected AFTER INSERT ON plugin_routes BEGIN SELECT 1; END");
        const auto refused=pruneMissingScans(file,f.request());
        check(refused.status==ScanPruneStatus::Failed && !refused.legacySkipped,
              "legacy no-op reuses the strict catalogue schema fence instead of trusting a version number");
    }
    const auto missing=f.clap/"old.clap";f.success("clap",missing);f.registry.reset();
    {
        SQLite::Database db(utf8(f.file),SQLite::OPEN_READWRITE);
        db.exec("DROP TRIGGER scan_failure_block_update; DROP TRIGGER scan_failure_block_insert; "
                "DROP TRIGGER scan_failure_block_delete; DROP TRIGGER scan_failure_success_insert; DROP TABLE plugin_scan_failures");
    }
    const auto before=bytes(f.file);const auto early=pruneMissingScans(f.file,f.request());
    check(early && early.legacySkipped && early.cacheRowsRemoved==0 && f.cached("clap",missing) && bytes(f.file)==before,
          "early success-only v3 stays readable and unpruned until explicit owner migration");
    {
        SQLite::Database db(utf8(f.file),SQLite::OPEN_READWRITE);db.exec("CREATE VIEW plugin_scan_failures AS SELECT format,path FROM plugin_scan_cache WHERE 0");
    }
    check(pruneMissingScans(f.file,f.request()).status==ScanPruneStatus::Failed && f.cached("clap",missing),
          "unexpected failure view is not mistaken for a genuine early-v3 no-op");
}
#if defined(_WIN32)
bool junction(const fs::path& link,const fs::path& target) {
    const auto print=fs::canonical(target).wstring(),substitute=L"\\??\\"+print;
    struct Data {DWORD tag=IO_REPARSE_TAG_MOUNT_POINT;WORD bytes=0,reserved=0;
        WORD substituteOffset=0,substituteBytes=0,printOffset=0,printBytes=0;wchar_t names[2048]{};} data;
    static_assert(offsetof(Data,names)==16);
    if(substitute.size()+print.size()+2>=2048)return false;
    data.substituteBytes=static_cast<WORD>(substitute.size()*sizeof(wchar_t));data.printOffset=static_cast<WORD>(data.substituteBytes+sizeof(wchar_t));
    data.printBytes=static_cast<WORD>(print.size()*sizeof(wchar_t));data.bytes=static_cast<WORD>(8+data.printOffset+data.printBytes+sizeof(wchar_t));
    std::copy(substitute.begin(),substitute.end(),data.names);std::copy(print.begin(),print.end(),data.names+substitute.size()+1);
    fs::create_directory(link);const auto handle=CreateFileW(link.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr);
    if(handle==INVALID_HANDLE_VALUE)return false;
    DWORD returned=0;
    const bool ok=DeviceIoControl(handle,FSCTL_SET_REPARSE_POINT,&data,static_cast<DWORD>(8+data.bytes),nullptr,0,&returned,nullptr)!=FALSE;
    CloseHandle(handle);return ok;
}
void movedAliasParent() {
    Fixture f;const auto parent=f.clap/"old",outside=f.root/"different";fs::create_directory(parent);fs::create_directory(outside);
    const auto key=parent/"gone.clap";f.success("clap",key);fs::remove(parent);
    if(!junction(parent,outside))throw std::runtime_error("owned junction creation failed");
    const auto result=pruneMissingScans(f.file,f.request());
    check(result && result.unverifiableKeysSkipped==1 && result.cacheRowsRemoved==0 && f.cached("clap",key),
          "a canonical key whose parent now backs out through a junction stays unowned and unpruned");
    fs::remove(parent); // remove the owned junction itself before fixture cleanup
}
void danglingAlias() {
    Fixture f;const auto target=f.clap/"gone-target",alias=f.clap/"dangling.clap",other=f.clap/"unrelated.clap";
    fs::create_directory(target);f.success("clap",target);f.success("clap",other);
    if(!junction(alias,target))throw std::runtime_error("owned dangling junction setup failed");
    fs::remove(target);
    const auto result=pruneMissingScans(f.file,f.request());
    if(!result || result.cacheRowsRemoved!=1 || !f.cached("clap",target) || f.cached("clap",other))
        std::printf("dangling diagnostic: status=%d removed=%llu target=%d other=%d error=%s\n",static_cast<int>(result.status),
            static_cast<unsigned long long>(result.cacheRowsRemoved),f.cached("clap",target)?1:0,f.cached("clap",other)?1:0,result.error.c_str());
    check(result && result.cacheRowsRemoved==1 && f.cached("clap",target) && !f.cached("clap",other),
          "confirmed dangling candidate alias preserves its canonical target but does not prevent unrelated cleanup");
    fs::remove(alias);
}
void rootRebind() {
    Fixture f;const auto a=f.root/"A",b=f.root/"B",alias=f.root/"configured";
    fs::create_directory(a);fs::create_directory(b);
    const auto key=b/"missing.clap";write(key);f.success("clap",key);fs::remove(key);
    if(!junction(alias,a))throw std::runtime_error("owned root junction setup failed");
    ScanPruneRequest request;request.roots={{"clap",alias}};
    const auto bound=bindScanPruneRoots(request);
    if(!bound)throw std::runtime_error(bound.error);
    fs::remove(alias);if(!junction(alias,b))throw std::runtime_error("owned root junction rebind failed");
    request.discoveryComplete=true;const auto result=pruneMissingScans(f.file,request);
    check(result.status==ScanPruneStatus::Incomplete && result.cacheRowsRemoved==0 && f.cached("clap",key),
          "pre-scan canonical binding prevents cleanup of B when the configured junction scanned A");
    fs::remove(alias);
    const auto optional=f.root/"later";request={};request.roots={{"clap",optional}};
    if(!bindScanPruneRoots(request))throw std::runtime_error("bind optional not installed root");
    const auto later=optional/"old.clap";f.success("clap",later);fs::create_directory(optional);
    request.discoveryComplete=true;
    check(pruneMissingScans(f.file,request).status==ScanPruneStatus::Incomplete && f.cached("clap",later),
          "a root absent at scan start cannot authorize pruning after it appears");
}
#endif
} // namespace
int main(){try{staleAndProtected();noWriteAndPartial();transactionalRecheck();legacyNoMigration();
#if defined(_WIN32)
movedAliasParent();danglingAlias();rootRebind();
#endif
}catch(const std::exception& e){++failures;std::printf("EXCEPTION: %s\n",e.what());}
std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
