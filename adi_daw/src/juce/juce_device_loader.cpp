// SPDX-License-Identifier: GPL-3.0-or-later
//
// See juce_device_loader.hpp. ADR-0122.

#include "juce/juce_device_loader.hpp"
#include "juce/builtin_host.hpp"
#include "juce/mix_pipeline_host.hpp"
#include "adi/mix_functions.hpp"
#include "adi/appdata.hpp"
#include "adi/engine/remote_devices.hpp"
#include "juce/net_plugin_admission.hpp"
#include "adi/scan_catalogue.hpp"
#include "adi/scan_prune.hpp"
#include "adi/media/media_ops.hpp"
#include "juce/vst3_scan_host.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <optional>
#include <thread>
#include <string>
#include <utility>

namespace adi::device {

namespace {

bool fitsCatalogue(std::size_t vst3, std::size_t clap) {
    return clap<=scan::kMaximumPlugins && vst3<=scan::kMaximumPlugins-clap;
}

void appendClapMetadata(std::vector<ClapPluginRef>& catalogue,
                        const std::string& path, const std::vector<scan::Plugin>& plugins) {
    for(const auto& plugin:plugins) {
        ClapPluginRef ref;ref.bundlePath=path;ref.id=plugin.id;ref.name=plugin.name;
        ref.vendor=plugin.vendor;ref.version=plugin.version;ref.features=plugin.features;
        ref.isInstrument=plugin.instrument;
        catalogue.push_back(std::move(ref));
    }
}

} // namespace

struct JuceDeviceLoader::DiscoveryJob {
    struct Output {
        std::shared_ptr<const DiscoveryResult> receipt;
        std::unique_ptr<juce::KnownPluginList> vst3;
        std::vector<ClapPluginRef> clap;
        scan::Report report;
    };
    std::uint64_t id = 0;
    std::uint64_t generation = 0;
    DiscoveryMode mode = DiscoveryMode::Incremental;
    std::atomic<bool> cancelled{false};
    std::atomic<DiscoveryPhase> phase{DiscoveryPhase::Queued};
    scan::BatchProgress vst3Progress, clapProgress;
    std::future<Output> result;

