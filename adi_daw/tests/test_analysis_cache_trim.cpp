// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/media/analysis_cache.hpp"
#include "adi/audio/io_audit.hpp"
#include "temp_directory.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <new>
#include <stdexcept>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
// A one-shot allocation observation, not a production I/O override: request
// cancellation after readFile's first stop check, at its actual read buffer.
std::atomic<std::size_t> stopAtSize{0};
std::atomic<unsigned> readsUntilAction{1};
std::stop_source* pendingStop=nullptr;
#if defined(_WIN32)
HANDLE changedFiles[3]{INVALID_HANDLE_VALUE,INVALID_HANDLE_VALUE,INVALID_HANDLE_VALUE};
FILETIME changedTimes[3]{};
unsigned changedCount=0;
#endif
}
void* operator new(std::size_t n) {
    if(n!=0 && n==stopAtSize.load() && readsUntilAction.fetch_sub(1)==1) {
        stopAtSize=0;
        if(pendingStop)pendingStop->request_stop();
#if defined(_WIN32)
        for(unsigned i=0;i<3;++i)if(changedFiles[i]!=INVALID_HANDLE_VALUE && SetFileTime(changedFiles[i],nullptr,nullptr,&changedTimes[i]))++changedCount;
#endif
    }
    if(void* p=std::malloc(n?n:1))return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n){return operator new(n);}
void operator delete(void* p)noexcept{std::free(p);}
void operator delete[](void* p)noexcept{operator delete(p);}
void operator delete(void* p,std::size_t)noexcept{operator delete(p);}
void operator delete[](void* p,std::size_t)noexcept{operator delete(p);}

