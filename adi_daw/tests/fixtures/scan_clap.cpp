// SPDX-License-Identifier: GPL-3.0-or-later
// Controlled scan-only library. Never installed or used for audio.
#include <clap/clap.h>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <process.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#endif
namespace {
std::string mode;
[[noreturn]] void crash() {
#if defined(_WIN32)
    TerminateProcess(GetCurrentProcess(),0xC0000005UL);
#else
    rlimit limit{0,0};setrlimit(RLIMIT_CORE,&limit);
#endif
    std::abort();
}
struct StaticLifetime {
    StaticLifetime() {
#if defined(ADI_SCAN_STATIC_CRASH)
        crash();
#endif
    }
    ~StaticLifetime(){if(mode.find("unload-crash")!=mode.npos)crash();}
} lifetime;
bool CLAP_ABI initialize(const char* path) {
    const std::filesystem::path native(std::u8string(path,path+std::strlen(path)));
    const auto bytes=native.filename().u8string();mode.assign(bytes.begin(),bytes.end());
    std::ofstream marker(std::filesystem::path(std::u8string(path,path+std::strlen(path))) += ".pids",std::ios::app);
#if defined(_WIN32)
    marker<<_getpid()<<'\n';
#else
    marker<<getpid()<<'\n';
#endif
    marker.close();
    if(mode.find("init-crash")!=mode.npos)crash();
    if(mode.find("init-hang")!=mode.npos)std::this_thread::sleep_for(std::chrono::seconds(30));
    if(mode.find("noisy")!=mode.npos){std::printf("plug-in stdout chatter\n{not metadata}\n");std::fprintf(stderr,"plug-in stderr chatter\n");}
    return true;
}
void CLAP_ABI deinitialize() {
    if(mode.find("deinit-crash")!=mode.npos)crash();
    if(mode.find("deinit-hang")!=mode.npos)std::this_thread::sleep_for(std::chrono::seconds(30));
}
const char* features[]={CLAP_PLUGIN_FEATURE_INSTRUMENT,"stereo",nullptr};
const clap_plugin_descriptor_t first{CLAP_VERSION,"org.adi.scan.first","Scan caf\xc3\xa9","ADI","","","","1.2.3","fixture",features};
const clap_plugin_descriptor_t second{CLAP_VERSION,"org.adi.scan.second","Second","ADI","","","","2.0","fixture",nullptr};
std::uint32_t CLAP_ABI count(const clap_plugin_factory_t*) {
    if(mode.find("factory-crash")!=mode.npos)crash();
    if(mode.find("too-many")!=mode.npos)return 65537;
    return 2;
}
const clap_plugin_descriptor_t* CLAP_ABI descriptor(const clap_plugin_factory_t*,std::uint32_t index) {
    if(mode.find("descriptor-crash")!=mode.npos)crash();
    if(mode.find("null-descriptor")!=mode.npos)return nullptr;
    if(mode.find("duplicate")!=mode.npos)return &first;
    if(mode.find("oversized")!=mode.npos) {
        static const std::string huge(65537,'x');static auto value=first;value.name=huge.c_str();return &value;
    }
    if(mode.find("bad-utf8")!=mode.npos){static auto value=first;value.name="bad\xff";return &value;}
    return index==0?&first:&second;
}
const clap_plugin_t* CLAP_ABI create(const clap_plugin_factory_t*,const clap_host_t*,const char*) {return nullptr;}
const clap_plugin_factory_t factory{count,descriptor,create};
const void* CLAP_ABI getFactory(const char* id) {return std::strcmp(id,CLAP_PLUGIN_FACTORY_ID)==0?&factory:nullptr;}
} // namespace
extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry{CLAP_VERSION,initialize,deinitialize,getFactory};
