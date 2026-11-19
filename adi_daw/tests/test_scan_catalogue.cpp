// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/scan_catalogue.hpp"
#include "adi/audio/io_audit.hpp"
#include "temp_directory.hpp"
#include "legacy_plugin_registry.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <thread>
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
void write(const fs::path& path,const std::string& value){std::ofstream out(path,std::ios::binary);out<<value;if(!out)throw std::runtime_error("fixture write");}
std::string read(const fs::path& path){std::ifstream in(path,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
fs::path marker(const fs::path& path){return native(utf8(path)+".launches");}
std::size_t launches(const fs::path& path){const auto text=read(marker(path));return static_cast<std::size_t>(std::count(text.begin(),text.end(),'\n'));}
std::int64_t clock() noexcept{return 123;}
bool validate(const std::vector<Plugin>&,const std::string&,void*,std::string&){return true;}
std::string packet(const std::string& format,const std::string& path,const std::string& version) {
    Plugin plugin;plugin.id="org.adi.catalogue";plugin.name="Catalogue fixture";plugin.version=version;
    if(format=="vst3")plugin.data="<PLUGIN fixture='stored XML for the host validator'/>";
    std::string error;auto result=encodeMetadata(format,path,{plugin},error);
    if(!result)throw std::runtime_error(error);return *result;
}
int helper(const std::vector<std::string>& args) {
    const auto path=native(args[3]);std::ofstream(marker(path),std::ios::app)<<"launch\n";
    const auto version=read(path);if(version=="reject")return 65;
    if(version=="crash") {
#if defined(_WIN32)
        TerminateProcess(GetCurrentProcess(),0xC0000005UL);
#endif
        std::abort();
    }
    const auto value=packet(args[2],args[3],version);std::fwrite(value.data(),1,value.size(),stdout);return 0;
}
struct Fixture {
    test::TempDirectory temp{"scan_catalogue","owner"};Config config;std::string error;
    std::unique_ptr<PluginRegistry> registry;std::unique_ptr<Scanner> scanner;
    explicit Fixture(const fs::path& self) {
        config.executable=temp.path()/self.filename();fs::copy_file(self,config.executable);
        config.registry=temp.path()/"registry.sqlite";config.clock=clock;
        scanner=Scanner::open(config,error);registry=PluginRegistry::open(config.registry,error);
        if(!scanner || !registry)throw std::runtime_error(error);
    }
    fs::path populate(const char* name,const char* format="clap") {
        const auto path=temp.path()/name;write(path,"generation0");
        if(scanner->probe(format,path,validate).state!=State::Ready)throw std::runtime_error("real child population failed");
        return fs::canonical(path);
    }
    void removeHelper(){scanner.reset();if(!fs::remove(config.executable))throw std::runtime_error("owned helper removal failed");}
};
void snapshotWithoutHelper(const fs::path& self) {
    Fixture f(self);const auto a=f.populate("a.clap"),b=f.populate("b.clap"),v=f.populate("v.vst3","vst3");
    f.removeHelper();SQLite::Database observer(utf8(f.config.registry),SQLite::OPEN_READWRITE);
    const auto database=read(f.config.registry);const auto wal=native(utf8(f.config.registry)+"-wal");const auto walBytes=read(wal);
    const auto dataVersion=observer.execAndGet("PRAGMA data_version").getInt();
    const auto catalogue=readCatalogue(f.config.registry,f.error);
    check(catalogue && catalogue->modules.size()==3 && catalogue->pluginCount==3 && !fs::exists(f.config.executable),
        "real child-populated cache hydrates a complete catalogue after its owned helper is removed");
    if(!catalogue)return;
    const auto* found=catalogue->find("vst3",utf8(v));
    check(found && found->canonicalPath==utf8(v) && found->stamp.size()==64 && found->updatedUtc==123 &&
        found->plugins.size()==1 && found->plugins.front().data=="<PLUGIN fixture='stored XML for the host validator'/>",
        "snapshot retains canonical identity, opaque build stamp and VST3 XML for later host validation/rebinding");
    check(!catalogue->find("clap",utf8(v)) && !catalogue->find("clap",utf8(a)+"missing"),
        "snapshot key lookup is exact and format-scoped");
    check(launches(a)==1 && launches(b)==1 && launches(v)==1 && f.registry->scanBlocks(f.error)->empty() &&
        observer.execAndGet("PRAGMA data_version").getInt()==dataVersion && read(f.config.registry)==database && read(wal)==walBytes,
        "hydration launches no child, inserts no pending and leaves database/WAL bytes and data version unchanged");
    fs::remove(a);const auto stored=readCatalogue(f.config.registry,f.error);
    check(stored && stored->find("clap",utf8(a)),
        "snapshot is explicitly stored metadata, not filesystem freshness or runtime readiness validation");
    CatalogueLimits limits;limits.maximumModules=2;
    check(!readCatalogue(f.config.registry,f.error,limits) && !f.error.empty(),"module bound refuses the entire snapshot");
    limits={};limits.maximumPlugins=2;check(!readCatalogue(f.config.registry,f.error,limits),"total descriptor bound refuses the entire snapshot");
    limits={};limits.maximumTextBytes=1;check(!readCatalogue(f.config.registry,f.error,limits),"aggregate text bound refuses before packet extraction");
    limits={};limits.maximumModules=0;check(!readCatalogue(f.config.registry,f.error,limits),"invalid read limits refuse");
    const auto missing=f.temp.path()/"never-created"/"missing.sqlite";
    check(!readCatalogue(missing,f.error) && !fs::exists(missing.parent_path()),"reader does not create a missing registry or parent directory");
}
void exclusions(const fs::path& self) {
    Fixture f(self);const auto good=f.populate("good.clap"),manual=f.populate("manual.clap"),pending=f.populate("pending.clap");
    const auto negative=f.populate("negative.clap"),first=f.populate("first.clap"),automatic=f.populate("automatic.clap");
    f.registry->blockScan("clap",utf8(manual),"manual",124,f.error);
    const auto ticket=f.registry->beginScan("clap",utf8(pending),124,f.error);if(!ticket)throw std::runtime_error(f.error);
    write(automatic,"crash");f.scanner->probe("clap",automatic);f.scanner->probe("clap",automatic);
    // Seed stale positive/failure overlaps deliberately. Current writer purges
    // positives on failure, but the reader must not rely on that alone.
    SQLite::Database db(utf8(f.config.registry),SQLite::OPEN_READWRITE);
    SQLite::Statement reject(db,"INSERT INTO plugin_scan_failures(format,path,stamp,kind,reason,strikes,block_id,updated_utc) "
        "SELECT format,path,stamp,?,'seeded stale overlap',?,NULL,124 FROM plugin_scan_cache WHERE path=?");
    reject.bind(1,"rejected");reject.bind(2,0);reject.bind(3,utf8(negative));reject.exec();reject.reset();reject.clearBindings();
    reject.bind(1,"crashed");reject.bind(2,1);reject.bind(3,utf8(first));reject.exec();
    f.removeHelper();const auto catalogue=readCatalogue(f.config.registry,f.error);
    check(catalogue && catalogue->modules.size()==1 && catalogue->find("clap",utf8(good)),
        "one snapshot excludes manual, live pending, automatic two-strike, negative and first-strike candidates");
    check(launches(good)==1 && launches(manual)==1 && launches(pending)==1 && launches(automatic)==3,
        "exclusion/hydration itself never starts another child");
    check(f.registry->scanBlocks(f.error)->size()==3,"read keeps manual, pending and automatic blocks intact");
}
void setPacket(SQLite::Database& db,const fs::path& path,const std::string& value) {
    SQLite::Statement update(db,"UPDATE plugin_scan_cache SET packet=? WHERE path=?");update.bind(1,value);update.bind(2,utf8(path));update.exec();
}
void corruptionAndCompatibility(const fs::path& self) {
    Fixture f(self);const auto a=f.populate("a.clap"),b=f.populate("b.clap");f.removeHelper();
    SQLite::Database db(utf8(f.config.registry),SQLite::OPEN_READWRITE);
    setPacket(db,b,"{broken");check(!readCatalogue(f.config.registry,f.error),"bad later metadata refuses the whole snapshot, never publishes its healthy prefix");
    f.registry->blockScan("clap",utf8(b),"do not use",124,f.error);const auto filtered=readCatalogue(f.config.registry,f.error);
    check(filtered && filtered->modules.size()==1,"excluded blocked metadata is not decoded or returned even if corrupt");
    setPacket(db,a,packet("clap",utf8(a)+"different","generation0"));
    check(!readCatalogue(f.config.registry,f.error),"packet identity must match its stored module key exactly");
    std::string error;const auto empty=encodeMetadata("clap",utf8(a),{},error);if(!empty)throw std::runtime_error(error);
    setPacket(db,a,*empty);const auto noPositive=readCatalogue(f.config.registry,f.error);
    check(noPositive && noPositive->modules.empty() && noPositive->pluginCount==0,"legacy valid empty packets are excluded from positive catalogue metadata");
    setPacket(db,a,packet("clap",utf8(a),"generation0"));
    db.exec("UPDATE plugin_scan_cache SET stamp=upper(stamp)");
    check(!readCatalogue(f.config.registry,f.error),"noncanonical or corrupt stored stamp refuses hydration");
    db.exec("UPDATE plugin_scan_cache SET stamp=lower(stamp)");
    db.exec("DROP TRIGGER scan_failure_block_update; DROP TRIGGER scan_failure_block_insert; "
        "DROP TRIGGER scan_failure_block_delete; DROP TRIGGER scan_failure_success_insert; DROP TABLE plugin_scan_failures");
    const auto early=readCatalogue(f.config.registry,f.error);
    check(early && early->modules.size()==1 && db.execAndGet("SELECT count(*) FROM sqlite_master WHERE name='plugin_scan_failures'").getInt()==0,
        "early success-only v3 is readable without schema migration and still excludes manual blocks");
    db.exec("CREATE VIEW plugin_scan_failures AS SELECT format,path FROM plugin_scan_cache WHERE 0");
    check(!readCatalogue(f.config.registry,f.error),"unexpected failure schema object refuses rather than silently bypassing negative-state exclusion");
    db.exec("DROP VIEW plugin_scan_failures");
    db.exec("PRAGMA user_version=4");check(!readCatalogue(f.config.registry,f.error) && db.execAndGet("PRAGMA user_version").getInt()==4,
        "future registry is refused unchanged");
    db.exec("PRAGMA user_version=2");check(!readCatalogue(f.config.registry,f.error) && db.execAndGet("PRAGMA user_version").getInt()==2,
        "lowered version on a v3 schema is refused without owner migration");
    db.exec("PRAGMA user_version=3; PRAGMA application_id=1");
    check(!readCatalogue(f.config.registry,f.error) && db.execAndGet("PRAGMA application_id").getInt()==1,"foreign registry is refused unchanged");
}
void legacyClassification() {
    test::TempDirectory temp("scan_catalogue","legacy");std::string error;
    CatalogueReadStatus status=CatalogueReadStatus::Ready;
    const auto missing=temp.path()/"missing.sqlite";
    check(!readCatalogue(missing,error,{},&status) && status==CatalogueReadStatus::Missing && !fs::exists(missing),
        "missing registry is typed distinctly without file creation");
    for(const int version:{1,2}) {
        const auto file=temp.path()/("legacy-"+std::to_string(version)+".sqlite");
        test::createLegacyPluginRegistry(file,version);const auto beforeBytes=read(file);
        check(!readCatalogue(file,error,{},&status) && status==CatalogueReadStatus::Legacy && read(file)==beforeBytes,
            "genuine historical v1/v2 schema is typed Legacy with unchanged database bytes");
        SQLite::Database db(utf8(file),SQLite::OPEN_READWRITE);
        check(db.execAndGet("PRAGMA user_version").getInt()==version &&
            db.execAndGet("SELECT count(*) FROM sqlite_master WHERE name='plugin_scan_cache'").getInt()==0,
            "legacy read creates no cache or schema migration");
        error="untouched";status=CatalogueReadStatus::Ready;bool refused=false;
        {audio::CallbackScope callback;refused=!readCatalogue(file,error,{},&status);}
        check(refused && error=="untouched" && status==CatalogueReadStatus::Ready,
            "callback refusal leaves typed status and diagnostics untouched");
        db.exec("PRAGMA application_id=1");
        check(!readCatalogue(file,error,{},&status) && status==CatalogueReadStatus::Invalid,
            "foreign legacy-looking schema is Invalid rather than permission to discover");
        db.exec("PRAGMA application_id="+std::to_string(PluginRegistry::kApplicationId));
        db.exec("PRAGMA user_version=4");
        check(!readCatalogue(file,error,{},&status) && status==CatalogueReadStatus::Invalid,
            "future registry is Invalid even with compatible-looking old columns");
        db.exec("PRAGMA user_version="+std::to_string(version));
        db.exec("ALTER TABLE plugin_routes RENAME COLUMN route TO invalid_route");
        check(!readCatalogue(file,error,{},&status) && status==CatalogueReadStatus::Invalid,
            "corrupt legacy route schema cannot enable discovery");
    }
    const auto missingBlocks=temp.path()/"bad-v2.sqlite";test::createLegacyPluginRegistry(missingBlocks,2);
    {SQLite::Database db(utf8(missingBlocks),SQLite::OPEN_READWRITE);db.exec("DROP TABLE plugin_scan_blocks");}
    check(!readCatalogue(missingBlocks,error,{},&status) && status==CatalogueReadStatus::Invalid,
        "version-two quarantine table is mandatory for legacy discovery admission");
    const auto invalid=temp.path()/"invalid.sqlite";write(invalid,"not sqlite");
    check(!readCatalogue(invalid,error,{},&status) && status==CatalogueReadStatus::Invalid,
        "corrupt existing file is Invalid rather than Missing or Legacy");
}
void coherentReads(const fs::path& self) {
    Fixture f(self);const auto a=f.populate("a.clap"),b=f.populate("b.clap");f.removeHelper();
    SQLite::Database writer(utf8(f.config.registry),SQLite::OPEN_READWRITE);writer.setBusyTimeout(2000);
    writer.exec("BEGIN IMMEDIATE");setPacket(writer,a,packet("clap",utf8(a),"generation1"));setPacket(writer,b,packet("clap",utf8(b),"generation1"));
    auto snapshot=readCatalogue(f.config.registry,f.error);
    check(snapshot && snapshot->modules.size()==2 && snapshot->modules[0].plugins.front().version=="generation0" &&
        snapshot->modules[1].plugins.front().version=="generation0","read transaction sees preceding complete state while another owner holds uncommitted replacements");
    writer.exec("COMMIT");snapshot=readCatalogue(f.config.registry,f.error);
    check(snapshot && snapshot->modules[0].plugins.front().version=="generation1" && snapshot->modules[1].plugins.front().version=="generation1",
        "next snapshot sees both committed replacements together");
    std::atomic<bool> start{false};std::atomic<unsigned> writes{0};std::string writerError;
    std::thread changing([&]{try {
        SQLite::Database connection(utf8(f.config.registry),SQLite::OPEN_READWRITE);connection.setBusyTimeout(2000);
        while(!start.load())std::this_thread::yield();
        for(unsigned i=2;i<22;++i){SQLite::Transaction transaction(connection);const auto version="generation"+std::to_string(i);
            setPacket(connection,a,packet("clap",utf8(a),version));setPacket(connection,b,packet("clap",utf8(b),version));
            transaction.commit();++writes;std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    }catch(const std::exception& e){writerError=e.what();}});
    start=true;bool coherent=true;
    for(unsigned i=0;i<40;++i){const auto current=readCatalogue(f.config.registry,f.error);coherent=coherent && current && current->modules.size()==2 &&
        current->modules[0].plugins.front().version==current->modules[1].plugins.front().version;}
    changing.join();check(coherent && writerError.empty() && writes==20,"concurrent owner commits never yield a mixed-generation catalogue snapshot");
}
int entry(const std::vector<std::string>& args){
    if(args.size()==4 && args[1]=="--scan")return helper(args);
    try{const auto self=fs::absolute(native(args[0]));snapshotWithoutHelper(self);exclusions(self);corruptionAndCompatibility(self);legacyClassification();coherentReads(self);}
    catch(const std::exception& e){++failures;std::printf("EXCEPTION: %s\n",e.what());}
    std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;
}
} // namespace
#if defined(_WIN32)
int wmain(int argc,wchar_t** argv){std::vector<std::string> args;for(int i=0;i<argc;++i)args.push_back(utf8(fs::path(argv[i])));return entry(args);}
#else
int main(int argc,char** argv){std::vector<std::string> args;for(int i=0;i<argc;++i)args.emplace_back(argv[i]);return entry(args);}
#endif
