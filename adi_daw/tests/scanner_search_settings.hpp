// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "scanner_discovery.hpp"
#include "juce/plugin_search_settings.hpp"

namespace adi::test {
namespace search_settings_test_detail {
inline device::JuceDeviceLoader* changingLoader=nullptr;
inline const device::PluginSearchPaths* replacement=nullptr;
inline bool changedDuringScan=false;
inline std::int64_t changeRootsClock() noexcept {
    if(changingLoader) {
        auto* loader=changingLoader;changingLoader=nullptr;
        try {loader->setSearchPaths(*replacement);changedDuringScan=true;}catch(...){}
    }
    return scan::systemUtcMicros();
}
struct ResetClock {
    ~ResetClock(){changingLoader=nullptr;replacement=nullptr;}
};
}

inline bool scannerSearchSettingsContracts() {
    using namespace discovery_test_detail;
    using device::DiscoveryMode;using device::DiscoveryOutcome;
    try {
        unsigned checks=0;
        const auto verify=[&](bool value,const char* what){++checks;require(value,what);};
        TempDirectory scratch("scanner_settings","sources");
        const auto base=juce::File(juce::String::fromUTF8(text(scratch.path()).c_str()));
        const auto original=media::pathFromUtf8(ADI_CATALOGUE_FIXTURE);
        for(const auto* name:{"vst-default","clap-default","vst-custom","clap-custom"}) {
            const auto dir=scratch.path()/name;fs::create_directory(dir);
            fs::copy(original,dir/original.filename(),fs::copy_options::recursive);
            copyClap(dir/"source.clap");
        }
        settings::AppSettings settings(appdata::App::Daw,scratch.path()/"settings.json");
        settings.setLogUserChanges(false);std::string error;const settings::ChangeContext user{};
        verify(settings.set("plugins.useVst3SystemFolders",false,user,error),"set VST3 defaults off");
        verify(settings.set("plugins.vst3Folders",Json::array({"vst-custom","","vst-custom"}),user,error),"set distinct VST3 source");
        verify(settings.set("plugins.clapFolders",Json::array({"clap-custom"}),user,error),"set distinct CLAP source");
        verify(settings.save(error),"persist existing source settings");
        settings::AppSettings reopened(appdata::App::Daw,settings.file());
        device::PluginSearchPaths defaults;defaults.vst3.add(base.getChildFile("vst-default"));
        defaults.clap.push_back(base.getChildFile("clap-default").getFullPathName().toStdString());
        auto paths=device::pluginSearchPathsFromSettings(reopened,defaults,base,error);
        verify(paths && paths->vst3.getNumPaths()==1 && paths->vst3[0]==base.getChildFile("vst-custom") &&
            paths->clap.size()==2 && paths->clap[0]==defaults.clap[0] &&
            paths->clap[1]==base.getChildFile("clap-custom").getFullPathName().toStdString(),
            "saved settings resolve per format; disabling VST3 defaults retains CLAP conventions");
        scan::Config config;config.registry=scratch.path()/"registry.sqlite";config.executable=scratch.path()/"owned-helper.exe";
        device::JuceDeviceLoader loader(false);loader.setScanConfig(config);
        verify(device::configurePluginSearchPaths(loader,reopened,error),"production Settings adapter accepts saved values");
        for(int i=0;i<3;++i)loader.setSearchPaths(*paths);
        verify(loader.stats().scans==0 && loader.stats().catalogueReads==0 && loader.clap().libraryCount()==0 &&
            !fs::exists(config.registry) && !fs::exists(config.executable),"configuration neither checks helper nor opens registry/loads/scans");
        fs::copy_file(media::pathFromUtf8(ADI_CATALOGUE_SCANNER),config.executable);
        const auto cold=loader.discover(DiscoveryMode::Incremental);
        verify(cold && cold->outcome==DiscoveryOutcome::Published && cold->catalogue->vst3.size()==3 &&
            cold->catalogue->clap.size()==2 && cold->report.failures.empty(),"actual factories discover only their configured format roots");
        for(const auto& p:cold->catalogue->vst3)
            verify(p.fileOrIdentifier==juce::String::fromUTF8(text(fs::canonical(scratch.path()/"vst-custom"/original.filename())).c_str()),
                "VST3 excludes CLAP-only and disabled system roots");
        verify(pids(scratch.path()/"vst-default/source.clap").child==0 && pids(scratch.path()/"vst-custom/source.clap").child==0 &&
            pids(scratch.path()/"clap-default/source.clap").child==1 && pids(scratch.path()/"clap-custom/source.clap").child==1,
            "CLAP native entry receipts prove format root separation");
        loader.setSearchPaths(*paths);const auto warm=loader.discover(DiscoveryMode::Incremental);
        verify(warm && warm->outcome==DiscoveryOutcome::Published && pids(scratch.path()/"clap-custom/source.clap").child==1,
            "unchanged configuration and Incremental reuse native cache");
        rows::PluginRef reference;reference.format="clap";reference.uid="org.adi.catalogue.runtime";
        reference.pathHint=text(fs::canonical(scratch.path()/"clap-custom/source.clap"));
        engine::DeviceRequest request;request.ref=&reference;request.sampleRate=48000;request.maxFrames=64;
        auto live=loader.fn()(request,error);verify(live && live->loaded(),"instantiate one owned CLAP before changing roots");
        const auto known=loader.knownPlugins();const auto prior=loader.stats();device::PluginSearchPaths empty;
        loader.setSearchPaths(empty);
        verify(live->loaded() && loader.knownPlugins()==known && loader.stats().scans==prior.scans &&
            loader.stats().catalogueReads==prior.catalogueReads && warm->catalogue->clap.size()==2,
            "root replacement preserves loaded instance and immutable accepted catalogue without reads/probes");
        verify(fs::remove(config.executable),"remove private helper after discovery");
        auto second=loader.fn()(request,error);
        verify(second && second->loaded() && loader.stats().scans==prior.scans,"known project instance still loads without helper after root change");
        const auto cached=loader.discover(DiscoveryMode::CacheOnly);
        verify(cached && cached->outcome==DiscoveryOutcome::Published && cached->catalogue->vst3.size()==3 &&
            cached->catalogue->clap.size()==2,"CacheOnly does not reinterpret configured roots as deletion or hide cached metadata");
        verify(reopened.set("plugins.vst3Folders",Json::array({std::string("prefix\0other",12)}),user,error),"fixture writes a schema-valid malformed path");
        verify(!device::configurePluginSearchPaths(loader,reopened,error) && !error.empty() && loader.knownPlugins()==known,
            "embedded NUL cannot silently select a truncated folder or replace accepted metadata");
        verify(reopened.set("plugins.vst3Folders",Json::array({"vst-custom"}),user,error) &&
            reopened.set("plugins.useVst3SystemFolders",true,user,error),"enable only VST3 conventions again");
        paths=device::pluginSearchPathsFromSettings(reopened,defaults,base,error);
        verify(paths && paths->vst3.getNumPaths()==2 && paths->clap.size()==2,"enabled VST3 defaults append its custom roots without changing CLAP");
        loader.setSearchPaths(*paths);
        {audio::CallbackScope callback;loader.setSearchPaths(empty);}
        fs::copy_file(media::pathFromUtf8(ADI_CATALOGUE_SCANNER),config.executable);
        const auto refreshed=loader.discover(DiscoveryMode::Refresh);
        verify(refreshed && refreshed->outcome==DiscoveryOutcome::Published && refreshed->catalogue->vst3.size()==6 &&
            refreshed->catalogue->clap.size()==2 && pids(scratch.path()/"clap-custom/source.clap").child==2,
            "callback root change refused; explicit Refresh consumes newly configured roots");
        // A controlled owner callback changes configuration after admission.
        // Both format phases must retain the old roots; only the next job uses new ones.
        config.clock=search_settings_test_detail::changeRootsClock;
        device::JuceDeviceLoader reentrant(false);reentrant.setScanConfig(config);reentrant.setSearchPaths(*paths);
        search_settings_test_detail::ResetClock reset;
        search_settings_test_detail::changingLoader=&reentrant;search_settings_test_detail::replacement=&empty;
        search_settings_test_detail::changedDuringScan=false;
        const auto admitted=reentrant.discover(DiscoveryMode::Refresh);
        verify(search_settings_test_detail::changedDuringScan && admitted && admitted->outcome==DiscoveryOutcome::Published &&
            admitted->catalogue->vst3.size()==6 && admitted->catalogue->clap.size()==2,"admitted discovery captures roots across a reentrant owner change");
        const auto next=reentrant.discover(DiscoveryMode::Incremental);
        verify(next && next->outcome==DiscoveryOutcome::Published && next->catalogue->vst3.empty() && next->catalogue->clap.empty() &&
            admitted->catalogue->vst3.size()==6,"next explicit operation uses changed roots; earlier immutable result remains intact");
        std::printf("SCANNER_SEARCH_SETTINGS {\"checks\":%u,\"status\":\"PASS\",\"audio_device_opened\":false,\"installed_roots_scanned\":false}\n",checks);
        return true;
    }catch(const std::exception& e){std::printf("FAIL -- scanner source settings: %s\n",e.what());return false;}
}
} // namespace adi::test
