// Original diagnostic platform support. MIT; provenance: the director's research.
#pragma once
#include <bit>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#define ADI_EXPORT __declspec(dllexport)
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#else
#include <sched.h>
#include <sys/syscall.h>
#endif
#define ADI_EXPORT __attribute__((visibility("default")))
#endif

namespace adi_platform {
static_assert(sizeof(void*)==8 && std::endian::native==std::endian::little,
              "The diagnostic byte ABI requires a little-endian 64-bit platform");
#ifdef _WIN32
using Atomic64=LONG64;
using Atomic32=LONG;
inline Atomic64 fetchAdd(volatile Atomic64* p,Atomic64 n){return InterlockedExchangeAdd64(p,n);}
inline void publish(volatile Atomic64* p,Atomic64 n){InterlockedExchange64(p,n);}
inline Atomic64 acquire(const volatile Atomic64* p){return InterlockedCompareExchange64(const_cast<volatile Atomic64*>(p),0,0);}
inline Atomic32 acquire(const volatile Atomic32* p){return InterlockedCompareExchange(const_cast<volatile Atomic32*>(p),0,0);}
inline void publish(volatile Atomic32* p,Atomic32 n){InterlockedExchange(p,n);}
#else
using Atomic64=std::int64_t;
using Atomic32=std::int32_t;
static_assert(__atomic_always_lock_free(8,nullptr)&&__atomic_always_lock_free(4,nullptr),
              "Shared counters require lock-free process-shared atomics");
inline Atomic64 fetchAdd(volatile Atomic64* p,Atomic64 n){return __atomic_fetch_add(p,n,__ATOMIC_RELAXED);}
inline void publish(volatile Atomic64* p,Atomic64 n){__atomic_store_n(p,n,__ATOMIC_RELEASE);}
inline Atomic64 acquire(const volatile Atomic64* p){return __atomic_load_n(p,__ATOMIC_ACQUIRE);}
inline Atomic32 acquire(const volatile Atomic32* p){return __atomic_load_n(p,__ATOMIC_ACQUIRE);}
inline void publish(volatile Atomic32* p,Atomic32 n){__atomic_store_n(p,n,__ATOMIC_RELEASE);}
#endif
static_assert(sizeof(Atomic64)==8&&sizeof(Atomic32)==4);
// Mapping bases are page-aligned. Every typed region has a compile-time aligned
// offset, rather than converting an arbitrary unaligned byte pointer.
template<class T,std::size_t Offset> inline T* mappedAt(void* base){
    static_assert(Offset%alignof(T)==0);
    return static_cast<T*>(static_cast<void*>(static_cast<std::byte*>(base)+Offset));
}
inline std::uint32_t pid(){
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<std::uint32_t>(getpid());
#endif
}
inline std::uint32_t tid(){
#ifdef _WIN32
    return GetCurrentThreadId();
#elif defined(__APPLE__)
    std::uint64_t value=0;
    if(pthread_threadid_np(nullptr,&value)!=0||value>UINT32_MAX)return UINT32_MAX;
    return static_cast<std::uint32_t>(value);
#else
    return static_cast<std::uint32_t>(syscall(SYS_gettid));
#endif
}
inline std::uint32_t cpu(){
#ifdef _WIN32
    return GetCurrentProcessorNumber();
#elif defined(__APPLE__)
    return UINT32_MAX; // No public current-logical-CPU API on this platform.
#else
    const int value=sched_getcpu();return value<0?UINT32_MAX:static_cast<std::uint32_t>(value);
#endif
}
inline std::int32_t priority(){
#ifdef _WIN32
    return GetThreadPriority(GetCurrentThread());
#else
    return INT32_MIN; // No fabricated Windows priority scale.
#endif
}
inline std::int64_t ticks(){
#ifdef _WIN32
    LARGE_INTEGER value{};QueryPerformanceCounter(&value);return value.QuadPart;
#else
    timespec value{};if(clock_gettime(CLOCK_MONOTONIC,&value)!=0)return -1;
    return static_cast<std::int64_t>(value.tv_sec)*1000000000LL+static_cast<std::int64_t>(value.tv_nsec);
#endif
}
inline std::int64_t frequency(){
#ifdef _WIN32
    LARGE_INTEGER value{};QueryPerformanceFrequency(&value);return value.QuadPart;
#else
    return 1000000000LL;
#endif
}
#ifdef _WIN32
inline std::wstring wide(const std::string& value){
    if(value.size()>static_cast<std::size_t>(std::numeric_limits<int>::max()))return {};
    const int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0);
    if(n<=0)return {};std::wstring result(static_cast<std::size_t>(n),L'\0');
    MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),result.data(),n);return result;
}
inline std::string utf8(const std::wstring& value){
    if(value.size()>static_cast<std::size_t>(std::numeric_limits<int>::max()))return {};
    const int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);
    if(n<=0)return {};std::string result(static_cast<std::size_t>(n),'\0');
    WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),result.data(),n,nullptr,nullptr);return result;
}
#endif
inline std::string environment(const char* key){
#ifdef _WIN32
    const auto name=wide(key);const auto size=GetEnvironmentVariableW(name.c_str(),nullptr,0);if(!size)return {};
    std::wstring value(size,L'\0');const auto n=GetEnvironmentVariableW(name.c_str(),value.data(),size);
    if(!n||n>=size)return {};value.resize(n);return utf8(value);
#else
    const char* value=std::getenv(key);return value?value:"";
#endif
}
inline bool setEnvironment(const char* key,const std::string& value){
#ifdef _WIN32
    const auto name=wide(key),data=wide(value);
    if(name.empty()||(!value.empty()&&data.empty()))return false;
    return SetEnvironmentVariableW(name.c_str(),value.empty()?nullptr:data.c_str())!=0;
#else
    return value.empty()?unsetenv(key)==0:setenv(key,value.c_str(),1)==0;
#endif
}
struct Environment {
    std::string key,previous;
    explicit Environment(const char* name):key(name),previous(environment(name)){}
    ~Environment(){setEnvironment(key.c_str(),previous);}
};

