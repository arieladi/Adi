// SPDX-License-Identifier: GPL-3.0-or-later
//
// See juce_device_loader.hpp. ADR-0122.

#include "juce/juce_device_loader.hpp"
#include "juce/builtin_host.hpp"
#include "juce/mix_pipeline_host.hpp"
#include "adi/mix_functions.hpp"
#include "adi/appdata.hpp"
#include "adi/scan_catalogue.hpp"
#include "juce/vst3_scan_host.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace adi::device {

JuceDeviceLoader::JuceDeviceLoader(bool includeDefaultSearchPaths) {
    if(includeDefaultSearchPaths)setSearchPaths(defaultSearchPaths());
}

PluginSearchPaths JuceDeviceLoader::defaultSearchPaths() const {
    if(audio::onAudioThread)return {};
    return {vst3_.defaultSearchPaths(),ClapHost::defaultSearchPaths()};
}

void JuceDeviceLoader::setSearchPaths(const PluginSearchPaths& paths) {
    if(audio::onAudioThread)return;
    bool same=searchPaths_->clap==paths.clap && searchPaths_->vst3.getNumPaths()==paths.vst3.getNumPaths();
    for(int i=0;same && i<paths.vst3.getNumPaths();++i)
        same=searchPaths_->vst3.getRawString(i)==paths.vst3.getRawString(i);
    if(same)return;
    auto next=std::make_shared<PluginSearchPaths>(paths);
    searchPaths_=std::move(next);
    scanned_=false;
}

void JuceDeviceLoader::addSearchPath(const juce::File& dir) {
    if(audio::onAudioThread)return;
    auto next=*searchPaths_;
    next.vst3.add(dir);
    next.clap.push_back(dir.getFullPathName().toStdString());
    setSearchPaths(next);
}

void JuceDeviceLoader::scan() {
    (void)discover(DiscoveryMode::Incremental);
}

bool JuceDeviceLoader::hydrateCatalogue(std::string& error) {
    if(audio::onAudioThread)return false;
    const auto result=discoverOwned(DiscoveryMode::CacheOnly,true);
    const bool accepted=result->outcome==DiscoveryOutcome::Published ||
        result->outcome==DiscoveryOutcome::Missing || result->outcome==DiscoveryOutcome::Legacy;
    error=accepted?std::string{}:result->error;return accepted;
}

std::shared_ptr<const DiscoveryResult> JuceDeviceLoader::discover(DiscoveryMode mode) {
    if(audio::onAudioThread)return {};
    return discoverOwned(mode,false);
}