    // The future joins the worker before its borrowed atomics are destroyed.
    ~DiscoveryJob(){cancelled.store(true,std::memory_order_relaxed);}
};

JuceDeviceLoader::JuceDeviceLoader(bool includeDefaultSearchPaths) {
    if(includeDefaultSearchPaths)setSearchPaths(defaultSearchPaths());
}

JuceDeviceLoader::~JuceDeviceLoader() = default;

void JuceDeviceLoader::invalidateDiscovery() noexcept {
    ++discoveryGeneration_;
    if(discoveryJob_)discoveryJob_->cancelled.store(true,std::memory_order_relaxed);
}

void JuceDeviceLoader::setScanConfig(const scan::Config& config) {
    if(audio::onAudioThread)return;
    invalidateDiscovery();
    scanConfig_=config;scanned_=false;known_->clear();clapCatalogue_.clear();
    hydrationAttempted_=false;cacheOnly_=false;hydrationError_.clear();
    publishedCatalogue_.reset();
}

DiscoveryAdmission JuceDeviceLoader::startDiscovery(DiscoveryMode mode,unsigned workers) {
    if(audio::onAudioThread)return {DiscoveryStart::CallbackRefused,0,{}};
    if(discoveryJob_)return {DiscoveryStart::Busy,discoveryJob_->id,"a discovery job is awaiting adoption"};
    if((mode!=DiscoveryMode::Incremental && mode!=DiscoveryMode::Refresh && mode!=DiscoveryMode::CacheOnly) ||
        workers>scan::kMaximumProbeWorkers || nextDiscoveryJob_==0)
        return {DiscoveryStart::Invalid,0,"invalid discovery mode or worker count"};
    try {
        auto job=std::make_unique<DiscoveryJob>();
        job->id=nextDiscoveryJob_;job->generation=discoveryGeneration_;job->mode=mode;
        auto* state=job.get();
        auto config=scanConfig_;
        config.process.cancel=&state->cancelled;
        config.progress=nullptr;
        config.workers=workers==0 ?
            std::clamp(std::thread::hardware_concurrency()/2U,1U,scan::kMaximumProbeWorkers):workers;
        job->result=std::async(std::launch::async,[config,paths=searchPaths_,state,mode] {
            JuceDeviceLoader worker(false);
            worker.setScanConfig(config);
            worker.searchPaths_=paths;
            auto receipt=worker.discoverOwned(mode,false,state);
            return DiscoveryJob::Output{std::move(receipt),std::move(worker.known_),
                std::move(worker.clapCatalogue_),std::move(worker.scanReport_)};
        });
        const auto id=nextDiscoveryJob_++;
        discoveryJob_=std::move(job);
        managedDiscovery_=true;
        return {DiscoveryStart::Started,id,{}};
    } catch(const std::exception& error) {
        return {DiscoveryStart::Unavailable,0,error.what()};
    }
}

DiscoveryProgress JuceDeviceLoader::discoveryProgress() const {
    if(audio::onAudioThread || !discoveryJob_)return {};
    const auto& job=*discoveryJob_;
    const auto phase=job.result.wait_for(std::chrono::seconds(0))==std::future_status::ready
        ? DiscoveryPhase::Ready : job.phase.load(std::memory_order_relaxed);
    DiscoveryProgress result{job.id,phase,job.cancelled.load(std::memory_order_relaxed)};
    const auto* batch=phase==DiscoveryPhase::Vst3 ? &job.vst3Progress :
        (phase==DiscoveryPhase::Clap ? &job.clapProgress : nullptr);
    if(batch && batch->enumerated.load(std::memory_order_acquire)) {
        result.hasCandidateTotal=true;
        result.total=batch->total.load(std::memory_order_relaxed);
        result.completed=batch->completed.load(std::memory_order_relaxed);
    }
    return result;
}

bool JuceDeviceLoader::cancelDiscovery(std::uint64_t id) noexcept {
    if(audio::onAudioThread || !discoveryJob_ || discoveryJob_->id!=id ||
        discoveryProgress().phase==DiscoveryPhase::Ready)return false;
    discoveryJob_->cancelled.store(true,std::memory_order_relaxed);
    return true;
}

std::shared_ptr<const DiscoveryResult> JuceDeviceLoader::adoptDiscovery(std::uint64_t id) {
    if(audio::onAudioThread)return {};
    const auto progress=discoveryProgress();
    if(progress.job!=id || progress.phase!=DiscoveryPhase::Ready)return {};
    auto job=std::move(discoveryJob_);
    const auto refuse=[&](DiscoveryOutcome outcome,const std::string& error) {
        auto failure=std::make_shared<DiscoveryResult>();
        failure->mode=job->mode;failure->catalogue=publishedCatalogue_;
        failure->outcome=outcome;failure->error=error;failure->report.complete=false;
        failure->report.cancelled=outcome==DiscoveryOutcome::Cancelled;
        const auto state=failure->report.cancelled?scan::State::Cancelled:scan::State::Unavailable;
        failure->report.failures.push_back({"",error,state});
        scanReport_=failure->report;
        return failure;
    };
    if(job->mode==DiscoveryMode::CacheOnly)++stats_.catalogueReads;
    else ++stats_.scans;
    if(job->generation!=discoveryGeneration_)
        return refuse(DiscoveryOutcome::Stale,"discovery settings changed during the job");
    if(job->cancelled.load(std::memory_order_relaxed))
        return refuse(DiscoveryOutcome::Cancelled,"discovery cancelled");
    try {
        auto output=job->result.get();
        if(output.receipt->outcome==DiscoveryOutcome::Published) {
            known_.swap(output.vst3);clapCatalogue_.swap(output.clap);
            publishedCatalogue_=output.receipt->catalogue;
            scanned_=true;cacheOnly_=job->mode==DiscoveryMode::CacheOnly;
            hydrationAttempted_=true;hydrationError_.clear();
            scanReport_=std::move(output.report);
            return output.receipt;
        }
        auto failure=std::make_shared<DiscoveryResult>(*output.receipt);
        failure->catalogue=publishedCatalogue_;
        if(job->mode==DiscoveryMode::CacheOnly) {
            hydrationAttempted_=true;
            hydrationError_=failure->outcome==DiscoveryOutcome::Failed?failure->error:std::string{};
        }
        scanReport_=std::move(output.report);
        return failure;
    } catch(const std::exception& error) {
        return refuse(DiscoveryOutcome::Failed,error.what());
    }
}

std::shared_ptr<const DiscoveryCatalogue> JuceDeviceLoader::catalogue() const noexcept {
    if(audio::onAudioThread)return {};
    return publishedCatalogue_;
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
    invalidateDiscovery();
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
    if(managedDiscovery_) {
        error=hydrationError_;
        return error.empty();
    }
    const auto result=discoverOwned(DiscoveryMode::CacheOnly,true);
    const bool accepted=result->outcome==DiscoveryOutcome::Published ||
        result->outcome==DiscoveryOutcome::Missing || result->outcome==DiscoveryOutcome::Legacy;
    error=accepted?std::string{}:result->error;return accepted;
}

std::shared_ptr<const DiscoveryResult> JuceDeviceLoader::discover(DiscoveryMode mode) {
    if(audio::onAudioThread)return {};
    if(discoveryJob_) {
        auto busy=std::make_shared<DiscoveryResult>();busy->mode=mode;
        busy->catalogue=publishedCatalogue_;busy->report.complete=false;
        busy->error="an asynchronous discovery job is awaiting adoption";
        return busy;
    }
    return discoverOwned(mode,false);
}

std::shared_ptr<const DiscoveryResult> JuceDeviceLoader::discoverOwned(DiscoveryMode mode,bool allowLegacyFallback,
                                                               DiscoveryJob* job) {
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
    const auto maintenanceFailed=[&](const scan::ScanPruneResult& reason) {
        result->report.cancelled=reason.status==scan::ScanPruneStatus::Cancelled;
        const auto state=result->report.cancelled?scan::State::Cancelled:scan::State::Unavailable;
        const auto diagnostic=reason.error.empty()?"plug-in catalogue maintenance did not complete":reason.error;
        result->report.failures.push_back({"",diagnostic,state});
        failed(diagnostic,state);
    };
    const auto noteUnreadable=[&](const scan::ScanPruneResult& reason) {
        if(std::ranges::none_of(result->report.failures,[&](const auto& failure) {
            return failure.state==scan::State::UnreadablePath && failure.path==reason.unreadablePath;
        }))
            result->report.failures.push_back({reason.unreadablePath,reason.error,scan::State::UnreadablePath});
    };
    try {
        if(mode!=DiscoveryMode::Incremental && mode!=DiscoveryMode::CacheOnly && mode!=DiscoveryMode::Refresh) {
            failed("invalid discovery mode",scan::State::Invalid);return result;
        }
        hydrationAttempted_=true;
        // Explicit CacheOnly stays in force across subsequent project hydrations.
        if(mode==DiscoveryMode::CacheOnly && !allowLegacyFallback)cacheOnly_=true;
        auto stagedVst3=std::make_unique<juce::KnownPluginList>();
        std::vector<ClapPluginRef> stagedClap;
        scan::ScanPruneRequest prune;
        scan::ScanPruneResult bound;
        if(mode!=DiscoveryMode::CacheOnly) {
            ++stats_.scans;
            auto config=scanConfig_;
            config.cacheMode=mode==DiscoveryMode::Refresh?ScanCacheMode::Refresh:ScanCacheMode::Reuse;
            // SC-319: the user's Rescan tries again every plug-in a scan blocked -- two crashes, two
            // timeouts (a licence box waiting on its user) -- as Live's rescan does; an Incremental
            // discovery keeps the blocks. A registry that cannot be read is named in the report and
            // the scan goes on: the blocked stay blocked.
            std::string liftError;
            if(mode==DiscoveryMode::Refresh) {
                const auto file=config.registry.empty()?appdata::pluginRegistryFile():config.registry;
                if(auto registry=PluginRegistry::open(file,liftError)) {
                    if(const auto lifted=registry->unblockBlockedScans(liftError))
                        stats_.scanBlocksLifted+=static_cast<std::int64_t>(*lifted);
                }
            }
            if(scanConfig_.process.cancel && scanConfig_.process.cancel->load(std::memory_order_relaxed)) {
                result->report.cancelled=true;failed("discovery cancelled",scan::State::Cancelled);return result;
            }
            if(mode==DiscoveryMode::Incremental) {
                prune.cancel=config.process.cancel;
                for(int i=0;i<searchPaths->vst3.getNumPaths();++i) {
                    const auto root=searchPaths->vst3[i].getFullPathName().toStdString();
                    prune.roots.push_back({"vst3",media::pathFromUtf8(root)});
                }
                for(const auto& root:searchPaths->clap)
                    prune.roots.push_back({"clap",media::pathFromUtf8(root)});
                bound=scan::bindScanPruneRoots(prune);
                if(!bound && bound.unreadablePath.empty()){maintenanceFailed(bound);return result;}
            }
            if(job) {
                config.progress=&job->vst3Progress;
                job->phase.store(DiscoveryPhase::Vst3,std::memory_order_relaxed);
            }
            result->report=vst3_.scan(searchPaths->vst3,*stagedVst3,config);
            if(!bound.unreadablePath.empty())noteUnreadable(bound);
            if(!liftError.empty())
                result->report.failures.push_back({"","the blocked plug-ins could not be tried again: "+liftError,scan::State::Unavailable});
            if(result->report.complete && !result->report.cancelled) {
                if(job) {
                    config.progress=&job->clapProgress;
                    job->phase.store(DiscoveryPhase::Clap,std::memory_order_relaxed);
                }
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
                        scan::stopsDiscovery(failure.state);
                });
                const auto diagnostic=fatal==result->report.failures.end() || fatal->error.empty()
                    ?(result->report.cancelled?std::string{"discovery cancelled"}:std::string{"discovery did not complete"})
                    :fatal->error;
                const auto state=result->report.cancelled?scan::State::Cancelled:
                    (fatal==result->report.failures.end()?scan::State::Unavailable:fatal->state);
                failed(diagnostic,state);
                return result;
            }
        } else {
            if(job)job->phase.store(DiscoveryPhase::ReadingCache,std::memory_order_relaxed);
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
                    appendClapMetadata(stagedClap,module.canonicalPath,module.plugins);
                }
            }
        }
        if(!fitsCatalogue(static_cast<std::size_t>(stagedVst3->getNumTypes()),stagedClap.size()))
            throw std::runtime_error("combined catalogue descriptor bound");
        auto snapshot=std::make_shared<DiscoveryCatalogue>();snapshot->clap=stagedClap;
        for(const auto& description:stagedVst3->getTypes())snapshot->vst3.push_back(description);
        std::sort(snapshot->vst3.begin(),snapshot->vst3.end(),[](const auto& a,const auto& b) {
            return a.fileOrIdentifier==b.fileOrIdentifier?a.createIdentifierString()<b.createIdentifierString():a.fileOrIdentifier<b.fileOrIdentifier;
        });
        std::sort(snapshot->clap.begin(),snapshot->clap.end(),[](const auto& a,const auto& b) {
            return a.bundlePath==b.bundlePath?a.id<b.id:a.bundlePath<b.bundlePath;
        });
        // An unreadable search subtree cannot prove any cached module is missing.
        // Publish the modules we did validate and defer all destructive pruning.
        const bool incompleteWalk=std::ranges::any_of(result->report.failures,[](const auto& failure) {
            return failure.state==scan::State::UnreadablePath;
        });
        if(mode==DiscoveryMode::Incremental && !incompleteWalk) {
            if(job)job->phase.store(DiscoveryPhase::Reconciling,std::memory_order_relaxed);
            prune.discoveryComplete=true;
            const auto registry=scanConfig_.registry.empty()?appdata::pluginRegistryFile():scanConfig_.registry;
            const auto pruned=scan::pruneMissingScans(registry,prune);
            if(!pruned) {
                if(pruned.unreadablePath.empty()){maintenanceFailed(pruned);return result;}
                noteUnreadable(pruned);
            }
        }
        auto report=result->report;
        // Both formats, snapshot ownership and the copied report are prepared
        // before replacing either runtime catalogue. Old const snapshots live on.
        known_.swap(stagedVst3);clapCatalogue_.swap(stagedClap);
        publishedCatalogue_=std::move(snapshot);result->catalogue=publishedCatalogue_;
        result->outcome=DiscoveryOutcome::Published;scanReport_=std::move(report);
        scanned_=true;
        if(mode!=DiscoveryMode::CacheOnly)cacheOnly_=false;
        hydrationError_.clear();
    } catch(const std::exception& failure){failed(failure.what(),scan::State::Invalid);}
    return result;
}