// POSIX names are deliberately <=31 bytes (macOS limit). Hash the complete UTF-8
// logical name, including any _PCM suffix, with FNV-1a/64. Windows names are intact.
inline std::string posixName(const std::string& name){
    std::uint64_t hash=14695981039346656037ULL;
    for(char value:name){hash^=static_cast<unsigned char>(value);hash*=1099511628211ULL;}
    char result[32]{};std::snprintf(result,sizeof(result),"/adi_%016llx",static_cast<unsigned long long>(hash));return result;
}
inline std::string nativeName(const std::string& name){
#ifdef _WIN32
    return name;
#else
    return posixName(name);
#endif
}
class Mapping {
#ifdef _WIN32
    HANDLE handle_=nullptr;
#else
    int handle_=-1;
#endif
    std::string ownedName_;
    std::size_t bytes_=0;
    void* address_=nullptr;
public:
    Mapping()=default;Mapping(const Mapping&)=delete;Mapping& operator=(const Mapping&)=delete;
    ~Mapping(){close();}
    void* data()const{return address_;}
    bool open(const std::string& logical,std::size_t bytes,bool create=false){
        close();if(logical.empty()||!bytes)return false;
        auto name=nativeName(logical);bytes_=bytes;
#ifdef _WIN32
        const auto wideName=wide(name);if(wideName.empty())return false;
        if(create){
            handle_=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,
                static_cast<DWORD>(static_cast<std::uint64_t>(bytes)>>32),static_cast<DWORD>(bytes),wideName.c_str());
            if(handle_&&GetLastError()==ERROR_ALREADY_EXISTS){close();return false;}
        }else handle_=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,wideName.c_str());
        if(!handle_)return false;
        address_=MapViewOfFile(handle_,FILE_MAP_ALL_ACCESS,0,0,bytes);
#else
        if (bytes > static_cast<std::size_t>(std::numeric_limits<off_t>::max())) return false;
        handle_=shm_open(name.c_str(),create?(O_RDWR|O_CREAT|O_EXCL):O_RDWR,0600);
        if(handle_<0)return false;
        if (create) {
            ownedName_.swap(name);
            if (ftruncate(handle_, static_cast<off_t>(bytes)) != 0) { close(); return false; }
        }
        struct stat info{};
        if(fstat(handle_,&info)!=0||info.st_size<0||static_cast<std::uint64_t>(info.st_size)<bytes){close();return false;}
        address_=mmap(nullptr,bytes,PROT_READ|PROT_WRITE,MAP_SHARED,handle_,0);
        if(address_==MAP_FAILED)address_=nullptr;