std::shared_ptr<const DiscoveryResult> JuceDeviceLoader::discoverOwned(DiscoveryMode mode,bool allowLegacyFallback) {
    const auto searchPaths=searchPaths_; // Keep both formats on one admitted configuration.
    auto result=std::make_shared<DiscoveryResult>();result->mode=mode;result->catalogue=publishedCatalogue_;
    const auto failed=[&](const std::string& message,scan::State state) {
        result->error=message;result->report.complete=false;
        if(result->report.failures.empty())result->report.failures.push_back({"",message,state});
        result->outcome=result->report.cancelled?DiscoveryOutcome::Cancelled:DiscoveryOutcome::Failed;
        scanReport_=result->report;scanned_=false;
        // Corrupt persisted metadata fails closed even if a previous snapshot
        // remains available to a browser. A failed refresh otherwise retains
        // the last accepted runtime catalogue, not its successful prefix.
        if(mode==DiscoveryMode::CacheOnly || !publishedCatalogue_)hydrationError_=message;
    };
    try {
        if(mode!=DiscoveryMode::Incremental && mode!=DiscoveryMode::CacheOnly && mode!=DiscoveryMode::Refresh) {
            failed("invalid discovery mode",scan::State::Invalid);return result;
        }
        hydrationAttempted_=true;
        // Explicit discovery never silently switches to the legacy lazy path.
        // The ordinary hydrateCatalogue compatibility call is distinct.
        if(!allowLegacyFallback)catalogueHydrated_=true;
        auto stagedVst3=std::make_unique<juce::KnownPluginList>();
        std::vector<ClapPluginRef> stagedClap;
        if(mode!=DiscoveryMode::CacheOnly) {
            ++stats_.scans;
            auto config=scanConfig_;
            config.cacheMode=mode==DiscoveryMode::Refresh?ScanCacheMode::Refresh:ScanCacheMode::Reuse;
            if(scanConfig_.process.cancel && scanConfig_.process.cancel->load(std::memory_order_relaxed)) {
                result->report.cancelled=true;failed("discovery cancelled",scan::State::Cancelled);return result;
            }
            result->report=vst3_.scan(searchPaths->vst3,*stagedVst3,config);
            if(result->report.complete && !result->report.cancelled) {
                auto clapReport=clap_.scan(searchPaths->clap,config);
                result->report.complete=clapReport.complete;result->report.cancelled=clapReport.cancelled;
                for(auto& failure:clapReport.failures)result->report.failures.push_back(std::move(failure));
                if(result->report.complete && !result->report.cancelled)stagedClap=clap_.plugins();
            }
            if(scanConfig_.process.cancel && scanConfig_.process.cancel->load(std::memory_order_relaxed))
                result->report.cancelled=true;
            if(!result->report.complete || result->report.cancelled) {
                // Earlier rejected candidates remain in the report but did
                // not stop the batch. Name its fatal cause in the headline.
                const auto fatal=std::find_if(result->report.failures.begin(),result->report.failures.end(),[&](const auto& failure) {
                    return result->report.cancelled?failure.state==scan::State::Cancelled:
                        failure.state==scan::State::Unavailable || failure.state==scan::State::Invalid;
                });
                const auto diagnostic=fatal==result->report.failures.end() || fatal->error.empty()
                    ?(result->report.cancelled?std::string{"discovery cancelled"}:std::string{"discovery did not complete"})
                    :fatal->error;
                failed(diagnostic,
                    result->report.cancelled?scan::State::Cancelled:scan::State::Unavailable);
                return result;
            }
        } else {
            const auto file=scanConfig_.registry.empty()?appdata::pluginRegistryFile():scanConfig_.registry;
            ++stats_.catalogueReads;
            scan::CatalogueReadStatus status{};std::string error;
            auto stored=scan::readCatalogue(file,error,{},&status);
            if(status==scan::CatalogueReadStatus::Missing || status==scan::CatalogueReadStatus::Legacy) {
                result->outcome=status==scan::CatalogueReadStatus::Missing?DiscoveryOutcome::Missing:DiscoveryOutcome::Legacy;
                result->report.complete=false;
                result->error=status==scan::CatalogueReadStatus::Missing?"plug-in catalogue does not exist":"plug-in catalogue requires explicit discovery of its legacy registry";
                scanReport_=result->report;hydrationError_.clear();return result;
            }
            if(!stored)throw std::runtime_error(error);
            for(const auto& module:stored->modules) {
                if(module.format=="vst3") {
                    std::vector<juce::PluginDescription> descriptions;
                    if(!decodeVst3Metadata(module.plugins,module.canonicalPath,&descriptions,error))
                        throw std::runtime_error(error);
                    for(const auto& description:descriptions)stagedVst3->addType(description);
                } else {
                    for(const auto& plugin:module.plugins) {
                        ClapPluginRef ref;ref.bundlePath=module.canonicalPath;ref.id=plugin.id;
                        ref.name=plugin.name;ref.vendor=plugin.vendor;ref.version=plugin.version;
                        ref.features=plugin.features;ref.isInstrument=plugin.instrument;
                        stagedClap.push_back(std::move(ref));
                    }
                }
            }
        }
        if(stagedClap.size()>scan::kMaximumPlugins ||
            static_cast<std::size_t>(stagedVst3->getNumTypes())>scan::kMaximumPlugins-stagedClap.size())
            throw std::runtime_error("combined catalogue descriptor bound");
        auto snapshot=std::make_shared<DiscoveryCatalogue>();snapshot->clap=stagedClap;
        for(const auto& description:stagedVst3->getTypes())snapshot->vst3.push_back(description);
        std::sort(snapshot->vst3.begin(),snapshot->vst3.end(),[](const auto& a,const auto& b) {
            return a.fileOrIdentifier==b.fileOrIdentifier?a.createIdentifierString()<b.createIdentifierString():a.fileOrIdentifier<b.fileOrIdentifier;
        });
        std::sort(snapshot->clap.begin(),snapshot->clap.end(),[](const auto& a,const auto& b) {
            return a.bundlePath==b.bundlePath?a.id<b.id:a.bundlePath<b.bundlePath;
        });
        auto report=result->report;
        // Both formats, snapshot ownership and the copied report are prepared
        // before replacing either runtime catalogue. Old const snapshots live on.
        known_.swap(stagedVst3);clapCatalogue_.swap(stagedClap);
        publishedCatalogue_=std::move(snapshot);result->catalogue=publishedCatalogue_;
        result->outcome=DiscoveryOutcome::Published;scanReport_=std::move(report);
        scanned_=true;catalogueHydrated_=true;hydrationError_.clear();
    } catch(const std::exception& failure){failed(failure.what(),scan::State::Invalid);}
    return result;
}

