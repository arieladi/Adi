// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/plugin_scanner.hpp"
#include "adi/audio/io_audit.hpp"
#include "juce/clap_host.hpp"
#include "temp_directory.hpp"
#include <nlohmann/json.hpp>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <new>
#include <thread>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
namespace {std::atomic<unsigned> allocations{0};}
namespace {
// MSVC's Debug CRT (_ITERATOR_DEBUG_LEVEL 2) allocates an iterator proxy whenever a
// std::string, std::vector or std::filesystem::path is constructed, so an empty result
// returned from the callback path costs one allocation per container there and none on
// the other CRTs. The callback check allows exactly what the empty results cost on this
// CRT, measured once before its baseline is read.
template<class Result> unsigned emptyResultCost(){const auto before=allocations.load();{adi::audio::CallbackScope scope;Result empty;(void)empty;}return allocations.load()-before;}
}
#if defined(__GNUC__) && !defined(__clang__)
#define ADI_TEST_NOINLINE __attribute__((noinline))
#else
#define ADI_TEST_NOINLINE
#endif
ADI_TEST_NOINLINE void* operator new(std::size_t n){if(adi::audio::onAudioThread)++allocations;if(void* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
ADI_TEST_NOINLINE void* operator new[](std::size_t n){return operator new(n);}
ADI_TEST_NOINLINE void operator delete(void* p) noexcept{std::free(p);}
ADI_TEST_NOINLINE void operator delete[](void* p) noexcept{operator delete(p);}
ADI_TEST_NOINLINE void operator delete(void* p,std::size_t) noexcept{operator delete(p);}
ADI_TEST_NOINLINE void operator delete[](void* p,std::size_t) noexcept{operator delete(p);}
#undef ADI_TEST_NOINLINE
namespace {
using namespace adi;using namespace adi::scan;namespace fs=std::filesystem;using Json=nlohmann::json;
int checks=0,failures=0;
void check(bool ok,const char* label){++checks;if(!ok){++failures;std::printf("FAIL: %s\n",label);}}
std::string utf8(const fs::path& path){const auto bytes=path.u8string();return {bytes.begin(),bytes.end()};}
fs::path native(const std::string& value){return fs::path(std::u8string(value.begin(),value.end()));}
std::int64_t clock() noexcept{return 123;}
Plugin value(){return {"org.adi.id","caf\xc3\xa9\nquoted \" name","vendor","1.25","instrument,stereo","",true};}
struct Fixture {
    test::TempDirectory temp{"plugin_scanner","packet"};
    std::string error;Config config;
    std::unique_ptr<PluginRegistry> registry;
    Fixture(){config.registry=temp.path()/"registry.sqlite";config.executable=native(ADI_SCAN_HELPER);config.clock=clock;
        registry=PluginRegistry::open(config.registry,error);if(!registry)throw std::runtime_error(error);}
    fs::path candidate(const std::string& name){const auto path=temp.path()/native(name);std::ofstream(path)<<"candidate";return path;}
    std::unique_ptr<Scanner> scanner(){auto result=Scanner::open(config,error);if(!result)throw std::runtime_error(error);return result;}
    fs::path bundle(const std::string& mode,bool staticCrash=false) {
        const auto path=temp.path()/native(mode+".clap");
#if defined(__APPLE__)
        const auto binary=path/"Contents"/"MacOS"/native(mode);fs::create_directories(binary.parent_path());
#else
        const auto binary=path;
#endif
        fs::copy_file(native(staticCrash?ADI_SCAN_STATIC_FIXTURE:ADI_SCAN_FIXTURE),binary);return path;
    }
};
void protocol() {
    Fixture f;const auto path=utf8(f.temp.path()/"metadata.clap");const std::string format="clap";
    const std::vector<Plugin> plugins{value()};const auto encoded=encodeMetadata(format,path,plugins,f.error);
    const auto decoded=encoded?decodeMetadata(*encoded,format,path,f.error):std::nullopt;
    check(decoded && decoded->size()==1 && decoded->front().name==plugins.front().name && decoded->front().instrument && decoded->front().data.empty(),"strict metadata roundtrips exact UTF-8/escape values and types");if(!encoded)return;
    auto wrongFormat=Json::parse(*encoded);wrongFormat["format"]="vst3";
    check(!decodeMetadata(*encoded,"vst3",path,f.error) && !decodeMetadata(wrongFormat.dump(),format,path,f.error) &&
        !decodeMetadata(*encoded,format,path+"different",f.error),"reply is bound to both format and canonical request path");
    auto root=Json::parse(*encoded);bool invalid=true;
    for(const auto& schema:std::array<Json,4>{0,2,1.0,"1"}){auto changed=root;changed["schema"]=schema;invalid=invalid && !decodeMetadata(changed.dump(),format,path,f.error);}
    check(invalid,"future/zero/floating/text schema values refuse");
    auto changed=root;changed["unknown"]=true;invalid=!decodeMetadata(changed.dump(),format,path,f.error);
    changed=root;changed["plugins"][0]["unknown"]=true;invalid=invalid && !decodeMetadata(changed.dump(),format,path,f.error);
    changed=root;changed["plugins"][0].erase("vendor");invalid=invalid && !decodeMetadata(changed.dump(),format,path,f.error);
    check(invalid,"unknown/missing root or descriptor fields refuse");
    changed=root;changed["plugins"].push_back(changed["plugins"][0]);
    check(!decodeMetadata(changed.dump(),format,path,f.error),"duplicate descriptor ids refuse a whole otherwise valid reply");
    changed=root;changed["plugins"][0]["instrument"]=1;invalid=!decodeMetadata(changed.dump(),format,path,f.error);
    changed=root;changed["plugins"][0]["version"]=1.25;invalid=invalid && !decodeMetadata(changed.dump(),format,path,f.error);
    changed=root;changed["plugins"][0]["data"]="unexpected";invalid=invalid && !decodeMetadata(changed.dump(),format,path,f.error);
    check(invalid,"descriptor numeric/boolean/string and format-specific types are exact");
    std::string duplicate=*encoded;const auto schema=duplicate.find("\"schema\":1");duplicate.replace(schema,10,"\"schema\":1,\"schema\":1");
    auto escapedDuplicate=*encoded;escapedDuplicate.replace(schema,10,"\"schema\":1,\"sche\\u006da\":1");
    check(!decodeMetadata(duplicate,format,path,f.error) && !decodeMetadata(escapedDuplicate,format,path,f.error),"duplicate JSON keys, including escaped spellings, never use last-value-wins parsing");
    check(!decodeMetadata(encoded->substr(0,encoded->size()-1),format,path,f.error) && !decodeMetadata(*encoded+"x",format,path,f.error),"truncated/trailing malformed metadata never yields a valid prefix");
    changed=root;changed["plugins"][0]["name"]=std::string(65537,'x');
    check(!decodeMetadata(changed.dump(),format,path,f.error),"individual wire strings are preflight bounded");
    check(!decodeMetadata(std::string(kMaximumPacket+1,' '),format,path,f.error),"whole metadata byte limit precedes JSON parsing");
    const auto exactPacket=*encoded+std::string(kMaximumPacket-encoded->size(),' ');
    check(decodeMetadata(exactPacket,format,path,f.error).has_value(),"packet exactly at its wire byte limit remains valid");
    check(!decodeMetadata(std::string(1000,'[')+"0"+std::string(1000,']'),format,path,f.error),"deep hostile nesting refuses before parser recursion");
    auto bad=plugins;bad[0].name="bad\xff";invalid=!encodeMetadata(format,path,bad,f.error);
    bad=plugins;bad[0].id="";invalid=invalid && !encodeMetadata(format,path,bad,f.error);
    bad=plugins;bad[0].name=std::string("a\0b",3);invalid=invalid && !encodeMetadata(format,path,bad,f.error);
    check(invalid,"encoder refuses malformed UTF-8/NUL/empty identity without partial packet");
    const auto zero=encodeMetadata(format,path,{},f.error);check(zero && decodeMetadata(*zero,format,path,f.error)->empty(),"explicit empty CLAP factory metadata is valid");
    std::string large="{\"schema\":1,\"format\":\"clap\",\"path\":"+Json(path).dump()+",\"plugins\":[";
    std::size_t boundary=0;
    for(std::size_t index=0;index<=kMaximumPlugins;++index) {
        if(index==kMaximumPlugins)boundary=large.size();
        if(index!=0)large+=',';
        large+="{\"id\":\""+std::to_string(index)+"\",\"name\":\"\",\"vendor\":\"\",\"version\":\"\",\"features\":\"\",\"instrument\":false,\"data\":\"\"}";
    }
    const auto full=decodeMetadata(large.substr(0,boundary)+"]}",format,path,f.error);
    check(full && full->size()==kMaximumPlugins,"full 65536-descriptor reply remains within parser/count bounds");
    large+="]}";
    check(large.size()<kMaximumPacket && !decodeMetadata(large,format,path,f.error),"65537 valid unique descriptors refuse as a whole despite fitting the byte budget");
}
void vst3ClassIds() {
    Fixture f;
    const auto path=utf8(f.temp.path()/"classes.vst3");
    auto first=value();first.id="legacy-one";first.data="native description one";
    auto second=first;second.id="legacy-two";second.data="native description two";
    const auto legacy=encodeMetadata("vst3",path,{first,second},f.error);
    if(!legacy)throw std::runtime_error(f.error);
    const auto old=decodeMetadata(*legacy,"vst3",path,f.error);
    check(old && old->size()==2 && old->front().id==first.id && old->front().classId.empty() &&
          Json::parse(*legacy)["schema"]==kPacketSchema,
          "legacy VST3 packets retain their existing UID and empty class ID");
    first.classId="00112233445566778899aabbccddeeff";
    second.classId="ffeeddccbbaa99887766554433221100";
    const auto next=encodeMetadata("vst3",path,{first,second},f.error);
    if(!next)throw std::runtime_error(f.error);
    const auto parsed=Json::parse(*next);
    const auto newer=decodeMetadata(*next,"vst3",path,f.error);
    check(newer && newer->size()==2 && newer->at(0).id==first.id && newer->at(1).id==second.id &&
          newer->at(0).classId==first.classId && newer->at(1).classId==second.classId &&
          parsed["schema"]==kVst3ClassIdPacketSchema,
          "a VST3 module packet retains distinct class IDs beside its legacy UIDs");
    auto malformed=parsed;malformed["plugins"][0]["class_id"]="0022";
    auto absent=parsed;absent["plugins"][0].erase("class_id");
    auto duplicate=parsed;duplicate["plugins"][1]["class_id"]=first.classId;
    check(!decodeMetadata(malformed.dump(),"vst3",path,f.error) &&
          !decodeMetadata(absent.dump(),"vst3",path,f.error) &&
          !decodeMetadata(duplicate.dump(),"vst3",path,f.error),
          "class-ID packets reject malformed, missing and duplicate IDs as a whole");
    second.classId.clear();
    check(!encodeMetadata("vst3",path,{first,second},f.error),
          "the scanner cannot publish a partial set of class IDs");
    first.data.clear();
    const auto clap=utf8(f.temp.path()/"classes.clap");
    check(!encodeMetadata("clap",clap,{first},f.error),"VST3 class IDs cannot enter a CLAP packet");
}
int fakeHelper(const std::vector<std::string>& args) {
    const auto mode=native(args[3]).stem().u8string();const std::string name(mode.begin(),mode.end());
    if(name=="unsupported")return 78;
    if(name=="reply-flood"){const std::string flood(32768,'x');std::fwrite(flood.data(),1,flood.size(),stdout);return 0;}
    std::string error;auto packet=encodeMetadata(args[2],args[3],{value()},error);if(!packet)return 65;
    if(name=="wrong-path"){auto root=Json::parse(*packet);root["path"]=args[3]+"elsewhere";packet=root.dump();}
    if(name=="bad-reply")packet="{not JSON";
    if(name=="trailing-reply")*packet+="unexpected";
    std::fwrite(packet->data(),1,packet->size(),stdout);
    return name=="nonzero-reply"?23:0;
}
void protocolProcess(const fs::path& self) {
    Fixture f;f.config.executable=self;auto scanner=f.scanner();
    const auto good=f.candidate("good-reply.clap");const auto goodResult=scanner->probe("clap",good);
    check(goodResult.state==State::Ready && goodResult.plugins.size()==1 && f.registry->scanBlocks(f.error)->empty(),"actual child reply validates before exact ticket cleanup");
    bool failed=true;for(const auto* name:std::array{"bad-reply","wrong-path","trailing-reply","nonzero-reply"}) {
        const auto result=scanner->probe("clap",f.candidate(std::string(name)+".clap"));
        failed=failed && result.state==State::Failed && result.plugins.empty();
    }
    check(failed && f.registry->scanBlocks(f.error)->empty(),"bad identity/malformed/trailing/nonzero replies are negative results, never quarantine");
    const auto unsupported=scanner->probe("clap",f.candidate("unsupported.clap"));
    check(unsupported.state==State::Unavailable && f.registry->scanBlocks(f.error)->empty(),"unsupported helper deployment abandons without blaming the candidate");
    f.config.process.maximumOutput=4096;auto bounded=f.scanner();
    const auto flood=bounded->probe("clap",f.candidate("reply-flood.clap"));
    check(flood.state==State::Failed && flood.plugins.empty() && f.registry->scanBlocks(f.error)->empty(),"child output limit becomes a durable named negative result without quarantine");
    struct Late {
        PluginRegistry* registry;bool cancel=false;std::atomic<bool>* cancellation=nullptr;
        static bool validate(const std::vector<Plugin>&,const std::string& path,void* raw,std::string& error) {
            auto& late=*static_cast<Late*>(raw);
            if(late.cancel){late.cancellation->store(true);return true;}
            return late.registry->blockScan("clap",path,"explicit later block",124,error);
        }
    } late{f.registry.get()};
    const auto stale=scanner->probe("clap",good,Late::validate,&late);
    check(stale.state==State::Quarantined && stale.plugins.empty(),"late native block refuses successful metadata publication");
    const auto rejected=f.candidate("owner-reject.clap");
    const auto refuse=+[](const std::vector<Plugin>&,const std::string&,void*,std::string& error){error="owner format rejection";return false;};
    check(scanner->probe("clap",rejected,refuse).state==State::Failed,"format-specific validation precedes quarantine removal");
    std::atomic<bool> cancel{false};f.config.process.cancel=&cancel;auto cancellable=f.scanner();
    late.cancel=true;late.cancellation=&cancel;
    const auto cancelled=cancellable->probe("clap",f.candidate("late-cancel.clap"),Late::validate,&late);
    check(cancelled.state==State::Cancelled && cancelled.plugins.empty(),"cancellation during final format validation abandons unpublished metadata");
    const auto before=f.registry->scanBlocks(f.error)->size();
    check(cancellable->probe("clap",fs::path("does-not-exist")).state==State::Cancelled && f.registry->scanBlocks(f.error)->size()==before,"already-cancelled scan does not resolve a path or create pending state");
    check(scanner->probe("au",good).state==State::Invalid && scanner->probe("vst3",good).state==State::Invalid,"unknown formats and missing VST3 validators refuse before launch");
}
void realLibraries() {
    Fixture f;auto scanner=f.scanner();const auto good=f.bundle("valid caf\xc3\xa9 and spaces");
    const auto result=scanner->probe("clap",good);
    check(result.state==State::Ready && result.plugins.size()==2 && result.plugins.front().id=="org.adi.scan.first" && result.plugins.front().instrument && !result.plugins.back().instrument && result.plugins.front().name=="Scan caf\xc3\xa9","real CLAP factory returns exact multiple descriptors and feature classification from the child");
    std::ifstream marker(native(utf8(good)+".pids"));long long pid=0;bool seen=false,foreign=true;
    while(marker>>pid){seen=true;foreign=foreign && pid!=static_cast<long long>(test::processId());}
    check(seen && foreign,"every actual plug-in init runs under a different PID");
    const auto initBytes=fs::file_size(native(utf8(good)+".pids"));
    const auto warm=scanner->probe("clap",good);scanner.reset();scanner=f.scanner();
    const auto reopened=scanner->probe("clap",good);
    check(warm.state==State::Ready && warm.cacheHit && reopened.state==State::Ready && reopened.cacheHit &&
          fs::file_size(native(utf8(good)+".pids"))==initBytes,
          "real CLAP module has no further init calls over three unchanged probes including Scanner reopen");
    const auto noisy=scanner->probe("clap",f.bundle("noisy"));
    check(noisy.state==State::Ready && noisy.plugins.size()==2,"plug-in stdout/stderr chatter cannot corrupt reserved metadata channel");
    const auto empty=f.bundle("no-classes");const auto empty1=scanner->probe("clap",empty);
    const auto emptyLog=native(utf8(empty)+".pids");const auto emptyBytes=fs::file_size(emptyLog);
    const auto empty2=scanner->probe("clap",empty);
    check(empty1.state==State::Failed && empty2.state==State::Failed && empty2.failureCacheHit &&
        empty2.failureStrikes==0 && fs::file_size(emptyLog)==emptyBytes && f.registry->scanBlocks(f.error)->empty(),
        "real CLAP factory with zero classes is negative-cached, not quarantined or initialized again");
    bool contained=true;for(const auto* mode:std::array{"init-crash","factory-crash","descriptor-crash","deinit-crash","unload-crash","duplicate","too-many","oversized","bad-utf8","null-descriptor"}) {
        const auto failure=scanner->probe("clap",f.bundle(mode));contained=contained && failure.state==State::Failed && failure.plugins.empty();
    }
    check(contained,"init/factory/descriptor/deinit/unload crashes and invalid descriptors are contained with no partial publication");
    check(scanner->probe("clap",f.bundle("static-crash",true)).state==State::Failed,"static library initializer crash is contained before CLAP entry init");
    f.config.process.timeout=std::chrono::milliseconds(80);auto shortScan=f.scanner();
    const auto hang=f.bundle("init-hang");const auto deinit=f.bundle("deinit-hang");
    check(shortScan->probe("clap",hang).state==State::Failed && shortScan->probe("clap",deinit).state==State::Failed,"actual init and deinit hangs hit the process deadline without parent hangs");
    check(shortScan->probe("clap",hang).failureStrikes==2,
        "second unchanged actual CLAP timeout reaches the two-strike threshold");
    const auto log=native(utf8(hang)+".pids");const bool existed=fs::exists(log);
    const auto before=existed?fs::file_size(log):0;
    check(shortScan->probe("clap",hang).state==State::Quarantined && fs::exists(log)==existed &&
        (!existed || fs::file_size(log)==before),"quarantined library is never reloaded on the next probe, including a timeout before init");
    fs::create_directory(f.temp.path()/"alias");const auto alias=f.temp.path()/"alias"/".."/good.filename();
    f.registry->blockScan("clap",utf8(fs::canonical(good)),"disabled",125,f.error);
    check(scanner->probe("clap",alias).state==State::Quarantined,"native canonical path prevents dot-component aliases bypassing quarantine");
    f.registry->unblockScan("clap",utf8(fs::canonical(good)),f.error);
    check(scanner->probe("clap",good).state==State::Ready,"explicit re-enable allows a new isolated scan");
}
void reservedCandidateExits() {
    Fixture f;device::ClapHost host;
    const auto exit74=f.bundle("a-init-exit74"),exit75=f.bundle("b-init-exit75"),
        healthy=f.bundle("z-healthy");
    const auto first=host.scan({utf8(f.temp.path())},f.config);
    check(first.complete && first.failures.size()==2 && host.plugins().size()==2 && host.libraryCount()==0,
        "plug-in exits 74 and 75 do not stop discovery of a healthy sibling");
    auto scanner=f.scanner();
    for(const auto& module:{exit74,exit75}) {
        const auto result=scanner->probe("clap",module);
        const auto path=native(utf8(module)+".pids");
        std::ifstream markers(path);long long pid=0,extra=0;
        check(result.state==State::Failed && result.failureCacheHit && result.failureStrikes==0 &&
            (markers>>pid) && pid!=static_cast<long long>(test::processId()) && !(markers>>extra),
            "plug-in reserved exit is a stable rejected result with exactly one child init");
    }
    check(fs::exists(native(utf8(healthy)+".pids")) && f.registry->scanBlocks(f.error)->empty(),
        "healthy sibling ran and candidate exits made no pending or manual block");
}
void helperStartFailures() {
    Fixture f;
    device::ClapHost host;
    const auto healthy=f.bundle("working");
    const std::vector<std::string> roots{utf8(f.temp.path())};
    check(host.scan(roots,f.config).complete && host.plugins().size()==2,"fixture adopts working metadata");
    const auto marker=native(utf8(healthy)+".pids");
    const auto initBytes=fs::file_size(marker);

    auto unavailable=f.config;
    unavailable.executable=f.temp.path()/"missing-helper";
    State reason{};
    check(!Scanner::open(unavailable,f.error,&reason) && reason==State::HelperCouldNotStart,
        "the scanner identifies a missing helper before candidate admission");
    auto wrongRegistry=f.config;
    wrongRegistry.registry="relative-registry.sqlite";
    check(!Scanner::open(wrongRegistry,f.error,&reason) && reason==State::Unavailable,
        "a different configuration failure retains its own category");

    unavailable.executable=f.candidate("invalid-helper.exe");
#if !defined(_WIN32)
    fs::permissions(unavailable.executable,fs::perms::owner_exec,fs::perm_options::add);
#endif
    auto scanner=Scanner::open(unavailable,f.error);
    if(!scanner)throw std::runtime_error(f.error);
    for(int attempt=0;attempt<2;++attempt) {
        const auto result=scanner->probe("clap",healthy);
        check(result.state==State::HelperCouldNotStart && !result.failureCacheHit &&
            result.failureStrikes==0 && result.plugins.empty(),"an OS launch refusal is never a candidate failure");
    }
    const auto report=host.scan(roots,unavailable);
    check(!report.complete && !report.cancelled && report.failures.size()==1 &&
        report.failures.front().state==State::HelperCouldNotStart &&
        !report.failures.front().path.empty() && report.failures.front().error=="scanner helper could not start",
        "the CLAP batch preserves the typed launch refusal and its explanation");
    auto blocks=f.registry->scanBlocks(f.error);
    check(blocks && blocks->empty() && host.plugins().size()==2 && host.libraryCount()==0 &&
        fs::file_size(marker)==initBytes,"helper launch refusal leaves no pending ticket or parent module load");
    fs::remove(unavailable.executable);
    check(scanner->probe("clap",healthy).state==State::HelperCouldNotStart,
        "a helper removed after Scanner::open has the same typed deployment state");
    check(host.scan(roots,f.config).complete && host.plugins().size()==2 &&
        fs::file_size(marker)==initBytes,"the working helper can use the prior accepted catalogue");
}
void hostAndFailurePolicy() {
    Fixture f;device::ClapHost host;
    const auto healthy=f.bundle("healthy");(void)healthy;
    auto report=host.scan({utf8(f.temp.path())},f.config);
    check(report.complete && report.failures.empty() && host.plugins().size()==2 && host.libraryCount()==0,"ClapHost browser scan uses child metadata and leaves parent library cache empty");
    f.bundle("init-crash");report=host.scan({utf8(f.temp.path())},f.config);
    check(report.complete && report.failures.size()==1 && host.plugins().size()==2 && host.libraryCount()==0,"one bad library does not hide healthy siblings or load one in parent");
    std::ifstream markers(native(utf8(healthy)+".pids"));long long pid=0;bool seen=false,foreign=true;
    while(markers>>pid){seen=true;foreign=foreign && pid!=static_cast<long long>(test::processId());}
    check(seen && foreign,"all browser discovery init records belong to children even across repeated scans");
    auto unavailable=f.config;unavailable.executable=f.temp.path()/"missing-helper";
    report=host.scan({utf8(f.temp.path())},unavailable);
    check(!report.complete && host.plugins().size()==2 && host.libraryCount()==0,"missing helper has no in-process fallback and retains previous complete catalogue");
    auto blocked=f.registry->scanBlocks(f.error);check(blocked && blocked->empty(),"missing helper creates no block and preserves a first crash's retry eligibility");
    f.registry->unblockScan("clap",utf8(fs::canonical(f.temp.path()/"init-crash.clap")),f.error);
    std::atomic<bool> cancel{false};auto cancelled=f.config;cancelled.process.cancel=&cancel;
    const auto hang=f.bundle("init-hang");(void)hang;
    std::thread cancellation([&]{std::this_thread::sleep_for(std::chrono::milliseconds(100));cancel=true;});
    report=host.scan({utf8(f.temp.path())},cancelled);cancellation.join();
    check(!report.complete && report.cancelled && host.plugins().size()==2 && host.libraryCount()==0,"live scan cancellation retains previous catalogue without parent loading");
    const auto rows=f.registry->scanBlocks(f.error);bool noCancelledBlock=rows.has_value();
    if(rows)for(const auto& row:*rows)noCancelledBlock=noCancelledBlock && row.path.find("init-hang")==row.path.npos;
    check(noCancelledBlock,"cancelled library attempt is explicitly abandoned instead of quarantined");
    const auto scanner=f.scanner();const std::string format="clap",path=utf8(healthy),packet="{}",sentinel="untouched";const std::vector<Plugin> plugins{value()};f.error=sentinel;
    const std::vector<std::string> roots{utf8(f.temp.path())},none{};
    // The refused calls hand back an empty Result, path and Report and two empty string lists.
    const auto cost=emptyResultCost<Result>()+emptyResultCost<fs::path>()+emptyResultCost<Report>()+2U*emptyResultCost<std::vector<std::string>>();
    const auto baseline=allocations.load();bool callback=false;
    {audio::CallbackScope scope;
        const auto refused=scanner->probe(format,healthy);
        callback=refused.state==State::CallbackRefused && !decodeMetadata(packet,format,path,f.error) && !encodeMetadata(format,path,plugins,f.error) &&
            !Scanner::open(f.config,f.error) && !scanner->retry(format,healthy,f.error) && defaultScannerExecutable().empty() && !host.scan(none,f.config).complete &&
            device::ClapHost::findBundles(roots).empty() && device::ClapHost::defaultSearchPaths().empty();
    }
    check(callback && f.error==sentinel && allocations==baseline+cost && audio::callbackFileIo==0 && host.plugins().size()==2,"scanner/protocol/host callback guards precede copying, filesystem, SQL and catalogue mutation");
}
int entry(const std::vector<std::string>& arguments) {
    if(arguments.size()==4 && arguments[1]=="--scan")return fakeHelper(arguments);
    if(arguments.size()!=1)return 64;
    try {protocol();vst3ClassIds();protocolProcess(fs::absolute(native(arguments[0])));realLibraries();reservedCandidateExits();helperStartFailures();hostAndFailurePolicy();}
    catch(const std::exception& error){++failures;std::printf("EXCEPTION: %s\n",error.what());}
    std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;
}
} // namespace
#if defined(_WIN32)
int wmain(int argc,wchar_t** argv) {
    std::vector<std::string> arguments;arguments.reserve(static_cast<std::size_t>(argc));
    for(int i=0;i<argc;++i){const int size=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,argv[i],-1,nullptr,0,nullptr,nullptr);if(size<=0)return 64;
        std::string value(static_cast<std::size_t>(size),'\0');if(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,argv[i],-1,value.data(),size,nullptr,nullptr)!=size)return 64;
        value.pop_back();arguments.push_back(std::move(value));}
    return entry(arguments);
}
#else
int main(int argc,char** argv){std::vector<std::string> arguments;for(int i=0;i<argc;++i)arguments.emplace_back(argv[i]);return entry(arguments);}
#endif
