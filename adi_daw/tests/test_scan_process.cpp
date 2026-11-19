// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/scan_process.hpp"
#include "adi/audio/io_audit.hpp"
#include "temp_directory.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <new>
#include <thread>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
namespace { std::atomic<unsigned> allocations{0}; }
namespace {
// MSVC's Debug CRT (_ITERATOR_DEBUG_LEVEL 2) allocates an iterator proxy whenever a
// std::string or std::vector is constructed, so a result object returned from the
// callback path costs one allocation per container there and none on the other
// CRTs. The refusal check below allows exactly what an empty result costs on this
// CRT, measured once before the baseline is read.
template<class Result> unsigned emptyResultCost(){const auto before=allocations.load();{adi::audio::CallbackScope scope;Result empty;(void)empty;}return allocations.load()-before;}
}
void* operator new(std::size_t n) {if(adi::audio::onAudioThread)++allocations;if(void* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n) {return operator new(n);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {operator delete(p);}
void operator delete(void* p,std::size_t) noexcept {operator delete(p);}
void operator delete[](void* p,std::size_t) noexcept {operator delete(p);}
namespace {
using namespace adi::scan;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
fs::path fromUtf8(const std::string& value) {return fs::path(std::u8string(value.begin(),value.end()));}
int checks = 0, failures = 0;
void check(bool ok, const char* label) {++checks;if(!ok){++failures;std::printf("FAIL: %s\n",label);}}
int fixture(int argc, char** argv) {
    const std::string mode = argc > 2 ? argv[2] : "";
    if(mode=="echo") {for(int i=3;i<argc;++i)std::printf("%zu:%s\n",std::strlen(argv[i]),argv[i]);return 0;}
    if(mode=="binary") {const std::array<unsigned char,5> data{0,1,127,128,255};std::fwrite(data.data(),1,data.size(),stdout);return 0;}
    if(mode=="large") {const std::string bytes(2U*1024U*1024U,'z');return std::fwrite(bytes.data(),1,bytes.size(),stdout)==bytes.size()?0:93;}
    if(mode=="stdin") {std::printf("%s",std::getchar()==EOF?"eof":"unexpected");return 0;}
    if(mode=="closed-stdio") {
        for(int fd=0;fd<3;++fd) {
#if defined(_WIN32)
            _close(fd);
#else
            ::close(fd);
#endif
        }
        const auto result=runProcess(fs::absolute(fromUtf8(argv[0])),{"--fixture","echo","nested"});
        return result.succeeded() && result.output=="6:nested\n"?0:94;
    }
    if(mode=="exit") return 23;
    if(mode=="crash") {
#if defined(_WIN32)
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
        TerminateProcess(GetCurrentProcess(),0xC0000005UL);
#else
        rlimit limit{0,0};setrlimit(RLIMIT_CORE,&limit);raise(SIGABRT);
#endif
        return 99;
    }
    if(mode=="flood") {const std::string bytes(4096,'x');for(;;)if(std::fwrite(bytes.data(),1,bytes.size(),stdout)!=bytes.size())return 1;}
    if(mode=="stderr") {const std::string bytes(4096,'!');for(int i=0;i<512;++i)std::fwrite(bytes.data(),1,bytes.size(),stderr);std::printf("ok");return 0;}
    if(mode=="sleep") {std::this_thread::sleep_for(10s);return 0;}
    if(mode=="delayed-marker") {std::this_thread::sleep_for(400ms);std::ofstream(fromUtf8(argv[3]))<<"escaped";return 0;}
    if(mode=="tree" || mode=="exit-tree") {
#if defined(_WIN32)
        // Intentionally bypass the runner in this controlled child so the
        // grandchild inherits its job, as a plug-in's helper normally does.
        const auto self=fs::absolute(fromUtf8(argv[0])).wstring();
        const auto marker=fromUtf8(argv[3]).wstring();
        std::wstring command=L"\""+self+L"\" --fixture delayed-marker \""+marker+L"\"";
        STARTUPINFOW startup{};startup.cb=sizeof startup;PROCESS_INFORMATION info{};
        if(!CreateProcessW(self.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&info))return 90;
        CloseHandle(info.hThread);CloseHandle(info.hProcess);
#else
        const pid_t child=fork();if(child<0)return 90;
        if(child==0){std::this_thread::sleep_for(400ms);std::ofstream(fromUtf8(argv[3]))<<"escaped";_exit(0);}
#endif
        std::printf("ready");std::fflush(stdout);
        if(mode=="tree") std::this_thread::sleep_for(10s);
        return 0;
    }
    return 91;
}
std::string utf8(const fs::path& path) {const auto bytes=path.u8string();return {bytes.begin(),bytes.end()};}
void contract(const fs::path& self) {
    const std::vector<std::string> exact{"--fixture","echo","","space value","quote\"value","trailing\\","$() `x`; & | >","line\nbreak","caf\xc3\xa9"};
    const auto echoed=runProcess(self,exact);
    check(echoed.succeeded() && echoed.output=="0:\n11:space value\n11:quote\"value\n9:trailing\\\n14:$() `x`; & | >\n10:line\nbreak\n5:caf\xc3\xa9\n","arguments retain empty/space/quote/backslash/metacharacter/newline/UTF-8 bytes without shell expansion");
    const auto binary=runProcess(self,{"--fixture","binary"});
    const std::array<char,5> expected{0,1,127,static_cast<char>(128),static_cast<char>(255)};
    check(binary.succeeded() && binary.output==std::string(expected.data(),expected.size()),"stdout retains binary bytes including NUL");
    const auto large=runProcess(self,{"--fixture","large"});
    check(large.succeeded() && large.output==std::string(2U*1024U*1024U,'z'),"output exceeding pipe capacity is drained completely before success");
    const auto input=runProcess(self,{"--fixture","stdin"});
    check(input.succeeded() && input.output=="eof","child stdin is disconnected from the application");
    check(runProcess(self,{"--fixture","closed-stdio"}).succeeded(),"runner also works inside a child with all standard descriptors closed");
    const auto exited=runProcess(self,{"--fixture","exit"});
    check(exited.status==ProcessStatus::Exited && exited.exitCode==23 && !exited.succeeded(),"nonzero child exit is distinct from crash and success");
    const auto crashed=runProcess(self,{"--fixture","crash"});
    check(crashed.status==ProcessStatus::Crashed && !crashed.succeeded(),"actual crashing child is contained and classified");
    check(runProcess(self,{"--fixture","stderr"}).output=="ok","large stderr cannot fill a pipe or contaminate stdout");
    check(runProcess(self.parent_path()/"missing scanner executable",{}).status==ProcessStatus::LaunchFailed,"missing absolute executable reports launch failure");
    check(runProcess("relative-program",{}).status==ProcessStatus::Invalid,"relative executable never searches PATH");
    check(runProcess(self,{std::string("bad\0argument",12)}).status==ProcessStatus::Invalid,"embedded argument NUL refuses before launch");
    check(runProcess(self,std::vector<std::string>(257,"x")).status==ProcessStatus::Invalid,"argument count is bounded");
    check(runProcess(self,{std::string(128U*1024U,'x')}).status==ProcessStatus::Invalid,"aggregate argument bytes are bounded");
    ProcessOptions options;
    bool invalid=true;
    for(const auto limit:std::array<std::size_t,2>{0,8U*1024U*1024U+1}) {options.maximumOutput=limit;invalid=invalid && runProcess(self,{},options).status==ProcessStatus::Invalid;}
    options={};for(const auto timeout:std::array{0ms,300001ms}) {options.timeout=timeout;invalid=invalid && runProcess(self,{},options).status==ProcessStatus::Invalid;}
    check(invalid,"zero/oversized output and deadline bounds refuse");
    options={};options.maximumOutput=4096;
    const auto limited=runProcess(self,{"--fixture","flood"},options);
    check(limited.status==ProcessStatus::OutputLimit && limited.output.size()<=4096,"continuous child stdout is killed at the explicit byte bound");
    options={};options.timeout=40ms;const auto start=std::chrono::steady_clock::now();
    check(runProcess(self,{"--fixture","sleep"},options).status==ProcessStatus::TimedOut && std::chrono::steady_clock::now()-start<2s,"hung child is terminated and reaped near its deadline");
    std::atomic<bool> cancel{true};options={};options.cancel=&cancel;
    check(runProcess(self,{"--fixture","echo","must not run"},options).status==ProcessStatus::Cancelled,"already-cancelled request never starts a child");
    cancel=false;std::thread cancellation([&]{std::this_thread::sleep_for(40ms);cancel=true;});
    const auto cancelled=runProcess(self,{"--fixture","sleep"},options);cancellation.join();
    check(cancelled.status==ProcessStatus::Cancelled,"live cancellation terminates and reaps a running child");
    const auto cost=emptyResultCost<ProcessResult>();
    const auto allocated=allocations.load(),io=adi::audio::callbackFileIo.load();
    {adi::audio::CallbackScope callback;const auto result=runProcess(self,exact);if(result.status!=ProcessStatus::CallbackRefused)++failures;}
    check(allocations==allocated+cost && adi::audio::callbackFileIo==io,"callback refuses before allocation or file I/O");
    adi::test::TempDirectory temp("scanner","tree");
    const auto unicodeSelf=temp.path()/fromUtf8("scanner caf\xc3\xa9 with spaces")/self.filename();
    fs::create_directories(unicodeSelf.parent_path());fs::copy_file(self,unicodeSelf);
    const auto unicodeRun=runProcess(unicodeSelf,{"--fixture","echo","identity"});
    check(unicodeRun.succeeded() && unicodeRun.output=="8:identity\n","absolute executable path retains spaces and Unicode");
    options={};options.maximumOutput=5;
    check(runProcess(self,{"--fixture","binary"},options).succeeded(),"output exactly at its byte limit remains valid");
    options.maximumOutput=4;
    check(runProcess(self,{"--fixture","binary"},options).status==ProcessStatus::OutputLimit,"one byte over the output boundary refuses whole success");
    const auto deadlineMarker=temp.path()/"timeout marker";
    const auto exitMarker=temp.path()/"exit marker";
    options={};options.timeout=80ms;
    const auto tree=runProcess(self,{"--fixture","tree",utf8(deadlineMarker)},options);
    const auto exitTree=runProcess(self,{"--fixture","exit-tree",utf8(exitMarker)});
    std::this_thread::sleep_for(600ms);
    check(tree.status==ProcessStatus::TimedOut && tree.output=="ready" && !fs::exists(deadlineMarker),"timeout terminates child descendants as well as their inherited output writer");
    check(exitTree.succeeded() && exitTree.output=="ready" && !fs::exists(exitMarker),"normal exit terminates remaining descendants without waiting for inherited-pipe EOF");
#if defined(_WIN32)
    DWORD handlesBefore=0;GetProcessHandleCount(GetCurrentProcess(),&handlesBefore);
#endif
    bool repeated=true;
    for(unsigned i=0;i<24;++i) {const auto result=runProcess(self,{"--fixture","echo","again"});repeated=repeated && result.succeeded() && result.output=="5:again\n";}
    check(repeated,"repeated children preserve exact stdout without stale status or output");
    std::array<bool,4> independent{};std::array<std::thread,4> owners;
    for(std::size_t index=0;index<owners.size();++index)owners[index]=std::thread([&,index]{
        independent[index]=true;
        for(unsigned turn=0;turn<6;++turn) {
            const auto result=runProcess(self,{"--fixture","echo",std::to_string(index)});
            independent[index]=independent[index] && result.succeeded() && result.output=="1:"+std::to_string(index)+"\n";
        }
    });
    for(auto& owner:owners)owner.join();
    check(independent[0] && independent[1] && independent[2] && independent[3],"concurrent owners retain independent process groups, arguments and outputs");
#if defined(_WIN32)
    DWORD handlesAfter=0;GetProcessHandleCount(GetCurrentProcess(),&handlesAfter);
    check(handlesBefore==handlesAfter,"repeated children retain no process/thread/pipe/job handles");
#else
    int status=0;errno=0;const auto remaining=waitpid(-1,&status,WNOHANG);
    check(remaining==-1 && errno==ECHILD,"all directly spawned children are reaped, including failed runs");
#endif
}
} // namespace
int entry(int argc,char** argv) {
    if(argc>1 && std::strcmp(argv[1],"--fixture")==0)return fixture(argc,argv);
    if(argc>1)return 95;
    try {contract(fs::absolute(fromUtf8(argv[0])));}catch(const std::exception& error){++failures;std::printf("EXCEPTION: %s\n",error.what());}
    std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;
}

#if defined(_WIN32)
int wmain(int argc,wchar_t** argv) {
    _setmode(_fileno(stdout),_O_BINARY);
    std::vector<std::string> values;values.reserve(static_cast<std::size_t>(argc));
    for(int i=0;i<argc;++i) {
        const int bytes=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,argv[i],-1,nullptr,0,nullptr,nullptr);
        if(bytes<=0)return 92;
        std::string value(static_cast<std::size_t>(bytes),'\0');
        if(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,argv[i],-1,value.data(),bytes,nullptr,nullptr)!=bytes)return 92;
        value.pop_back();values.push_back(std::move(value));
    }
    std::vector<char*> pointers;for(auto& value:values)pointers.push_back(value.data());
    return entry(argc,pointers.data());
}
#else
int main(int argc,char** argv) {return entry(argc,argv);}
#endif
