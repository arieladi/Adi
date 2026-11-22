// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Opt-in counterpart to research ScanProbe's controls. Unlike that metadata-only
// fixture this module has real components, as JUCE discovery requires them.
// Only factory callbacks use these controls; no processing callback logs I/O.
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif
namespace scanner_case {
inline std::string environment(const char* key) {
#if defined(_WIN32)
    char* value=nullptr;std::size_t length=0;
    if(_dupenv_s(&value,&length,key)!=0)return {};
    const std::string result=value?value:"";std::free(value);return result;
#else
    const char* value=std::getenv(key);return value?value:"";
#endif
}
inline bool enabled(){return !environment("ADI_SCAN_LOG").empty();}
inline int option(const char* key){return std::atoi(environment(key).c_str());}
inline void receipt(const char* call) {
    const auto path=environment("ADI_SCAN_LOG");if(path.empty())return;
    std::ofstream out(std::filesystem::path(std::u8string(path.begin(),path.end())),std::ios::app);
#if defined(_WIN32)
    out<<_getpid();
#else
    out<<getpid();
#endif
    out<<','<<call<<'\n';
}
} // namespace scanner_case
