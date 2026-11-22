// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "temp_directory.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/scan_catalogue.hpp"
#include "juce/juce_device_loader.hpp"
#include <nlohmann/json.hpp>
#include <array>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace adi::test {
namespace scanner_readiness_detail {
enum class Phase : std::size_t {Discovered,Instantiated,StateRestored,AudioVerified};
inline constexpr std::array<const char*,4> names{"discovered","instantiated","state_restored","audio_verified"};
inline std::string text(const std::filesystem::path& path){const auto bytes=path.u8string();return {bytes.begin(),bytes.end()};}
struct Pids {unsigned child=0,parent=0;};
inline Pids readPids(const std::filesystem::path& path) {
    Pids result;std::ifstream input(path);long long pid=0;
    while(input>>pid){if(pid==static_cast<long long>(processId()))++result.parent;else ++result.child;}
    return result;
}
}
// Case 16: persisted discovery is metadata, not runtime/audio readiness.
// Own isolated paths keep other catalogue tests and fixture counters independent.
inline bool scannerReadinessContracts() {
    using namespace scanner_readiness_detail;
    namespace fs=std::filesystem;
    using Json=nlohmann::json;
    Json receipt={{"case",16},{"format","clap"},{"path","real_loader_and_device_instance"},{"phases",Json::array()}};
    for(const auto* name:names)receipt["phases"].push_back({{"phase",name},{"status","not_run"}});
    const auto mark=[&](Phase phase,bool ok,Json data) {
        auto& item=receipt["phases"][static_cast<std::size_t>(phase)];item["status"]=ok?"passed":"failed";item["evidence"]=std::move(data);return ok;
    };
    const auto finish=[&](bool ok){receipt["passed"]=ok;std::printf("SCANNER_READINESS %s\n",receipt.dump().c_str());return ok;};
    try {
        juce::ScopedJuceInitialiser_GUI initialize;
        TempDirectory scratch("scanner_readiness","case16");
        scan::Config config;config.registry=scratch.path()/"registry.sqlite";config.executable=scratch.path()/"owned-scanner.exe";
        fs::copy_file(media::pathFromUtf8(ADI_CATALOGUE_SCANNER),config.executable);
        const auto bundle=scratch.path()/"readiness.clap";
#if defined(__APPLE__)
        const auto binary=bundle/"Contents"/"MacOS"/"readiness";fs::create_directories(binary.parent_path());
#else
        const auto binary=bundle;
#endif
        fs::copy_file(media::pathFromUtf8(ADI_CATALOGUE_CLAP),binary);
        const auto canonical=text(fs::canonical(bundle));
        const auto pidFile=media::pathFromUtf8(canonical+".pids");
        std::string error;auto scanner=scan::Scanner::open(config,error);
        if(!scanner){receipt["error"]=error;return finish(false);}
        const auto found=scanner->probe("clap",bundle);scanner.reset();
        const auto stored=scan::readCatalogue(config.registry,error);
        device::JuceDeviceLoader loader;loader.setScanConfig(config);
        const auto discovered=readPids(pidFile);
        const bool hydrated=loader.hydrateCatalogue(error);
        if(!mark(Phase::Discovered,found.state==scan::State::Ready && found.plugins.size()==1 && stored && stored->pluginCount==1 &&
                 hydrated && loader.knownPlugins().size()==1 && loader.clap().libraryCount()==0 && loader.stats().requests==0 &&
                 discovered.child==1 && discovered.parent==0,
                 {{"metadata_plugins",found.plugins.size()},{"parent_libraries",loader.clap().libraryCount()},
                  {"loader_requests",loader.stats().requests},{"child_module_inits",discovered.child},{"parent_module_inits",discovered.parent}}))return finish(false);
        // Subsequent runtime work must succeed without consulting discovery.
        if(!fs::remove(config.executable)){receipt["error"]="cannot remove owned scanner";return finish(false);}
        rows::PluginRef ref;ref.format="clap";ref.uid="org.adi.catalogue.runtime";ref.pathHint=canonical;
        engine::DeviceRequest request;request.ref=&ref;request.sampleRate=48000;request.maxFrames=64;request.channels=2;
        auto instance=loader.fn()(request,error);
        juce::DynamicLibrary observedModule;
        if(!observedModule.open(juce::String::fromUTF8(text(binary).c_str()))){receipt["error"]="cannot observe already instantiated fixture";return finish(false);}
        using Counter=std::uint64_t(*)(std::uint32_t);
        const auto counter=reinterpret_cast<Counter>(observedModule.getFunction("adiCatalogueReadCounter"));
        if(!counter){receipt["error"]="fixture counter export missing";return finish(false);}
        const auto instantiated=readPids(pidFile);
        if(!mark(Phase::Instantiated,instance && instance->loaded() && instance->identity().uid==ref.uid && error.empty() &&
                 loader.stats().clap==1 && loader.stats().scans==0 && loader.clap().libraryCount()==1 &&
                 instantiated.child==1 && instantiated.parent==1 && counter(0)==1 && counter(1)==0 && counter(2)==0,
                 {{"loaded",instance && instance->loaded()},{"creates",counter(0)},{"restores",counter(1)},{"process_calls",counter(2)},
                  {"parent_module_inits",instantiated.parent},{"implicit_scans",loader.stats().scans}}))return finish(false);
        const std::vector<std::uint8_t> initial{'A','D','C','1',1,0,0,0},quarter{'A','D','C','1',4,0,0,0};
        auto invalid=quarter;invalid[4]=3;
        const bool before=instance->saveState("chunk")==initial;
        const bool rejected=!instance->loadState("chunk",invalid) && !instance->loadState("unsupported",quarter) && instance->saveState("chunk")==initial;
        const bool restored=instance->loadState("chunk",quarter);
        if(!mark(Phase::StateRestored,before && rejected && restored && instance->saveState("chunk")==quarter && counter(1)==1 && counter(2)==0,
                 {{"loaded_state",restored},{"invalid_state_preserved_previous",rejected},{"restores",counter(1)},{"process_calls",counter(2)}}))return finish(false);
        std::array<float,80> left{},right{},outLeft{},outRight{};
        for(std::size_t index=0;index<left.size();++index){left[index]=static_cast<float>(index+1)/256;right[index]=-static_cast<float>(index+2)/128;}
        const float* inputs[]{left.data(),right.data()};float* outputs[]{outLeft.data(),outRight.data()};
        bool exact=true;std::uint64_t frames=0;bool firstVerified=false;
        for(const auto count:{19,64,1}) {
            outLeft.fill(-7.0f);outRight.fill(-11.0f);
            engine::NodeIo io;io.in=inputs;io.out=outputs;io.channels=2;io.frames=count;io.blockOffset=5;io.sampleRate=48000;
            {audio::CallbackScope callback;instance->process(io);}
            for(std::size_t index=0;index<left.size();++index) {
                const bool inside=index>=5 && index<static_cast<std::size_t>(count+5);
                exact=exact && outLeft[index]==(inside?left[index]*0.25f:-7.0f) && outRight[index]==(inside?right[index]*0.25f:-11.0f);
            }
            frames+=static_cast<std::uint64_t>(count);
            if(count==19)firstVerified=exact && counter(2)==1 && counter(3)==19;
        }
        const bool passed=mark(Phase::AudioVerified,firstVerified && exact && counter(2)==3 && counter(3)==frames && counter(1)==1,
                 {{"first_process_verified",firstVerified},{"exact_stereo_pcm_and_guard_regions",exact},{"process_calls",counter(2)},
                  {"processed_frames",counter(3)},{"gain",0.25},{"sample_rate",48000},{"maximum_block",64}});
        return finish(passed);
    }catch(const std::exception& exception){receipt["error"]=exception.what();return finish(false);}
}
}
