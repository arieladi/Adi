// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "legacy_plugin_registry.hpp"
#include "temp_directory.hpp"
#include "adi/appdata.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/scan_catalogue.hpp"
#include "adi/ui/plugin_scan.hpp"
#include "juce/juce_device_loader.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <thread>
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
// The existing scan-cache executable is also a controlled protocol helper.
// These two cases exercise aggregate error policy, not a native plug-in factory.
inline std::atomic<bool>* cancelAfterRejection=nullptr;
inline unsigned rejectionClockCalls=0;
inline std::int64_t rejectionClock() noexcept {
    // A fresh candidate's admission is the first clock call. Its completed
    // rejected-result transaction is the second; cancel before the next probe.
    if(++rejectionClockCalls==2 && cancelAfterRejection)cancelAfterRejection->store(true,std::memory_order_relaxed);
    return 123;
}
struct RejectionCancellation {
    explicit RejectionCancellation(std::atomic<bool>& cancel){cancelAfterRejection=&cancel;rejectionClockCalls=0;}
    ~RejectionCancellation(){cancelAfterRejection=nullptr;rejectionClockCalls=0;}
};
inline bool fatalHeadlines() {
    using device::DiscoveryMode;using device::DiscoveryOutcome;bool passed=true;
    for(const bool cancelCase:{false,true}) {
        TempDirectory scratch("discovery_operation",cancelCase?"fatal_cancel":"fatal_deployment");
        const auto root=scratch.path()/"modules";fs::create_directory(root);
        const auto rejected=root/"a-rejected.clap",fatal=root/"b-fatal.clap";
        {std::ofstream file(rejected,std::ios::binary);file<<"reject";}
        {std::ofstream file(fatal,std::ios::binary);file<<"unsupported";}
        scan::Config config;config.registry=scratch.path()/"registry.sqlite";config.executable=scratch.path()/"controlled-helper.exe";
        fs::copy_file(media::pathFromUtf8(ADI_DISCOVERY_TEST_HELPER),config.executable);
        std::atomic<bool> cancel{false};RejectionCancellation cancellation(cancel);
        if(cancelCase){config.clock=rejectionClock;config.process.cancel=&cancel;}
        std::string error;{auto registry=PluginRegistry::open(config.registry,error);require(static_cast<bool>(registry),"fatal-case empty registry");}
        device::JuceDeviceLoader loader(false);loader.setScanConfig(config);loader.addSearchPath(juce::File(juce::String::fromUTF8(text(root).c_str())));
        const auto accepted=loader.discover(DiscoveryMode::CacheOnly);
        require(accepted && accepted->outcome==DiscoveryOutcome::Published && accepted->catalogue,"fatal-case baseline publication");
        const auto result=loader.discover(DiscoveryMode::Incremental);
        const auto launches=[](const fs::path& module) {std::ifstream file(media::pathFromUtf8(text(module)+".launches"));
            return std::count(std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>{},'\n');};
        const auto expectedState=cancelCase?scan::State::Cancelled:scan::State::Unavailable;
        const auto expectedOutcome=cancelCase?DiscoveryOutcome::Cancelled:DiscoveryOutcome::Failed;
        const bool sequence=result && result->outcome==expectedOutcome && !result->report.complete &&
            result->report.failures.size()==2 && result->report.failures[0].state==scan::State::Failed &&
            result->report.failures[1].state==expectedState && launches(rejected)==1 && launches(fatal)==(cancelCase?0:1) &&
            result->catalogue==accepted->catalogue && accepted->report.complete;
        const auto expected=cancelCase?std::string{"discovery cancelled"}:std::string{"scanner helper unavailable or scan cancelled"};
        const bool headline=sequence && result->error==expected && result->error!=result->report.failures[0].error;
        std::printf("DISCOVERY_FATAL %s\n",Json{{"case",cancelCase?"cancel_after_rejection":"unavailable_after_rejection"},
            {"controlled_protocol_helper",true},{"sequence_verified",sequence},{"expected",expected},
            {"actual",result?result->error:"no result"},{"passed",headline}}.dump().c_str());
        passed=passed && headline;
    }
    return passed;
}
inline void unreadableSearchDirectory() {
#if !defined(_WIN32)
    using device::DiscoveryMode;using device::DiscoveryOutcome;
    TempDirectory scratch("discovery_operation","unreadable");
    const auto root=scratch.path()/"VST3",denied=root/"Unreadable",other=root/"Readable";
    fs::create_directories(denied);
    fs::create_directories(other);
    const auto fixture=media::pathFromUtf8(ADI_CATALOGUE_FIXTURE);
    fs::copy(fixture,other/fixture.filename(),fs::copy_options::recursive);
    fs::copy(fixture,denied/fixture.filename(),fs::copy_options::recursive);
    scan::Config config;
    config.registry=scratch.path()/"registry.sqlite";
    config.executable=media::pathFromUtf8(ADI_CATALOGUE_SCANNER);
    device::PluginSearchPaths paths;
    paths.vst3.add(juce::File(juce::String::fromUTF8(text(root).c_str())));
    device::JuceDeviceLoader before(false);
    before.setScanConfig(config);before.setSearchPaths(paths);
    const auto full=before.discover(DiscoveryMode::Incremental);
    require(full && full->outcome==DiscoveryOutcome::Published && full->catalogue->vst3.size()==6,
            "SC-425: both VST3 bundles are found while the folders are readable");

    struct RestorePermission {
        fs::path path;
        ~RestorePermission() { std::error_code error;fs::permissions(path,fs::perms::owner_all,error); }
    } restore{denied};
    fs::permissions(denied,fs::perms::none);
    std::error_code deniedError;
    const fs::directory_iterator inaccessible(denied,deniedError);
    if (!deniedError) return; // Root can read this fixture despite chmod.
    require(scan::isAccessDenied(deniedError),"SC-425: test fixture denies directory enumeration");

    device::JuceDeviceLoader loader(false);
    loader.setScanConfig(config);loader.setSearchPaths(paths);
    for(const auto mode:{DiscoveryMode::Incremental,DiscoveryMode::Refresh}) {
        const auto result=loader.discover(mode);
        require(result && result->outcome==DiscoveryOutcome::Published && result->report.complete &&
                result->report.failures.size()==1 && result->catalogue->vst3.size()==3 &&
                result->report.failures.front().state==scan::State::UnreadablePath &&
                result->report.failures.front().path==text(fs::canonical(denied)),
                "SC-425: the unreadable folder is named and the readable VST3s publish");
        ui::PluginScan displayed;
        displayed.phase=ui::PluginScan::Phase::Done;
        displayed.listing.plugins.resize(result->catalogue->vst3.size());
        ui::applyReport(displayed.listing,result->report);
        require(ui::pluginScanStatus(displayed).find("1 unreadable search location was skipped")!=std::string::npos,
                "SC-425: the published scan explains the skipped location");
    }
    std::string error;
    const auto retained=scan::readCatalogue(config.registry,error);
    require(retained && retained->modules.size()==2,
            "SC-425: scanning with an unreadable folder leaves its cached metadata intact");
#endif
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
inline void refreshModes() {
    using device::DiscoveryMode;using device::DiscoveryOutcome;
    TempDirectory scratch("discovery_operation","refresh");const auto root=scratch.path()/"modules";fs::create_directory(root);
    const auto module=root/"refresh.clap";copyClap(module);
    scan::Config config;config.registry=scratch.path()/"registry.sqlite";config.executable=scratch.path()/"owned-helper.exe";
    config.cacheMode=ScanCacheMode::Refresh;config.workers=4;
    fs::copy_file(media::pathFromUtf8(ADI_CATALOGUE_SCANNER),config.executable);
    std::atomic<bool> cancel{false};config.process.cancel=&cancel;
    device::JuceDeviceLoader loader(false);loader.setScanConfig(config);loader.addSearchPath(juce::File(juce::String::fromUTF8(text(root).c_str())));
    const auto cold=loader.discover(DiscoveryMode::Incremental),warm=loader.discover(DiscoveryMode::Incremental);
    require(cold && warm && cold->outcome==DiscoveryOutcome::Published && warm->outcome==DiscoveryOutcome::Published &&
        pids(module).child==1 && pids(module).parent==0,"Incremental must explicitly reuse even if the stored raw Config requested Refresh");
    const auto binary=fs::is_directory(module)?module/"Contents"/"MacOS"/module.stem():module;
    const auto stamp=fs::last_write_time(binary);const auto size=fs::file_size(binary);
    const auto refreshed=loader.discover(DiscoveryMode::Refresh);
    require(refreshed && refreshed->mode==DiscoveryMode::Refresh && refreshed->outcome==DiscoveryOutcome::Published &&
        refreshed->catalogue->clap.size()==1 && pids(module).child==2 && pids(module).parent==0 &&
        fs::last_write_time(binary)==stamp && fs::file_size(binary)==size && loader.clap().libraryCount()==0,
        "real Refresh must enumerate the unchanged CLAP in one new child and publish metadata only");
    const auto reused=loader.discover(DiscoveryMode::Incremental);
    require(reused && reused->outcome==DiscoveryOutcome::Published && pids(module).child==2,
        "Refresh must not turn the next Incremental into another fresh scan");
    cancel=true;const auto cancelled=loader.discover(DiscoveryMode::Refresh);cancel=false;
    require(cancelled && cancelled->outcome==DiscoveryOutcome::Cancelled && cancelled->catalogue==reused->catalogue && pids(module).child==2,
        "cancelled Refresh must retain the accepted catalogue without native module loading");
    fs::remove(config.executable);const auto unavailable=loader.discover(DiscoveryMode::Refresh);
    require(unavailable && unavailable->outcome==DiscoveryOutcome::Failed && unavailable->catalogue==reused->catalogue,
        "unavailable Refresh helper must retain the accepted immutable catalogue");
    const auto cached=loader.discover(DiscoveryMode::CacheOnly);
    require(cached && cached->outcome==DiscoveryOutcome::Published && cached->catalogue->clap.size()==1 && pids(module).child==2,
        "abandoned Refresh must retain the persisted good metadata for helper-free CacheOnly");
    fs::copy_file(media::pathFromUtf8(ADI_CATALOGUE_SCANNER),config.executable);
    const auto rejected=root/"rejected.clap";copyClap(rejected);std::string error;
    auto scanner=scan::Scanner::open(config,error);require(scanner!=nullptr,"open real scanner for rejected metadata fixture");
    const auto reject=+[](const std::vector<scan::Plugin>&,const std::string&,void*,std::string& diagnostic) {
        diagnostic="owned format validation refusal";return false;
    };
    require(scanner->probe("clap",rejected,reject).state==scan::State::Failed && pids(rejected).child==1,
        "actual CLAP factory plus explicit owner rejection must seed a deterministic negative");
    const auto skipped=loader.discover(DiscoveryMode::Incremental);
    require(skipped && skipped->outcome==DiscoveryOutcome::Published && skipped->catalogue->clap.size()==1 &&
        skipped->report.failures.size()==1 && pids(rejected).child==1,"Incremental must retain the deterministic negative without a child");
    const auto recovered=loader.discover(DiscoveryMode::Refresh);
    require(recovered && recovered->outcome==DiscoveryOutcome::Published && recovered->report.failures.empty() &&
        recovered->catalogue->clap.size()==2 && pids(rejected).child==2 && pids(rejected).parent==0,
        "Refresh must retry and replace the same-stamp owner rejection through the real CLAP factory");
    auto registry=PluginRegistry::open(config.registry,error);require(registry!=nullptr,"open Refresh block owner");
    const auto canonical=text(fs::canonical(rejected));require(registry->blockScan("clap",canonical,"manual refresh block",123,error),"write Refresh manual block");
    const auto blocked=loader.discover(DiscoveryMode::Refresh);
    require(blocked && blocked->outcome==DiscoveryOutcome::Published && blocked->catalogue->clap.size()==1 &&
        blocked->report.failures.size()==1 && blocked->report.failures.front().state==scan::State::Quarantined && pids(rejected).child==2,
        "Refresh must omit a manually blocked module without re-enabling it");
    require(cold->catalogue->clap.size()==1 && recovered->catalogue->clap.size()==2,"Refresh must not mutate retained snapshots");
    {audio::CallbackScope callback;require(!loader.discover(DiscoveryMode::Refresh),"audio callback accepted Refresh");}
    std::printf("DISCOVERY_REFRESH %s\n",Json{{"passed",true},{"real_clap_factory",true},{"same_stamp_refresh",true},
        {"negative_retried",true},{"manual_preserved",true},{"abandon_retains_catalogue",true},{"parent_loads",0}}.dump().c_str());
}
inline void parallelDiscovery() {
    using device::DiscoveryMode;
    using device::DiscoveryOutcome;
    TempDirectory scratch("discovery_operation", "parallel");
    const auto root = scratch.path() / "modules";
    const auto vstOriginal = media::pathFromUtf8(ADI_CATALOGUE_FIXTURE);
    std::vector<fs::path> clapPaths;
    for (unsigned i = 0; i < 4; ++i) {
        const auto directory = root / std::to_string(i);
        fs::create_directories(directory);
        fs::copy(vstOriginal, directory / vstOriginal.filename(), fs::copy_options::recursive);
        const auto clap = directory / "probe.clap";
        copyClap(clap);
        clapPaths.push_back(clap);
    }
    std::vector<std::string> serial;
    for (const auto workers : {1U, 4U}) {
        scan::Config config;
        config.executable = media::pathFromUtf8(ADI_CATALOGUE_SCANNER);
        config.registry = scratch.path() / ("workers-" + std::to_string(workers) + ".sqlite");
        config.workers = workers;
        device::JuceDeviceLoader loader(false);
        loader.setScanConfig(config);
        loader.addSearchPath(juce::File(juce::String::fromUTF8(text(root).c_str())));
        const auto cold = loader.discover(DiscoveryMode::Incremental);
        require(cold && cold->outcome == DiscoveryOutcome::Published && cold->report.failures.empty() &&
                    cold->catalogue && cold->catalogue->vst3.size() == 12 && cold->catalogue->clap.size() == 4,
                "cold worker comparison discovers all real modules in both formats");
        const auto list = loader.knownPlugins();
        if (workers == 1) serial = list;
        else require(list == serial, "K=4 publishes the same ordered catalogue as K=1");
        const auto warm = loader.discover(DiscoveryMode::Incremental);
        require(warm && warm->outcome == DiscoveryOutcome::Published && warm->report.failures.empty() &&
                    loader.knownPlugins() == list && loader.clap().libraryCount() == 0,
                "warm discovery retains every class without loading any plug-in in the owner");
        for (const auto& path : clapPaths)
            require(pids(path).child == (workers == 1 ? 1U : 2U) && pids(path).parent == 0,
                    "each fresh registry probes each unchanged CLAP once in a child");
    }
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
inline void clapHintAndMove() {
    using device::DiscoveryMode;
    TempDirectory scratch("discovery_operation","clap_hint_and_move");
    const auto oldPath=scratch.path()/"a-original.clap",currentPath=scratch.path()/"z-current.clap";
    copyClap(oldPath);
    scan::Config config;config.registry=scratch.path()/"registry.sqlite";
    config.executable=media::pathFromUtf8(ADI_CATALOGUE_SCANNER);
    std::string error;
    {auto scanner=scan::Scanner::open(config,error);
        require(scanner && scanner->probe("clap",oldPath).state==scan::State::Ready,"seed original CLAP row");}
    rows::PluginRef ref;ref.format="clap";ref.uid="org.adi.catalogue.runtime";ref.pathHint=text(oldPath);
    engine::DeviceRequest request;request.ref=&ref;request.sampleRate=48000;request.maxFrames=64;
    const auto load=[&](device::JuceDeviceLoader& loader) {
        const auto instance=loader.fn()(request,error);return instance && instance->loaded();
    };
    {
        device::JuceDeviceLoader loader(false);loader.setScanConfig(config);
        require(load(loader) && pids(oldPath).parent==1,"original CLAP opens from the catalogue");
    }
    const auto oldCanonical=text(fs::canonical(oldPath));
    copyClap(currentPath);fs::remove_all(oldPath);
    {
        device::JuceDeviceLoader loader(false);loader.setScanConfig(config);
        const auto stored=scan::readCatalogue(config.registry,error);
        require(stored && stored->find("clap",oldCanonical),"CacheOnly retains the stale row until owner discovery");
        ref.pathHint=text(currentPath);
        require(load(loader) && pids(currentPath).child==1 && loader.stats().scans==0,
            "compatibility load probes the installed CLAP hint when only a stale candidate exists");
        require(load(loader) && pids(currentPath).child==1 && loader.stats().catalogueReads==1,
            "known installed CLAP hint instantiates without a helper or SQL reread");
    }
    for(const auto& hint:{text(oldPath),std::string{}}) {
        device::JuceDeviceLoader loader(false);loader.setScanConfig(config);ref.pathHint=hint;
        const auto snapshot=loader.discover(DiscoveryMode::CacheOnly);
        require(snapshot && snapshot->catalogue && snapshot->catalogue->clap.size()==2,"both existing and stale CLAP rows hydrate");
        require(load(loader) && pids(currentPath).child==1 && loader.stats().scans==0,
            "moved CLAP falls back to the live copy for stale and empty hints");
    }
    copyClap(oldPath);
    {
        device::JuceDeviceLoader loader(false);loader.setScanConfig(config);ref.pathHint=text(currentPath);
        require(load(loader) && pids(currentPath).parent==4,"the saved hint selects between both installed CLAP copies");
    }
    fs::remove_all(oldPath);
    device::JuceDeviceLoader owner(false);owner.setScanConfig(config);
    owner.addSearchPath(juce::File(juce::String::fromUTF8(text(scratch.path()).c_str())));
    const auto fresh=owner.discover(DiscoveryMode::Incremental);
    const auto stored=scan::readCatalogue(config.registry,error);
    require(fresh && fresh->outcome==device::DiscoveryOutcome::Published && fresh->catalogue &&
        fresh->catalogue->clap.size()==1 && stored && !stored->find("clap",oldCanonical) &&
        stored->modules.size()==1 && pids(currentPath).child==1,
        "a complete owner walk retires the stale row and reuses the live module");
}
struct DiscoveryGate {
    std::atomic<bool> entered{false}, arrived{false}, released{false}, ranOnCaller{false};
    std::thread::id caller=std::this_thread::get_id();
    static inline DiscoveryGate* active=nullptr;

    DiscoveryGate(){active=this;}
    ~DiscoveryGate(){active=nullptr;}
    void reset(){entered=false;arrived=false;released=false;ranOnCaller=false;}

    static std::int64_t clock() noexcept {
        using namespace std::chrono_literals;
        auto& gate=*active;
        if(!gate.entered.exchange(true)) {
            gate.ranOnCaller=std::this_thread::get_id()==gate.caller;
            gate.arrived=true;
            const auto deadline=std::chrono::steady_clock::now()+5s;
            while(!gate.released.load() && std::chrono::steady_clock::now()<deadline)
                std::this_thread::sleep_for(1ms);
        }
        return scan::systemUtcMicros();
    }
};

inline void savedPathAfterDiscovery(const juce::PluginDescription& instrument) {
    using namespace std::chrono_literals;
    using device::DiscoveryMode;using device::DiscoveryOutcome;using device::DiscoveryPhase;
    TempDirectory scratch("discovery_operation","saved_path");
    const auto root=fs::canonical(scratch.path());
    const auto empty=root/"empty";fs::create_directory(empty);
    const auto clap=root/"outside.clap";copyClap(clap);
    const auto source=media::pathFromUtf8(ADI_CATALOGUE_FIXTURE);
    const auto vst3=root/source.filename();
    fs::copy(source,vst3,fs::copy_options::recursive);
    scan::Config config;
    config.registry=root/"registry.sqlite";
    config.executable=root/"saved-path-scanner.exe";
    fs::copy_file(media::pathFromUtf8(ADI_CATALOGUE_SCANNER),config.executable);
    device::JuceDeviceLoader loader(false);
    loader.setScanConfig(config);
    loader.addSearchPath(juce::File(juce::String::fromUTF8(text(empty).c_str())));
    const auto job=loader.startDiscovery(DiscoveryMode::Incremental,1);
    const auto deadline=std::chrono::steady_clock::now()+15s;
    while(loader.discoveryProgress().phase!=DiscoveryPhase::Ready && std::chrono::steady_clock::now()<deadline)
        std::this_thread::sleep_for(1ms);
    const auto initial=loader.adoptDiscovery(job.job);
    std::string error;
    require(initial && initial->outcome==DiscoveryOutcome::Published && initial->catalogue &&
            initial->catalogue->clap.empty() && initial->catalogue->vst3.empty() && loader.hydrateCatalogue(error),
            "an adopted empty Browser catalogue starts the saved-path fixture");
    rows::PluginRef ref;
    ref.format="clap";ref.uid="org.adi.catalogue.runtime";ref.pathHint=text(clap);
    engine::DeviceRequest request;request.ref=&ref;request.sampleRate=48000;request.maxFrames=64;
    {
        auto registry=PluginRegistry::open(config.registry,error);
        require(registry && registry->blockScan("clap",text(fs::canonical(clap)),"blocked fixture",1,error),
                "block the installed saved-path fixture before its first lookup");
        require(!loader.fn()(request,error) && pids(clap).child==0 && pids(clap).parent==0,
                "an explicitly blocked saved path stays blocked after discovery");
        require(registry->unblockScan("clap",text(fs::canonical(clap)),error),"enable the saved CLAP fixture");
    }
    const auto nativeClap=loader.fn()(request,error);
    require(nativeClap && nativeClap->loaded() && pids(clap).child==1 && pids(clap).parent==1,
            "a saved CLAP outside the search roots is probed in a child before loading");
    auto description=instrument;
    description.fileOrIdentifier=juce::File(juce::String::fromUTF8(text(vst3).c_str())).getFullPathName();
    ref.format="vst3";ref.uid=description.createIdentifierString().toStdString();ref.pathHint=text(vst3);
    const auto nativeVst3=loader.fn()(request,error);
    require(nativeVst3 && nativeVst3->loaded(),"a saved VST3 outside the search roots loads after discovery");
    ref.uid="unknown identity";ref.pathHint.clear();
    require(!loader.fn()(request,error) && error.find("NeedsDiscovery")!=error.npos &&
            loader.stats().scans==1 && loader.catalogue()==initial->catalogue &&
            initial->catalogue->clap.empty() && initial->catalogue->vst3.empty(),
            "a request without a path does not start a new scan or republish the Browser catalogue");
}
inline void asynchronousDiscovery() {
    using namespace std::chrono_literals;
    using namespace device;
    TempDirectory scratch("discovery_operation","async_startup");
    const auto root=scratch.path()/"modules";fs::create_directory(root);
    const auto clap=root/"owned.clap";copyClap(clap);
    const auto original=media::pathFromUtf8(ADI_CATALOGUE_FIXTURE);
    fs::copy(original,root/original.filename(),fs::copy_options::recursive);
    const auto wait=[&](auto done,const char* message) {
        const auto deadline=std::chrono::steady_clock::now()+15s;
        while(!done() && std::chrono::steady_clock::now()<deadline)
            std::this_thread::sleep_for(1ms);
        require(done(),message);
    };
    DiscoveryGate gate;
    scan::Config config;config.registry=scratch.path()/"registry.sqlite";
    config.executable=scratch.path()/"async-owned-scanner.exe";config.clock=DiscoveryGate::clock;
    fs::copy_file(media::pathFromUtf8(ADI_CATALOGUE_SCANNER),config.executable);
    JuceDeviceLoader loader(false);loader.setScanConfig(config);
    loader.addSearchPath(juce::File(juce::String::fromUTF8(text(root).c_str())));
    std::string error;
    const auto hydrate=loader.startDiscovery(DiscoveryMode::CacheOnly);
    require(hydrate.status==DiscoveryStart::Started,"start read-only startup hydration");
    wait([&]{return loader.discoveryProgress().phase==DiscoveryPhase::Ready;},"startup hydration completes");
    const auto hydrated=loader.adoptDiscovery(hydrate.job);
    require(hydrated && hydrated->outcome==DiscoveryOutcome::Missing && !fs::exists(config.registry),
        "read-only startup hydration reports missing storage without creating it");
    const auto startup=loader.startDiscovery(DiscoveryMode::Incremental);
    require(startup.status==DiscoveryStart::Started && startup.job!=0,"start the automatic worker count scan");
    wait([&]{return gate.arrived.load();},"worker reaches the held VST3 admission");
    const auto progress=loader.discoveryProgress();
    require(!gate.ranOnCaller && progress.job==startup.job && progress.phase==DiscoveryPhase::Vst3 &&
        progress.hasCandidateTotal && progress.total==1 && progress.completed==0 &&
        !loader.adoptDiscovery(startup.job) && !loader.catalogue(),"startup progress is visible while the worker is held");
    require(loader.startDiscovery(DiscoveryMode::Refresh).status==DiscoveryStart::Busy,
        "startup and Rescan share one worker owner");
    rows::PluginRef ref;ref.format="clap";ref.uid="org.adi.catalogue.runtime";ref.pathHint=text(clap);
    engine::DeviceRequest request;request.ref=&ref;request.sampleRate=48000;request.maxFrames=64;
    require(!loader.fn()(request,error) && error.find("NeedsDiscovery")!=error.npos &&
        pids(clap).child==0 && pids(clap).parent==0 && loader.stats().catalogueReads==1,
        "a runtime hint cannot start a competing probe or reread SQL");
    const auto finish=[&](std::uint64_t id) {
        gate.released=true;
        wait([&]{return loader.discoveryProgress().phase==DiscoveryPhase::Ready;},"worker reaches adoption");
        return loader.adoptDiscovery(id);
    };
    const auto first=finish(startup.job);
    require(first && first->outcome==DiscoveryOutcome::Published && loader.catalogue()==first->catalogue &&
        first->catalogue->clap.size()==1 && first->catalogue->vst3.size()==3 &&
        std::any_of(first->catalogue->vst3.begin(),first->catalogue->vst3.end(),
            [](const auto& description){return description.isInstrument;}) &&
        pids(clap).child==1 && pids(clap).parent==0,"startup adopts typed metadata without loading plug-ins in the owner");
    require(loader.hydrateCatalogue(error) && loader.catalogue()==first->catalogue &&
        loader.stats().catalogueReads==1,"managed project open keeps the adopted catalogue without synchronous SQL");
    const auto instance=loader.fn()(request,error);
    require(instance && instance->loaded() && pids(clap).child==1 && pids(clap).parent==1,
        "the adopted catalogue serves an ordinary device request without another helper");
    const auto instrument=std::find_if(first->catalogue->vst3.begin(),first->catalogue->vst3.end(),
        [](const auto& description){return description.isInstrument;});
    savedPathAfterDiscovery(*instrument);
    const auto warmStart=loader.startDiscovery(DiscoveryMode::Incremental,1);
    require(warmStart.status==DiscoveryStart::Started,"start an explicit one-worker Incremental");
    const auto warm=finish(warmStart.job);
    require(warm && warm->outcome==DiscoveryOutcome::Published && pids(clap).child==1,
        "warm asynchronous Incremental reuses unchanged plugins");
    gate.reset();
    const auto rescan=loader.startDiscovery(DiscoveryMode::Refresh,1);
    require(rescan.status==DiscoveryStart::Started,"start explicit Rescan");
    wait([&]{return gate.arrived.load();},"Rescan reaches its held admission");
    require(loader.cancelDiscovery(rescan.job),"cancel the current scan job");
    const auto cancelled=finish(rescan.job);
    require(cancelled && cancelled->outcome==DiscoveryOutcome::Cancelled && loader.catalogue()==warm->catalogue &&
        pids(clap).child==1,"cancelled Rescan retains the previous catalogue");
    const auto freshStart=loader.startDiscovery(DiscoveryMode::Refresh,1);
    require(freshStart.status==DiscoveryStart::Started,"start a complete Rescan");
    const auto fresh=finish(freshStart.job);
    require(fresh && fresh->outcome==DiscoveryOutcome::Published && loader.catalogue()==fresh->catalogue &&
        pids(clap).child==2,"Rescan performs a new probe and adopts its catalogue");
    gate.reset();
    const auto obsolete=loader.startDiscovery(DiscoveryMode::Refresh,1);
    require(obsolete.status==DiscoveryStart::Started,"start a Rescan before a settings change");
    wait([&]{return gate.arrived.load();},"obsolete scan reaches its held admission");
    loader.setSearchPaths({});
    const auto stale=finish(obsolete.job);
    require(stale && stale->outcome==DiscoveryOutcome::Stale && loader.catalogue()==fresh->catalogue,
        "a settings change cancels and invalidates the older job without replacing the catalogue");
    loader.addSearchPath(juce::File(juce::String::fromUTF8(text(root).c_str())));
    require(fs::remove(config.executable),"remove only the private helper");
    const auto failed=loader.startDiscovery(DiscoveryMode::Incremental,1);
    require(failed.status==DiscoveryStart::Started,"start owner job with unavailable helper");
    const auto refused=finish(failed.job);
    require(refused && refused->outcome==DiscoveryOutcome::Failed && !refused->report.complete &&
        refused->error=="scanner helper could not start" && refused->report.failures.size()==1 &&
        refused->report.failures.front().state==scan::State::HelperCouldNotStart &&
        refused->report.failures.front().path.empty() && loader.catalogue()==fresh->catalogue &&
        loader.lastScanReport().failures.size()==1 &&
        loader.lastScanReport().failures.front().state==scan::State::HelperCouldNotStart,
        "missing deployment arrives as a typed owner report and preserves adopted metadata");
    std::printf("DISCOVERY_ASYNC startup, reuse, refresh, cancellation, stale adoption and deployment refusal passed\n");
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
    require(cli({"adi_play","--scan-workers","4","never-open.adi"})==2 &&
                cli({"adi_play","--list","--cache-only","--scan-workers","4"})==2,
            "scanner workers require an incremental list operation");
    for (const auto* invalid : {"0", "5", "3x", "-1"})
        require(cli({"adi_play","--list","--scan-workers",invalid})==2,"scanner workers reject invalid counts");
    const auto root=scratch.path()/"modules";fs::create_directory(root);const auto module=root/"command.clap";copyClap(module);
    require(cli({"adi_play","--list","--search-only",text(root),"--scan-workers","4"})==0 &&
                pids(module).child==1 && pids(module).parent==0,
        "actual headless incremental operation failed or used parent module loading");
    require(cli({"adi_play","--list","--cache-only"})==0 && pids(module).child==1 && pids(module).parent==0,
        "actual headless CacheOnly rescanned its module");
    require(cli({"adi_play","--list","--refresh","--search-only",text(root),"--scan-workers","4"})==0 &&
                pids(module).child==2 && pids(module).parent==0,"actual headless Refresh did not reprobe the native module");
    require(cli({"adi_play","--list","--search-only",text(root)})==0 && pids(module).child==2,
        "actual headless Incremental failed to reuse refreshed metadata");
    require(cli({"adi_play","--refresh","never-open.adi"})==2 &&
                cli({"adi_play","--list","--refresh","--cache-only"})==2,
        "Refresh must require list and refuse CacheOnly without opening a project");
}
} // namespace discovery_test_detail
inline bool scannerDiscoveryContracts() {
    juce::ScopedJuceInitialiser_GUI initialize;
    try {discovery_test_detail::require(discovery_test_detail::fatalHeadlines(),"fatal discovery headline described an earlier nonfatal candidate");
        discovery_test_detail::unreadableSearchDirectory();
        auto receipt=discovery_test_detail::modes();discovery_test_detail::refreshModes();discovery_test_detail::parallelDiscovery();
        discovery_test_detail::cacheStates();discovery_test_detail::command();
        discovery_test_detail::clapHintAndMove();discovery_test_detail::asynchronousDiscovery();
        receipt["passed"]=true;receipt["typed_cache_states"]=true;receipt["actual_cli_modes"]=true;
        receipt["serial_parallel_catalogue_equal"]=true;
        std::printf("DISCOVERY_OPERATION %s\n",receipt.dump().c_str());return true;
    }catch(const std::exception& error){std::printf("DISCOVERY_OPERATION %s\n",discovery_test_detail::Json{{"passed",false},{"error",error.what()}}.dump().c_str());return false;}
}
} // namespace adi::test
