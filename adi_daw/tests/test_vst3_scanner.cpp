// SPDX-License-Identifier: GPL-3.0-or-later
#include "juce/vst3_scan_host.hpp"
#include "temp_directory.hpp"
#include <cstdio>
#include <atomic>
#include <cstdlib>
#include <new>
#include <fstream>

namespace {std::atomic<unsigned> allocations{0};}
void* operator new(std::size_t n){if(adi::audio::onAudioThread)++allocations;if(void* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return operator new(n);}
void operator delete(void* p) noexcept{std::free(p);}
void operator delete[](void* p) noexcept{operator delete(p);}
void operator delete(void* p,std::size_t) noexcept{operator delete(p);}
void operator delete[](void* p,std::size_t) noexcept{operator delete(p);}

namespace {
using namespace adi;namespace fs=std::filesystem;
int checks=0,failures=0;
void check(bool ok,const char* label){++checks;if(!ok){++failures;std::printf("FAIL: %s\n",label);}}
std::string utf8(const fs::path& path){const auto bytes=path.u8string();return {bytes.begin(),bytes.end()};}
fs::path native(const std::string& value){return fs::path(std::u8string(value.begin(),value.end()));}
void contract() {
    test::TempDirectory temp("vst3_scanner","headless");std::string error;
    scan::Config config;config.executable=native(ADI_SCAN_HELPER);config.registry=temp.path()/"registry.sqlite";
    auto scanner=scan::Scanner::open(config,error);if(!scanner)throw std::runtime_error(error);
    const auto file=native(ADI_TEST_VST3);std::vector<juce::PluginDescription> descriptions;
    const auto result=device::scanVst3File(*scanner,juce::String::fromUTF8(utf8(file).c_str()),descriptions);
    check(result.state==scan::State::Ready && descriptions.size()==3 && result.plugins.size()==3,"actual dedicated helper discovers all three existing VST3 fixture classes");
    if(result.state!=scan::State::Ready){std::printf("diagnostic: %s\n",result.error.c_str());return;}
    bool exact=true;
    for(std::size_t index=0;index<descriptions.size();++index) {
        const auto& description=descriptions[index];
        exact=exact && description.pluginFormatName=="VST3" && description.isInstrument &&
            !description.name.isEmpty() && description.fileOrIdentifier==juce::File(juce::String::fromUTF8(utf8(file).c_str())).getFullPathName();
    }
    check(exact,"owned VST3 descriptions retain identity, names, instrument and original absolute path");
    auto registry=PluginRegistry::open(config.registry,error);if(!registry)throw std::runtime_error(error);
    check(registry->scanBlocks(error)->empty(),"valid VST3 format metadata clears its own pending quarantine");
    const auto canonical=utf8(fs::canonical(file));std::vector<juce::PluginDescription> decoded;
    check(device::decodeVst3Metadata(result.plugins,canonical,&decoded,error) && decoded.size()==3,"same production format validator decodes a full canonical reply");
    auto changed=result.plugins;
    const auto declaration=changed.front().data.find("?>");
    changed.front().data.insert(declaration==changed.front().data.npos?0:declaration+2,"\n<!DOCTYPE PLUGIN [<!ENTITY e 'bad'>]>\n");
    check(!device::decodeVst3Metadata(changed,canonical,&decoded,error) && decoded.size()==3,"DTD/entity declarations refuse before XML parsing and preserve prior destination");
    changed=result.plugins;auto nested=juce::XmlDocument::parse(juce::String::fromUTF8(changed.front().data.c_str()));
    nested->createNewChildElement("unexpected");changed.front().data=nested->toString().toStdString();
    check(!device::decodeVst3Metadata(changed,canonical,&decoded,error) && decoded.size()==3,"nested XML content refuses instead of being silently ignored by PluginDescription");
    changed=result.plugins;changed.front().name+=" mismatch";
    check(!device::decodeVst3Metadata(changed,canonical,&decoded,error) && decoded.size()==3,"XML and common metadata must agree, with no partial destination replacement");
    check(!device::decodeVst3Metadata(result.plugins,canonical+"different",&decoded,error),"serialized VST3 description must bind exact canonical path");
    changed=result.plugins;changed.back().data="<PLUGIN name='truncated'";
    check(!device::decodeVst3Metadata(changed,canonical,&decoded,error) && decoded.size()==3,"bad later XML cannot publish a good prefix");
    const auto previous=descriptions.front().createIdentifierString();
    registry->blockScan("vst3",canonical,"disabled by owner",123,error);
    const auto blocked=device::scanVst3File(*scanner,juce::String::fromUTF8(utf8(file).c_str()),descriptions);
    check(blocked.state==scan::State::Quarantined && descriptions.size()==3 && descriptions.front().createIdentifierString()==previous,"path-hint discovery respects quarantine and leaves prior owned descriptors intact");
    registry->unblockScan("vst3",canonical,error);
    fs::create_directory(temp.path()/"alias");
    const auto parent=juce::String::fromUTF8(utf8(file).c_str());
    const auto again=device::scanVst3File(*scanner,parent,descriptions);
    check(again.state==scan::State::Ready && descriptions.front().createIdentifierString()==previous,"explicit re-enable and repeated child scans preserve saved JUCE identifiers");
    const auto alias=temp.path()/"linked fixture.vst3";std::error_code ec;fs::create_directory_symlink(file,alias,ec);
    const auto requested=ec?file:alias;
    const auto aliasResult=device::scanVst3File(*scanner,juce::String::fromUTF8(utf8(requested).c_str()),descriptions);
    auto expected=decoded.front();expected.fileOrIdentifier=juce::File(juce::String::fromUTF8(utf8(requested).c_str())).getFullPathName();
    check(aliasResult.state==scan::State::Ready && descriptions.front().createIdentifierString()==expected.createIdentifierString() && descriptions.front().fileOrIdentifier==expected.fileOrIdentifier,"native alias scan retains original JUCE UID path spelling after canonical protocol validation");
    if(ec)std::printf("native symlink unavailable; original path case checked\n");
    const auto bad=temp.path()/"invalid.vst3";std::ofstream(bad)<<"not a module";
    check(device::scanVst3File(*scanner,juce::String::fromUTF8(utf8(bad).c_str()),descriptions).state==scan::State::Failed,"invalid VST3 module becomes a named child scan failure");
    const auto held=descriptions.size();scan::Result callback;bool refused=false;error="untouched";
    {audio::CallbackScope scope;callback=device::scanVst3File(*scanner,parent,descriptions);
        refused=!device::decodeVst3Metadata(result.plugins,canonical,&decoded,error);}
    check(callback.state==scan::State::CallbackRefused && refused && allocations==0 && audio::callbackFileIo==0 &&
        error=="untouched" && descriptions.size()==held,"VST3 discovery/decoder callbacks refuse before allocation or descriptor/diagnostic/OS mutation");
}
} // namespace
int main(){try{contract();}catch(const std::exception& error){++failures;std::printf("EXCEPTION: %s\n",error.what());}
    std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
