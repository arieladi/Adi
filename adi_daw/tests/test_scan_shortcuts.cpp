// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/plugin_scanner.hpp"
#include "adi/windows_shortcut.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/media/blake3.hpp"
#include "temp_directory.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <thread>

namespace {
namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;
int checks = 0, failures = 0;
void check(bool value, const char* text) {
    ++checks; if (!value) { ++failures; std::printf("FAIL: %s\n", text); }
}
void require(HRESULT value, const char* text) { if (FAILED(value)) throw std::runtime_error(text); }
struct ComScope {
    HRESULT result;
    explicit ComScope(DWORD mode = COINIT_MULTITHREADED) : result(CoInitializeEx(nullptr, mode)) {
        require(result, "cannot initialize fixture COM");
    }
    ~ComScope() { CoUninitialize(); }
};
std::string utf8(const fs::path& path) { const auto bytes=path.u8string(); return {bytes.begin(),bytes.end()}; }
fs::path native(const std::string& path) { return fs::path(std::u8string(path.begin(),path.end())); }
void link(const fs::path& file, const fs::path& target) {
    ComPtr<IShellLinkW> object;
    require(CoCreateInstance(__uuidof(ShellLink), nullptr, CLSCTX_INPROC_SERVER,
                             IID_PPV_ARGS(object.GetAddressOf())), "create native Shell Link");
    require(object->SetPath(target.c_str()), "set native shortcut target");
    require(object->SetArguments(L"--ignored-by-discovery"), "set inert shortcut arguments");
    ComPtr<IPersistFile> persist;
    require(object.As(&persist), "query native link persistence");
    const auto saved=persist->Save(file.c_str(), TRUE);
    if(FAILED(saved))throw std::runtime_error("save native .lnk fixture "+utf8(file)+
        " HRESULT="+std::to_string(static_cast<unsigned long>(saved)));
}
void linkGraph(const std::vector<fs::path>& files, const std::vector<fs::path>& targets) {
    // SetPath/Save normally flatten existing shortcut targets. Author native
    // links to empty owned directories, then replace those directories with
    // the authored files. This tests actual stored paths after filesystem
    // changes, without handcrafting or patching Shell Link bytes.
    for(const auto& file:files)fs::create_directory(file);
    for(std::size_t n=0;n<files.size();++n) {
        auto stage=files[n]; stage += ".fixture.lnk"; link(stage,targets[n]);
    }
    for(const auto& file:files) {
        auto stage=file; stage += ".fixture.lnk";
        fs::remove(file); fs::rename(stage,file);
    }
}
std::vector<std::string> candidates(const fs::path& root, std::string_view format = ".clap") {
    return adi::scan::findCandidates({utf8(root)}, format);
}
struct Junction {
    fs::path path;
    Junction(fs::path linkPath, const fs::path& target) : path(std::move(linkPath)) {
        const auto print=fs::canonical(target).wstring(), substitute=L"\\??\\"+print;
        struct Data { DWORD tag=IO_REPARSE_TAG_MOUNT_POINT; WORD bytes=0,reserved=0;
            WORD substituteOffset=0,substituteBytes=0,printOffset=0,printBytes=0;
            wchar_t names[2048]{}; } data;
        static_assert(offsetof(Data,names)==16);
        if(substitute.size()+print.size()+2>=2048)throw std::runtime_error("junction fixture path too long");
        data.substituteBytes=static_cast<WORD>(substitute.size()*sizeof(wchar_t));
        data.printOffset=static_cast<WORD>(data.substituteBytes+sizeof(wchar_t));
        data.printBytes=static_cast<WORD>(print.size()*sizeof(wchar_t));
        data.bytes=static_cast<WORD>(8+data.printOffset+data.printBytes+sizeof(wchar_t));
        std::copy(substitute.begin(),substitute.end(),data.names);
        std::copy(print.begin(),print.end(),data.names+substitute.size()+1);
        fs::create_directory(path);
        const auto handle=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr);
        if(handle==INVALID_HANDLE_VALUE)throw std::runtime_error("cannot open owned junction");
        DWORD returned=0;
        const bool okay=DeviceIoControl(handle,FSCTL_SET_REPARSE_POINT,&data,
            static_cast<DWORD>(8+data.bytes),nullptr,0,&returned,nullptr)!=FALSE;
        CloseHandle(handle);
        if(!okay)throw std::runtime_error("cannot create owned junction");
    }
    ~Junction() { (void)RemoveDirectoryW(path.c_str()); }
    Junction(const Junction&)=delete;
    Junction& operator=(const Junction&)=delete;
};
void nativeLinks() {
    ComScope com;
    adi::test::TempDirectory temp("scanner_shortcuts", "native");
    const auto root=temp.path()/"search", vendor=temp.path()/"outside-vendor";
    fs::create_directories(root); fs::create_directories(vendor);
    const auto module=vendor/fs::path(L"owned-\u05d0-\U0001f3b5.clap");
    fs::copy_file(native(ADI_SHORTCUT_MODULE), module);
    const auto moduleHash=adi::media::blake3File(module).hex;
    const std::vector<std::string> expected{utf8(fs::canonical(module))};
    const auto direct=root/"direct.LNK", folder=root/"folder.lnk", duplicate=root/"duplicate.lnk";
    link(direct,module); link(folder,vendor); link(duplicate,module);
    const auto directHash=adi::media::blake3File(direct).hex;
    check(candidates(direct)==expected, "explicit native shortcut root resolves an owned Unicode module");
    check(candidates(folder)==expected, "explicit directory shortcut root discovers its actual module");
    check(candidates(root)==expected, "nested module and directory shortcuts deduplicate canonical targets");
    check(adi::scan::findCandidates({utf8(root),utf8(vendor),utf8(folder),utf8(direct)},".clap")==expected,
          "overlapping native and shortcut roots return one sorted target");
    {
        Junction ordinary(root/"ordinary-junction",vendor);
        Junction cycle(vendor/"junction-cycle",root);
        check(candidates(root)==expected && candidates(ordinary.path)==expected,
              "ordinary native junctions and cycles retain their canonical discovery behavior");
    }
    const auto symlink=root/"ordinary-symlink";
    std::error_code linkError; fs::create_directory_symlink(vendor,symlink,linkError);
    if(!linkError) {
        check(candidates(symlink)==expected && candidates(root)==expected,
              "ordinary directory symlink roots retain canonical discovery behavior");
        fs::remove(symlink);
    } else std::printf("SKIP directory symlink creation: %s\n",linkError.message().c_str());
    const auto back=vendor/"back.lnk"; link(back,root);
    check(candidates(root)==expected, "directory shortcut cycle terminates without losing the module");
    const auto chain=root/"chain.lnk", chainTarget=root/"chain-target.lnk";
    linkGraph({chain,chainTarget},{chainTarget,module});
    check(adi::scan::windowsShortcutTarget(chain)==chainTarget && candidates(chain)==expected,
          "stored shortcut-to-shortcut chain is not flattened and reaches the module");
    const auto loopA=root/"loop-a.lnk", loopB=root/"loop-b.lnk";
    linkGraph({loopA,loopB},{loopB,loopA});
    check(candidates(loopA).empty() && candidates(root)==expected, "native file-shortcut cycle is skipped and siblings survive");
    const auto bundle=vendor/"Owned.VST3"; fs::create_directory(bundle);
    std::ofstream(bundle/"Inner.vst3") << "must not traverse a matched bundle";
    const auto bundleLink=root/"bundle.lnk"; link(bundleLink,bundle);
    check(candidates(bundleLink,".vst3")==std::vector<std::string>{utf8(fs::canonical(bundle))} &&
          candidates(root,".vst3")==std::vector<std::string>{utf8(fs::canonical(bundle))},
          "shortcut target is matched by target extension and bundle traversal stops");
    const auto missing=root/"missing.lnk", absent=vendor/"late.clap";
    link(missing,absent);
    check(candidates(missing).empty(), "missing stored target is skipped without target search");
    fs::copy_file(module,absent);
    check(candidates(missing)==std::vector<std::string>{utf8(fs::canonical(absent))},
          "next explicit discovery observes a previously missing target");
    const auto moved=vendor/"moved.clap"; fs::rename(absent,moved);
    check(candidates(missing).empty(), "moving a target does not trigger shell recovery/search");
    std::ofstream(root/"invalid.lnk") << "not a native Shell Link";
    std::ofstream(root/"large.lnk",std::ios::binary) << std::string(1024*1024+1,'x');
    check(candidates(root/"invalid.lnk").empty() && candidates(root/"large.lnk").empty(),
          "malformed and over-1MiB shortcut files are skipped");
    std::vector<fs::path> deep;
    for(unsigned n=0;n<33;++n)deep.push_back(temp.path()/("hop-"+std::to_string(n)+".lnk"));
    auto targets=deep; targets.erase(targets.begin()); targets.push_back(module);
    linkGraph(deep,targets);
    check(candidates(deep[1])==expected && candidates(deep[0]).empty(), "32 shortcut hops allowed, 33 refused");
    bool freshThread=false, staThread=false;
    std::thread fresh([&] { freshThread=candidates(direct)==expected;
        const auto result=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
        freshThread=freshThread && SUCCEEDED(result); if(SUCCEEDED(result))CoUninitialize(); });
    fresh.join();
    std::thread sta([&] { try { ComScope apartment(COINIT_APARTMENTTHREADED); staThread=candidates(direct)==expected; }
        catch(...) { staThread=false; } });
    sta.join();
    check(freshThread && staThread, "fresh MTA initialization balances and existing STA ownership is preserved");
    { adi::audio::CallbackScope callback;
      check(candidates(root).empty() && !adi::scan::windowsShortcutTarget(direct), "callback refuses walk and shortcut I/O"); }
    auto marker=module; marker += ".pids";
    check(!fs::exists(marker), "discovery never initializes the owned CLAP module");
    check(adi::media::blake3File(direct).hex==directHash && adi::media::blake3File(module).hex==moduleHash,
          "shortcut and module bytes remain unchanged");
}
}

int main() {
    try { nativeLinks(); }
    catch(const std::exception& error) { ++failures; std::printf("EXCEPTION: %s\n",error.what()); }
    std::printf("%s -- native Windows shortcuts: %d checks, %d failures\n", failures?"FAILED":"PASS",checks,failures);
    return failures?1:0;
}
