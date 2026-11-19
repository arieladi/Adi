// SPDX-License-Identifier: GPL-3.0-or-later
// Exact DEV-25 cases 1..15 on Windows, through the production scanner helper.
// Case 16 is in the hosted readiness test; no scan success means audio Ready.
#include "juce/vst3_scan_host.hpp"
#include "juce/clap_host.hpp"
#include "adi/scan_stamp.hpp"
#include "adi/scan_catalogue.hpp"
#include "adi/media/blake3.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
#include <set>
#include <thread>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>

namespace {
namespace fs=std::filesystem;using namespace adi;using namespace adi::scan;
using Json=nlohmann::json;
std::string text(const fs::path& path){const auto bytes=path.u8string();return {bytes.begin(),bytes.end()};}
fs::path native(const std::string& value){return fs::path(std::u8string(value.begin(),value.end()));}
std::string read(const fs::path& path){std::ifstream in(path,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
std::string environment(const char* key){char* value=nullptr;std::size_t bytes=0;
    if(_dupenv_s(&value,&bytes,key)!=0)throw std::runtime_error("environment read");
    const std::string result=value?value:"";std::free(value);return result;}
void set(const char* key,const std::string& value){if(_putenv_s(key,value.c_str())!=0)throw std::runtime_error("environment write");}
struct Environment {
    std::map<std::string,std::string> saved;
    Environment(){for(const auto* key:{"ADI_SCAN_LOG","ADI_SCAN_ONE_CLASS","ADI_SCAN_VERSION_TWO","ADI_SCAN_FAIL"})saved[key]=environment(key);}
    ~Environment(){for(const auto& [key,value]:saved)(void)_putenv_s(key.c_str(),value.c_str());}
};
bool junction(const fs::path& link,const fs::path& target) {
    const auto print=fs::canonical(target).wstring(),substitute=L"\\??\\"+print;
    struct Data {DWORD tag=IO_REPARSE_TAG_MOUNT_POINT;WORD bytes=0,reserved=0;
        WORD substituteOffset=0,substituteBytes=0,printOffset=0,printBytes=0;wchar_t names[2048]{};} data;
    static_assert(offsetof(Data,names)==16);
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

bool nativeSearchLinks(const fs::path& parent) {
    const auto search = parent / "linked-search";
    const auto vendor = parent / "linked-vendor";
    fs::create_directory(search);
    fs::create_directory(vendor);
    const auto module = vendor / "Original.clap";
    std::ofstream(module) << "discovery fixture";
    const auto vstBundle = vendor / "Original.VST3";
    fs::create_directory(vstBundle);
    std::ofstream(vstBundle / "Nested.vst3") << "inside a bundle";
    const auto one = search / "first";
    const auto two = search / "second";
    const auto cycle = vendor / "cycle";
    if (!junction(one, vendor) || !junction(two, vendor) || !junction(cycle, search))
        throw std::runtime_error("discovery fixture junction unavailable");
    const std::vector<std::string> aliasRoots{text(search)};
    const std::vector<std::string> overlappingRoots{text(search), text(vendor), text(one)};
    const std::vector<std::string> clapExpected{text(fs::canonical(module))};
    const std::vector<std::string> vstExpected{text(fs::canonical(vstBundle))};
    const bool clapMatches = device::ClapHost::findBundles(aliasRoots) == clapExpected &&
                             device::ClapHost::findBundles(overlappingRoots) == clapExpected;
    const bool vstMatches = scan::findCandidates(aliasRoots, ".vst3") == vstExpected &&
                            scan::findCandidates(overlappingRoots, ".vst3") == vstExpected;
    if (!RemoveDirectoryW(cycle.c_str()) || !RemoveDirectoryW(two.c_str()) ||
        !RemoveDirectoryW(one.c_str()) || !fs::exists(module))
        throw std::runtime_error("discovery fixture junction cleanup failed");
    return clapMatches && vstMatches;
}

struct Receipt {
    std::map<std::string,unsigned> calls;std::set<unsigned> pids;
    explicit Receipt(const fs::path& file){std::ifstream in(file);std::string line;
        while(std::getline(in,line)){const auto comma=line.find(',');if(comma==line.npos)continue;
            pids.insert(static_cast<unsigned>(std::stoul(line.substr(0,comma))));++calls[line.substr(comma+1)];}}
    unsigned count(const char* name)const{const auto it=calls.find(name);return it==calls.end()?0:it->second;}
};
bool ended(unsigned pid){const auto handle=OpenProcess(SYNCHRONIZE,FALSE,pid);
    if(handle==nullptr)return GetLastError()==ERROR_INVALID_PARAMETER;
    const bool result=WaitForSingleObject(handle,0)==WAIT_OBJECT_0;CloseHandle(handle);return result;}
const char* name(State state){switch(state){case State::Ready:return "Ready metadata";case State::Quarantined:return "Quarantined";
    case State::Unavailable:return "Unavailable";case State::Cancelled:return "Cancelled";case State::Failed:return "Failed";
    case State::Invalid:return "Invalid";case State::CallbackRefused:return "CallbackRefused";}return "unknown";}
struct Sequence {
    test::TempDirectory temp{"scanner_cases","real_factory"};Environment restore;
    Config config;std::unique_ptr<Scanner> scanner;std::unique_ptr<PluginRegistry> registry;
    std::string error,helperHash;fs::path receipt=temp.path()/"factory.csv";
    int passed=0,failed=0;Json events=Json::array();
    Sequence(){
        config.executable=native(ADI_SCAN_HELPER);config.registry=temp.path()/"registry.sqlite";
        config.process.timeout=std::chrono::seconds(5);
        const auto digest=media::blake3File(config.executable);if(!digest)throw std::runtime_error("helper digest");helperHash=digest.hex;
        scanner=Scanner::open(config,error);registry=PluginRegistry::open(config.registry,error);
        if(!scanner || !registry)throw std::runtime_error(error);
        set("ADI_SCAN_LOG",text(receipt));set("ADI_SCAN_ONE_CLASS","0");set("ADI_SCAN_VERSION_TWO","0");set("ADI_SCAN_FAIL","0");
        std::printf("BUILD helper_blake3=%s fixture=%s registry=%s parent_pid=%lu\n",helperHash.c_str(),ADI_SCAN_MODULE,
            text(config.registry).c_str(),GetCurrentProcessId());
    }
    fs::path module(const char* name){const auto path=temp.path()/name;fs::copy_file(native(ADI_SCAN_MODULE),path);return path;}
    void changed(const fs::path& path){fs::last_write_time(path,fs::last_write_time(path)+std::chrono::seconds(3));}
    Result probe(const fs::path& path){std::vector<juce::PluginDescription> descriptions;
        return device::scanVst3File(*scanner,juce::String::fromUTF8(text(path).c_str()),descriptions);}
    std::string row(const fs::path& path){SQLite::Database db(text(config.registry),SQLite::OPEN_READONLY);
        SQLite::Statement q(db,"SELECT stamp,packet FROM plugin_scan_cache WHERE format='vst3' AND path=?");q.bind(1,text(fs::canonical(path)));
        return q.executeStep()?q.getColumn(0).getString()+q.getColumn(1).getString():"";}
    void check(int number,bool ok,const char* meaning,const fs::path& path,const Result& result){
        ok?++passed:++failed;const Receipt seen(receipt);const auto stamp=candidateStamp("vst3",fs::canonical(path),helperHash,error);
        Json classes=Json::array();for(const auto& plugin:result.plugins)classes.push_back({{"uid",plugin.id},{"version",plugin.version}});
        Json event{{"case",number},{"passed",ok},{"meaning",meaning},{"path",text(path)},
            {"stamp",stamp.value_or("")},{"state",name(result.state)},{"cacheHit",result.cacheHit},
            {"failureCacheHit",result.failureCacheHit},{"strikes",result.failureStrikes},{"diagnostic",result.error},
            {"classes",classes},{"factoryCalls",seen.calls},{"childPids",seen.pids}};
        events.push_back(event);std::printf("CASE %02d %s %s\n",number,ok?"PASS":"FAIL",event.dump().c_str());
    }
    void save(){
        const auto output=environment("ADI_SCANNER_CASE_EVIDENCE");if(output.empty())return;
        const auto path=native(output);fs::create_directories(path);
        std::ofstream(path/"cases-01-15.json")<<events.dump(2)<<'\n';
        fs::copy_file(receipt,path/"factory-calls.csv",fs::copy_options::overwrite_existing);
        // Completed logical contents; do not copy a live SQLite file apart from its WAL.
        SQLite::Database db(text(config.registry),SQLite::OPEN_READONLY);Json rows=Json::array();
        for(const auto* table:{"plugin_scan_cache","plugin_scan_failures","plugin_scan_blocks"}) {
            SQLite::Statement q(db,std::string("SELECT * FROM ")+table);
            while(q.executeStep()){Json object{{"table",table}};for(int i=0;i<q.getColumnCount();++i){const auto column=q.getColumn(i);
                object[q.getColumnName(i)]=column.isNull()?Json(nullptr):column.isInteger()?Json(column.getInt64()):Json(column.getString());}rows.push_back(std::move(object));}
        }
        std::ofstream(path/"registry-final-rows.json")<<rows.dump(2)<<'\n';
    }
};
bool run() {
    Sequence s;const auto a=s.module("A.vst3");auto result=s.probe(a);const Receipt cold(s.receipt);
    const auto original=result.plugins;
    s.check(1,result.state==State::Ready && original.size()==2 && cold.count("GetPluginFactory")==1 &&
        cold.count("createInstance")==2 && !cold.pids.contains(GetCurrentProcessId()),"real factory discovers two classes only inside child",a,result);
    if(original.size()!=2)return false;
    result=s.probe(a);s.scanner.reset();s.scanner=Scanner::open(s.config,s.error);if(!s.scanner)throw std::runtime_error(s.error);
    result=s.probe(a);
    s.check(2,result.state==State::Ready && result.cacheHit && Receipt(s.receipt).count("GetPluginFactory")==1,
        "two warm reads across reopen make no factory call",a,result);
    const auto oldA=s.row(a);const auto b=s.module("B.vst3");result=s.probe(b);const auto retainedB=s.row(b);
    s.check(3,result.state==State::Ready && result.plugins.size()==2 && s.row(a)==oldA && s.probe(a).cacheHit &&
        Receipt(s.receipt).count("GetPluginFactory")==2,"one new module leaves prior row and factory count unchanged",b,result);
    s.changed(a);result=s.probe(a);
    s.check(4,result.state==State::Ready && !result.cacheHit && s.row(b)==retainedB && Receipt(s.receipt).count("GetPluginFactory")==3,
        "timestamp-only change reprobes one module",a,result);
    set("ADI_SCAN_ONE_CLASS","1");s.changed(a);result=s.probe(a);
    const auto shrunk=s.row(a);std::string decodeError;const auto snapshot=readCatalogue(s.config.registry,decodeError);
    const auto* module=snapshot?snapshot->find("vst3",text(fs::canonical(a))):nullptr;
    s.check(5,result.state==State::Ready && result.plugins.size()==1 && result.plugins[0].id==original[0].id &&
        module && module->plugins.size()==1 && module->plugins[0].id==original[0].id && s.row(b)==retainedB,
        "class count change replaces entire cached module including removed class",a,result);
    set("ADI_SCAN_VERSION_TWO","1");s.changed(a);result=s.probe(a);
    s.check(6,result.state==State::Ready && result.plugins.size()==1 && result.plugins[0].id==original[0].id &&
        result.plugins[0].version=="2.0.0" && s.row(a)!=shrunk && s.row(b)==retainedB,
        "version change retains same exact saved UID and replaces metadata",a,result);
    const auto oldTime=fs::last_write_time(a);const auto beforeSize=fs::file_size(a);const Receipt beforeBytes(s.receipt);
    {std::ofstream append(a,std::ios::binary|std::ios::app);append.put('\0');}
    fs::last_write_time(a,oldTime);result=s.probe(a);
    s.check(7,result.state==State::Ready && !result.cacheHit && fs::file_size(a)==beforeSize+1 && fs::last_write_time(a)==oldTime &&
        Receipt(s.receipt).count("GetPluginFactory")==beforeBytes.count("GetPluginFactory")+1 && s.row(b)==retainedB,
        "owned PE module size changes with restored time trigger real factory reprobe",a,result);
    set("ADI_SCAN_FAIL","1");const auto rejected=s.module("Rejected.vst3");result=s.probe(rejected);
    s.check(8,result.state==State::Failed && result.failureStrikes==0 && s.registry->scanBlocks(s.error)->empty() &&
        s.row(rejected).empty() && s.row(b)==retainedB,"null factory is negative without quarantine or sibling damage",rejected,result);
    const Receipt negative(s.receipt);result=s.probe(rejected);
    s.check(9,result.state==State::Failed && result.failureCacheHit && Receipt(s.receipt).calls==negative.calls &&
        s.row(b)==retainedB,"unchanged null factory is cached without another failed probe",rejected,result);
    set("ADI_SCAN_FAIL","2");const auto blocked=s.module("Blocked.vst3");
    auto blockedConfig=s.config;blockedConfig.process.timeout=std::chrono::milliseconds(500);
    s.scanner=Scanner::open(blockedConfig,s.error);if(!s.scanner)throw std::runtime_error(s.error);
    s.probe(blocked);s.probe(blocked);const auto held=s.probe(blocked);
    s.scanner=Scanner::open(s.config,s.error);if(!s.scanner)throw std::runtime_error(s.error);
    set("ADI_SCAN_FAIL","0");s.changed(blocked);result=s.probe(blocked);
    s.check(10,held.state==State::Quarantined && held.failureCacheHit && held.failureStrikes==2 && result.state==State::Ready &&
        !result.cacheHit && s.registry->scanBlocks(s.error)->empty() && s.row(b)==retainedB,
        "new fingerprint releases two-strike automatic block and real factory succeeds",blocked,result);
    s.events.back()["beforeReplacement"]={{"state",name(held.state)},{"strikes",held.failureStrikes},
        {"failureCacheHit",held.failureCacheHit},{"diagnostic",held.error}};
    set("ADI_SCAN_FAIL","3");const auto exiting=s.module("Exit.vst3");result=s.probe(exiting);
    s.check(11,result.state==State::Failed && result.error.find("73")!=result.error.npos &&
        s.row(b)==retainedB,"owned child exit 73 preserves unrelated cache (classification recorded separately)",exiting,result);
    set("ADI_SCAN_FAIL","2");const auto hanging=s.module("Hang.vst3");const auto survivor=s.temp.path()/"independent-survived";
    const auto self=native(juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName().toStdString());
    ProcessResult independent;
    std::thread other([&]{independent=runProcess(self,{"--sentinel",text(survivor)});});
    auto shortConfig=s.config;shortConfig.process.timeout=std::chrono::milliseconds(500);
    auto shortScan=Scanner::open(shortConfig,s.error);std::vector<juce::PluginDescription> descriptions;
    if(!shortScan){other.join();throw std::runtime_error(s.error);}
    const Receipt beforeTimeout(s.receipt);
    const auto start=std::chrono::steady_clock::now();
    result=device::scanVst3File(*shortScan,juce::String::fromUTF8(text(hanging).c_str()),descriptions);
    const auto elapsed=std::chrono::steady_clock::now()-start;const Receipt afterTimeout(s.receipt);
    bool timeoutChildEnded=true;unsigned newChildren=0;
    for(const auto pid:afterTimeout.pids)if(!beforeTimeout.pids.contains(pid)){++newChildren;timeoutChildEnded=timeoutChildEnded && ended(pid);}
    other.join();
    s.check(12,result.state==State::Failed && result.failureStrikes==1 && result.error.find("timed out")!=result.error.npos &&
        elapsed<std::chrono::seconds(4) && newChildren==1 && timeoutChildEnded && independent.succeeded() && fs::exists(survivor) && s.row(b)==retainedB,
        "deadline reaps only its owned scanner while independent child completes",hanging,result);
    s.events.back()["timeoutEvidence"]={{"deadlineMs",500},{"elapsedMs",std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()},
        {"scannerChildEnded",timeoutChildEnded},{"independentChildSucceeded",independent.succeeded()}};
    set("ADI_SCAN_FAIL","0");
    const auto retry=[&](int number,const fs::path& alias,const char* label){
        const Receipt before(s.receipt);const bool marked=s.registry->blockScan("vst3",text(fs::canonical(a)),"manual retry case",123,s.error);
        const bool retried=s.scanner->retry("vst3",alias,s.error);result=s.probe(a);
        s.check(number,marked && retried && result.state==State::Ready && !result.cacheHit &&
            Receipt(s.receipt).count("GetPluginFactory")==before.count("GetPluginFactory")+1 && s.row(b)==retainedB,label,a,result);
    };
    auto slash=text(a);std::replace(slash.begin(),slash.end(),'\\','/');retry(13,native(slash),"explicit retry with alternate slash clears same canonical block");
    auto upper=text(a);for(char& c:upper)if(c>='a' && c<='z')c=static_cast<char>(c-'a'+'A');
    retry(14,native(upper),"explicit retry with Windows case alias clears same canonical block");
    const auto link=s.temp.path()/"directory-link";if(!junction(link,s.temp.path()))throw std::runtime_error("native directory junction unavailable");
    retry(15,link/a.filename(),"explicit retry through actual directory junction clears same canonical block");
    if(!RemoveDirectoryW(link.c_str()) || !fs::exists(a))throw std::runtime_error("junction cleanup failed");
    const bool linkedSearch = nativeSearchLinks(s.temp.path());
    std::printf("CLAP and VST3 discovery junctions, overlapping roots and directory cycle: %s\n",
                linkedSearch ? "PASS" : "FAIL");
    s.save();std::printf("SCANNER CASES: %d passed, %d failed; actual VST3 factory callbacks, case16 separate\n",s.passed,s.failed);
    return s.passed==15 && s.failed==0 && linkedSearch;
}
} // namespace
int main(int argc,char** argv){
    if(argc==3 && std::string(argv[1])=="--sentinel"){
        std::this_thread::sleep_for(std::chrono::milliseconds(900));std::ofstream(native(argv[2]))<<"completed";return 0;}
    try{return run()?0:1;}catch(const std::exception& error){std::printf("FAIL: %s\n",error.what());return 1;}
}