namespace {
using namespace adi;using namespace adi::media;namespace fs=std::filesystem;
int checks=0,failures=0;
void check(bool good,const char* name){++checks;if(!good){++failures;std::printf("FAIL: %s\n",name);}}
struct Fixture {
    test::TempDirectory temp{"analysis","trim-refusals"};
    AnalysisCache cache{temp.path()/"analysed"};
    SampleAnalysis sample{{std::string(64,'a'),48000,1,128},{{"APKS",{1,0,std::vector<std::byte>(777,std::byte{7})}}}};
    std::uint64_t size=0;
    fs::path path(char id)const{return cache.folder()/(std::string(64,id)+".adia");}
    Fixture() {
        fs::create_directory(cache.folder());
        const auto old=fs::file_time_type::clock::now()-std::chrono::hours(3);
        for(char id='a';id<='c';++id) {
            auto data=sample;data.identity.hash=std::string(64,id);const auto bytes=encodeAnalysis(data);size=bytes.size();
            std::ofstream out(path(id),std::ios::binary);out.exceptions(std::ios::badbit|std::ios::failbit);
            out.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));out.close();
            fs::last_write_time(path(id),old+std::chrono::hours(id-'a'));
        }
        std::ofstream(cache.folder()/"authored.txt")<<"unrelated data";
        std::ofstream(cache.folder()/(std::string(64,'d')+".adia"))<<"damaged";
    }
};
void cancellation() {
    Fixture fixture;std::string error="keep caller diagnostic";std::stop_source stop;
    pendingStop=&stop;readsUntilAction=3;stopAtSize=fixture.size;
    const auto cancelled=fixture.cache.trimToBytes(0,error,stop.get_token());
    stopAtSize=0;pendingStop=nullptr;
    check(stop.stop_requested(),"cancellation observation reached the third actual read buffer");
    check(cancelled.problem==AnalysisCacheProblem::Cancelled && error=="keep caller diagnostic",
          "cancellation inside readFile preserves the caller diagnostic");
    check(!cancelled.inventoryComplete && cancelled.filesRemoved==0 && cancelled.bytesBefore==2*fixture.size &&
          cancelled.bytesAfter==cancelled.bytesBefore && fs::exists(fixture.path('a')) &&
          fs::exists(fixture.path('b')) && fs::exists(fixture.path('c')),"cancelled inventory never starts deletion");
    error="keep caller diagnostic";const auto pre=fixture.cache.trimToBytes(0,error,stop.get_token());
    check(pre.problem==AnalysisCacheProblem::Cancelled && error=="keep caller diagnostic","pre-cancel remains nonmutating");
    AnalysisTrimResult realtime;
    {audio::CallbackScope callback;realtime=fixture.cache.trimToBytes(0,error);}
    check(realtime.problem==AnalysisCacheProblem::Realtime && error=="keep caller diagnostic","callback refusal remains nonmutating");
}
#if defined(_WIN32)
std::string utf8(const fs::path& path){const auto text=path.u8string();return {text.begin(),text.end()};}
struct Reader {
    HANDLE value=INVALID_HANDLE_VALUE;
    Reader(const fs::path& path,DWORD sharing) {
        value=CreateFileW(path.c_str(),GENERIC_READ,sharing,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(value==INVALID_HANDLE_VALUE)throw std::runtime_error("cannot open owned blocking reader");
    }
    ~Reader(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
};
void unreadable() {
    Fixture fixture;std::string error;
    {
        Reader locked(fixture.path('a'),0);
        const auto result=fixture.cache.trimToBytes(0,error);
        check(result.problem==AnalysisCacheProblem::Io && result.inventoryComplete && result.filesRemoved==2 &&
              result.bytesBefore==2*fixture.size && result.bytesAfter==0 && !result.budgetMet && result.skippedEntries==1,
              "unreadable entry does not stop full validated inventory or healthy sibling eviction");
        check(error.find(utf8(fixture.path('a')))!=std::string::npos && fs::exists(fixture.path('a')) &&
              !fs::exists(fixture.path('b')) && !fs::exists(fixture.path('c')),"unreadable path is attributed and never removed or charged");
        check(fs::exists(fixture.cache.folder()/"authored.txt") && fs::exists(fixture.cache.folder()/(std::string(64,'d')+".adia")),
              "unknown and damaged entries remain untouched beside a read refusal");
    }
    const auto retained=fixture.cache.read(fixture.sample.identity);
    check(retained && *retained.data==fixture.sample,"unreadable owned file keeps its exact complete payload");
    const auto retry=fixture.cache.trimToBytes(0,error);
    check(retry && retry.filesRemoved==1 && error.empty(),"retry after read-share release removes the remaining validated entry");
}
void undeletable(bool allowOne) {
    Fixture fixture;std::string error;
    {
        Reader locked(fixture.path('a'),FILE_SHARE_READ);
        const auto result=fixture.cache.trimToBytes(allowOne?fixture.size:0,error);
        check(result.problem==AnalysisCacheProblem::Io && result.inventoryComplete && result.filesRemoved==2 &&
              result.bytesBefore==3*fixture.size && result.bytesAfter==fixture.size && result.budgetMet==allowOne && result.skippedEntries==1,
              "oldest undeletable entry is skipped while later eligible siblings are evicted");
        check(error.find(utf8(fixture.path('a')))!=std::string::npos && fs::exists(fixture.path('a')) &&
              !fs::exists(fixture.path('b')) && !fs::exists(fixture.path('c')),"delete refusal retains the original and names its path");
    }
    const auto retained=fixture.cache.read(fixture.sample.identity);
    check(retained && *retained.data==fixture.sample,"undeletable entry retains its exact complete payload");
    const auto retry=fixture.cache.trimToBytes(0,error);
    check(retry && retry.filesRemoved==1 && error.empty(),"retry after non-delete-sharing reader release succeeds");
}
void changedDuringInventory() {
    Fixture fixture;std::string error;
    struct Close {
        ~Close(){stopAtSize=0;for(auto& file:changedFiles)if(file!=INVALID_HANDLE_VALUE){CloseHandle(file);file=INVALID_HANDLE_VALUE;}}
    } close;
    for(unsigned i=0;i<3;++i) {
        const auto path=fixture.path(static_cast<char>('a'+i));
        changedFiles[i]=CreateFileW(path.c_str(),FILE_WRITE_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(changedFiles[i]==INVALID_HANDLE_VALUE || !GetFileTime(changedFiles[i],nullptr,nullptr,&changedTimes[i]))throw std::runtime_error("open metadata-change fixture");
        ULARGE_INTEGER ticks{};ticks.LowPart=changedTimes[i].dwLowDateTime;ticks.HighPart=changedTimes[i].dwHighDateTime;ticks.QuadPart+=10000000;
        changedTimes[i]={ticks.LowPart,ticks.HighPart};
    }
    changedCount=0;readsUntilAction=1;stopAtSize=fixture.size;
    const auto result=fixture.cache.trimToBytes(0,error);
    stopAtSize=0;
    check(changedCount==3,"owned native timestamps change after inventory's stamp capture");
    check(result.problem==AnalysisCacheProblem::Io && !result.inventoryComplete && result.filesRemoved==0 &&
          result.skippedEntries==0 && error.find("changed during inventory")!=std::string::npos,
          "a detected concurrent change still aborts instead of being skipped");
    check(fs::exists(fixture.path('a')) && fs::exists(fixture.path('b')) && fs::exists(fixture.path('c')),
          "concurrent-change abort preserves all files before completed inventory");
}
#endif
}
int main(){try{
    cancellation();
#if defined(_WIN32)
    unreadable();undeletable(false);undeletable(true);changedDuringInventory();
#else
    std::printf("SKIP: Windows native sharing refusals require Windows\n");
#endif
}catch(const std::exception& e){++failures;std::printf("EXCEPTION: %s\n",e.what());}
std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