engine::DeviceLoader JuceDeviceLoader::fn(engine::DeviceLoader grantedRemote, Vst3ClassLookup localVst3) {
    return engine::withRemoteDevices(
        [this, localVst3 = std::move(localVst3)](const engine::DeviceRequest& rq, std::string& err) {
            return make(rq, err, localVst3);
        }, std::move(grantedRemote));
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
                                                       std::string& err, const Vst3ClassLookup& localVst3) {
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
    if (f == "vst3") return makeVst3(rq, err, localVst3);
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

namespace {
// SC-324: the catalogue's description a saved VST3 identity names once its path is set aside. The
// identity is "<format>-<name>-<hex hash of the module's path>-<hex class ID>" (JUCE's
// createIdentifierString); the name may hold dashes, so it is read from the end. One plug-in installed
// in two folders gives the first in the catalogue's order.
std::optional<juce::PluginDescription> movedVst3(const juce::KnownPluginList& catalog, const juce::String& uid) {
    const int idAt=uid.lastIndexOfChar('-');
    if(idAt<=0)return std::nullopt;
    const int hashAt=uid.substring(0,idAt).lastIndexOfChar('-');
    if(hashAt<=0)return std::nullopt;
    const auto prefix=uid.substring(0,hashAt);const auto classId=uid.substring(idAt+1);
    if(classId.isEmpty())return std::nullopt;
    for(const auto& d:catalog.getTypes()) {
        if(d.pluginFormatName+"-"+d.name!=prefix)continue;
        const auto idToUse=d.uniqueId!=0?d.uniqueId:d.deprecatedUid;
        if(juce::String::toHexString(idToUse)==classId)return d;
    }
    return std::nullopt;
}
} // namespace

std::unique_ptr<DeviceInstance> JuceDeviceLoader::makeVst3(const engine::DeviceRequest& rq,
                                                           std::string& err, const Vst3ClassLookup& localVst3) {
    const rows::PluginRef& ref = *rq.ref;
    const auto catalogueId = localVst3 && net::isVst3ClassId(ref.uid) ? localVst3(ref.uid) : std::nullopt;
    const auto uid = juce::String::fromUTF8((catalogueId ? *catalogueId : ref.uid).c_str());
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
    // The catalogue's own description when `found` is a projection of it onto the saved spelling of its
    // path (a symlink or dots): the identity stays the project's, the plug-in loads from the real bundle.
    std::unique_ptr<juce::PluginDescription> module;
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
                if(projected.createIdentifierString()==uid) {
                    found=std::make_unique<juce::PluginDescription>(std::move(projected));
                    module=std::make_unique<juce::PluginDescription>(description);
                    break;
                }
            }
        }
    }
    // SC-324: JUCE's identifier string carries a hash of the module's full path, so a project saved on
    // another computer, or before the plug-in moved to another folder, names a path this catalogue does
    // not have. As Live finds a plug-in by its own ID wherever it is installed, the catalogue's plug-in of
    // the same format, name and class ID is this one: with the saved path's spelling it gives back the
    // project's exact identity (the saved hint), and it loads from where it is now.
    if(!found) {
        if(const auto moved=movedVst3(*known_,uid)) {
            auto projected=*moved;
            if(!hint.isEmpty())projected.fileOrIdentifier=hint;
            module=std::make_unique<juce::PluginDescription>(*moved);
            found=std::make_unique<juce::PluginDescription>(projected.createIdentifierString()==uid?projected:*moved);
            ++stats_.moved;
        }
    }
    // A completed discovery job owns the Browser catalogue. A project's
    // explicit path can still discover exactly one installed module when the
    // job has finished, using the same isolated helper and registry checks.
    if (!found && !cacheOnly_ && !discoveryJob_ && absoluteHint &&
        juce::File(hint).hasFileExtension("vst3") && juce::File(hint).exists()) {
        juce::KnownPluginList types;
        const auto report=vst3_.scanFile(hint,types,scanConfig_);
        if(!report.complete || !report.failures.empty()) {
            err=report.failures.empty()?"VST3 path-hint scan incomplete":report.failures.front().error;
            if(!report.complete && publishedCatalogue_) {
                ++stats_.notFound;++stats_.needsDiscovery;err="NeedsDiscovery: "+err;
            } else ++stats_.failed;
            return nullptr;
        }
        const auto prospective=static_cast<std::size_t>(known_->getNumTypes())+
            static_cast<std::size_t>(types.getNumTypes());
        if(!fitsCatalogue(prospective,clapCatalogue_.size())) {
            ++stats_.failed;err="combined catalogue descriptor bound";return nullptr;
        }
        for (const auto& description : types.getTypes()) known_->addType(description);
        found = find(types);
    }
    if (!found && !knownHint && !cacheOnly_ && !managedDiscovery_ && !scanned_ && !publishedCatalogue_) {
        scan();
        if(!scanned_){++stats_.failed;err="VST3 discovery unavailable or cancelled";return nullptr;}
        found = find(*known_);
    }
    if (!found) {
        ++stats_.notFound;
        if(cacheOnly_ || managedDiscovery_ || publishedCatalogue_) {
            ++stats_.needsDiscovery;
            err="NeedsDiscovery: VST3 identity is not in the validated catalogue";
            return nullptr;
        }
        err = "no VST3 with uid '" + ref.uid + "' in the search path (" +
              std::to_string(known_->getNumTypes()) + " known" +
              (ref.pathHint.empty() ? std::string() : "; hint '" + ref.pathHint + "'") + ")";
        return nullptr;
    }

    // Whatever found the description (the catalogue, a projection, or a path-hint scan, which keeps the saved
    // alias's spelling), the plug-in loads from the real bundle its path names (DF-E76).
    if (!module) {
        const auto bytes = found->fileOrIdentifier.toStdString();
        std::error_code ec;
        const auto canonical = std::filesystem::canonical(std::filesystem::path(std::u8string(bytes.begin(), bytes.end())), ec);
        if (!ec) {
            const auto path = canonical.u8string();
            const auto text = juce::String::fromUTF8(reinterpret_cast<const char*>(path.c_str()));
            if (text != found->fileOrIdentifier) {
                module = std::make_unique<juce::PluginDescription>(*found);
                module->fileOrIdentifier = text;
            }
        }
    }
    std::unique_ptr<DeviceInstance> dev =
        vst3_.makeDevice(*found, module ? *module : *found, rq.sampleRate, rq.maxFrames, err,
                         catalogueId ? std::string_view(ref.uid) : std::string_view{});
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
    std::filesystem::path installedHint;
    if(!preferred.empty()) {
        const auto path=media::pathFromUtf8(preferred);
        std::error_code error;
        if(path.is_absolute()) {
            const auto canonical=std::filesystem::canonical(path,error);
            if(!error) {
                const auto bytes=canonical.u8string();
                preferred.assign(bytes.begin(),bytes.end());
                if(juce::File(juce::String::fromUTF8(preferred.c_str())).hasFileExtension("clap"))
                    installedHint=canonical;
            }
        }
    }
    const auto find=[&]() -> const ClapPluginRef* {
        const ClapPluginRef* first=nullptr;
        for(const auto& candidate:clapCatalogue_) {
            if(candidate.id!=ref.uid)continue;
            std::error_code error;
            if(!std::filesystem::exists(media::pathFromUtf8(candidate.bundlePath),error))continue;
            if(candidate.bundlePath==preferred)return &candidate;
            if(first==nullptr)first=&candidate;
        }
        return first;
    };
    auto* pick=find();
    if(pick==nullptr && !cacheOnly_ && !discoveryJob_ && !installedHint.empty()) {
        auto scanner=scan::Scanner::open(scanConfig_,err);
        if(!scanner) {
            ++stats_.notFound;++stats_.needsDiscovery;err="NeedsDiscovery: "+err;
            return nullptr;
        }
        const auto result=scanner->probe("clap",installedHint);
        if(result.state!=scan::State::Ready) {
            ++stats_.failed;err=result.error.empty()?"CLAP path-hint scan incomplete":result.error;
            return nullptr;
        }
        const auto sameModule=[&](const auto& candidate){return candidate.bundlePath==result.path;};
        const auto retained=clapCatalogue_.size()-static_cast<std::size_t>(
            std::count_if(clapCatalogue_.begin(),clapCatalogue_.end(),sameModule));
        if(!fitsCatalogue(static_cast<std::size_t>(known_->getNumTypes()),retained+result.plugins.size())) {
            ++stats_.failed;err="combined catalogue descriptor bound";return nullptr;
        }
        std::erase_if(clapCatalogue_,sameModule);
        appendClapMetadata(clapCatalogue_,result.path,result.plugins);
        pick=find();
    }
    if(pick==nullptr && !cacheOnly_ && !managedDiscovery_ && !scanned_ && !publishedCatalogue_) {
        scan();
        if(!scanned_){++stats_.failed;err="CLAP discovery unavailable or cancelled";return nullptr;}
        pick=find();
    }
    if(pick==nullptr) {
        ++stats_.notFound;
        if(cacheOnly_ || managedDiscovery_ || publishedCatalogue_) {
            ++stats_.needsDiscovery;
            err="NeedsDiscovery: CLAP identity is not in the validated catalogue";
            return nullptr;
        }
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