engine::DeviceLoader JuceDeviceLoader::fn() {
    return [this](const engine::DeviceRequest& rq, std::string& err) { return make(rq, err); };
}

std::vector<std::string> JuceDeviceLoader::knownPlugins() const {
    std::vector<std::string> out;
    for (const juce::PluginDescription& d : known_->getTypes()) {
        const DeviceIdentity id = identityOf(d);
        out.push_back(id.format + "  " + id.uid + "  " + id.name + " -- " + id.vendor + " " +
                      id.version + "  (" + d.fileOrIdentifier.toStdString() + ")");
    }
    for (const ClapPluginRef& p : clapCatalogue_)
        out.push_back("clap  " + p.id + "  " + p.name + " -- " + p.vendor + " " + p.version +
                      "  (" + p.bundlePath + ")" + (p.isInstrument ? "  [instrument]" : ""));
    return out;
}

std::unique_ptr<DeviceInstance> JuceDeviceLoader::make(const engine::DeviceRequest& rq,
                                                       std::string& err) {
    if(audio::onAudioThread)return nullptr;
    ++stats_.requests;
    if (rq.ref == nullptr) {
        ++stats_.failed;
        err = "no plugin reference";
        return nullptr;
    }
    const std::string& f = rq.ref->format;
    if(f=="vst3" || f=="clap") {
        if(!hydrationAttempted_)(void)hydrateCatalogue(err);
        if(!hydrationError_.empty()){++stats_.failed;err=hydrationError_;return nullptr;}
    }
    if (f == "vst3") return makeVst3(rq, err);
    if (f == "clap") return makeClap(rq, err);
    if (f == "internal") {
        const auto* function=mix::functionForReference(f,rq.ref->uid);
        auto instance=function&&function->kind==mix::Function::Limiter?makeFullMixFunction(rq,err):makeBuiltinDevice(rq,err);
        if(instance)++stats_.builtin;else ++stats_.failed;
        return instance;
    }
    ++stats_.unhosted;
    err = "format '" + f + "' is not hosted (ADR-0041)";
    return nullptr;
}

