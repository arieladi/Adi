// SPDX-License-Identifier: GPL-3.0-or-later
// Dedicated executable. No parent calls its factory-enumeration functions.
#include "adi/plugin_scanner.hpp"
#include "juce/clap_host.hpp"
#if defined(ADI_SCAN_WITH_VST3)
#include "juce/vst3_scan_host.hpp"
#endif
#include <cstdio>
#include <cstring>
#include <stdexcept>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#else
#include <unistd.h>
#endif

namespace {
std::string bounded(const char* value) {
    if(value==nullptr)return {};
    std::size_t count=0;
    while(count<=65536 && value[count]!='\0')++count;
    if(count>65536)throw std::runtime_error("CLAP descriptor string bound");
    return {value,count};
}
std::vector<adi::scan::Plugin> clap(const std::string& path) {
    adi::device::ClapLibrary library;std::string error;
    if(!library.open(path,error))throw std::runtime_error("CLAP scanner could not open bundle");
    const auto count=library.pluginCount();
    if(count>adi::scan::kMaximumPlugins)throw std::runtime_error("CLAP descriptor count bound");
    std::vector<adi::scan::Plugin> result;result.reserve(count);
    for(std::uint32_t index=0;index<count;++index) {
        const auto* descriptor=library.descriptorAt(index);
        if(descriptor==nullptr || descriptor->id==nullptr)throw std::runtime_error("missing CLAP descriptor identity");
        adi::scan::Plugin value;value.id=bounded(descriptor->id);value.name=bounded(descriptor->name);
        value.vendor=bounded(descriptor->vendor);value.version=bounded(descriptor->version);
        const auto* features=descriptor->features;
        for(std::size_t feature=0;features!=nullptr && features[feature]!=nullptr;++feature) {
            if(feature>=1024)throw std::runtime_error("CLAP feature count bound");
            const auto label=bounded(features[feature]);
            if(value.features.size()+label.size()+1>65536)throw std::runtime_error("CLAP feature byte bound");
            if(!value.features.empty())value.features+=',';
            value.features+=label;
            if(label==CLAP_PLUGIN_FEATURE_INSTRUMENT)value.instrument=true;
        }
        result.push_back(std::move(value));
    }
    return result; // library deinit/unload completes before metadata is written
}
struct Channel {
    std::FILE* file=nullptr;
    ~Channel(){if(file!=nullptr)std::fclose(file);}
    bool reserve() {
        // Preserve the pipe solely for metadata before any plug-in static
        // initializer/init/factory/deinit can print to stdout. stderr is null.
        std::fflush(stdout);
#if defined(_WIN32)
        const int descriptor=_dup(_fileno(stdout));
        if(descriptor<0)return false;
        if(_dup2(_fileno(stderr),_fileno(stdout))!=0){_close(descriptor);return false;}
        _setmode(descriptor,_O_BINARY);file=_fdopen(descriptor,"wb");
        if(file==nullptr)_close(descriptor);
#else
        const int descriptor=dup(STDOUT_FILENO);
        if(descriptor<0)return false;
        if(dup2(STDERR_FILENO,STDOUT_FILENO)<0){close(descriptor);return false;}
        file=fdopen(descriptor,"wb");if(file==nullptr)close(descriptor);
#endif
        return file!=nullptr;
    }
};
int entry(const std::vector<std::string>& arguments) {
    if(arguments.size()!=4 || arguments[1]!="--scan")return 64;
    Channel channel;if(!channel.reserve())return 74;
    try {
        std::vector<adi::scan::Plugin> plugins;
        if(arguments[2]=="clap")plugins=clap(arguments[3]);
        else if(arguments[2]=="vst3") {
#if defined(ADI_SCAN_WITH_VST3)
            plugins=adi::device::probeVst3Bundle(arguments[3]);
#else
            return 78;
#endif
        } else return 78;
        std::string error;const auto packet=adi::scan::encodeMetadata(arguments[2],arguments[3],plugins,error);
        if(!packet)return 65;
        if(std::fwrite(packet->data(),1,packet->size(),channel.file)!=packet->size() || std::fflush(channel.file)!=0)return 74;
        return 0;
    } catch(const std::exception&) {return 65;}
}
} // namespace
#if defined(_WIN32)
int wmain(int argc,wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    std::vector<std::string> arguments;arguments.reserve(static_cast<std::size_t>(argc));
    for(int index=0;index<argc;++index) {
        const int bytes=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,argv[index],-1,nullptr,0,nullptr,nullptr);
        if(bytes<=0)return 64;
        std::string value(static_cast<std::size_t>(bytes),'\0');
        if(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,argv[index],-1,value.data(),bytes,nullptr,nullptr)!=bytes)return 64;
        value.pop_back();arguments.push_back(std::move(value));
    }
    return entry(arguments);
}
#else
int main(int argc,char** argv) {
    std::vector<std::string> arguments;arguments.reserve(static_cast<std::size_t>(argc));
    for(int index=0;index<argc;++index)arguments.emplace_back(argv[index]);
    return entry(arguments);
}
#endif
