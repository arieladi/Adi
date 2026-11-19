// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "legacy_plugin_registry.hpp"
#include "temp_directory.hpp"
#include "adi/appdata.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/scan_catalogue.hpp"
#include "juce/juce_device_loader.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace adi::test {
namespace discovery_test_detail {
namespace fs=std::filesystem;
using Json=nlohmann::json;
inline std::string text(const fs::path& path){const auto bytes=path.u8string();return {bytes.begin(),bytes.end()};}
inline void require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
struct Pids {unsigned child=0,parent=0;};
inline Pids pids(const fs::path& module) {
    Pids result;std::ifstream input(media::pathFromUtf8(text(fs::canonical(module))+".pids"));long long pid=0;
    while(input>>pid){if(pid==static_cast<long long>(processId()))++result.parent;else ++result.child;}
    return result;
}
inline void copyClap(const fs::path& module) {
#if defined(__APPLE__)
    const auto binary=module/"Contents"/"MacOS"/module.stem();fs::create_directories(binary.parent_path());
#else
    const auto binary=module;
#endif
    fs::copy_file(media::pathFromUtf8(ADI_CATALOGUE_CLAP),binary);
}
inline bool missesWithoutDiscovery(device::JuceDeviceLoader& loader) {
    rows::PluginRef reference;reference.format="clap";reference.uid="not.in.catalogue";
    engine::DeviceRequest request;request.ref=&reference;std::string error;
    return !loader.fn()(request,error) && error.find("NeedsDiscovery")!=error.npos && loader.stats().scans==0;
}
inline Json modes() {
    using device::DiscoveryMode;using device::DiscoveryOutcome;
    TempDirectory scratch("discovery_operation","modes");const auto root=scratch.path()/"modules";fs::create_directory(root);
    const auto clap=root/"first.clap";copyClap(clap);
    const auto vstOriginal=media::pathFromUtf8(ADI_CATALOGUE_FIXTURE);const auto vst=root/vstOriginal.filename();
    fs::copy(vstOriginal,vst,fs::copy_options::recursive);
    scan::Config config;config.registry=scratch.path()/"registry.sqlite";config.executable=scratch.path()/"owned-helper.exe";
    fs::copy_file(media::pathFromUtf8(ADI_CATALOGUE_SCANNER),config.executable);
    std::atomic<bool> cancel{false};config.process.cancel=&cancel;
    device::JuceDeviceLoader loader(false);loader.setScanConfig(config);loader.addSearchPath(juce::File(juce::String::fromUTF8(text(root).c_str())));
    const auto cold=loader.discover(DiscoveryMode::Incremental);
    require(cold && cold->outcome==DiscoveryOutcome::Published && cold->report.complete && cold->report.failures.empty() &&
        cold->catalogue && cold->catalogue->vst3.size()==3 && cold->catalogue->clap.size()==1,"cold real two-format discovery");
    require(pids(clap).child==1 && pids(clap).parent==0 && loader.clap().libraryCount()==0 && loader.stats().requests==0,
        "discovery loaded a CLAP in the parent or missed its one child init");
    const auto warm=loader.discover(DiscoveryMode::Incremental);
    require(warm && warm->outcome==DiscoveryOutcome::Published && warm->catalogue->vst3.size()==3 && warm->catalogue->clap.size()==1 &&
        pids(clap).child==1 && pids(clap).parent==0,"incremental unchanged discovery reran the CLAP factory");
    const auto added=root/"second.clap";copyClap(added);
    const auto grown=loader.discover(DiscoveryMode::Incremental);
    require(grown && grown->outcome==DiscoveryOutcome::Published && grown->catalogue->clap.size()==2 && grown->catalogue->vst3.size()==3 &&
        pids(clap).child==1 && pids(added).child==1 && pids(added).parent==0,"incremental add did not isolate the new candidate");
    require(cold->catalogue->clap.size()==1 && cold->report.failures.empty() && cold->outcome==DiscoveryOutcome::Published,
        "old immutable result changed after publication");
    {std::ofstream invalid(root/"bad.clap",std::ios::binary);invalid<<"owned invalid plug-in candidate";}
    const auto accepted=loader.discover(DiscoveryMode::Incremental);
    require(accepted && accepted->outcome==DiscoveryOutcome::Published && accepted->report.complete &&
        accepted->report.failures.size()==1 && accepted->catalogue->clap.size()==2 && accepted->catalogue->vst3.size()==3,
        "one bad candidate aborted discovery of its healthy siblings");
    cancel=true;const auto cancelled=loader.discover(DiscoveryMode::Incremental);cancel=false;
    require(cancelled && cancelled->outcome==DiscoveryOutcome::Cancelled && cancelled->report.cancelled &&
        cancelled->catalogue==accepted->catalogue && pids(clap).child==1 && pids(added).child==1,"cancelled refresh published partial metadata");
    require(fs::remove(config.executable),"remove private helper");
    const auto failed=loader.discover(DiscoveryMode::Incremental);
    require(failed && failed->outcome==DiscoveryOutcome::Failed && !failed->report.complete &&
        failed->catalogue==accepted->catalogue && !failed->report.failures.empty(),"helper failure lost or republished the accepted catalogue");
    require(grown->report.complete && grown->report.failures.empty(),"failure mutated an earlier report");
    // Moving every candidate away proves CacheOnly does not require even its
    // existence. This is intentionally metadata-only, not a freshness claim.
    fs::rename(root,scratch.path()/"temporarily-absent");
    const auto cached=loader.discover(DiscoveryMode::CacheOnly);
    require(cached && cached->outcome==DiscoveryOutcome::Published && cached->catalogue->vst3.size()==3 &&
        cached->catalogue->clap.size()==2 && loader.clap().libraryCount()==0,"CacheOnly needed a helper or candidate");
    fs::rename(scratch.path()/"temporarily-absent",root);
    require(pids(clap).child==1 && pids(clap).parent==0 && pids(added).child==1 && pids(added).parent==0,"CacheOnly executed a native module");
    const auto beforeStats=loader.stats();
    {audio::CallbackScope callback;require(!loader.discover(DiscoveryMode::Incremental) && !loader.discover(DiscoveryMode::CacheOnly),"callback accepted discovery");}
    require(loader.stats().scans==beforeStats.scans && loader.stats().catalogueReads==beforeStats.catalogueReads,"callback mutated discovery stats");

    // A syntactically valid common packet with bad VST3 XML must refuse the
    // complete two-format refresh, including the healthy staged CLAP prefix.
    std::string error;const auto stored=scan::readCatalogue(config.registry,error);require(stored.has_value(),"read raw accepted catalogue");
    const auto v=std::find_if(stored->modules.begin(),stored->modules.end(),[](const auto& m){return m.format=="vst3";});
    require(v!=stored->modules.end(),"stored VST3 module absent");auto broken=v->plugins;broken.back().data="<PLUGIN/>";
    const auto packet=scan::encodeMetadata("vst3",v->canonicalPath,broken,error);require(packet.has_value(),"encode common-valid malformed format data");
    {SQLite::Database db(text(config.registry),SQLite::OPEN_READWRITE);SQLite::Statement update(db,"UPDATE plugin_scan_cache SET packet=? WHERE format='vst3'");
        update.bind(1,*packet);update.exec();}
    const auto corrupt=loader.discover(DiscoveryMode::CacheOnly);
    require(corrupt && corrupt->outcome==DiscoveryOutcome::Failed && corrupt->catalogue==cached->catalogue &&
        !corrupt->error.empty() && cached->catalogue->clap.size()==2,"corrupt format data published a partial catalogue");
    device::JuceDeviceLoader fresh(false);fresh.setScanConfig(config);
    const auto freshCorrupt=fresh.discover(DiscoveryMode::CacheOnly);
    require(freshCorrupt && freshCorrupt->outcome==DiscoveryOutcome::Failed && !freshCorrupt->catalogue && fresh.knownPlugins().empty(),
        "fresh corrupt snapshot published its valid prefix");
    return {{"cold_vst3_classes",3},{"cold_clap_classes",1},{"added_clap_classes",1},{"clap_child_inits",2},
        {"clap_parent_inits",0},{"warm_reused_children",true},{"cache_only_without_helper_or_candidates",true},
        {"old_snapshots_unchanged",true},{"cancel_retains_snapshot",true},{"failure_retains_snapshot",true},
        {"bad_candidate_keeps_healthy_siblings",true},{"corrupt_format_whole_publication_refused",true},{"callback_refused",true}};
}
inline void cacheStates() {
    using device::DiscoveryMode;using device::DiscoveryOutcome;
    TempDirectory scratch("discovery_operation","cache_states");scan::Config config;
    config.executable=scratch.path()/"absent-helper.exe";config.registry=scratch.path()/"missing.sqlite";
    {std::atomic<bool> cancel{true};auto cancelledConfig=config;cancelledConfig.process.cancel=&cancel;
        device::JuceDeviceLoader loader(false);loader.setScanConfig(cancelledConfig);const auto result=loader.discover(DiscoveryMode::Incremental);
        require(result && result->outcome==DiscoveryOutcome::Cancelled && result->report.cancelled && !fs::exists(config.registry),
            "pre-cancelled empty-root operation published or created storage");}
    {device::JuceDeviceLoader loader(false);loader.setScanConfig(config);const auto result=loader.discover(DiscoveryMode::CacheOnly);
        require(result && result->outcome==DiscoveryOutcome::Missing && !result->catalogue && !fs::exists(config.registry) &&
            missesWithoutDiscovery(loader),"explicit missing CacheOnly silently allowed discovery");}
    for(const auto version:{1,2}) {
        config.registry=scratch.path()/("legacy"+std::to_string(version)+".sqlite");createLegacyPluginRegistry(config.registry,version);
        device::JuceDeviceLoader loader(false);loader.setScanConfig(config);const auto result=loader.discover(DiscoveryMode::CacheOnly);
        require(result && result->outcome==DiscoveryOutcome::Legacy && !result->catalogue && missesWithoutDiscovery(loader),"explicit legacy CacheOnly allowed lazy discovery");
        SQLite::Database db(text(config.registry),SQLite::OPEN_READONLY);require(db.execAndGet("PRAGMA user_version").getInt()==version,"CacheOnly migrated legacy storage");
    }
    config.registry=scratch.path()/"empty.sqlite";std::string error;
    {auto registry=PluginRegistry::open(config.registry,error);require(static_cast<bool>(registry),"create valid empty registry");}
    device::JuceDeviceLoader loader(false);loader.setScanConfig(config);const auto result=loader.discover(DiscoveryMode::CacheOnly);
    require(result && result->outcome==DiscoveryOutcome::Published && result->report.complete && result->catalogue &&
        result->catalogue->clap.empty() && result->catalogue->vst3.empty() && missesWithoutDiscovery(loader),"valid empty catalogue differs from missing storage");
}
struct ScopedHome {
#if defined(_WIN32)
    std::wstring saved;
    explicit ScopedHome(const fs::path& home) {
        wchar_t* value=nullptr;std::size_t count=0;
        if(_wdupenv_s(&value,&count,L"ADI_HOME")!=0)throw std::runtime_error("read test home");
        if(value){saved=value;std::free(value);}
        if(_wputenv_s(L"ADI_HOME",home.c_str())!=0)throw std::runtime_error("set test home");
    }
    ~ScopedHome(){(void)_wputenv_s(L"ADI_HOME",saved.c_str());}
#else
    std::string saved;bool existed=false;
    explicit ScopedHome(const fs::path& home){if(const auto* value=std::getenv("ADI_HOME")){saved=value;existed=true;}
        if(setenv("ADI_HOME",home.c_str(),1)!=0)throw std::runtime_error("set test home");}
    ~ScopedHome(){if(existed)(void)setenv("ADI_HOME",saved.c_str(),1);else(void)unsetenv("ADI_HOME");}
#endif
};
inline int cli(std::vector<std::string> arguments){std::vector<char*> pointers;for(auto& argument:arguments)pointers.push_back(argument.data());
    return ::adi_play_command_main(static_cast<int>(pointers.size()),pointers.data());}
inline void command() {
    TempDirectory scratch("discovery_operation","command");ScopedHome home(scratch.path()/"home");
    require(cli({"adi_play","--list","--cache-only"})==1 && !fs::exists(appdata::pluginRegistryFile()),"CLI missing CacheOnly reported success or wrote storage");
    require(cli({"adi_play","--cache-only","never-open.adi"})==2,"cache-only flag silently applied to project open");
    const auto root=scratch.path()/"modules";fs::create_directory(root);const auto module=root/"command.clap";copyClap(module);
    require(cli({"adi_play","--list","--search-only",text(root)})==0 && pids(module).child==1 && pids(module).parent==0,
        "actual headless incremental operation failed or used parent module loading");
    require(cli({"adi_play","--list","--cache-only"})==0 && pids(module).child==1 && pids(module).parent==0,
        "actual headless CacheOnly rescanned its module");
}
} // namespace discovery_test_detail
inline bool scannerDiscoveryContracts() {
    juce::ScopedJuceInitialiser_GUI initialize;
    try {auto receipt=discovery_test_detail::modes();discovery_test_detail::cacheStates();discovery_test_detail::command();
        receipt["passed"]=true;receipt["typed_cache_states"]=true;receipt["actual_cli_modes"]=true;
        std::printf("DISCOVERY_OPERATION %s\n",receipt.dump().c_str());return true;
    }catch(const std::exception& error){std::printf("DISCOVERY_OPERATION %s\n",discovery_test_detail::Json{{"passed",false},{"error",error.what()}}.dump().c_str());return false;}
}
} // namespace adi::test
