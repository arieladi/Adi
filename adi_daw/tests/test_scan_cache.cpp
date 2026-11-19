// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/plugin_scanner.hpp"
#include "adi/scan_batch.hpp"
#include "adi/scan_stamp.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <future>
#include <iterator>
#include <thread>
#include <cstdlib>
#include <vector>
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
void check(bool ok,const char* label){++checks;if(!ok){++failures;std::printf("FAIL: %s\n",label);}}
std::string utf8(const fs::path& path){const auto bytes=path.u8string();return {bytes.begin(),bytes.end()};}
fs::path native(const std::string& value){return fs::path(std::u8string(value.begin(),value.end()));}
std::int64_t clock() noexcept{return 123;}
fs::path marker(const fs::path& path){return native(utf8(path)+".launches");}
fs::path binary(const fs::path& path){return fs::is_directory(path)?path/"Contents"/"test.clap":path;}
std::size_t launches(const fs::path& path){std::ifstream in(marker(path));return static_cast<std::size_t>(std::count(std::istreambuf_iterator<char>(in),{},'\n'));}
void write(const fs::path& path,const std::string& value){std::ofstream out(path,std::ios::binary);out<<value;if(!out)throw std::runtime_error("fixture write");}
std::string content(const fs::path& path){std::ifstream in(path,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
#if defined(_WIN32)
bool makeJunction(const fs::path& link,const fs::path& target) {
    // Both paths belong to this test's exclusive TempDirectory. Mount-point
    // reparse data creates a native directory junction without symlink privilege.
    const auto print=fs::canonical(target).wstring();const auto substitute=L"\\??\\"+print;
    struct MountPoint {
        DWORD tag=IO_REPARSE_TAG_MOUNT_POINT;WORD bytes=0,reserved=0;
        WORD substituteOffset=0,substituteBytes=0,printOffset=0,printBytes=0;
        wchar_t names[2048]{};
    } data;
    static_assert(offsetof(MountPoint,names)==16);
    if(substitute.size()+print.size()+2>=2048)return false;
    data.substituteBytes=static_cast<WORD>(substitute.size()*sizeof(wchar_t));
    data.printOffset=static_cast<WORD>(data.substituteBytes+sizeof(wchar_t));
    data.printBytes=static_cast<WORD>(print.size()*sizeof(wchar_t));
    data.bytes=static_cast<WORD>(8+data.printOffset+data.printBytes+sizeof(wchar_t));
    std::copy(substitute.begin(),substitute.end(),data.names);
    std::copy(print.begin(),print.end(),data.names+substitute.size()+1);
    fs::create_directory(link);
    const auto handle=CreateFileW(link.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr);
    if(handle==INVALID_HANDLE_VALUE)return false;
    DWORD returned=0;const bool ok=DeviceIoControl(handle,FSCTL_SET_REPARSE_POINT,&data,
        static_cast<DWORD>(8+data.bytes),nullptr,0,&returned,nullptr)!=FALSE;
    CloseHandle(handle);return ok;
}
#endif
struct Fixture {
    test::TempDirectory temp{"scan_cache","owner"};
    Config config;std::string error;
    explicit Fixture(const fs::path& self){config.executable=self;config.registry=temp.path()/"registry.sqlite";config.clock=clock;}
    fs::path candidate(const char* name="module.clap",const char* value="v1") {
        const auto path=temp.path()/name;write(path,value);return path;
    }
    std::unique_ptr<Scanner> scanner(){auto s=Scanner::open(config,error);if(!s)throw std::runtime_error(error);return s;}
    std::unique_ptr<PluginRegistry> registry(){auto r=PluginRegistry::open(config.registry,error);if(!r)throw std::runtime_error(error);return r;}
};
int helper(const std::vector<std::string>& args) {
    const auto path=native(args[3]);std::ofstream(marker(path),std::ios::app)<<"launch\n";
    const auto module=binary(path);if(!fs::exists(module))return 65;
    const auto version=content(module);
    if (version == "parallel-gate") {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{4};
        for (;;) {
            unsigned arrivals = 0;
            for (unsigned peer = 0; peer < 4; ++peer) {
                const auto sibling = path.parent_path() / ("peer-" + std::to_string(peer) + ".clap");
                if (launches(sibling) == 1) ++arrivals;
            }
            if (arrivals == 4) break;
            if (std::chrono::steady_clock::now() >= deadline) return 65;
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
    }
    if(version=="reject")return 65;
    if(version=="bad-request")return 64;
    if(version=="channel-setup")return 74;
    if(version=="channel-write")return 75;
    if(version=="unsupported")return 78;
    if(version=="hang")std::this_thread::sleep_for(std::chrono::seconds(10));
    if(version=="crash" || (version=="flaky" && launches(path)==1)) {
#if defined(_WIN32)
        TerminateProcess(GetCurrentProcess(),0xC0000005UL);
#else
        std::abort();
#endif
        return 66;
    }
    if(version=="change-fail"){write(module,"changed failed module");return 65;}
    if(version=="mutate")write(module,"mutated during child");
    Plugin plugin;plugin.id="org.adi.cache";plugin.name="Cache fixture";plugin.version=version;
    if(args[2]=="vst3")plugin.data="format validation fixture";
    std::string error;auto packet=encodeMetadata(args[2],args[3],version=="empty"?std::vector<Plugin>{}:std::vector<Plugin>{plugin},error);
    if(!packet)return 65;
    std::fwrite(packet->data(),1,packet->size(),stdout);return 0;
}
void reuseAndChanges(const fs::path& self) {
    Fixture f(self);const auto path=f.candidate();auto scanner=f.scanner();
    const auto a=scanner->probe("clap",path),b=scanner->probe("clap",path);
    scanner.reset();scanner=f.scanner();const auto c=scanner->probe("clap",path);
    check(a.state==State::Ready && !a.cacheHit && b.state==State::Ready && b.cacheHit &&
          c.state==State::Ready && c.cacheHit && launches(path)==1,"three unchanged probes across reopen launch exactly one real child");
    check(c.plugins.size()==1 && c.plugins.front().version=="v1","cache rehydrates the full validated descriptor");
    const auto originalTime=fs::last_write_time(path);
    fs::last_write_time(path,originalTime+std::chrono::seconds(3));
    const auto timestamp=scanner->probe("clap",path);
    check(timestamp.state==State::Ready && !timestamp.cacheHit && launches(path)==2,"timestamp-only change triggers one child");
    const auto retainedTime=fs::last_write_time(path);write(path,"v2-larger");fs::last_write_time(path,retainedTime);
    const auto size=scanner->probe("clap",path);
    check(size.state==State::Ready && !size.cacheHit && size.plugins.front().version=="v2-larger" && launches(path)==3,"size-only change with restored timestamp replaces cached descriptors");
    const auto sibling=f.candidate("sibling.clap");scanner->probe("clap",sibling);
    check(scanner->probe("clap",path).cacheHit && launches(path)==3 && launches(sibling)==1,"adding a module does not reprobe unchanged siblings");
    fs::create_directory(f.temp.path()/"alias");
    const auto dotted=f.temp.path()/"alias"/".."/path.filename();
    auto slash=utf8(path);std::replace(slash.begin(),slash.end(),'\\','/');
    check(scanner->probe("clap",dotted).cacheHit && scanner->probe("clap",native(slash)).cacheHit && launches(path)==3,"dot and alternate separator aliases share the canonical success");
    const auto link=f.temp.path()/"module-link.clap";std::error_code ec;fs::create_symlink(path,link,ec);
    if(!ec)check(scanner->probe("clap",link).cacheHit && launches(path)==3,"native file symlink shares canonical cache identity");
    else std::printf("SKIP: native symlink unavailable (%s)\n",ec.message().c_str());
#if defined(_WIN32)
    auto casing=utf8(path);for(char& ch:casing)if(ch>='a' && ch<='z')ch=static_cast<char>(ch-'a'+'A');
    check(scanner->probe("clap",native(casing)).cacheHit && launches(path)==3,"Windows case-equivalent path shares canonical cache identity");
#endif
    auto registry=f.registry();registry->blockScan("clap",utf8(fs::canonical(path)),"manual",124,f.error);
    check(scanner->probe("clap",path).state==State::Quarantined && launches(path)==3,"a manual block beats an otherwise warm success cache");
    registry->unblockScan("clap",utf8(fs::canonical(path)),f.error);
    check(!scanner->probe("clap",path).cacheHit && launches(path)==4,"explicit re-enable discards cached success and performs a new child scan");
}
void validationAndRaces(const fs::path& self) {
    Fixture f(self);const auto path=f.candidate();auto scanner=f.scanner();scanner->probe("clap",path);
    {
        SQLite::Database db(utf8(f.config.registry),SQLite::OPEN_READWRITE);
        db.exec("UPDATE plugin_scan_cache SET packet='{bad json'");
    }
    const auto repaired=scanner->probe("clap",path);
    check(repaired.state==State::Ready && !repaired.cacheHit && launches(path)==2,"corrupt cache packet is never trusted and is replaced through a real child");
    {
        SQLite::Database db(utf8(f.config.registry),SQLite::OPEN_READWRITE);
        db.exec("UPDATE plugin_scan_cache SET packet=replace(packet,'\"schema\":1','\"schema\":2')");
    }
    const auto future=scanner->probe("clap",path);
    check(future.state==State::Ready && !future.cacheHit && launches(path)==3,
          "unsupported stored protocol schema is discarded rather than rehydrated");
    struct Late {
        PluginRegistry* registry=nullptr;bool mutate=false;std::atomic<bool>* cancel=nullptr;
        static bool validate(const std::vector<Plugin>&,const std::string& path,void* raw,std::string& error) {
            auto& late=*static_cast<Late*>(raw);
            if(late.cancel){late.cancel->store(true);return true;}
            if(late.mutate){write(native(path),"changed during owner validation");return true;}
            return late.registry->blockScan("clap",path,"late explicit block",124,error);
        }
    };
    auto registry=f.registry();Late late{registry.get()};
    check(scanner->probe("clap",path,Late::validate,&late).state==State::Quarantined && launches(path)==3,
          "late block during cache validation prevents metadata publication without child launch");
    registry->unblockScan("clap",utf8(fs::canonical(path)),f.error);
    scanner->probe("clap",path);late.mutate=true;
    const auto changed=scanner->probe("clap",path,Late::validate,&late);
    check(changed.state==State::Unavailable && changed.plugins.empty() && registry->scanBlocks(f.error)->empty(),
          "change during cache validation abandons admission without stale metadata or quarantine");
    check(!scanner->probe("clap",path).cacheHit,"changed cache-hit candidate is probed again next time");
    const auto childChange=f.candidate("changing.clap","mutate");
    const auto changedChild=scanner->probe("clap",childChange);
    check(changedChild.state==State::Unavailable && changedChild.plugins.empty() && launches(childChange)==1 &&
          registry->scanBlocks(f.error)->empty(),"a child changing its candidate cannot publish a success stamp");
    const auto next=scanner->probe("clap",childChange);
    check(next.state==State::Ready && !next.cacheHit && launches(childChange)==2 &&
          next.plugins.front().version=="mutated during child","stable replacement succeeds on a later explicit probe");
    std::atomic<bool> cancel{false};f.config.process.cancel=&cancel;auto cancellable=f.scanner();late.cancel=&cancel;
    check(cancellable->probe("clap",path,Late::validate,&late).state==State::Cancelled &&
          registry->scanBlocks(f.error)->empty(),"cancellation during cached format validation publishes nothing and leaves no pending ticket");
}
void readOnlyWarmSuccess(const fs::path& self) {
    Fixture f(self);const auto path=f.candidate();auto scanner=f.scanner();
    check(scanner->probe("clap",path).state==State::Ready && launches(path)==1,
          "read-only warm fixture first populates success through a real child");
    SQLite::Database db(utf8(f.config.registry),SQLite::OPEN_READWRITE);
    db.exec("CREATE TRIGGER refuse_pending BEFORE INSERT ON plugin_scan_blocks "
            "WHEN NEW.state='pending' BEGIN SELECT RAISE(ABORT,'pending write forbidden'); END");
    const auto before=db.execAndGet("PRAGMA data_version").getInt64();
    const auto sequence=db.execAndGet("SELECT seq FROM sqlite_sequence WHERE name='plugin_scan_blocks'").getInt64();
    const auto warm=scanner->probe("clap",path);
    check(warm.state==State::Ready && warm.cacheHit && launches(path)==1 &&
          db.execAndGet("PRAGMA data_version").getInt64()==before &&
          db.execAndGet("SELECT seq FROM sqlite_sequence WHERE name='plugin_scan_blocks'").getInt64()==sequence,
          "warm success performs no registry write or child launch while pending INSERT is forbidden");
    scanner.reset();scanner=f.scanner();const auto reopened=scanner->probe("clap",path);
    check(reopened.state==State::Ready && reopened.cacheHit && launches(path)==1,
          "read-only warm success survives Scanner reopen and still launches only the original child");
    const auto cold=f.candidate("cold.clap");
    check(scanner->probe("clap",cold).state==State::Quarantined && launches(cold)==0,
          "positive control proves the INSERT trigger still refuses a cold admission before child launch");
    db.exec("DROP TRIGGER refuse_pending");
    auto registry=f.registry(),foreign=f.registry();
    const auto canonical=utf8(fs::canonical(path));
    const auto stamp=db.execAndGet("SELECT stamp FROM plugin_scan_cache").getString();
    const auto read=registry->readScanSuccess("clap",canonical,stamp,f.error);
    check(read && registry->confirmScanSuccess(*read,f.error) && !foreign->confirmScanSuccess(*read,f.error),
          "a bounded warm receipt confirms only on the registry connection that read it");
    db.exec("INSERT INTO plugin_scan_failures(format,path,stamp,kind,reason,strikes,block_id,updated_utc) "
            "SELECT format,path,stamp,'rejected','late rejection',0,NULL,124 FROM plugin_scan_cache");
    check(read && !registry->confirmScanSuccess(*read,f.error) &&
          !registry->readScanSuccess("clap",canonical,stamp,f.error),
          "typed negative result excludes an otherwise matching success from both read observations");
    const auto negative=scanner->probe("clap",path);
    check(negative.state==State::Failed && negative.failureCacheHit && !negative.cacheHit && launches(path)==1,
          "ordinary typed-negative precedence remains authoritative over a stale positive without child launch");
}
void warmRowRecheck(const fs::path& self) {
    for(const auto* mutation : {"DELETE FROM plugin_scan_cache",
            "UPDATE plugin_scan_cache SET updated_utc=updated_utc+1",
            "UPDATE plugin_scan_cache SET stamp='aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'",
            "UPDATE plugin_scan_cache SET packet=replace(packet,'Cache fixture','Cache changed')"}) {
        Fixture f(self);const auto path=f.candidate();auto scanner=f.scanner();
        check(scanner->probe("clap",path).state==State::Ready,"row-recheck fixture is populated");
        SQLite::Database db(utf8(f.config.registry),SQLite::OPEN_READWRITE);
        db.exec("CREATE TRIGGER refuse_pending BEFORE INSERT ON plugin_scan_blocks "
                "WHEN NEW.state='pending' BEGIN SELECT RAISE(ABORT,'pending write forbidden'); END");
        struct Mutation {
            SQLite::Database* db;const char* sql;bool called=false;
            static bool validate(const std::vector<Plugin>&,const std::string&,void* raw,std::string&) {
                auto& change=*static_cast<Mutation*>(raw);change.called=true;change.db->exec(change.sql);return true;
            }
        } change{&db,mutation};
        const auto result=scanner->probe("clap",path,Mutation::validate,&change);
        check(change.called && result.state==State::Quarantined && !result.cacheHit && result.plugins.empty() && launches(path)==1,
              "cache deletion, update value, stamp or packet change during validation cannot publish the old receipt; ticket fallback is enforced");
    }
    Fixture f(self);const auto path=f.candidate();auto scanner=f.scanner();scanner->probe("clap",path);
    auto registry=f.registry();
    struct Pending {
        PluginRegistry* registry;std::optional<ScanTicket> ticket;
        static bool validate(const std::vector<Plugin>&,const std::string& path,void* raw,std::string& error) {
            auto& race=*static_cast<Pending*>(raw);race.ticket=race.registry->beginScan("clap",path,124,error);
            return race.ticket.has_value();
        }
    } pending{registry.get(),{}};
    const auto result=scanner->probe("clap",path,Pending::validate,&pending);
    const auto blocks=registry->scanBlocks(f.error);
    check(pending.ticket && result.state==State::Quarantined && result.plugins.empty() && launches(path)==1 &&
          blocks && blocks->size()==1 && blocks->front().state==ScanBlockState::Pending,
          "a competing pending ticket inserted by the validator defeats warm publication and is never completed by this owner");
    if(pending.ticket)registry->finishScan(*pending.ticket,ScanDisposition::Abandoned,"",125,f.error);
    std::atomic<bool> cancel{false};f.config.process.cancel=&cancel;auto cancellable=f.scanner();
    struct BlockCancel {
        PluginRegistry* registry;std::atomic<bool>* cancel;
        static bool validate(const std::vector<Plugin>&,const std::string& path,void* raw,std::string& error) {
            auto& race=*static_cast<BlockCancel*>(raw);
            const bool blocked=race.registry->blockScan("clap",path,"late manual block and cancel",126,error);
            race.cancel->store(true);return blocked;
        }
    } blocked{registry.get(),&cancel};
    const auto cancelled=cancellable->probe("clap",path,BlockCancel::validate,&blocked);
    check(cancelled.state==State::Quarantined && cancelled.plugins.empty() && launches(path)==1,
          "late manual quarantine remains authoritative when format validation also requests cancellation");
    for(const bool manual : {false,true}) {
        Fixture change(self);const auto module=change.candidate();auto owner=change.scanner();
        owner->probe("clap",module);auto competing=change.registry();
        struct BlockMutation {
            PluginRegistry* registry;bool manual;
            static bool validate(const std::vector<Plugin>&,const std::string& path,void* raw,std::string& error) {
                auto& race=*static_cast<BlockMutation*>(raw);
                const bool accepted=race.manual?race.registry->blockScan("clap",path,"late manual block",124,error):
                    race.registry->beginScan("clap",path,124,error).has_value();
                write(native(path),"changed while another owner blocks");return accepted;
            }
        } race{competing.get(),manual};
        const auto changed=owner->probe("clap",module,BlockMutation::validate,&race);
        const auto retained=competing->scanBlocks(change.error);
        check(changed.state==State::Quarantined && changed.plugins.empty() && launches(module)==1 &&
              retained && retained->size()==1 && retained->front().state==
                  (manual?ScanBlockState::Blocked:ScanBlockState::Pending),
              "late manual/pending state beats simultaneous candidate instability without completing another owner's row");
    }
}
void stampsAndTransaction(const fs::path& self) {
    Fixture f(self);auto scanner=f.scanner();const auto bundle=f.temp.path()/"Bundle.vst3";
    fs::create_directories(bundle/"Contents");write(binary(bundle),"bundle1");
    const auto metadata=bundle/"Contents"/"moduleinfo.json";write(metadata,"{}");
    const auto validate=+[](const std::vector<Plugin>& plugins,const std::string&,void*,std::string&){return plugins.size()==1 && plugins.front().data=="format validation fixture";};
    check(scanner->probe("vst3",bundle,validate).state==State::Ready && scanner->probe("vst3",bundle,validate).cacheHit,
          "bundle success packet is format-validated on initial and cached reads");
    const auto directoryTime=fs::last_write_time(bundle);const auto moduleTime=fs::last_write_time(binary(bundle));
    write(binary(bundle),"bundle2-larger");fs::last_write_time(binary(bundle),moduleTime);fs::last_write_time(bundle,directoryTime);
    check(!scanner->probe("vst3",bundle,validate).cacheHit && launches(bundle)==2,"bundle module size invalidates even with directory and module timestamps restored");
    const auto metaTime=fs::last_write_time(metadata);write(metadata,"{changed}");fs::last_write_time(metadata,metaTime);
    check(!scanner->probe("vst3",bundle,validate).cacheHit && launches(bundle)==3,"moduleinfo content size is included independently of bundle time");
    const std::string hash(64,'a');std::string error;
    const auto clap=candidateStamp("clap",binary(bundle),hash,error),vst=candidateStamp("vst3",binary(bundle),hash,error);
    const auto otherBuild=candidateStamp("clap",binary(bundle),std::string(64,'b'),error);
    check(clap && vst && otherBuild && *clap!=*vst && *clap!=*otherBuild,"format and actual helper build digest are distinct fingerprint inputs");
    const auto mac=f.temp.path()/"Relocated.vst3";fs::create_directories(mac/"Contents"/"MacOS");
    const auto macBinary=mac/"Contents"/"MacOS"/"Relocated";write(macBinary,"extensionless code");
    const auto macStamp=candidateStamp("vst3",mac,hash,error);const auto macTime=fs::last_write_time(macBinary);
    write(macBinary,"extensionless code changed");fs::last_write_time(macBinary,macTime);
    const auto macChanged=candidateStamp("vst3",mac,hash,error);
    check(macStamp && macChanged && *macStamp!=*macChanged,"relocated macOS-style bundle stamps its extensionless Contents/MacOS binary");
    const auto path=utf8(fs::canonical(binary(bundle)));auto registry=f.registry();
    auto ticket=registry->beginScan("clap",path,1,error);if(!ticket)throw std::runtime_error(error);
    auto other=f.registry();
    check(!other->finishCachedScan(*ticket,hash,"{}",2,error),"foreign connection cannot publish a ticket's success cache");
    other->blockScan("clap",path,"newer user choice",3,error);
    check(!registry->finishCachedScan(*ticket,hash,"{}",4,error),"stale cached completion does not clear a newer block");
    registry->unblockScan("clap",path,error);ticket=registry->beginScan("clap",path,5,error);
    if(!ticket)throw std::runtime_error(error);
    if(!registry->finishCachedScan(*ticket,hash,"previous complete packet",5,error))throw std::runtime_error(error);
    ticket=registry->beginScan("clap",path,5,error);if(!ticket)throw std::runtime_error(error);
    {
        SQLite::Database db(utf8(f.config.registry),SQLite::OPEN_READWRITE);
        db.exec("CREATE TRIGGER refuse_cache BEFORE INSERT ON plugin_scan_cache BEGIN SELECT RAISE(ABORT,'injected'); END");
    }
    check(!registry->finishCachedScan(*ticket,hash,"{}",6,error),"late SQL failure refuses atomic cache publication");
    const auto blocks=registry->scanBlocks(error);
    check(blocks && blocks->size()==1 && blocks->front().state==ScanBlockState::Pending,"failed cache transaction restores the pending ticket for retry");
    check(registry->cachedScan(*ticket,hash,error)==std::optional<std::string>("previous complete packet"),
          "failed replacement keeps the preceding complete cached packet");
    {
        SQLite::Database db(utf8(f.config.registry),SQLite::OPEN_READWRITE);db.exec("DROP TRIGGER refuse_cache");
    }
    check(registry->finishCachedScan(*ticket,hash,"{}",7,error),"same ticket can complete after its transaction rollback");
    ticket=registry->beginScan("clap",path,8,error);if(!ticket)throw std::runtime_error(error);
    check(registry->cachedScan(*ticket,hash,error)==std::optional<std::string>("{}") &&
          !registry->cachedScan(*ticket,std::string(64,'b'),error),"cache lookup requires the exact stamp under an admitted ticket");
    check(!registry->finishCachedScan(*ticket,hash,std::string(PluginRegistry::kMaximumScanPacket+1,'x'),9,error),
          "oversized cache write is refused before changing ticket ownership");
    registry->finishScan(*ticket,ScanDisposition::Abandoned,"",10,error);
}
void migration(const fs::path& self) {
    Fixture f(self);auto registry=f.registry();const auto path=utf8(f.candidate());
    registry->remember("clap","legacy",engine::RouteChoice::MpeMidi,42,f.error);
    registry->blockScan("clap",path,"preserved v2 block",43,f.error);registry.reset();
    {
        SQLite::Database db(utf8(f.config.registry),SQLite::OPEN_READWRITE);
        db.exec("DROP TABLE plugin_scan_cache");db.exec("PRAGMA user_version=2");
    }
    registry=f.registry();const auto blocks=registry->scanBlocks(f.error);
    check(registry->route("clap","legacy")==engine::RouteChoice::MpeMidi && blocks && blocks->size()==1 &&
          blocks->front().reason=="preserved v2 block" && blocks->front().updatedUtc==43,"v2 migration preserves authored routes and all existing quarantine semantics");
    SQLite::Database db(utf8(f.config.registry),SQLite::OPEN_READONLY);
    check(db.execAndGet("PRAGMA user_version").getInt()==3 && db.execAndGet("SELECT count(*) FROM plugin_scan_cache").getInt()==0,
          "v2 migration adds an empty v3 cache atomically");
}
void helperIdentity(const fs::path& self) {
    Fixture f(self);const auto path=f.candidate();auto scanner=f.scanner();
    check(scanner->probe("clap",path).state==State::Ready && launches(path)==1,"helper identity baseline is a real successful child");
#if defined(_WIN32)
    const auto copy=f.temp.path()/"second-helper.exe";
#else
    const auto copy=f.temp.path()/"second-helper";
#endif
    fs::copy_file(self,copy);f.config.executable=copy;scanner=f.scanner();
    check(scanner->probe("clap",path).cacheHit && launches(path)==1,"identical helper bytes at another path retain the same cache build identity");
    std::ofstream(copy,std::ios::binary|std::ios::app)<<"new helper build overlay";
    const auto changed=scanner->probe("clap",path);
    check(changed.state==State::Ready && !changed.cacheHit && launches(path)==2,"changed helper build invalidates success even within one Scanner lifetime");
    scanner.reset();scanner=f.scanner();
    check(scanner->probe("clap",path).cacheHit && launches(path)==2,"new helper build's success persists across reopen");
}
void uncacheableBundle(const fs::path& self) {
    Fixture f(self);auto scanner=f.scanner();const auto empty=f.temp.path()/"empty.vst3";
    fs::create_directory(empty);
    check(scanner->probe("clap",empty).state==State::Failed && launches(empty)==1,
          "empty malformed bundle still reaches its isolated child instead of aborting discovery as unavailable");
    const auto large=f.temp.path()/"large.clap";fs::create_directories(large/"Contents");write(binary(large),"bounded child");
    for(unsigned i=0;i<4097;++i)write(large/"Contents"/(std::to_string(i)+".dll"),"");
    const auto result=scanner->probe("clap",large);
    check(result.state==State::Ready && !result.cacheHit && !result.cacheWarning.empty() && launches(large)==1,
          "unreliable oversized fingerprint is explicit uncached discovery, not an unavailable plug-in");
    check(scanner->probe("clap",large).state==State::Ready && launches(large)==2,
          "unstampable result is not reused as a weak cache entry");
    write(binary(large),"reject");const auto uncachedNegative=scanner->probe("clap",large);
    check(uncachedNegative.state==State::Failed && !uncachedNegative.cacheWarning.empty() &&
        !uncachedNegative.failureCacheHit && scanner->probe("clap",large).state==State::Failed &&
        launches(large)==4 && f.registry()->scanBlocks(f.error)->empty(),
        "unstampable deterministic failure remains uncached and never quarantines the candidate");
    const auto outside=f.temp.path()/"external";fs::create_directory(outside);write(outside/"test.clap","external module");
    const auto linked=f.temp.path()/"linked.clap";fs::create_directory(linked);std::error_code ec;
    fs::create_directory_symlink(outside,linked/"Contents",ec);
    if(!ec) {
        const auto external=scanner->probe("clap",linked);
        check(external.state==State::Ready && !external.cacheHit && !external.cacheWarning.empty(),
              "external internal-directory link still reaches child but is not traversed for the cache");
    } else std::printf("SKIP: directory symlink unavailable (%s)\n",ec.message().c_str());
}
void negativeCache(const fs::path& self) {
    Fixture f(self);auto scanner=f.scanner();auto registry=f.registry();
    const auto rejected=f.candidate("rejected.clap","reject");
    const auto first=scanner->probe("clap",rejected),second=scanner->probe("clap",rejected);
    scanner.reset();scanner=f.scanner();const auto third=scanner->probe("clap",rejected);
    check(first.state==State::Failed && first.failureStrikes==0 && second.state==State::Failed &&
        second.failureCacheHit && third.failureCacheHit && launches(rejected)==1 && second.error==first.error,
        "unchanged deterministic failure reuses exact diagnostic across reopen with one real child");
    check(registry->scanBlocks(f.error)->empty(),"deterministic negative result never appears in quarantine");
    const auto empty=f.candidate("no-classes.clap","empty");
    const auto empty1=scanner->probe("clap",empty),empty2=scanner->probe("clap",empty);
    check(empty1.state==State::Failed && empty2.state==State::Failed && empty2.failureCacheHit &&
        empty2.failureStrikes==0 && empty2.plugins.empty() && launches(empty)==1 && registry->scanBlocks(f.error)->empty(),
        "decoded empty class list is a reusable negative result with no quarantine or crash strikes");
    const auto pending=registry->beginScan("clap",utf8(fs::canonical(empty)),123,f.error);
    const auto whilePending=scanner->probe("clap",empty);
    check(pending && whilePending.state==State::Quarantined && !whilePending.failureCacheHit && launches(empty)==1,
        "another owner's live pending ticket wins over matching negative metadata");
    if(pending)registry->finishScan(*pending,ScanDisposition::Abandoned,"",124,f.error);
    check(scanner->probe("clap",empty).failureCacheHit && launches(empty)==1,
        "abandoned competing attempt leaves the prior negative cache intact");
    const auto vstEmpty=f.candidate("no-classes.vst3","empty");
    const auto accept=+[](const std::vector<Plugin>&,const std::string&,void*,std::string&){return true;};
    scanner->probe("vst3",vstEmpty,accept);const auto vstNegative=scanner->probe("vst3",vstEmpty,accept);
    check(vstNegative.state==State::Failed && vstNegative.failureCacheHit && launches(vstEmpty)==1 &&
        registry->scanBlocks(f.error)->empty(),"empty VST3-format metadata is negative-cached before any format validator can accept it");
    fs::last_write_time(rejected,fs::last_write_time(rejected)+std::chrono::seconds(3));
    check(scanner->probe("clap",rejected).state==State::Failed && launches(rejected)==2,
        "timestamp-only change releases only the typed automatic negative result");
    const auto time=fs::last_write_time(rejected);write(rejected,"recovered larger");fs::last_write_time(rejected,time);
    check(scanner->probe("clap",rejected).state==State::Ready && launches(rejected)==3,
        "size-only change recovers a failed module without explicit re-enable");
    const auto good=f.candidate("formerly-good.clap","good");const auto originalTime=fs::last_write_time(good);
    scanner->probe("clap",good);write(good,"reject");
    check(scanner->probe("clap",good).state==State::Failed,"replacement failure follows earlier success");
    write(good,"good");fs::last_write_time(good,originalTime);const auto restored=scanner->probe("clap",good);
    check(restored.state==State::Ready && !restored.cacheHit && launches(good)==3,
        "a recorded failure deletes stale positive metadata even if original stamp is later restored");
    const auto changing=f.candidate("changing-failure.clap","change-fail");
    check(scanner->probe("clap",changing).state==State::Unavailable && launches(changing)==1 &&
        scanner->probe("clap",changing).state==State::Ready && launches(changing)==2,
        "changed-during-failure result is abandoned and cannot poison the replacement stamp");
    registry->blockScan("clap",utf8(fs::canonical(rejected)),"manual",124,f.error);
    write(rejected,"another replacement");const auto manual=scanner->probe("clap",rejected);
    check(manual.state==State::Quarantined && !manual.failureCacheHit && launches(rejected)==3,
        "changed reliable stamp never clears a manual block");
}
void strikesAndAbandonment(const fs::path& self) {
    Fixture f(self);auto scanner=f.scanner();auto registry=f.registry();
    const auto crash=f.candidate("crash.clap","crash");
    const auto first=scanner->probe("clap",crash);
    check(first.state==State::Failed && first.failureStrikes==1 && launches(crash)==1 && registry->scanBlocks(f.error)->empty(),
        "first contained crash records one strike without a durable blocking marker");
    scanner.reset();scanner=f.scanner();const auto second=scanner->probe("clap",crash),third=scanner->probe("clap",crash);
    check(second.state==State::Failed && second.failureStrikes==2 && third.state==State::Quarantined &&
        third.failureCacheHit && third.failureStrikes==2 && launches(crash)==2,
        "second unchanged crash across reopen blocks a third real child");
    fs::last_write_time(crash,fs::last_write_time(crash)+std::chrono::seconds(3));
    const auto reset=scanner->probe("clap",crash);
    check(reset.state==State::Failed && reset.failureStrikes==1 && launches(crash)==3,
        "reliable changed stamp resets two crash strikes before the new attempt");
    write(crash,"healthy replacement");
    check(scanner->probe("clap",crash).state==State::Ready && launches(crash)==4,
        "changed formerly crashing module recovers automatically");
    const auto flaky=f.candidate("flaky.clap","flaky");scanner->probe("clap",flaky);
    check(scanner->probe("clap",flaky).state==State::Ready && scanner->probe("clap",flaky).cacheHit && launches(flaky)==2,
        "successful second same-stamp attempt clears first strike and caches metadata");
    f.config.process.timeout=std::chrono::milliseconds(500);auto shortScan=f.scanner();
    const auto hang=f.candidate("hang.clap","hang");
    const auto timeout1=shortScan->probe("clap",hang),timeout2=shortScan->probe("clap",hang),timeout3=shortScan->probe("clap",hang);
    check(timeout1.failureStrikes==1 && timeout2.failureStrikes==2 && timeout3.failureCacheHit && launches(hang)==2,
        "two deadline failures reap two children then reuse the negative result");
    const auto unavailable=f.candidate("unsupported.clap","unsupported");
    check(scanner->probe("clap",unavailable).state==State::Unavailable &&
        scanner->probe("clap",unavailable).state==State::Unavailable && launches(unavailable)==2,
        "unsupported helper capability is not a cached candidate failure or strike");
    const auto cancelled=f.candidate("cancel.clap","hang");std::atomic<bool> cancel{false};
    f.config.process.cancel=&cancel;f.config.process.timeout=std::chrono::seconds(3);auto cancellable=f.scanner();
    std::thread cancellation([&]{std::this_thread::sleep_for(std::chrono::milliseconds(250));cancel=true;});
    const auto result=cancellable->probe("clap",cancelled);cancellation.join();
    SQLite::Database db(utf8(f.config.registry),SQLite::OPEN_READWRITE);
    SQLite::Statement failed(db,"SELECT count(*) FROM plugin_scan_failures WHERE path=?");failed.bind(1,utf8(fs::canonical(cancelled)));failed.executeStep();
    check(result.state==State::Cancelled && result.failureStrikes==0 && launches(cancelled)==1 && failed.getColumn(0).getInt()==0,
        "cancellation reaps the child without a cached result or crash strike");
    const auto coldBusy=f.candidate("busy-cold.clap","healthy");
    const auto before=launches(crash);db.exec("BEGIN IMMEDIATE");
    const auto warm=scanner->probe("clap",crash);
    const auto busy=scanner->probe("clap",coldBusy);db.exec("ROLLBACK");
    check(warm.state==State::Ready && warm.cacheHit && launches(crash)==before,
        "a committed matching success remains readable under an unrelated WAL writer without a ticket");
    check(busy.state==State::Quarantined && busy.plugins.empty() && launches(coldBusy)==0,
        "busy registry admission for a cache miss launches no child and returns no success metadata");
}
void infrastructureFailures(const fs::path& self) {
    Fixture f(self);
    auto scanner = f.scanner();
    auto registry = f.registry();
    for (const auto* reason : {"bad-request", "channel-setup", "channel-write", "unsupported"}) {
        const auto path = f.temp.path() / (std::string(reason) + ".clap");
        write(path, reason);
        const auto first = scanner->probe("clap", path);
        const auto second = scanner->probe("clap", path);
        const auto blocks = registry->scanBlocks(f.error);
        check(first.state == State::Unavailable && second.state == State::Unavailable &&
              !first.failureCacheHit && !second.failureCacheHit &&
              first.failureStrikes == 0 && second.failureStrikes == 0 &&
              blocks && blocks->empty() && launches(path) == 2,
              "helper infrastructure failure remains retriable without marking the candidate");
    }
}
void retryAliases(const fs::path& self) {
    Fixture f(self);auto scanner=f.scanner();auto registry=f.registry();const auto path=f.candidate();
    fs::create_directory(f.temp.path()/"alias");auto slash=utf8(path);std::replace(slash.begin(),slash.end(),'\\','/');
    std::vector<fs::path> aliases{f.temp.path()/"alias"/".."/path.filename(),native(slash)};
#if defined(_WIN32)
    auto casing=utf8(path);for(char& ch:casing)if(ch>='a' && ch<='z')ch=static_cast<char>(ch-'a'+'A');aliases.push_back(native(casing));
#endif
    const auto link=f.temp.path()/"retry-link.clap";std::error_code ec;fs::create_symlink(path,link,ec);
    if(!ec)aliases.push_back(link);else std::printf("SKIP: retry symlink unavailable (%s)\n",ec.message().c_str());
#if defined(_WIN32)
    const auto junction=f.temp.path()/"owned-junction";
    const bool junctionMade=makeJunction(junction,f.temp.path());
    check(junctionMade && fs::canonical(junction/path.filename())==fs::canonical(path),
        "native Windows junction is created without symlink privilege and resolves to the owned module");
    if(junctionMade)aliases.push_back(junction/path.filename());
#endif
    bool passed=true;std::size_t count=0;
    for(const auto& alias:aliases) {
        registry->blockScan("clap",utf8(fs::canonical(path)),"manual retry",124,f.error);
        passed=passed && scanner->retry("clap",alias,f.error);
        const auto result=scanner->probe("clap",path);++count;
        passed=passed && result.state==State::Ready && !result.cacheHit && launches(path)==count;
    }
    check(passed,"explicit retry resolves supported separator/dot/case/link aliases to the probe identity");
#if defined(_WIN32)
    // Remove this owned reparse point itself, never recursively follow it.
    check(RemoveDirectoryW(junction.c_str())!=FALSE && fs::exists(path),
        "junction retry used the canonical block and cleanup removes only the junction, retaining its target");
#endif
    write(path,"reject");scanner->probe("clap",path);
    check(scanner->retry("clap",path,f.error) && scanner->probe("clap",path).state==State::Failed && launches(path)==count+2,
        "explicit retry discards matching negative metadata and runs a fresh child");
    check(!scanner->retry("au",path,f.error) && !scanner->retry("clap",f.temp.path()/"missing",f.error),
        "retry refuses unsupported formats and missing paths without lexical alias guessing");
}
void failureTransactions(const fs::path& self) {
    Fixture f(self);auto registry=f.registry();const auto path=utf8(f.candidate());const std::string stamp(64,'a');
    auto ticket=registry->beginScan("clap",path,1,f.error);if(!ticket)throw std::runtime_error(f.error);
    registry->finishCachedScan(*ticket,stamp,"old complete packet",2,f.error);
    ticket=registry->beginScan("clap",path,3,f.error);if(!ticket)throw std::runtime_error(f.error);
    SQLite::Database db(utf8(f.config.registry),SQLite::OPEN_READWRITE);
    db.exec("CREATE TRIGGER refuse_failure BEFORE INSERT ON plugin_scan_failures BEGIN SELECT RAISE(ABORT,'injected'); END");
    check(!registry->finishFailedScan(*ticket,stamp,ScanFailureKind::Rejected,"reject",4,f.error) &&
        registry->cachedScan(*ticket,stamp,f.error)==std::optional<std::string>("old complete packet") &&
        registry->scanBlocks(f.error)->front().state==ScanBlockState::Pending,
        "late negative-cache SQL failure rolls back its block, stale-positive deletion and exact pending ticket");
    db.exec("DROP TRIGGER refuse_failure");auto other=f.registry();
    check(!other->finishFailedScan(*ticket,stamp,ScanFailureKind::Crashed,"crash",4,f.error),
        "foreign connection cannot consume strikes or complete a ticket");
    other->blockScan("clap",path,"late manual",5,f.error);
    check(!registry->finishFailedScan(*ticket,stamp,ScanFailureKind::Rejected,"reject",6,f.error) &&
        !registry->beginStampedScan("clap",path,std::string(64,'b'),7,f.error),
        "late manual block defeats negative completion and changed-stamp retry");
    registry->unblockScan("clap",path,f.error);ticket=registry->beginScan("clap",path,8,f.error);
    if(!ticket)throw std::runtime_error(f.error);
    registry->finishFailedScan(*ticket,stamp,ScanFailureKind::Crashed,"crash",9,f.error);
    ticket=registry->beginScan("clap",path,9,f.error);if(!ticket)throw std::runtime_error(f.error);
    registry->finishFailedScan(*ticket,stamp,ScanFailureKind::Crashed,"crash",9,f.error);
    // An early v3/manual writer knows only this UPDATE; even same text/time must
    // retire automatic ownership so future stamped admission cannot clear it.
    db.exec("UPDATE plugin_scan_blocks SET reason=reason,updated_utc=updated_utc");
    check(!registry->beginStampedScan("clap",path,std::string(64,'b'),10,f.error) &&
        db.execAndGet("SELECT count(*) FROM plugin_scan_failures").getInt()==0,
        "legacy/manual SQL block updates retire typed ownership even when every value stays the same");
    registry->unblockScan("clap",path,f.error);ticket=registry->beginScan("clap",path,11,f.error);
    if(!ticket)throw std::runtime_error(f.error);
    registry->finishFailedScan(*ticket,stamp,ScanFailureKind::Crashed,"crash",12,f.error);
    auto admission=registry->beginStampedScan("clap",path,stamp,13,f.error);
    check(admission && admission->ticket && !registry->beginStampedScan("clap",path,std::string(64,'b'),14,f.error),
        "an interrupted second attempt remains pending even if a different reliable stamp appears");
    registry->unblockScan("clap",path,f.error);registry.reset();other.reset();
    db.exec("DROP TRIGGER scan_failure_block_update; DROP TRIGGER scan_failure_block_insert; "
        "DROP TRIGGER scan_failure_block_delete; DROP TRIGGER scan_failure_success_insert; DROP TABLE plugin_scan_failures");
    registry=f.registry();
    check(db.execAndGet("PRAGMA user_version").getInt()==3 &&
        db.execAndGet("SELECT count(*) FROM sqlite_master WHERE name='plugin_scan_failures'").getInt()==1,
        "early success-only v3 registry gains the additive failure table transactionally without version bump");
}
void waitsForRegistryWriter(const fs::path& self) {
    using namespace std::chrono_literals;
    Fixture f(self);
    auto registry = f.registry();
    const auto path = utf8(fs::canonical(f.candidate()));
    const std::string stamp(64, 'a');
    SQLite::Database competing(utf8(f.config.registry), SQLite::OPEN_READWRITE);
    competing.exec("CREATE TABLE test_writer_receipts (sequence INTEGER)");
    const auto withWriter = [&](auto work) {
        SQLite::Transaction write(competing, SQLite::TransactionBehavior::IMMEDIATE);
        competing.exec("INSERT INTO test_writer_receipts VALUES (1)");
        std::promise<void> ready;
        auto started = ready.get_future();
        auto result = std::async(std::launch::async, [&] {
            ready.set_value();
            return work();
        });
        started.wait();
        const bool waited = result.wait_for(100ms) == std::future_status::timeout;
        write.commit();
        return std::pair(waited, result.get());
    };
    const auto [waitedForAdmission, admitted] = withWriter([&] {
        return registry->beginStampedScan("clap", path, stamp, 1, f.error);
    });
    check(waitedForAdmission && admitted && admitted->ticket,
          "stamped admission waits for another registry writer and receives a valid ticket");
    const auto ticket = admitted && admitted->ticket ? admitted->ticket
                                                     : registry->beginScan("clap", path, 1, f.error);
    if (!ticket) throw std::runtime_error(f.error);
    const auto [waitedForCompletion, completed] = withWriter([&] {
        return registry->finishFailedScan(*ticket, stamp, ScanFailureKind::Rejected, "fixture rejection", 2, f.error);
    });
    const auto blocks = registry->scanBlocks(f.error);
    check(waitedForCompletion && completed == 0 && blocks && blocks->empty(),
          "typed failure completion waits for the writer and releases its pending ticket");
}

void parallelProbeGate(const fs::path& self) {
    Fixture f(self);
    std::vector<fs::path> paths;
    for (unsigned i = 0; i < 4; ++i)
        paths.push_back(f.candidate(("peer-" + std::to_string(i) + ".clap").c_str(), "parallel-gate"));
    const auto probe = [&](Scanner& scanner, std::size_t i) { return scanner.probe("clap", paths[i]); };
    f.config.workers = 4;
    for (const bool warm : {false, true}) {
        std::size_t next = 0;
        const auto visit = [&](std::size_t i, const Result& result) {
            const bool ordered = i == next++;
            return ordered && result.state == State::Ready && result.cacheHit == warm &&
                   result.path == utf8(fs::canonical(paths[i]));
        };
        const auto completed = scan::detail::visitProbes(paths.size(), f.config, probe, visit, f.error);
        check(completed && next == paths.size() &&
                  std::all_of(paths.begin(), paths.end(), [](const auto& path) { return launches(path) == 1; }),
              warm ? "four warm probes reuse the results in original order without any new child"
                   : "four cold children are concurrently alive at the gate and publish in input order");
    }
}

int entry(const std::vector<std::string>& args) {
    if(args.size()==4 && args[1]=="--scan")return helper(args);
    try {const auto self=fs::absolute(native(args[0]));reuseAndChanges(self);validationAndRaces(self);readOnlyWarmSuccess(self);warmRowRecheck(self);stampsAndTransaction(self);migration(self);helperIdentity(self);uncacheableBundle(self);negativeCache(self);strikesAndAbandonment(self);infrastructureFailures(self);retryAliases(self);failureTransactions(self);waitsForRegistryWriter(self);parallelProbeGate(self);}
    catch(const std::exception& e){++failures;std::printf("EXCEPTION: %s\n",e.what());}
    std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;
}
} // namespace
#if defined(_WIN32)
int wmain(int argc,wchar_t** argv) {
    std::vector<std::string> args;
    for(int i=0;i<argc;++i){const auto bytes=fs::path(argv[i]).u8string();args.emplace_back(bytes.begin(),bytes.end());}
    return entry(args);
}
#else
int main(int argc,char** argv){std::vector<std::string> args;for(int i=0;i<argc;++i)args.emplace_back(argv[i]);return entry(args);}
#endif