std::unique_ptr<DeviceInstance> JuceDeviceLoader::makeVst3(const engine::DeviceRequest& rq,
                                                           std::string& err) {
    const rows::PluginRef& ref = *rq.ref;
    const auto uid = juce::String::fromUTF8(ref.uid.c_str());
    const auto find = [&](const juce::KnownPluginList& catalog) -> std::unique_ptr<juce::PluginDescription> {
        auto candidate = catalog.getTypeForIdentifierString(uid);
        if (!candidate || candidate->createIdentifierString() == uid) return candidate;
        // JUCE's lookup permits a suffix match. Keep ADI's exact persisted
        // identity when that broader match selects a different description.
        const auto types = catalog.getTypes();
        const auto match = std::find_if(types.begin(), types.end(), [&](const auto& description) {
            return description.createIdentifierString() == uid;
        });
        return match == types.end() ? nullptr : std::make_unique<juce::PluginDescription>(*match);
    };
    auto found = find(*known_);
    const auto hint = juce::String::fromUTF8(ref.pathHint.c_str());
    const bool absoluteHint = juce::File::isAbsolutePath(hint);
    bool knownHint = !found && absoluteHint &&
        known_->getTypeForFile(juce::File(hint).getFullPathName()) != nullptr;
    if(!found && absoluteHint) {
        const auto original=juce::File(hint).getFullPathName();const auto bytes=original.toStdString();
        std::error_code ec;const auto canonical=std::filesystem::canonical(
            std::filesystem::path(std::u8string(bytes.begin(),bytes.end())),ec);
        if(!ec) {
            const auto path=canonical.u8string();const auto canonicalText=juce::String::fromUTF8(
                reinterpret_cast<const char*>(path.c_str()));
            for(const auto& description:known_->getTypes())if(description.fileOrIdentifier==canonicalText) {
                knownHint=true;
                auto projected=description;projected.fileOrIdentifier=original;
                if(projected.createIdentifierString()==uid){found=std::make_unique<juce::PluginDescription>(std::move(projected));break;}
            }
        }
    }
    if(!found && catalogueHydrated_) {
        ++stats_.notFound;++stats_.needsDiscovery;
        err="NeedsDiscovery: VST3 identity is not in the validated catalogue";return nullptr;
    }

    // An unknown saved path still needs discovery until the startup catalogue
    // owns all discovery. A known module or matched identity needs no probe.
    if (!found && !knownHint && absoluteHint && juce::File(hint).exists()) {
        juce::KnownPluginList types;
        const auto report=vst3_.scanFile(hint,types,scanConfig_);
        if(!report.complete || !report.failures.empty()) {
            ++stats_.failed;err=report.failures.empty()?"VST3 path-hint scan incomplete":report.failures.front().error;
            return nullptr;
        }
        for (const auto& description : types.getTypes()) known_->addType(description);
        found = find(types);
    }
    if (!found && !knownHint) {
        if (!scanned_) scan();
        if(!scanned_){++stats_.failed;err="VST3 discovery unavailable or cancelled";return nullptr;}
        found = find(*known_);
    }
    if (!found) {
        ++stats_.notFound;
        err = "no VST3 with uid '" + ref.uid + "' in the search path (" +
              std::to_string(known_->getNumTypes()) + " known" +
              (ref.pathHint.empty() ? std::string() : "; hint '" + ref.pathHint + "'") + ")";
        return nullptr;
    }

    std::unique_ptr<DeviceInstance> dev = vst3_.makeDevice(*found, rq.sampleRate, rq.maxFrames, err);
    if (dev == nullptr || !dev->loaded()) {
        // The host's placeholder is discarded: the session builds its own,
        // with the mirror and the bytes (ADR-0122 d4).
        ++stats_.failed;
        if (err.empty()) err = "the plugin did not instantiate";
        return nullptr;
    }
    ++stats_.vst3;
    return dev;
}

std::unique_ptr<DeviceInstance> JuceDeviceLoader::makeClap(const engine::DeviceRequest& rq,
                                                           std::string& err) {
    const rows::PluginRef& ref = *rq.ref;
    // Discovery/quarantine resolves aliases. Preserve an older project's
    // preferred installed copy when its saved hint used a symlink or dots.
    std::string preferred=ref.pathHint;
    if(!preferred.empty()) {
        std::error_code error;
        const auto canonical=std::filesystem::canonical(
            std::filesystem::path(std::u8string(preferred.begin(),preferred.end())),error);
        if(!error){const auto bytes=canonical.u8string();preferred.assign(bytes.begin(),bytes.end());}
    }
    const ClapPluginRef* pick = nullptr;
    for (const ClapPluginRef& p : clapCatalogue_) {
        if (p.id != ref.uid) continue;
        if (pick == nullptr || p.bundlePath == preferred) pick = &p;
    }
    if(pick==nullptr && catalogueHydrated_) {
        ++stats_.notFound;++stats_.needsDiscovery;
        err="NeedsDiscovery: CLAP identity is not in the validated catalogue";return nullptr;
    }
    if(pick==nullptr && !scanned_) {
        scan();
        if(!scanned_){++stats_.failed;err="CLAP discovery unavailable or cancelled";return nullptr;}
        for(const auto& candidate:clapCatalogue_)if(candidate.id==ref.uid &&
            (pick==nullptr || candidate.bundlePath==preferred))pick=&candidate;
    }
    if (pick == nullptr) {
        ++stats_.notFound;
        err = "no CLAP with id '" + ref.uid + "' in the search path (" +
              std::to_string(clapCatalogue_.size()) + " known)";
        return nullptr;
    }
    std::unique_ptr<DeviceInstance> dev = clap_.makeDevice(*pick, rq.sampleRate, rq.maxFrames, err);
    if (dev == nullptr || !dev->loaded()) {
        ++stats_.failed;
        if (err.empty()) err = "the plugin did not instantiate";
        return nullptr;
    }
    ++stats_.clap;
    return dev;
}

}  // namespace adi::device