#endif
        if(!address_){close();return false;}return true;
    }
    bool openEnvironment(const char* key,std::size_t bytes,const char* suffix=""){
        const auto name=environment(key);return !name.empty()&&open(name+suffix,bytes);
    }
    void close(){
#ifdef _WIN32
        if(address_)UnmapViewOfFile(address_);if(handle_)CloseHandle(handle_);handle_=nullptr;
#else
        if(address_)munmap(address_,bytes_);if(handle_>=0)::close(handle_);handle_=-1;
        if(!ownedName_.empty())shm_unlink(ownedName_.c_str());
#endif
        address_=nullptr;bytes_=0;ownedName_.clear();
    }
};
template<std::size_t N> inline void copy(char (&to)[N],const char* from){
    static_assert(N>0);std::snprintf(to,N,"%s",from);
}
// VST3 TChar is UTF-16 on every platform; Unix wchar_t is not. Fixture labels
// and numeric parameter strings are ASCII, so convert individual code units.
template<class T> inline void text16(T* to,const wchar_t* from,std::size_t capacity){
    if(!capacity)return;std::size_t i=0;for(;i+1<capacity&&from[i];++i)to[i]=static_cast<T>(from[i]);to[i]=0;
}
template<class T> inline void number16(T* to,double value){
    char text[128]{};std::snprintf(text,sizeof(text),"%.6g",value);
    std::size_t i=0;for(;i+1<128&&text[i];++i)to[i]=static_cast<T>(text[i]);to[i]=0;
}
template<class T> inline bool parse16(const T* from,double& value){
    char text[128]{};std::size_t i=0;
    for(;i+1<sizeof(text)&&from[i];++i){if(from[i]>127)return false;text[i]=static_cast<char>(from[i]);}
    char* end=nullptr;value=std::strtod(text,&end);return end!=text;
}
inline void sleepMilliseconds(unsigned value){
#ifdef _WIN32
    Sleep(value);
#else
    timespec duration{static_cast<time_t>(value/1000),static_cast<long>(value%1000)*1000000L};
    while(nanosleep(&duration,&duration)!=0&&errno==EINTR){}
#endif
}
inline FILE* appendFile(const char* path){
#ifdef _WIN32
    FILE* result=nullptr;fopen_s(&result,path,"a");return result;
#else
    return std::fopen(path,"a");
#endif
}
[[noreturn]] inline void exitChild(int code){
#ifdef _WIN32
    TerminateProcess(GetCurrentProcess(),static_cast<UINT>(code));
#endif
    std::_Exit(code);
}
inline void* optionalHostSymbol(){
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"reaper_adi_engine.dll"),"ADI_GetReaperAPI"));
#else
    return dlsym(RTLD_DEFAULT,"ADI_GetReaperAPI");
#endif
}
class Module {
#ifdef _WIN32
    HMODULE handle_=nullptr;
#else
    void* handle_=nullptr;
#endif
    bool vst_=false;
#ifdef __APPLE__
    CFBundleRef bundle_=nullptr;
#endif
public:
    Module()=default;Module(const Module&)=delete;Module& operator=(const Module&)=delete;
    ~Module(){close();}
    void* symbol(const char* name)const{
#ifdef _WIN32
        return reinterpret_cast<void*>(GetProcAddress(handle_,name));
#else
        return dlsym(handle_,name);
#endif
    }
    bool open(const char* path,bool vst){
        close();
#ifdef _WIN32
        handle_=LoadLibraryW(wide(path).c_str());
#else
        handle_=dlopen(path,RTLD_NOW|RTLD_LOCAL);
#endif
        if(!handle_)return false;if(!vst)return true;
#ifdef _WIN32
        auto entry=reinterpret_cast<bool(*)()>(symbol("InitDll"));
        vst_=entry&&symbol("ExitDll")&&entry();
#elif defined(__APPLE__)
        std::string root(path);const auto at=root.rfind("/Contents/MacOS/");
        if(at==std::string::npos){close();return false;}root.resize(at);
        auto url=CFURLCreateFromFileSystemRepresentation(nullptr,reinterpret_cast<const UInt8*>(root.data()),static_cast<CFIndex>(root.size()),true);
        if(url){bundle_=CFBundleCreate(nullptr,url);CFRelease(url);}
        auto entry=reinterpret_cast<bool(*)(CFBundleRef)>(symbol("bundleEntry"));
        vst_=bundle_&&entry&&symbol("bundleExit")&&entry(bundle_);
#else
        auto entry=reinterpret_cast<bool(*)(void*)>(symbol("ModuleEntry"));vst_=entry&&symbol("ModuleExit")&&entry(handle_);
#endif
        if(!vst_){close();return false;}return true;
    }
    void close(){
        if(!handle_)return;
        if(vst_){
#ifdef _WIN32
            auto leave=reinterpret_cast<bool(*)()>(symbol("ExitDll"));
#elif defined(__APPLE__)
            auto leave=reinterpret_cast<bool(*)()>(symbol("bundleExit"));
#else
            auto leave=reinterpret_cast<bool(*)()>(symbol("ModuleExit"));
#endif
            if(leave)leave();
        }
#ifdef __APPLE__
        if(bundle_)CFRelease(bundle_);bundle_=nullptr;
#endif
#ifdef _WIN32
        FreeLibrary(handle_);
#else
        dlclose(handle_);
#endif
        handle_=nullptr;vst_=false;
    }
};
} // namespace adi_platform
