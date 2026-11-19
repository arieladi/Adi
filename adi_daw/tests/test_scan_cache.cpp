// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/plugin_scanner.hpp"
#include "adi/scan_stamp.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
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
    if(version=="mutate")write(module,"mutated during child");
    Plugin plugin;plugin.id="org.adi.cache";plugin.name="Cache fixture";plugin.version=version;
    if(args[2]=="vst3")plugin.data="format validation fixture";
    std::string error;auto packet=encodeMetadata(args[2],args[3],{plugin},error);
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
          registry->scanBlocks(f.error)->empty(),"cancellation during cached format validation publishes nothing and abandons its ticket");
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
    const auto outside=f.temp.path()/"external";fs::create_directory(outside);write(outside/"test.clap","external module");
    const auto linked=f.temp.path()/"linked.clap";fs::create_directory(linked);std::error_code ec;
    fs::create_directory_symlink(outside,linked/"Contents",ec);
    if(!ec) {
        const auto external=scanner->probe("clap",linked);
        check(external.state==State::Ready && !external.cacheHit && !external.cacheWarning.empty(),
              "external internal-directory link still reaches child but is not traversed for the cache");
    } else std::printf("SKIP: directory symlink unavailable (%s)\n",ec.message().c_str());
}
int entry(const std::vector<std::string>& args) {
    if(args.size()==4 && args[1]=="--scan")return helper(args);
    try {const auto self=fs::absolute(native(args[0]));reuseAndChanges(self);validationAndRaces(self);stampsAndTransaction(self);migration(self);helperIdentity(self);uncacheableBundle(self);}
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
